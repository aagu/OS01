# /proc/<pid>/maps — 进程内存映射查看

> **日期**: 2026-08-05
> **状态**: design-revising (v5 — fix path construction, column layout, heap premise, merge note, test assertions, truncation wording)

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

**0x600000 是栈守护页，不映射**：`USER_STACK_BASE=0x800000` (`task.h:298`)，task.c:989-996 映射的 2MB 在 0x800000(`vmm_map_page(user_pml4, ..., USER_STACK_BASE, ...)`)。0x600000 的 2MB 故意不映射——注释称为 "stack guard — overflow past the stack bottom triggers #PF"。`mm->start_stack = USER_STACK_BASE`。

**页面权限（按 PTE 写死）：**

| 区域 | PTE flags | maps 权限 |
|------|-----------|----------|
| ELF 加载区 | `PAGE_USER_Page`（无 `PAGE_XD`） | `rwxp` |
| Stack | `PAGE_USER_Page \| PAGE_XD` | `rw-p` |
| Heap | 在 ELF 的 2MB 页内，无独立映射 | `rwxp`（同 ELF 页） |

**Heap 与 ELF 同页的前提是二进制 <2MB**：`elf_load` 按 2MB 对齐映射，若 `end_code > 0x600000`（即二进制 >2MB），第二页会落 0x600000——恰好是栈守护页。此时守护页被 ELF 数据覆盖，且堆的权限不再是 `rwxp`（可能需要额外 2MB 页）。当前 busybox 规模远小于 2MB，无碍。与排序假设（ELF<brk<stack 需 ≤4MB）同类前提，注明即可。

## 目标

- `cat /proc/<pid>/maps` 输出本系统真实可达到的内存布局
- `cat /proc/self/maps` 同上（"self" 魔法目录已有）
- 每行 7 列（仿 Linux）：地址范围 + 权限 + 偏移 + dev + inode + path
- 不新增 `ino`/`dev` 到 `vfs_node_t`（复用 `fs_data`）
- 相邻同权限区域**不合并**（与 Linux 行为的已知差异）

## 输出格式

仿 Linux /proc/<pid>/maps 七列格式：

```
<vm_start>-<vm_end> <perms> <offset> <dev> <inode> <path>
```

- **地址**: `%012lx-%012lx`
- **地址对齐**: start round down、end round up 到 PAGE_SIZE (4KB)
- **权限**: `r`/`-` `w`/`-` `x`/`-` `p`/`s`
- **offset**: `%08lx`。VMA 用 `vm_pgoff << 12`；合成段恒为 `00000000`
- **dev**: 恒 `00`
- **inode**: `%8x`。合成段 `0`；VMA 有 vm_file → `(uintptr_t)vm_file->fs_data`；无 vm_file → `0`
- **path**: 合成段 `[stack]`/`[heap]` 或空（ELF 区）；VMA 有 vm_file → `vfs_resolve_path()` 结果

> **tmpfs 的 fs_data 是内核指针**（节点地址）。直接印进用户输出泄漏内核地址。本轮沿用 `vfs_stat` 现有行为（也把 fs_data 当 st_ino 返给用户态），待后续统一修复。

## gen_maps() 合成逻辑

`gen_maps(task_t *t, char *buf, int bufsz)` 输出四类条目，按地址升序 merge。

合成条目地址递增（ELF < brk < stack 对 ≤4MB 二进制成立），vma_list 已按 vm_start 升序。两路归并，免去收集+排序。

**不合并相邻同权限行**（与 Linux 差异）：[heap] 紧接 ELF 行，两者 rwxp 但不合并——显示为独立的两行。日后可改进。

### (A) 合成 ELF 加载区

仅当 `start_code < end_code`（用户进程的条件）：

```
start_code-end_code rwxp 00000000 00 0
```

- 不设 data/rodata 子段（mm_t 字段未被 elf_load 填充）
- offset=0（mm_t 未记录 p_offset）
- 内核线程由顶部 `mm == &init_mm` 早退

### (B) 合成 [stack]

仅当 `start_stack != 0`：

```
<stack_base>-<stack_top> rw-p 00000000 00 0  [stack]
```

- `stack_base = start_stack & ~(PAGE_4K_MASK)` → `0x800000`（`USER_STACK_BASE`）
- `stack_top = stack_base + 0x200000`（2MB 页）
- `0x600000` 的 2MB 是栈守护页（未映射），**不输出**

### (C) 合成 [heap]

仅当 `start_brk < end_brk`：

```
start_brk-end_brk rwxp 00000000 00 0  [heap]
```

- `SYS_brk` 只改 `end_brk`；初始 `start_brk == end_brk`（= `PAGE_4K_ALIGN(end_code)`）→ 不输出
- 权限 `rwxp`（在 ELF 的 2MB 页内；前提二进制 <2MB。见 §关键认识）

### (D) vma_list 条目

对 `t->mm->vma_list` 中每个 VMA：
```
vma->vm_start-vma->vm_end <r/w/x><p/s> <vm_pgoff<<12> <dev> <inode> <path>
```

- 权限: 从 `vm_flags` (VM_READ/WRITE/EXEC/SHARED) 推导
- offset: `vm_pgoff << 12`
- dev: `00`; inode: `vm_file` 非 NULL → `vm_file->fs_data` 转 `%x`；否则 `0`
- path: `vm_file` 非 NULL → `vfs_resolve_path()`；否则空

### 排序

