# /proc/<pid>/maps — 进程内存映射查看

> **日期**: 2026-08-05
> **状态**: design-revising (v4 — fix example/output/permission/path-end inconsistencies)

## 动机

当前 `/proc/<pid>/` 下只有 `status` 文件，调试体验有限。内核已有 VMA (`mm_t->vma_list`) 及 ELF 段描述字段 (`start_code`/`end_code` 等)，但没有任何方式查看进程内存布局。`/proc/<pid>/maps` 将其暴露为用户态可读的文本文件。

`/proc/<pid>/fd/` 本应同时实现，但 OS01 缺少 `readlink`/`symlink` syscall（路线图 P2），fd 目录需要符号链接才能有意义地显示 fd→文件 的映射关系。故推迟到 symlink 就绪后。

## 关键认识：进程内存≠vma_list

**`vma_list` 只包含 `do_mmap` 产生的区域。** 下列区域不在 VMA 中：

| 区域 | 实现方式 | 位置 |
|------|---------|------|
| ELF 加载区 | `elf_load()` → `vmm_map_page` 2MB 大页，不调 vma_insert | `elf.c:127` |
| 栈 | `task.c:997` 直接映射 `USER_STACK_BASE..USER_STACK_TOP` 2MB | `task.c` |
| 堆 | `SYS_brk` 只改 `mm->end_brk`，libc malloc 只用 brk | `trap.c:981` |

`vma_free_all` 注释明说 "Does NOT touch 2MB ELF pages (those are tracked outside VMA)" (`vma.c:58`)。

**更深层约束：elf_load 只填充 `start_code`/`end_code`** (`elf.c:157-161`)。`start_data`/`end_data`/`start_rodata`/`end_rodata` 仅 init_mm 被赋值 (`task.c:1679-1684`)，对任何用户进程这四个字段恒为 0。因此 gen_maps 只能输出**单条** ELF 区域。

**0x600000 是栈守护页，不映射**：`USER_STACK_BASE=0x800000` (`task.h:298`)，task.c:989-996 映射的 2MB 在 0x800000(`vmm_map_page(user_pml4, ..., USER_STACK_BASE, ...)`)。0x600000 的 2MB 故意不映射——"left unmapped as a stack guard — overflow past the stack bottom triggers #PF"。`mm->start_stack = USER_STACK_BASE`。

**页面权限：**

| 区域 | PTE flags | 实际权限 |
|------|-----------|---------|
| ELF 加载区 | `PAGE_USER_Page` (不含 XD) | `rwx` |
| Stack | `PAGE_USER_Page \| PAGE_XD` | `rw-` |
| Heap | 在 ELF 的 2MB 页内，无独立映射 | 同 ELF:`rwx` |

## 目标

- `cat /proc/<pid>/maps` 输出本系统真实可达到的内存布局
- `cat /proc/self/maps` 同上（"self" 魔法目录已有）
- 每一行：地址范围 + 权限 + 偏移 + 设备:inode + 路径
- 权限如实反映 PTE flags（见上方表格）

## 输出格式

```
<vm_start>-<vm_end> <r/w/x><p/s> <offset> <dev:inode> <path>
```

（本系统不跟踪 exec 路径，ELF 区域 path 列为空。）

- 地址: `%012lx-%012lx`（12 位十六进制，64 位安全；用户地址 < 0x800000000000）
- 地址对齐: start round down、end round up 到 PAGE_SIZE (4KB)
- 权限: `r`/`-` `w`/`-` `x`/`-` `p`/`s`。各区域按 PTE 写死（见 §合成条目权限）
- offset: VMA 用 `vm_pgoff << 12`；合成段恒为 `00000000`（mm_t 未记录 p_offset）
- dev:inode: 合成段 `00:00 0`；VMA 有 vm_file → `00:<fs_data> <fs_data>`；无 vm_file (anon) → `00:00 0`
- path: 合成段 `[stack]`/`[heap]` 或空（ELF 区）；VMA 有 vm_file → `vfs_resolve_path()`

## gen_maps() 合成逻辑

`gen_maps(task_t *t, char *buf, int bufsz)` 输出四类条目，按地址升序 merge 到输出缓冲。

合成条目地址递增（ELF < brk < stack 在常见 binary 尺寸下成立；注释注明排序假设），vma_list 已按 vm_start 升序。两路归并免去收集+排序的额外缓冲。

### 合成条目权限（按 PTE 写死，不泛化规则）

