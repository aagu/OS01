# /proc/<pid>/maps — 进程内存映射查看

> **日期**: 2026-08-05
> **状态**: design-revising (v3 — address second-round P0/P1/P2/P3 review defects)

## 动机

当前 `/proc/<pid>/` 下只有 `status` 文件，调试体验有限。内核已有 VMA (`mm_t->vma_list`) 及 ELF 段描述字段 (`start_code`/`end_code` 等)，但没有任何方式查看进程内存布局。`/proc/<pid>/maps` 将其暴露为用户态可读的文本文件。

`/proc/<pid>/fd/` 本应同时实现，但 OS01 缺少 `readlink`/`symlink` syscall（路线图 P2），fd 目录需要符号链接才能有意义地显示 fd→文件 的映射关系。故推迟到 symlink 就绪后。

## 关键认识：进程内存≠vma_list

**`vma_list` 只包含 `do_mmap` 产生的区域。** 下列区域不在 VMA 中：

| 区域 | 实现方式 | 位置 |
|------|---------|------|
| ELF 加载区 | `elf_load()` → `vmm_map_page` 2MB 大页，不调 vma_insert | `elf.c:31` |
| 栈 | `task.c:997` 直接映射 `USER_STACK_BASE..USER_STACK_TOP` 2MB | `task.c` |
| 堆 | `SYS_brk` 只改 `mm->end_brk`，libc malloc 只用 brk | `trap.c:981` |

`vma_free_all` 注释明说 "Does NOT touch 2MB ELF pages (those are tracked outside VMA)" (`vma.c:58`)。

**更深层约束：elf_load 只填充 `start_code`/`end_code`** (`elf.c:156-161`)。`start_data`/`end_data`/`start_rodata`/`end_rodata` 仅在使用 `INIT_TASK` 宏的 init_mm 中被赋值 (`task.c:1679-1684`)，对任何用户进程这四个字段恒为 0。因此 gen_maps 只能输出**单条** ELF 区域，不是四条。

此外 **ELF 映射权限是恒 RWX**：elf_load 对 2MB 大页注释 "always RWX for user space (2MB pages can't enforce per-4KB permissions)" (`elf.c:125-126`)。maps 应如实印 `rwxp`，而不是虚构的 `r-xp`/`rw-p`。

## 目标

- `cat /proc/<pid>/maps` 输出本系统真实可达到的内存布局
- `cat /proc/self/maps` 同上（"self" 魔法目录已有）
- 每一行：地址范围 + 权限 + 偏移 + 设备:inode + 路径
- 权限如实反映 2MB 页面限制（`rwxp`）
- 不新增 `ino`/`dev` 到 `vfs_node_t`（复用 `fs_data`，见 §VFS 扩展）

## 输出格式

```
<vm_start>-<vm_end> <r/w/x><p/s> <offset> <dev:inode> <path>
```

本系统可达的示例（实际输出）:
```
000000400000-000000600000 rwxp 00000000 00:00 0
000000600000-000000800000 rwxp 00000000 00:00 0        [stack]
000000800000-000000802000 rw-p 00000000 00:00 0        [heap]
```

- 地址: `%012lx-%012lx`（12 位十六进制，64 位安全；用户地址 < 0x800000000000 `USER_ADDR_LIMIT`）
- 地址对齐: start round down、end round up 到 PAGE_SIZE (4KB)
- 权限: `r`/`-` `w`/`-` `x`/`-` `p`/`s`。合成段恒为 `rwxp`（见 §ELF 区域的权限）
- offset: VMA 用 `vm_pgoff << 12`；合成段恒为 `00000000`（mm_t 未记录 p_offset）
- dev:inode: 合成段 `00:00 0`；VMA 有 vm_file 则 `00:<fs_data> <fs_data>`；无 vm_file 则 `00:00 0`
- path: 合成段空（不跟踪 exec 路径）或 `[stack]`/`[heap]`；VMA 若有 vm_file 调用 `vfs_resolve_path()`

### ELF 区域的权限

