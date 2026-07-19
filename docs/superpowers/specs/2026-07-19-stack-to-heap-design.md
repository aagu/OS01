# 内核栈空间改造方案

> **日期**: 2026-07-19
> **修订**: 2026-07-19 — 代码审查 14 条全量修复
> **范围**: 内核栈上大块内存迁移到堆，STACK_SIZE 从 64KB 降至 32KB，引入 -O2 优化
> **动机**: 虽然 64KB 栈可运行，但浪费内存。目标在保持稳定性的前提下降低栈需求。

> ⚠️ **警告**: 当前代码 `STACK_SIZE` 已改为 32KB（`task.h:40`），但堆迁移尚未实现！
> rename 路径峰值 ~34 KB > 32 KB。实施前必须先完成堆迁移，再缩栈。

---

## 1. 现状分析

### 1.1 编译环境

- 编译器: `clang -target x86_64-unknown-none`
- 当前 CFLAGS: `--sysroot=… -isystem=… -g -fno-stack-protector`（无 -O，即默认 -O0）
- 所有 ext2 函数标记 `__attribute__((noinline))`，帧不共享栈槽

### 1.2 实测帧大小（-O0，-fstack-usage）

| 函数 | 帧大小 | 栈上的大对象 |
|------|--------|-------------|
| `vfs_getdents` | 18,664 B | `vfs_dirent_t entries[64]` ≈ 17 KB |
| `ext2_vfs_rename` | 17,464 B | `bd[4096]` + `dir_data[4096]` + `indirect[1024]` + inode × N |
| `ext2_vfs_rmdir` | 8,616 B | `block_data[4096]` + `indirect[1024]` |
| `alloc_block` | 8,264 B | `buf[4096]` + 多个 inode 副本 |
| ext2 各内部函数 | ~4,200–4,400 B | `buf[4096]` / `block_data[4096]` / `indirect[1024]` |
| `do_system_call` | 3,224 B | 栈上 syscall 名表 + 寄存器溢出 |

### 1.3 最深调用链（rename 路径，-O0）

```
pt_regs (entry.S)                       192 B
do_system_call                        3,224 B
  → vfs_rename                          600 B
    → ext2_vfs_rename                17,464 B
      → dirent_del                    4,328 B
        → ext2_find_dirent            4,328 B
          → ext2_read_inode           4,168 B
──────────────────────────────────────────────
峰值                                  34,456 B  (> 32 KB)
```

### 1.4 slab 分配器现状

- 16 个 cache bucket: 32 B → 1 MB
- cache[7]（4 KB）已预分配 512 槽（1 个 2 MB 页），kmalloc/kfree 都是位操作
- cache[8]（8 KB）+ 未预分配，首次使用触发 `alloc_pages` 扩展
- 4 KB 子页分配器（subpage pool）与 slab 独立，不冲突

---

## 2. 改造目标

| 目标 | 原值 | 新值 |
|------|------|------|
| STACK_SIZE | 64 KB | **32 KB** |
| Release 优化 | -O0 | **-O2** |
| Debug 优化 | -O0 | **-O0**（不变） |
| ext2 栈上 4 KB 缓冲 | 有 | **全部迁移到堆** |
| vfs_getdents entries | 栈上 17 KB | **堆 kmalloc** |
| ext2 selftest save_* 缓冲 | 栈上 4–13 KB | **堆 kmalloc/kfree** |

### -O2 下栈峰值预估

```
Release (-O2 + 堆改造):
do_system_call                          568 B
  → vfs_rename + ext2_vfs_rename       ~600 B (子函数链)
──────────────────────────────────────────────
峰值                                   约 2–3 KB  <<< 32 KB

Debug (-O0 + 堆改造):
do_system_call                        3,224 B
  → ext2_vfs_rename 链 (无缓冲帧)     ~3,500 B
──────────────────────────────────────────────
峰值                                   约 15–18 KB  < 32 KB
```

32 KB 在两种模式下都有余量。

---

## 3. 改造方案

### 3.1 ext2: 所有 4 KB 缓冲 → kmalloc/kfree

**策略**: 每个函数按需 `kmalloc`，用完 `kfree`。不用共享缓冲池，保证未来去掉 spin_lock 后天然并发安全。

**通用模式**:

```c
// 原代码
uint8_t buf[4096];

// 改为
uint8_t *buf = kmalloc(4096);
if (!buf) return <failure-sentinel>;
// ... 使用 buf ...
kfree(buf);
```

#### 返回值兼容要求

⚠️ 各函数返回类型不同，`kmalloc` 失败时的 sentinel 值必须匹配原有语义：

| 返回类型 | 失败 sentinel | 适用函数 |
|----------|-------------|----------|
| `int` / `struct vfs_node *` | `-ENOMEM` / `NULL` | `ext2_read/write_inode`, `ext2_vfs_*`, `dirent_*` |
| `uint32_t`（block/ino 编号，0=失败） | `0` | `alloc_block`, `alloc_inode`, `ext2_bmap`, `ext2_bmap_alloc` |
| `void` | `return`（静默失败） | `free_block`, `free_inode` |