| 条目 | 权限 | 依据 |
|------|------|------|
| ELF 加载区 | `rwxp` | `PAGE_USER_Page`，无 `PAGE_XD`（elf.c:127） |
| Stack | `rw-p` | `PAGE_USER_Page \| PAGE_XD`（task.c:997） |
| Heap | `rwxp` | 在 ELF 的 2MB 页内，无独立映射——实际权限同 ELF |

> **未来：** 4KB 页面支持后，elf_load 可按 ELF p_flags 逐段设置 PTE（code 只读 +XD、data RW+XD、rodata 只读）。届时 maps 权限从 VMA `vm_page_prot` 读取即可。

### (A) 合成 ELF 加载区

仅当 `start_code < end_code`（用户进程才有）：

```
start_code-end_code rwxp 00000000 00:00 0
```

- 不设 data/rodata 子段（mm_t 字段未被 elf_load 填充）
- offset=0（mm_t 未记录 p_offset）
- 内核线程由顶部 `mm == &init_mm` 早退，不走到这里

### (B) 合成 [stack]

仅当 `start_stack != 0`：

```
<stack_base>-<stack_top> rw-p 00000000 00:00 0  [stack]
```

- `stack_base = start_stack & ~(PAGE_4K_MASK)` → `0x800000`（`USER_STACK_BASE`）
- `stack_top = stack_base + 0x200000`（2MB 页）
- `0x600000` 的 2MB 是栈守护页（未映射），**不输出**

### (C) 合成 [heap]

仅当 `start_brk < end_brk`：

```
start_brk-end_brk rwxp 00000000 00:00 0  [heap]
```

- `SYS_brk` 只改 `end_brk`；初始 `start_brk == end_brk`（= `PAGE_4K_ALIGN(end_code)`）→ 不输出
- 权限 `rwxp`（在 ELF 的 2MB 页内，硬件权限同 ELF 区；等 4KB 页面粒度后可按 VMA 精确设置）

### (D) vma_list 条目

对 `t->mm->vma_list` 中每个 VMA：
```
vma->vm_start-vma->vm_end <r/w/x><p/s> <vm_pgoff<<12> <dev:inode> <path>
```

- 权限: 从 `vm_flags` (VM_READ/WRITE/EXEC/SHARED) 推导，形如 `rw-p`/`r--p`/`rwx-`
- offset: `vm_pgoff << 12`
- dev:inode: `vm_file` 非 NULL → `00:0%x 0x%x`（`vm_file->fs_data`）；VM_ANON → `00:00 0`
- path: `vm_file` 非 NULL → `vfs_resolve_path()` 结果；否则空

**tmpfs 的 fs_data 是内核指针** (`tmpfs.c:222` 用节点地址作 ino)。直接印进用户输出泄漏内核地址。本轮沿用 `vfs_stat` 的现有行为（vfs.c:344 同样把 fs_data 当 st_ino 返回给用户态），待后续统一修复。

### 排序

合并假设：合成条目地址递增（ELF < brk < stack）对常见 ≤4MB 的静态二进制成立。加注释注明此假设；若未来 ELF >4MB 使 brk 越过 0x800000 则需调整。vma_list 已按 vm_start 升序保证有序。

## VFS 扩展

### `vfs_resolve_path()` 新增

```c
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz);
```

- 沿 `parent` 链回溯到**挂载根**（`node->parent == NULL` → 到达挂载根，此乃成功终止条件，不是错误）
- 构造 `/mount/a/b/file`，返回路径长度（不含 NUL）
- bufsz 不足返回实际所需长度

**终止条件**：`parent == NULL` 表示回溯到挂载根，终止。首次回溯时记录根节点为 `node`（此时 `node` 仍是叶子），以后每步 `node = node->parent`。此路径是：
```
leaf → parent → ... → mount_root (parent==NULL)
```
需从后向前构造 `/name(N-1)/.../name(1)/name(0)`。

**错误条件**（返回 -1）：`node` 为 NULL、`node->mount` 为 NULL、或 `node->name` 为 NULL。

**调用方处理溢出**：`vfs_resolve_path` 返回 >= pathsz 时，缓冲已满。maps 行需在调用前预留 4 字节给 `...\0`（路径列在行末，snprintf 到路径前 buffer 剩余 ≥ pathsz+4，截断时补 `...`）。

### 不新增 ino/dev 字段

`vfs_node_t` 不加 `ino`/`dev`。ino 概念已编码在 `fs_data`。单独加字段制造两个不一致的真相源。留待 `stat`/`fstat` syscall 统一引入。

## procfs 改动

### 新增常量

```c
#define PROCFS_TYPE_MAPS  5   // /proc/<pid>/maps
```

### `procfs_read` 栈缓冲扩容

