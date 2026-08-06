# /proc/<pid>/maps — 进程内存映射查看

> **日期**: 2026-08-05
> **状态**: design-revising (v2 — address P0/P1/P2 review defects)

## 动机

当前 `/proc/<pid>/` 下只有 `status` 文件，调试体验有限。内核已有 VMA (`mm_t->vma_list`) 及 ELF 段描述字段 (`start_code/end_code` 等)，但没有任何方式查看进程内存布局。`/proc/<pid>/maps` 将其暴露为用户态可读的文本文件。

`/proc/<pid>/fd/` 本应同时实现，但 OS01 缺少 `readlink`/`symlink` syscall（路线图 P2），fd 目录需要符号链接才能有意义地显示 fd→文件 的映射关系。故推迟到 symlink 就绪后。

## 关键认识：进程内存≠vma_list

**`vma_list` 只包含 `do_mmap` 产生的区域。** 下列区域不在 VMA 中：

| 区域 | 实现方式 | 位置 |
|------|---------|------|
| ELF 代码段 | `elf_load()` → `vmm_map_page` 2MB 大页，不调 vma_insert | `elf.c:31` |
| ELF 数据段/rodata | 同上 | `elf.c` |
| 栈 | `task.c:997` 直接映射 `USER_STACK_BASE..USER_STACK_TOP` 2MB | `task.c` |
| 堆 | `SYS_brk` 只改 `mm->end_brk`，libc malloc 只用 brk | `trap.c:981` |

`vma_free_all` 注释明说 "Does NOT touch 2MB ELF pages (those are tracked outside VMA)" (`vma.c:58`)。

因此 `gen_maps()` **不能只遍历 vma_list**，必须从 `mm_t` 字段合成全部区域。

## 目标

- `cat /proc/<pid>/maps` 输出 Linux 兼容的完整内存布局
- `cat /proc/self/maps` 同上（"self" 魔法目录已有）
- 每一行：地址范围 + 权限 + 偏移 + 设备:inode + 路径
- 不新增 `ino`/`dev` 到 `vfs_node_t`（复用 `fs_data`，见 §VFS 扩展）

## 输出格式

```
<vm_start>-<vm_end> <r/w/x><p/s> <offset> <dev:inode> <path>
```

示例:
```
00400000-00401000 r-xp 00000000 00:00 0
00401000-00402000 rw-p 00001000 00:00 0
00500000-00501000 r--p 00000000 00:00 0
00600000-00602000 rw-p 00000000 00:00 0        [heap]
40000000-40200000 rw-p 00000000 00:00 0        [stack]
7f000000-7f001000 rw-p 00000000 00:00 0        /lib/ld.so
```

- 地址: `%012lx-%012lx`（64 位安全，EL64 惯例）
- 地址对齐: start round down、end round up 到 PAGE_SIZE (4KB)
- 权限: `r`/`-` `w`/`-` `x`/`-` `p`/`s` (p=private, s=shared)
- offset: 对 VMA 用 `vm_pgoff << 12`; 对合成段用 `sec_start - mm->start_xxx`
- dev:inode: 合成 FS 一律 `00:00`; ext2/FAT 映射用 `00:0<fs_data>`
- path: 空（合成段不跟踪 exec 路径）或 VMA 路径

## gen_maps() 合成逻辑

`gen_maps(task_t *t, char *buf, int bufsz)` 按以下步骤生成输出：

### Step 1: 合成 ELF 段（取自 mm_t 字段）

EL64 ABI: code 段从 `p_vaddr` 开始但不保证 4KB 对齐；`start_code`/`end_code` 存的是 ELF 段的原始虚拟地址。输出时 round down/up 到 PAGE_SIZE。

```
addr_range(start_code, end_code) -> r-xp, offset=0,                 "/path" 为空
addr_range(start_data, end_data) -> rw-p, offset=end_code-start_code,"/path" 为空
addr_range(start_rodata, end_rodata) -> r--p,                         "/path" 为空
```

**可跳过零长度段**：如果 `start_xx == end_xx`（如无 rodata），不输出该行。

**不跟踪 exec 路径**：`mm_t` 没有对应 `vm_file`，path 列为空。这是设计取舍——加 `task_t->exe_path` 需要改 `sys_exec`/`elf_load`/`task_t`，scope 过大留待后续。

### Step 2: 合成 [heap]

```
addr_range(start_brk, end_brk) -> rw-p, path="[heap]"
```

注意: `SYS_brk` 只改 `end_brk`，初始 `start_brk == end_brk`（均为 ELF DATA 段末尾），未扩展时无 [heap] 行。

### Step 3: 合成 [stack]

栈基址 = `start_stack` (mm_t 字段，elf_load 时设置)。栈顶 = 该页顶部（当前为用户栈页的 2MB 上限 `USER_STACK_TOP`，但 `mm_t` 只有一个 `start_stack` 没有 end_stack——用 `start_stack & ~(0x200000-1) + 0x200000` 计算）。

```
addr_range(start_stack_page_base, start_stack_page_top) -> rw-p, path="[stack]"
```

### Step 4: 遍历 vma_list

对 `t->mm->vma_list` 中每个 VMA:
```
addr_range(vma->vm_start, vma->vm_end) -> perms, offset, path
```

- perms 从 `vm_flags` 提取 VM_READ/WRITE/EXEC/SHARED
- offset = `vm_pgoff << 12`
- path: 若 `vm_file` 非 NULL，调用 `vfs_resolve_path()`; 若 `VM_ANON`，path 为空

### Step 5: 排序

