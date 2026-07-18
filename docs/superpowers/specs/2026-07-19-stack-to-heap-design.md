# 内核栈空间改造方案

> **日期**: 2026-07-19
> **范围**: 内核栈上大块内存迁移到堆，STACK_SIZE 从 64KB 降至 32KB，引入 -O2 优化
> **动机**: 虽然 64KB 栈可运行，但浪费内存。目标在保持稳定性的前提下降低栈需求。

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

**模式**:

```c
// 原代码
uint8_t buf[4096];

// 改为
uint8_t *buf = kmalloc(4096);
if (!buf) return -ENOMEM;
// ... 使用 buf ...
kfree(buf);
```

**改造清单**（按源文件位置排序）:

| 函数 | 移除的栈分配 | 新分配 |
|------|-------------|--------|
| `ext2_read_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` |
| `ext2_write_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` |
| `ext2_write_superblock` | `uint8_t sb_buf[1024]` | `kmalloc(1024)` |
| `alloc_block` | `uint8_t buf[4096]` | `kmalloc(4096)` |
| `free_block` | `uint8_t buf[4096]` | `kmalloc(4096)` |
| `alloc_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` |
| `free_inode` | `uint8_t buf[4096]` | `kmalloc(4096)` |
| `ext2_bmap` | `uint32_t indirect[1024]` | `kmalloc(4096)` |
| `ext2_bmap_alloc` | `uint32_t indirect[1024]` | `kmalloc(4096)` |
| `ext2_find_dirent` | `uint8_t block_data[4096]` | `kmalloc(4096)` |
| `dirent_add` | `uint8_t block_data[4096]` | `kmalloc(4096)` |
| `dirent_del` | `uint8_t block_data[4096]` | `kmalloc(4096)` |
| `ext2_vfs_read` | `uint8_t block_buf[4096]` | `kmalloc(4096)` |
| `ext2_vfs_write` | `uint8_t block_buf[4096]` | `kmalloc(4096)` |
| `ext2_vfs_truncate` | `uint32_t indirect[1024]` | `kmalloc(4096)` |
| `ext2_vfs_rename` | `uint8_t bd[4096]` | `kmalloc(4096)` |
| `ext2_vfs_rename` | `uint8_t dir_data[4096]` | 复用同一 kmalloc（串行） |
| `ext2_vfs_rmdir` | `uint8_t bd[4096]` | `kmalloc(4096)` |
| `ext2_vfs_rmdir` 收缩 | `uint32_t indirect[1024]` | `kmalloc(4096)` |
| `ext2_vfs_rename` 收缩 | `uint32_t indirect[1024]` | `kmalloc(4096)` |
| `ext2_vfs_mkdir` | `uint8_t block_data[4096]` | `kmalloc(4096)` |
| `ext2_vfs_readdir` | `uint8_t block_data[4096]` | `kmalloc(4096)` |
| `ext2_vfs_unlink` | `uint8_t block_data[4096]` | `kmalloc(4096)` |

**错误处理**: 所有函数返回类型为 `int` / `uint32_t` / `struct vfs_node *`（可为 NULL），加 `if (!buf) return -ENOMEM;` 即可。调用者（VFS → syscall）已有错误传播路径。

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

### 3.4 实现 kcalloc

`kernel/include/kernel/slab.h` 中声明了 `kcalloc` 但未实现。补充实现：

```c
// kernel/memory/slab.c
void *kcalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}
```

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

---

## 6. 验证计划

1. **编译**: `make clean && make`（Release）和 `make NDEBUG=`（Debug）均通过
2. **启动**: `make run` 正常启动到 shell
3. **systest**: `make test-syscall` 全量 pass
4. **selftest**: `KERNEL_SELFTEST=1 make run` 测试全部通过
5. **栈溢出**: 在 `do_system_call` 入口加 RSP 检查（开发阶段断言），确认不会接近 32 KB 边界