合并假设：合成条目地址递增（ELF < brk < stack）对 ≤4MB 二进制成立。ELF >2MB 需同时注意 0x600000 守护页冲突（见 §关键认识）。vma_list 已按 vm_start 升序。

## VFS 扩展

### `vfs_resolve_path()` 新增

```c
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz);
```

**伪代码**（算法约定——实现须遵循）：

```
names[N] = []
cur = node
while cur != NULL:            // 回溯到 mount root
    names.append(cur->name)   // 挂载根的 name 是 "/"
    cur = cur->parent
// 此时 names = ["init.elf", "bin", "/"]

result = "" + node->mount->path    // 如 "/"
for i from N-1 down to 1:          // 跳过 index 0（挂载根的 "/"）
    result += "/" + names[i]
result += names[0]                 // 叶子节点名
// → "/bin/init.elf"
```

**终止条件**: `parent == NULL` → 到达挂载根，正常终止。

**挂载根的 `name` 不参与路径构造**：挂载根节点自身 name="/"(vfs.c:71)，若拼入则产出 `//bin/init.elf`。跳过之，只拼其以下各层 + 前置 `node->mount->path`。

**错误条件**（返回 -1）：`node`/`node->mount`/`node->name` 任一为 NULL。

**溢出**: bufsz 不足返回实际所需长度。调用方见下。

### 调用方截断处理

gen_maps 中 snprintf 一行时，路径列在行末。处理步骤：

```
path_slot = buf + written                    // 写完前 6 列后的剩余位置
path_room = bufsz - written - 4              // 留 4 字节给 "...\0"
n = vfs_resolve_path(vm_file, path_slot, path_room)
if (n >= path_room):                         // 溢出
    memcpy(path_slot + path_room - 4, "...", 3)
    path_slot[path_room - 1] = '\0'
```

等价于：路径槽为行缓冲剩余 −4，vfs_resolve_path 返回 ≥ 槽大小时，把槽末尾 4 字节覆盖为 `...\0`。

### 不新增 ino/dev 字段

`vfs_node_t` 不加 `ino`/`dev`。ino 概念已编码在 `fs_data`。留待 `stat`/`fstat` syscall 统一引入。

## procfs 改动

### 新增常量

```c
#define PROCFS_TYPE_MAPS  5   // /proc/<pid>/maps
```

### `procfs_read` 栈缓冲扩容

`procfs.c:129` `char local[512]` → `char local[4096]`。

### `procfs_readdir` 增加条目

`PROCFS_TYPE_PID_DIR` 和 `PROCFS_TYPE_SELF_DIR` 的 index=1：

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

两处 TOCTOU，不做防护：

1. **vma_list 遍历**: 另一 CPU 可并发 vma_remove/free
2. **mm 释放**: `find_task_by_pid` 返回后 `do_exit`→释放 mm

`status` 已有同款风险。mm 锁加固依赖 rwlock（路线图 P1 任务 B），届时统修。

## 错误处理 & 边界情况

| 场景 | 行为 |
|------|------|
| 内核线程（mm == &init_mm） | 返回 0 |
| Zombie 进程（mm == NULL） | 返回 0 |
| start_code == end_code | 无 ELF 行 |
| start_brk == end_brk | 无 [heap] 行 |
| start_stack == 0 | 无 [stack] 行 |
| vma_list 为空 + 无合成段 | 返回 0 |
| 路径缓冲溢出 | 槽末尾 4 字节覆盖为 `...\0` |
| node/mount/name 为 NULL | vfs_resolve_path 返回 -1 → 印 `?` |
| 并发 vma / mm 释放 | 已知风险，不做防护 |
| tmpfs fs_data 泄漏 | 已知，沿用 vfs_stat 行为 |

## 测试

`systest.c` 新增 `test_proc_maps()`:

1. `open("/proc/self/maps")` — 成功
2. `read()` → n > 0
3. 至少一行匹配 `^[0-9a-f]{8,12}-[0-9a-f]{8,12} [r-][w-][x-][ps]`
4. 存在 `[stack]` 标签
5. ≥2 行（ELF + stack；如果 brk 已扩展则有 heap）
6. stack 行地址范围为 `[0x800000, 0xa00000)`（`USER_STACK_BASE` ~ `+2MB`）
7. stack 行地址范围**不包含** `0x600000`（守护页）

## 文件变更预估

| 文件 | 改动 | 行数 |
|------|------|------|
| `kernel/fs/procfs.c` | gen_maps() + TYPE_MAPS + readdir + local 扩容 | ~110 |
| `kernel/include/fs/procfs.h` | PROCFS_TYPE_MAPS 常量 | +1 |
| `kernel/fs/vfs.c` | vfs_resolve_path() 实现 | ~35 |
| `kernel/include/fs/vfs.h` | vfs_resolve_path() 声明 | +2 |
| `user/systest.c` | test_proc_maps() | ~35 |

**总计: ~183 行，5 个文件。**

## 不在范围内

- `/proc/<pid>/fd/` — 推迟到 `readlink`/`symlink` 就绪后
- `/proc/<pid>/smaps` — 细粒度内存统计，远期
- `task_t->exe_path` — 不在本轮 scope
- `stat`/`fstat` — 不在本轮 scope
- VFS `ino`/`dev` 字段 — 复用 `fs_data`，不增字段
- `mm_t` 锁/并发安全 — 依赖 rwlock
- ELF 4KB 权限细分 — 依赖 4KB 页面支持
- tmpfs ino 内核地址泄漏 — 沿用 vfs_stat 行为，待统一修复
