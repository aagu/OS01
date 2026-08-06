# /proc/<pid>/maps — 进程内存映射查看

> **日期**: 2026-08-05
> **状态**: design-approved

## 动机

当前 `/proc/<pid>/` 下只有 `status` 文件，调试体验有限。内核已有 VMA (`mm_t->vma_list`)，但没有任何方式查看。`/proc/<pid>/maps` 将 VMA 列表暴露为用户态可读的文本文件，是 Linux 调试的基础设施。

`/proc/<pid>/fd/` 本应同时实现，但 OS01 缺少 `readlink`/`symlink` syscall（路线图 P2），fd 目录需要符号链接才能有意义地显示 fd→文件 的映射关系。故推迟到 symlink 就绪后。

## 目标

- `cat /proc/<pid>/maps` 输出 Linux 兼容的 VMA 列表
- `cat /proc/self/maps` 同上（"self" 魔法目录已有）
- 每一行：地址范围 + 权限 + 偏移 + 设备:inode + 路径
- 匿名区域标记为 `[stack]` / `[heap]` / 空
- `vfs_node_t` 新增 `ino`/`dev` 字段，为后续 `stat`/`fstat` 铺路

## 输出格式

```
<vm_start>-<vm_end> <r/w/x><p/s> <offset> <dev:inode> <path>
```

示例:
```
00400000-00401000 r-xp 00000000 01:00 2        /bin/init
00401000-00402000 rw-p 00000000 01:00 2        /bin/init
00600000-00602000 rw-p 00000000 00:00 0        [heap]
40000000-40200000 rw-p 00000000 00:00 0        [stack]
```

- 地址: `%08lx-%08lx` 十六进制
- 权限: `r`/`-` `w`/`-` `x`/`-` `p`/`s` (p=private, s=shared)
- offset: `vm_pgoff << 12` (hex)
- dev:inode: `vm_file` 非 NULL 时用 `vm_file->dev:vm_file->ino`，否则 `00:00`
- path: `vfs_resolve_path(vm_file)` 或匿名标记 `[stack]`/`[heap]`

### 匿名映射识别

- VMA 覆盖 `[start_stack_addr, start_stack_addr+stack_size)` → `[stack]`
- VMA 在 `[start_brk, end_brk)` 范围内且 flags 包含 VM_ANON → `[heap]`
- 其余 VM_ANON → 空字符串

## VFS 扩展

### `vfs_node_t` 新增字段

```c
typedef struct vfs_node {
    // ... 现有字段 ...
    uint64_t ino;    // filesystem-specific inode number
    uint32_t dev;    // device identifier: 0=synthetic, 1=block device
} vfs_node_t;
```

### 各 FS 填充规则

| FS | ino | dev |
|----|-----|-----|
| ext2 | 真实 inode 号（已有 `ext2_node_ino()`） | 1 |
| FAT32 | cluster number | 1 |
| procfs | `(uintptr_t)node->fs_data` | 0 |
| devfs | 0 | 0 |
| tmpfs | `(uintptr_t)node`（指针作为唯一 ID） | 0 |

### `vfs_resolve_path()`

```c
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz);
```

- 沿 `parent` 链回溯到挂载根，构造 `/mount/a/b/file` 格式路径
- 返回路径长度（不含 NUL），出错返回 -1
- `vfs_node_t` 已有 `refcount`，unlink 不会释放仍在引用的节点，parent 链安全

## procfs 改动

### 新增常量

```c
#define PROCFS_TYPE_MAPS  5   // /proc/<pid>/maps
```

### `gen_maps(task_t *t, char *buf, int bufsz)`

- 遍历 `t->mm->vma_list`
- 无 mm（zombie/内核线程）→ 返回 0
- 每行调用 `snprintf` 构造 maps 行
- buffer ≥ 4096 字节（建议值）

### `procfs_readdir` 增加条目

在 `PROCFS_TYPE_PID_DIR` 和 `PROCFS_TYPE_SELF_DIR` 的 index=1 处注册 maps:

```c
case 1:
    strcpy(entry->name, "maps");
    entry->type = VFS_FILE;
    entry->size = 4096;
    entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_MAPS, pid);
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

## 边界情况

| 场景 | 行为 |
|------|------|
| 内核线程 | t->mm == &init_mm → gen_maps 返回 0 |
| Zombie 进程 | t->mm == NULL → gen_maps 返回 0 |
| 空 VMA 列表 | 返回 0（空输出） |
| 路径缓冲区溢出 | 截断路径，末尾加 `...` 标记 |
| vm_file 指向已 unlink 文件 | refcount 保护，parent 链仍有效 |

## 测试

`systest.c` 新增 `test_proc_maps()`:

1. `fd = open("/proc/self/maps")` — 验证成功打开
2. `read(fd, buf, sizeof(buf))` — 验证读取成功（n > 0）
3. 检查至少一行匹配 `^[0-9a-f]+-[0-9a-f]+ [r-][w-][x-][ps]` 模式
4. 检查至少存在 `[stack]` 标签（用户进程必须有栈）

## 文件变更预估

| 文件 | 改动 | 行数 |
|------|------|------|
| `kernel/include/fs/vfs.h` | vfs_node_t 加 ino + dev | +2 |
| `kernel/fs/vfs.c` | vfs_resolve_path() 实现 | ~30 |
| `kernel/fs/ext2.c` | 节点创建填充 ino/dev | ~5 |
| `kernel/fs/procfs.c` | gen_maps + TYPE_MAPS + readdir 条目 | ~60 |
| `kernel/include/fs/procfs.h` | PROCFS_TYPE_MAPS 常量 | +1 |
| `kernel/fs/tmpfs.c` | 节点创建填充 ino | ~2 |
| `kernel/fs/devfs.c` | 节点创建填充 ino/dev | ~2 |
| `user/systest.c` | test_proc_maps() | ~25 |

**总计: ~127 行，8 个文件。**

## 不在范围内

- `/proc/<pid>/fd/` — 推迟到 `readlink`/`symlink` 就绪后
- `/proc/<pid>/smaps` — 细粒度内存统计，远期
- `stat`/`fstat` syscall — 不在本轮 scope（虽然 ino/dev 字段为其铺路）
- 真机测试 — QEMU 验证即可