`mm_t` 字段无序（start_code < start_data 不保证），VMA 有序（按 vm_start 升序）。**所有条目输出前按 `vm_start` 升序排序**，保证 maps 输出可读性。

在 gen_maps 内用一个栈上数组 `struct { uint64_t start, end; const char *fmt; ... } lines[16]` 收集所有行，再排序、用 snprintf 输出。

## VFS 扩展

### 不新增 ino/dev 字段

**`vfs_node_t` 不加 `ino`/`dev`。** 原因：ino 概念已编码在 `fs_data` 中：

| FS | ino 来源 |
|----|---------|
| ext2 | `ext2_node_ino()` 从 `fs_data` 取 inode 号 |
| FAT32 | `entry->ino = cluster`（`fat.c:129`） |
| tmpfs | `entry->ino = node` 指针（`tmpfs.c`） |
| procfs/devfs | `fs_data`（PROCFS_ENCODE 等） |

且 `vfs_stat`（`vfs.c:344`）已用 `fs_data` 作为 `st_ino`。单独加字段会制造两个不一致的真相源。

gen_maps 的 dev:inode 列：
- 合成段（ELF code/data/rodata、heap、stack）: `00:00 0`
- VMA 有 vm_file: `00:` + `vm_file->fs_data` 转 `%x`
- VMA 无 vm_file (anon): `00:00 0`

dev 统一为 `00`（当前没有设备号概念，留待 `stat`/`fstat` 统一引入）。

### `vfs_resolve_path()` 新增

```c
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz);
```

- 沿 `parent` 链回溯到挂载根，构造 `/mount/a/b/file`
- 返回路径长度（不含 NUL），bufsz 不足返回截断长度
- 调用方（gen_maps）检测返回值 >= pathsz 时，末尾补 `...`

## procfs 改动

### 新增常量

```c
#define PROCFS_TYPE_MAPS  5   // /proc/<pid>/maps
```

### `procfs_read` 栈缓冲扩容

`procfs.c:129` 的 `char local[512]` → `char local[4096]`。gen_maps 需要 4KB 最小缓冲，STACK_SIZE=32KB 放得下。

### `procfs_readdir` 增加条目

`PROCFS_TYPE_PID_DIR` 和 `PROCFS_TYPE_SELF_DIR` 的 index=1 处注册:

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

`find_task_by_pid` 注释自称 "caller holds spinlock"，但 `procfs_read` 不持任何锁。`gen_maps` 遍历 `vma_list` 期间另一 CPU 可 `vma_remove`/`free`，遍历到已释放节点→内核崩溃。

`mm_t` 无锁字段，加锁是 scope 外的工作（且 `status` 已有同款风险短期不需要修）。

**决定: 接受风险，显式注释。** 方案：
1. `procfs_read` 顶部加注释说明这是 debug 接口，不做锁
2. `gen_maps` 收集行到栈数组时，遍历用 `list_for_each_entry_safe` 也不能消除 TOCTOU（真正的安全需要 mm 锁，不做）
3. 不在本轮实现 mm 锁——这依赖路线图中 rwlock（P1），可在那时一并加固

内核线程 `t->mm == &init_mm` 的跳过可防 partial 问题（内核线程无 VMA）。

## 错误处理 & 边界情况

| 场景 | 行为 |
|------|------|
| 内核线程 | t->mm == &init_mm → 返回 0 |
| Zombie 进程 | t->mm == NULL → 返回 0 |
| 零长度段 | start_xx == end_xx → 不输出该行 |
| start_brk == end_brk | 无 [heap] 行 |
| 路径缓冲溢出 | snprintf 返回 ≥ pathsz → 尾部补 `...` |
| vm_file→parent 为 NULL | vfs_resolve_path 返回 "/?" |
| 并发 vma_remove | 已知风险，不做防护（见 §并发安全） |

## 测试

`systest.c` 新增 `test_proc_maps()`:

1. `fd = open("/proc/self/maps")` — 验证成功打开
2. `read(fd, buf, sizeof(buf))` — 验证读取成功（n > 0）
3. 检查至少一行匹配 `^[0-9a-f]{8,12}-[0-9a-f]{8,12} [r-][w-][x-][ps]` 模式（地址 ≥8 位十六进制）
4. 检查 `[stack]` 字符串出现在输出中（用户进程必须有栈）
5. 验证输出包含至少 3 行（code + data + stack 至少，如果 brk 有 heap 也包含）

## 文件变更预估

| 文件 | 改动 | 行数 |
|------|------|------|
| `kernel/fs/procfs.c` | gen_maps() + TYPE_MAPS + readdir 条目 + local 扩容 | ~120 |
| `kernel/include/fs/procfs.h` | PROCFS_TYPE_MAPS 常量 | +1 |
| `kernel/fs/vfs.c` | vfs_resolve_path() 实现 | ~30 |
| `kernel/include/fs/vfs.h` | vfs_resolve_path() 声明 | +2 |
| `user/systest.c` | test_proc_maps() | ~30 |

**总计: ~183 行，5 个文件。**（去掉了 vfs_node_t 字段变更 + ext2/tmpfs/devfs 改动的 3 文件）

## 不在范围内

- `/proc/<pid>/fd/` — 推迟到 `readlink`/`symlink` 就绪后
- `/proc/<pid>/smaps` — 细粒度内存统计，远期
- `task_t->exe_path` — 不在本轮 scope
- `stat`/`fstat` syscall — 不在本轮 scope
- `mm_t` 锁/并发安全 — 依赖 rwlock（路线图 P1 任务 B）
- VFS `ino`/`dev` 字段 — 复用 `fs_data`，不增字段