elf_load 用 2MB 大页映射，PTE 实际是 `rwx`。印 `rwxp` 如实反映硬件页表——调试工具输出真实权限比虚构 `r-xp` 更有用。权限细分（code 只读、data 可写不可执行）需等到 4KB 页面支持，届时可改为从 ELF p_flags 推导并更新 `elf_load` 按段设置 PTE。本轮注明限制即可。

## gen_maps() 合成逻辑

`gen_maps(task_t *t, char *buf, int bufsz)` 输出三类条目，按地址升序 merge（免去收集+排序的固定数组问题）。

### 条目标记

gen_maps 不收集到固定数组。改为：遍历 `vma_list` 时，与合成条目按地址做 merge。两类条目源码地址有序（合成条目地址递增、vma_list 递增），两路归并即可。

### (1) 合成 ELF 加载区

仅当 `start_code < end_code`（用户进程才有的条件）:

```
start_code-end_code rwxp 00000000 00:00 0
```

- 不设 data/rodata 子段（mm_t 字段未被 elf_load 填充）
- offset=0（mm_t 未记录 p_offset）
- 权限 rwxp（2MB 页面限制；elf.c:125-126）

### (2) 合成 [stack]

仅当 `start_stack != 0`:

```
<start_stack_page_base>-<start_stack_page_top> rwxp 00000000 00:00 0  [stack]
```

- start_stack 是 `elf_load` 设置的栈顶附近值（`task.c:997` 用 `start_stack - 8` 作为初始 RSP）
- 页基址 = `start_stack & ~(0x200000 - 1)`
- 页顶 = 基址 + 0x200000（`USER_STACK_TOP` 定义为 `USER_STACK_BASE + 0x200000`）

### (3) 合成 [heap]

仅当 `start_brk < end_brk`:

```
start_brk-end_brk rw-p 00000000 00:00 0  [heap]
```

- `SYS_brk` 只改 `end_brk`；初始 `start_brk == end_brk` 时不输出
- 堆权限用 `rw-p`（标准堆不具备 exec；但 2MB 页面下实际是 rwxp，等 4KB 细粒度时修正）

### (4) vma_list 条目

对 `t->mm->vma_list` 中每个 VMA:
```
vma->vm_start-vma->vm_end <r/w/x><p/s> <vm_pgoff<<12> <00:00 or 00:fs_data> <path or empty>
```

- 权限: 从 `vm_flags` (VM_READ/WRITE/EXEC/SHARED) 推导
- offset: `vm_pgoff << 12`
- dev:inode: 若 `vm_file` 非 NULL → `00:0%x 0x%x`（dev=00, ino=`vm_file->fs_data`）；若 VM_ANON → `00:00 0`
- path: `vm_file` 非 NULL → `vfs_resolve_path()` 结果；否则空

**tmpfs 的 fs_data 是内核指针** (`tmpfs.c:222` 用节点地址作 ino)。直接印进用户输出泄漏内核地址。本轮沿用 `vfs_stat` 的现有行为（vfs.c:344 同样把 fs_data 当 st_ino 返回给用户态），待后续统一修复。

## VFS 扩展

### 不新增 ino/dev 字段

**`vfs_node_t` 不加 `ino`/`dev`。** ino 概念已编码在 `fs_data`：

| FS | ino 来源 |
|----|---------|
| ext2 | `ext2_node_ino()` 从 `fs_data` 取 inode 号 |
| FAT32 | `entry->ino = cluster`（`fat.c`） |
| tmpfs | `entry->ino = node` 指针（`tmpfs.c`） |
| procfs/devfs | `fs_data`（PROCFS_ENCODE 等） |

`vfs_stat` (`vfs.c:344`) 已用 `fs_data` 作为 `st_ino`。单独加字段制造两个不一致的真相源。

### `vfs_resolve_path()` 新增

```c
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz);
```

- 沿 `parent` 链回溯到挂载根，构造 `/mount/a/b/file`
- 返回路径长度（不含 NUL），bufsz 不足返回实际所需长度
- 调用方检测返回值 >= pathsz 时，末尾补 `...`
- parent 为 NULL 时返回 -1，调用方印 `?`

