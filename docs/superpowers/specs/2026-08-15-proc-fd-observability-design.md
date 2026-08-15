# /proc/<pid>/fd/ — 进程文件描述符只读可观测

> **日期**: 2026-08-15
> **状态**: design (v1)
> **姊妹篇**: [2026-08-05-proc-maps-design.md](2026-08-05-proc-maps-design.md)

## 动机

进程已通过 `/proc/<pid>/status`（状态）和 `/proc/<pid>/maps`（内存映射）可观测，但打开的文件描述符无暴露途径。`files_t.fd[NOFILE]`（`file.h:96`）与 `file_t` 的 `type + node/pipe/pty/sock` 联合体数据齐全，只缺一个 procfs 暴露层。

`/proc/<pid>/fd/` 让 shell 能枚举 fd 号、查看每个 fd 指向什么：

```
$ ls /proc/self/fd
0  1  2  3
$ cat /proc/self/fd/3
/proc/meminfo
```

### 对 proc-maps spec 的修订

`2026-08-05-proc-maps-design.md` §动机 与 §不在范围内 断言 fd 目录「需要符号链接才能有意义地显示 fd→文件 映射，故推迟到 `readlink`/`symlink` 就绪后」。**本设计推翻该前提**：Linux 的 `/proc/<pid>/fd/N` 是 symlink，但「只读可观测」目标用**内容即目标路径的普通合成文件**（`read` 返回 `目标路径\n`）即可达成，无需 symlink / `readlink` syscall。代价是 `ls -l` 不自动解引用（显示为普通文件而非 `N -> /path`），本轮接受此差异。

## 目标

- `ls /proc/<pid>/fd` 枚举打开的 fd 号
- `cat /proc/<pid>/fd/N` 输出该 fd 指向目标的文本描述
- `/proc/self/fd` 对称支持（复用 `PROCFS_PID_SELF` 哨兵）
- 只读、无 write 重定向、无真 symlink
- 零 ABI 变更（不加 syscall、不改 `file_t`/`files_t`/`pipe_t`/`socket_t`/`vfs_node_t`）

## 关键设计决策

### 1. 文件名即 fd 号（不用 fs_data 存 fd）

`fs_data` 现有编码为 32 位（`procfs.h:10-12`）：8 位 type + 24 位 pid，装不下第三个字段 fd。

`vfs_lookup` 已在解析 `fd/3` 时把条目名 `"3"` `strdup` 进 `node->name`（`vfs.c:239`）。因此 **fd 号由文件名承载**：`readdir` 输出 `"0"/"1"/"2"` 作为条目名，`read` 时 `atoi(node->name)` 取回 fd。这与 Linux 语义一致（fd 目录里条目名就是 fd 号），零编码改动。

### 2. 魔法文件而非 symlink

`fd/N` 做成 `PROCFS_TYPE_FD_LINK` 合成文件，`read` 直接渲染目标路径文本。绕开 symlink 基础设施（路线图 P1 #6），把 `readlink` 语义折叠进普通 `read`。

### 3. pipe/socket 用指针占位

Linux 用 `pipe:[12345]`/`socket:[12345]`。OS01 无稳定 inode 概念，用 `pipe_t *`/`socket_t *` 地址当伪 inode 占位。稳定编号计数器属超出「可观测」最小代价的增强，记录为不在范围。

## 新增 procfs 节点类型

```c
// kernel/include/fs/procfs.h
#define PROCFS_TYPE_FD_DIR   7   // /proc/<pid>/fd/  目录
#define PROCFS_TYPE_FD_LINK  8   // /proc/<pid>/fd/<N>  只读合成文件
```

`FD_DIR` 的 `fs_data = ENCODE(FD_DIR, pid)`；`FD_LINK` 同编码，**fd 号走 `node->name`**。

## 组件

### `gen_fd_target(task_t *t, int fd, char *buf, int bufsz)`

唯一新增的核心函数。按 `file_t.type` 分发渲染目标路径，返回写入字节数（不含 NUL）：

| `file_t.type` | 输出 | 说明 |
|---------------|------|------|
| `FD_VFS` / `FD_DEV` | `<绝对路径>\n` | `vfs_resolve_path(f->node)`（`vfs.c:759`，已在 gen_maps 使用）；失败 `"?\n"` |
| `FD_PIPE` | `pipe:[<ptr>]\n` | `(uintptr_t)f->pipe` 作伪 inode |
| `FD_PTY_MASTER` | `/dev/ptmx\n` | |
| `FD_PTY_SLAVE` | `/dev/pts<index>\n` | `f->pty->index` |
| `FD_SOCKET` | `socket:[<ptr>]\n` | `(uintptr_t)f->sock` 作伪 inode |
| 空槽 / 其他 | 返回 0 | 安全降级为空读 |

前置校验：`!t || !t->files || fd < 0 || fd >= NOFILE` → 返回 0；`t->files->fd[fd] == NULL` → 返回 0。

### `procfs_readdir` 扩展

1. `PROCFS_TYPE_SELF_DIR` 与 `PROCFS_TYPE_PID_DIR` 各加一个 index=2 条目：

```c
case 2:
    strcpy(entry->name, "fd");
    entry->type = VFS_DIR;
    entry->size = 0;
    entry->ino = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_FD_DIR, pid /* 或 SELF */);
    return 0;
```