`procfs.c:129` 的 `char local[512]` → `char local[4096]`。gen_maps 需要 4KB 最小缓冲（~6 行 × 80 字节 + 路径），STACK_SIZE=32KB 放得下。

### `procfs_readdir` 增加条目

`PROCFS_TYPE_PID_DIR` 和 `PROCFS_TYPE_SELF_DIR` 的 index=1 处注册 maps:

```c
case 1:
    strcpy(entry->name, "maps");
    entry->type = VFS_FILE;
    entry->size = 4096;
    entry->ino = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_MAPS, pid);
    return 0;
```

### `procfs_read` 增加 case

```c
case PROCFS_TYPE_MAPS: {
    task_t *t = find_task_by_pid((int)pid);
    if (!t || !t->mm) return 0;
    len = gen_maps(t, local, sizeof(local));
    break;
}
```

## 并发安全：已知风险

`find_task_by_pid` 注释自称 "caller holds spinlock"，但 `procfs_read` 不持任何锁。两处 TOCTOU 窗口：

1. **vma_list 遍历**: 另一 CPU 可并发 `vma_remove`/`free`，遍历到已释放 VMA→悬垂指针→内核崩溃
2. **mm 释放**: `t->mm == NULL` 检查不原子；另一 CPU 在 `find_task_by_pid` 返回后、`mm` 解引用前执行 `do_exit`→释放 mm，同样崩溃

`mm_t` 无锁字段，加锁 scope 外；`status` (`gen_status`) 已有同款风险，短期不修。

**决定: 接受风险，显式注释。** 方案：
1. `procfs_read` 顶部加注释说明这是 debug 接口，不做锁
2. `gen_maps` 顶部注释列出两处 TOCTOU
3. mm 锁加固依赖 rwlock（路线图 P1 任务 B），届时 procfs/status/maps 统一修

## 错误处理 & 边界情况

| 场景 | 行为 |
|------|------|
| 内核线程（mm == &init_mm） | 返回 0 |
| Zombie 进程（mm == NULL） | 返回 0 |
| start_code == end_code | 无 ELF 行（但在 `mm != &init_mm` 前提下一般不存在） |
| start_brk == end_brk | 无 [heap] 行 |
| start_stack == 0 | 无 [stack] 行 |
| vma_list 为空 + 无合成段 | 返回 0（空输出） |
| 路径缓冲溢出 | vfs_resolve_path 返回所需长度 → 调用方补 `...`（预留 4 字节） |
| node/mount/name 为 NULL | vfs_resolve_path 返回 -1 → 印 `?` |
| 并发 vma_remove / mm 释放 | 已知风险，不做防护（见 §并发安全） |
| tmpfs fs_data 泄漏 | 已知，沿用现有 vfs_stat 行为，待统一修复 |

## 测试

`systest.c` 新增 `test_proc_maps()`:

1. `fd = open("/proc/self/maps")` — 成功打开
2. `read(fd, buf, sizeof(buf))` — 成功（n > 0）
3. 字段数检查：至少一行匹配 `^[0-9a-f]{8,12}-[0-9a-f]{8,12} [r-][w-][x-][ps]` 模式
4. 检查存在 `[stack]` 标签
5. 检查至少 2 行（ELF 加载区 + stack，如果 brk 已扩展则有 heap）
6. 检查 stack 行的地址范围不包含 `0x600000`（守护页，不应出现）

## 文件变更预估

| 文件 | 改动 | 行数 |
|------|------|------|
| `kernel/fs/procfs.c` | gen_maps() + TYPE_MAPS + readdir + local 扩容 | ~110 |
| `kernel/include/fs/procfs.h` | PROCFS_TYPE_MAPS 常量 | +1 |
| `kernel/fs/vfs.c` | vfs_resolve_path() 实现 | ~30 |
| `kernel/include/fs/vfs.h` | vfs_resolve_path() 声明 | +2 |
| `user/systest.c` | test_proc_maps() | ~35 |

**总计: ~178 行，5 个文件。**

## 不在范围内

- `/proc/<pid>/fd/` — 推迟到 `readlink`/`symlink` 就绪后
- `/proc/<pid>/smaps` — 细粒度内存统计，远期
- `task_t->exe_path` — 不在本轮 scope
- `stat`/`fstat` — 不在本轮 scope
- VFS `ino`/`dev` 字段 — 复用 `fs_data`，不增字段
- `mm_t` 锁/并发安全 — 依赖 rwlock（路线图 P1 任务 B）
- ELF 4KB 权限细分 — 依赖 4KB 页面支持 + elf_load 按 p_flags 设 PTE
- tmpfs ino 内核地址泄漏 — 复用 `vfs_stat` 现有行为，待统一修复