## procfs 改动

### 新增常量

```c
#define PROCFS_TYPE_MAPS  5   // /proc/<pid>/maps
```

### `procfs_read` 栈缓冲扩容

`procfs.c:129` 的 `char local[512]` → `char local[4096]`。gen_maps 需要 4KB 最小缓冲（~6 行 × ~80 字节 + 路径），STACK_SIZE=32KB 放得下。

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
2. **mm 释放**: `t->mm == NULL` 检查不原子；另一 CPU 在 `find_task_` 通过后、`mm` 解引用前执行 `do_exit`→释放 mm，同样崩溃

`mm_t` 无锁字段，加锁 scope 外；`status` (`gen_status`) 已有同款风险，短期不修。

**决定: 接受风险，显式注释。** 方案：
1. `procfs_read` 顶部加注释说明这是 debug 接口，不做锁
2. `gen_maps` 顶部注释列出两处 TOCTOU
3. mm 锁加固依赖 rwlock（路线图 P1 任务 B），届时 procfs/status/maps 统一修

## 错误处理 & 边界情况

| 场景 | 行为 |
|------|------|
| 内核线程 | t->mm == &init_mm → 返回 0 |
| Zombie 进程 | t->mm == NULL → 返回 0 |
| start_code == end_code | 无 ELF 行（内核线程，非用户进程） |
| start_brk == end_brk | 无 [heap] 行 |
| start_stack == 0 | 无 [stack] 行 |
| vma_list 为空 + 无合成段 | 返回 0（空输出） |
| 路径缓冲溢出 | vfs_resolve_path 返回 >= pathsz → 补 `...` |
| vm_file→parent 为 NULL | vfs_resolve_path 返回 -1 → 印 `?` |
| 并发 vma_remove / mm 释放 | 已知风险，不做防护（见 §并发安全） |
| tmpfs fs_data 泄漏 | 已知，沿用现有 vfs_stat 行为，待统一修复 |

## 测试

`systest.c` 新增 `test_proc_maps()`:

1. `fd = open("/proc/self/maps")` — 成功打开
2. `read(fd, buf, sizeof(buf))` — 成功（n > 0）
3. 字段数检查：至少一行匹配 `^[0-9a-f]{8,12}-[0-9a-f]{8,12} [r-][w-][x-][ps]` 模式
4. 检查存在 `[stack]` 标签（用户进程必须有栈）
5. 检查至少 2 行（ELF 加载区 + stack，如果 brk 已扩展则有 heap）

## 文件变更预估

| 文件 | 改动 | 行数 |
|------|------|------|
| `kernel/fs/procfs.c` | gen_maps() + TYPE_MAPS + readdir + local 扩容 | ~110 |
| `kernel/include/fs/procfs.h` | PROCFS_TYPE_MAPS 常量 | +1 |
| `kernel/fs/vfs.c` | vfs_resolve_path() 实现 | ~30 |
| `kernel/include/fs/vfs.h` | vfs_resolve_path() 声明 | +2 |
| `user/systest.c` | test_proc_maps() | ~30 |

**总计: ~173 行，5 个文件。**

## 不在范围内

- `/proc/<pid>/fd/` — 推迟到 `readlink`/`symlink` 就绪后
- `/proc/<pid>/smaps` — 细粒度内存统计，远期
- `task_t->exe_path` — 不在本轮 scope；即使不做，ELF 加载区印空路径也符合本系统现状（无动态链接、ET_EXEC 唯一可执行）
- `stat`/`fstat` — 不在本轮 scope
- VFS `ino`/`dev` 字段 — 复用 `fs_data`，不增字段
- `mm_t` 锁/并发安全 — 依赖 rwlock（路线图 P1 任务 B）
- ELF 4KB 权限细分 — 依赖 4KB 页面支持 + `elf_load` 按 p_flags 设置 PTE
- tmpfs ino 内核地址泄漏 — 复用 `vfs_stat` 现有行为，待统一修复