2. 新增 `PROCFS_TYPE_FD_DIR` case——先 resolve self 哨兵，再遍历 fd 表，按 index 输出第 k 个非空槽位：

```c
case PROCFS_TYPE_FD_DIR: {
    uint32_t p = pid;
    if (p == PROCFS_PID_SELF) { if (!current) return -1; p = (uint32_t)current->pid; }
    task_t *t = find_task_by_pid((int)p);
    if (!t || !t->files) return -1;
    uint64_t k = index;
    for (int fd = 0; fd < NOFILE; fd++) {
        if (!t->files->fd[fd]) continue;
        if (k == 0) {
            snprintf(entry->name, VFS_NAME_MAX, "%d", fd);
            entry->type = VFS_FILE;
            entry->size = 4096;
            entry->ino = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_FD_LINK, pid);
            return 0;   // 保留 SELF 哨兵给 read 解析
        }
        k--;
    }
    entry->name[0] = '\0';
    return 0;
}
```

### `procfs_read` 扩展

1. 目录守卫加 `PROCFS_TYPE_FD_DIR`（当前只挡 `PID_DIR`/`ROOT`/`SELF_DIR`，`procfs.c:290`）。
2. 新增 `PROCFS_TYPE_FD_LINK` case：

```c
case PROCFS_TYPE_FD_LINK: {
    uint32_t p = pid;
    if (p == PROCFS_PID_SELF) { if (!current) return 0; p = (uint32_t)current->pid; }
    task_t *t = find_task_by_pid((int)p);
    if (!t) return 0;
    int fd = atoi(node->name);   // fd 号来自文件名
    len = gen_fd_target(t, fd, local, sizeof(local));
    break;
}
```

`atoi` 已声明于 `libc/include/stdlib.h:30`，`procfs.c` 已 `#include <stdlib.h>`；kernel 通过 `-lk` 链接 `libc/libk.a`，无需手写解析。

## 数据流

```
ls /proc/self/fd
  → getdents64 → vfs_getdents → procfs_readdir(FD_DIR, SELF)
      → resolve SELF → 遍历 fd[] → 输出 "0"/"1"/"2"...

cat /proc/self/fd/3
  → open: vfs_lookup 匹配 "3" → node->name="3", fs_data=FD_LINK+SELF
  → read: procfs_read(FD_LINK) → resolve SELF → atoi("3")=3
      → gen_fd_target(files->fd[3]) → "/proc/meminfo\n"
```

## 并发安全：已知风险

沿用 `gen_maps` 现有策略（proc-maps spec §并发安全）：`find_task_by_pid` 不加锁，两处 TOCTOU 窗口可接受——(1) 遍历 `files->fd[]` 时另一 CPU 并发 `fd_close`；(2) `find_task_by_pid` 返回后 task 释放。rwlock 属路线图 P1 #4，届时统一收紧。

## 错误处理 & 边界情况

| 场景 | 行为 |
|------|------|
| 目标 task 不存在 | 返回 0（空读） |
| fd 越界 / 空槽 | `gen_fd_target` 返回 0（空读） |
| `vfs_resolve_path` 失败 | 印 `"?"` |
| 目录节点被 read | 返回 0（目录守卫） |
| self 哨兵 + 无 current | 返回 0 / -1 |
| fd 在 readdir 后被 close | 空读，安全降级不 crash |
| pipe/socket 指针作 inode | 每次重启不同、非稳定编号（已知差异） |

## 测试

`systest.c` 新增 `test_proc_fd()`，三个确定性断言：

1. **FD_VFS 反解**：`fd = open("/proc/meminfo")` → 读 `/proc/self/fd/<fd>` 内容 == `"/proc/meminfo\n"`。
2. **FD_PIPE**：`pipe(fds)` → 读 `/proc/self/fd/<fds[0]>` 前缀 `"pipe:["`。
3. **目录枚举**：`opendir("/proc/self/fd")` + `readdir` 循环，能看到 `"0"`、`"1"`、`"2"`。

libc `sscanf` 无宽度支持（proc-maps spec 已注明），但本测试只用 `strcmp`/`strncmp`/`strstr`，无碍。

## 文件变更预估

| 文件 | 改动 | 行数 |
|------|------|------|
| `kernel/include/fs/procfs.h` | `PROCFS_TYPE_FD_DIR` / `FD_LINK` 常量 | +2 |
| `kernel/fs/procfs.c` | `#include <kernel/file.h>`、`gen_fd_target()`、read/readdir 各加分支 | ~+85 |
| `user/systest.c` | `test_proc_fd()` + 注册进 `tests[]` | ~+50 |
| `docs/roadmap.md` | P0 #2 标 ✅ | +1（实现完成后） |

**总计: ~+138 行，4 个文件。零 ABI 变更。**

## 不在范围内

- `/proc/<pid>/fd/N` 的 **write 重定向**（`echo x > fd/N` 触发 dup2 语义）— 超出「可观测」
- **真 symlink** 语义（`ls -l` 显示 `N -> /path`）— 依赖 `symlink`/`readlink`（路线图 P1 #6）
- `fdinfo` — 记录 P1
- pipe/socket 稳定 inode 计数器 — 记录 P1
- `files_t` 锁 / 并发安全 — 依赖 rwlock（路线图 P1 #4）