**关键细节**: `alloc_block`、`alloc_inode`、`ext2_bmap`、`ext2_bmap_alloc` 返回 `uint32_t`，其调用者已将 `0` 视为 "not found / out of space"，返回 `0` 与 `-ENOMEM` 语义一致。`free_block` / `free_inode` 返回 `void`，kmalloc 失败时静默 `return`（与现有 I/O 错误处理一致——这些函数内部已经对 I/O 错误不做错误传播）。

**改造清单**（按源文件位置排序）:

| 函数 | 移除的栈分配 | 新增堆分配 | 失败 sentinel |
|------|-------------|----------|:---:|
| `ext2_read_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_write_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_write_superblock` | `uint8_t sb_buf[1024]` | `kmalloc(1024)` | `-ENOMEM` |
| `ext2_init` | `uint8_t sb_buf[1024]` | `kmalloc(1024)` | `-ENOMEM` |
| `alloc_block` | `uint8_t buf[4096]` | `kmalloc(4096)` | `0` |
| `alloc_block` | `uint8_t zero[4096]` | 复用上面的 `buf`（先 `memset(…,0,4096)`） | — |
| `free_block` | `uint8_t buf[4096]` | `kmalloc(4096)` | `return` (void) |
| `alloc_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` | `0` |
| `free_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` | `return` (void) |
| `ext2_bmap` | `uint32_t indirect[1024]` | `kmalloc(4096)` | `0` |
| `ext2_bmap_alloc` | `uint32_t indirect[1024]` | `kmalloc(4096)` | `0` |
| `ext2_find_dirent` | `uint8_t block_data[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `dirent_add` | `uint8_t block_data[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `dirent_del` | `uint8_t block_data[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_read` | `uint8_t block_buf[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_write` | `uint8_t block_buf[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_truncate` | `uint32_t indirect[1024]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_rename` | `uint8_t bd[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_rename` | `uint8_t dir_data[4096]` | `kmalloc(4096)`（独立分配，不复用） | `-ENOMEM` |
| `ext2_vfs_rmdir` | `uint8_t bd[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_rmdir` | `uint32_t indirect[1024]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_rename` | `uint32_t indirect[1024]` | `kmalloc(4096)`（两处互斥分支，可同一分配） | `-ENOMEM` |
| `ext2_vfs_mkdir` | `uint8_t block_data[4096]` | `kmalloc(4096)` | `NULL`（返回 `struct vfs_node *`） |
| `ext2_vfs_readdir` | `uint8_t block_data[4096]` | `kmalloc(4096)` | `-ENOMEM` |
| `ext2_vfs_unlink` | `uint8_t block_data[4096]` | `kmalloc(4096)` | `-ENOMEM` |

> **注**: 4 KB slab（cache[7]）已预分配 512 槽，rename / rmdir 路径的多个 4KB 分配不会触发额外的 2MB 页分配。`ext2_write_superblock`、`ext2_init` 和 selftest 中 `save_sb` 用的 1024B 分配走 cache[5]（未预分配），首次使用触发一次 2MB `alloc_pages`，之后复用。

#### `alloc_block` 零填充优化

`alloc_block` 原有两个独立栈数组：
- `uint8_t buf[4096]` — 读 bitmap block
- `uint8_t zero[4096]` — 全零数据块，写入新分配的 block

改造后只需要一次 `kmalloc(4096)`：先用 `memset(ptr, 0, 4096)` 全零 → 写入 block → 再复用同一块内存读 bitmap block（两步操作串行，无冲突）。

### 3.2 vfs: vfs_getdents entries → kmalloc

```c
// 原代码
#define VFS_GETDENTS_SORT_MAX 64
vfs_dirent_t entries[VFS_GETDENTS_SORT_MAX];

// 改为
vfs_dirent_t *entries = kmalloc(sizeof(vfs_dirent_t) * VFS_GETDENTS_SORT_MAX);
if (!entries) return -ENOMEM;

// ... 函数出口处 ...
kfree(entries);
```

- entries 内容通过 `memcpy` 显式填充，不依赖预初始化
- VFS_GETDENTS_SORT_MAX 保持 64，未来可优化为按需分配

### 3.3 ext2 selftest: save_* 缓冲 → kmalloc/kfree

6 个 selftest 函数全部改造，每个用 `goto cleanup` 统一释放：

| 函数 | 栈分配 | 改为 |
|------|--------|------|
| `ext2_selftest_magic` | 无大缓冲 | 不改 |
| `ext2_selftest_struct_sizes` | 无大缓冲 | 不改 |
| `ext2_selftest_block_alloc` | `save_bitmap[4096]` + `save_bgdesc[4096]` + `save_sb[1024]` + `save_blk[4096]` | 各 `kmalloc` |
| `ext2_selftest_inode_alloc` | `save_ibitmap[4096]` + `save_sb[1024]` + `save_itable[4096]` | 各 `kmalloc` |
| `ext2_selftest_dirent_roundtrip` | `save_dir_blk[4096]` + `save_itable_blk[4096]` + `save_sb[1024]` | 各 `kmalloc` |
| `ext2_selftest_write_read` | `block_data[4096]` + `save_data[4096]` + `write_buf[4096]` + `read_buf[4096]` | 各 `kmalloc` |

### 3.4 实现 `kzalloc`

`kernel/include/kernel/slab.h` 中声明了单参 `kcalloc`（非标准 `calloc(nmemb, size)` 签名），但未实现。重命名为 `kzalloc` 并实现：

```c
// kernel/memory/slab.c
void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}
```

同时更新 `kernel/include/kernel/slab.h` 中的声明。无需搜索调用者——该函数以前未实现，无调用者。

### 3.5 构建系统：NDEBUG → -O2

```makefile
# kernel/Makefile
ifeq ($(NDEBUG),)
  # Debug: keep -O0 for easy GDB debugging
  CFLAGS += -O0
else
  # Release: -O2 shrinks stack frames significantly
  CFLAGS += -O2
endif
```

#### 3.5.1 `noinline` 与 `-O2` 共存

所有 ext2 函数标记了 `__attribute__((noinline))`。在 `-O2` + `NDEBUG` 下，`noinline` 阻止了编译器内联，但 `-O2` 仍能将帧内的寄存器溢出、临时变量和栈槽共享。实测效果（见 §1.2）：无论 -O0 还是 -O2，4KB 缓冲改成 heap 之后帧大小差异不大（~200B vs ~150B）。保留 `noinline` 不变——它在 `-O0`（Debug）下提供清晰的栈回溯，在 `-O2`（Release）下的影响可忽略。

### 3.6 STACK_SIZE: 64 KB → 32 KB

```c
// kernel/include/kernel/task.h
#define STACK_SIZE (32 * 1024)  // 32 KB
```

无条件 32 KB，不与 NDEBUG 联动。

---

## 4. 运行模式矩阵

| 模式 | 命令 | -O | STACK_SIZE | 栈峰值 (est.) | 状态 |
|------|------|:--:|------------|------------|:---:|
| Release | `make` / `NDEBUG=1` | -O2 | 32 KB | ~3 KB | ✅ |
| Debug | `make debug` / 不设 NDEBUG | -O0 | 32 KB | ~18 KB | ✅ |
| Selftest | `KERNEL_SELFTEST=1 NDEBUG=1` | -O2 | 32 KB | ~5 KB | ✅ |

---

## 5. 不改的部分

- `fat.c` 的 `uint8_t sector[512]`（512 B，很小，栈安全）
- `vfs.c` 路径名 `char[VFS_NAME_MAX]`（256 B，数量多但每帧小）
- `task.c` 的 `str_offset[128]`（1 KB，32 KB 栈下余量充足）
- `panic.c` 的 `buf[4096]`（已是全局 `.bss` 变量）
- `printk.c` 的 `buf_color[4096]` / `buf_serial[4096]`（已是 `static` 全局变量）
- `procfs.c` 的 `char local[512]`（512 B，栈安全）

### 潜在的未来改动

- `GET_CURRENT` 宏（`task.h:239`）：`"andq $-32768, %rbx"` 硬编码了 32KB 掩码。若未来调整 STACK_SIZE 需同步修改。建议加 `static_assert(sizeof(union task_union) == STACK_SIZE)` 在编译期保护。

---

## 6. 验证计划

### 6.1 顺序要求

⚠️ **堆迁移必须在缩栈之前完成。** 实施顺序：
1. 堆迁移（ext2 + vfs_getdents + selftest）
2. `-O2` + NDEBUG 联动
3. `STACK_SIZE` 缩至 32 KB（如仍为 64 KB）

当前 `task.h:40` 已改为 32 KB，若实施前运行 rename 路径会 `#PF`。要么先还原为 64 KB 再开始实施，要么按上述顺序实施（第一步就跑堆迁移）。

### 6.2 测试项

1. **编译**: `make clean && make`（Release）和 `make NDEBUG=`（Debug）均通过
2. **启动**: `make run` 正常启动到 shell
3. **systest**: `make test-syscall` 全量 pass（覆盖 open/read/write/rename/unlink/mkdir/rmdir/getdents）
4. **selftest**: `KERNEL_SELFTEST=1 NDEBUG=1 make run` 测试全部通过
5. **栈溢出守卫**: 在 `do_system_call` 入口添加 Debug 模式 RSP 检查（保留至少一个 release 周期）：
   ```c
   #ifndef NDEBUG
   // STACK_SIZE - 2KB 余量
   uint64_t *stack_bottom = ((uint64_t)(current) & ~(STACK_SIZE - 1));
   if ((uint64_t)__builtin_frame_address(0) - (uint64_t)stack_bottom < 2048)
       log_err("WARNING: RSP within 2KB of stack bottom!\n");
   #endif
   ```
