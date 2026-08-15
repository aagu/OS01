# /proc/<pid>/fd/ — 进程文件描述符只读可观测

> **日期**: 2026-08-15
> **状态**: design (v2 — 修订并发 UAF、内核地址泄漏、PTY include)
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
- **并发安全：不引入 use-after-free**（本迭代的硬约束，见 §并发安全）
- **不向用户态泄漏内核地址**

## 关键设计决策

### 1. 文件名即 fd 号（不用 fs_data 存 fd）

`fs_data` 现有编码为 32 位（`procfs.h:10-12`）：8 位 type + 24 位 pid，装不下第三个字段 fd。

`vfs_lookup` 已在解析 `fd/3` 时把条目名 `"3"` `strdup` 进 `node->name`（`vfs.c:239`）。因此 **fd 号由文件名承载**：`readdir` 输出 `"0"/"1"/"2"` 作为条目名，`read` 时从 `node->name` 严格解析取回 fd。这与 Linux 语义一致（fd 目录里条目名就是 fd 号），零编码改动。

### 2. 魔法文件而非 symlink

`fd/N` 做成 `PROCFS_TYPE_FD_ENTRY` 合成文件，`read` 直接渲染目标路径文本。绕开 symlink 基础设施（路线图 P1 #6），把 `readlink` 语义折叠进普通 `read`。

### 3. pipe/socket 不暴露内核地址

Linux 用 `pipe:[12345]`/`socket:[12345]`。OS01 无稳定 inode 概念，若用 `pipe_t *`/`socket_t *` 地址当伪 inode，会把内核堆地址直接暴露给用户态——即便当前无 KASLR，也会固化一个不必要的信息泄露 ABI。故用占位符 `pipe:[?]` / `socket:[?]`，只区分对象类型、不暴露身份。稳定单调 ID 计数器列为 P1 增强（不在本迭代）。

### 4. FD_ENTRY 而非 FD_LINK

`fd/N` 是普通合成文件，不是链接。节点类型命名 `PROCFS_TYPE_FD_ENTRY`，避免「LINK」误导。

## 新增 procfs 节点类型

```c
// kernel/include/fs/procfs.h
#define PROCFS_TYPE_FD_DIR    7   // /proc/<pid>/fd/  目录
#define PROCFS_TYPE_FD_ENTRY  8   // /proc/<pid>/fd/<N>  只读合成文件
```

`FD_DIR` 的 `fs_data = ENCODE(FD_DIR, pid)`；`FD_ENTRY` 同编码，**fd 号走 `node->name`**。

## 组件

### `gen_fd_target(file_t *f, char *buf, int bufsz)`

唯一新增的核心渲染函数。按 `file_t.type` 分发输出目标路径，返回写入字节数（不含 NUL）。**入参是已持有稳定引用的 `file_t *`**（由调用方 `files_get_file` 取得，见 §并发安全）：

| `file_t.type` | 输出 | 说明 |
|---------------|------|------|
| `FD_VFS` / `FD_DEV` | `<绝对路径>\n` | `vfs_resolve_path(f->node)`（`vfs.c:759`，已在 gen_maps 使用）；失败 `"?\n"` |
| `FD_PIPE` | `pipe:[?]\n` | 不暴露 `f->pipe` 地址 |
| `FD_PTY_MASTER` | `/dev/ptmx\n` | |
| `FD_PTY_SLAVE` | `/dev/pts<index>\n` | `f->pty->index` |
| `FD_SOCKET` | `socket:[?]\n` | 不暴露 `f->sock` 地址 |
| 其他 | 返回 0 | |

### `parse_fd(const char *s)`

严格十进制解析（**不用 `atoi`**），拒绝负号、前导垃圾、尾随字符、溢出：

```c
static int parse_fd(const char *s)
{
    if (!s || *s < '0' || *s > '9') return -1;   // 空 / 负号 / 前导垃圾
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;     // 尾随字符
        v = v * 10 + (*s - '0');
        if (v >= NOFILE) return -1;              // 越界
    }
    return v;
}
```

### `procfs_readdir` 扩展

1. `PROCFS_TYPE_SELF_DIR` 与 `PROCFS_TYPE_PID_DIR` 各加一个 index=2 条目（`"fd"`，`VFS_DIR`，`ino = ENCODE(FD_DIR, pid/SELF)`）。
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
            entry->ino = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_FD_ENTRY, pid);
            return 0;
        }
        k--;
    }
    entry->name[0] = '\0';
    return 0;
}
```

> 注：`readdir` 阶段的 `t->files` 访问与 §并发安全 的读取器协议一致，见下。

### `procfs_read` 扩展

1. 目录守卫加 `PROCFS_TYPE_FD_DIR`（当前只挡 `PID_DIR`/`ROOT`/`SELF_DIR`，`procfs.c:290`）。
2. 新增 `PROCFS_TYPE_FD_ENTRY` case：

```c
case PROCFS_TYPE_FD_ENTRY: {
    uint32_t p = pid;
    if (p == PROCFS_PID_SELF) { if (!current) return 0; p = (uint32_t)current->pid; }
    int fd = parse_fd(node->name);
    if (fd < 0) return 0;
    file_t *f = files_get_file_for_pid((int)p, fd);   // 稳定引用，见 §并发安全
    if (!f) return 0;
    len = gen_fd_target(f, local, sizeof(local));
    files_put_file(f);
    break;
}
```

### `#include` 集合

`procfs.c` 需新增两个 include：

```c
#include <kernel/file.h>   // file_t / files_t 完整定义
#include <kernel/pty.h>    // pty_struct 完整定义（file.h 只前置声明，访问 f->pty->index 需要）
```

`file.h:25` 只 `struct pty_struct;` 前置声明，不完整类型无法访问成员；必须引入 `pty.h`（其 `pid_t` 已在 tty.h/pty.h 以 `typedef int` 存在，无冲突）。

## 并发安全（本迭代核心，评审修订）

### 问题定性

现有 fd 数据路径**没有 fd 表锁**：`fd_close`（`file.c:232`）无锁地 `fd[fd]=NULL` 后 `refcount--` 归零即 `file_free`（释放 `file_t` 及其 `node`/`pipe`/`sock`）。`t->files` 指针本身有两个释放点——`do_exit`（`task.c:848` 同步 `files_free`）与 schedule 收割器（`task.c:627` `deferred_files_free` 异步）。因此：

- **R3（file_t UAF，阻断）**：读取器 `f = fd[fd]` 后、解引用 `f->type/node/pipe` 前，另一 CPU 的 `fd_close` 可释放 `f`。**这是活进程正常 `close()` 触发的新竞态**，比 maps 的 teardown 竞态更严重。
- **R2（files_t 表 UAF）**：读取器取得 `t->files` 后，`do_exit`/收割器可释放整个表。
- **R1（task_t UAF）**：`find_task_by_pid` 返回裸指针，收割器可 `list_del`+释放 task。此为 maps/status **已有**的 teardown 竞态。

### 设计：`files_t` 引用计数 + 表锁

R3 与 R2 耦合（锁长在表里，表被释放则锁失效），必须一起修。给 `files_t` 增加两字段：

```c
// file.h
typedef struct files_struct {
    spinlock_T   lock;       // 护 fd[] 槽位的 detach vs 读取器 ref-bump
    int          refcount;   // 表生命周期（/proc 读取器 pin）
    file_t      *fd[NOFILE];
    char        *cwd;
} files_t;
```

新增引用协议 API（`file.c`/`file.h`）：

| 函数 | 语义 |
|------|------|
| `files_pin(fs)` | `refcount++`（在 `task_list_lock` 内调用） |
| `files_unpin(fs)` | `refcount--`；归零则 `deferred_files_free(fs)`（**统一 deferred，不同步 free**） |
| `files_get_file(fs, fd)` | `fs->lock` 内取槽位，非空则 `file->refcount++`，返回稳定 `file_t*` |
| `files_put_file(f)` | `file->refcount--`；归零则 `file_free(f)` |

**决策：`files_unpin` 归零时统一走 `deferred_files_free`（deferred），不做同步 `files_free`。** 理由：`deferred_files_free` 已在 schedule 收割器内 `task_list_lock` 下调用（`task.c:627` 现状），deferred 路径不会在 schedule() 内重入调度器；同步 `files_free` 会触发 `file_free → pipe wake → task_wake` 的重入风险。代价是 `do_exit` 的 fd 关闭时序轻微推迟（见 §已知语义变化）。

### 规则

**槽位访问统一走 `fs->lock`**：`fd_close`（detach 后释放引用）、`fd_alloc`（写入）、`files_get_file`（读取+ref-bump）都持锁。唯一免锁的是 `files_free`（refcount 0 时 teardown，无并发读取器）与 `files_dup`（源表是 `current` 自己的，仅 `file_t` refcount 原子自增，无释放）。

**表生命周期统一走 pin/unpin**：

- 读取器（procfs）：`task_list_lock` 内 `find_task_by_pid` → `t->files` 非空则 `files_pin`，解锁；渲染；`files_unpin`。
- `do_exit`（`task.c:848`）：改为 `task_list_lock` 内 `fs = current->files; current->files = NULL`，解锁后 `files_unpin(fs)`。
- schedule 收割器（`task.c:627`）：改为 `task_list_lock` 内 `fs = t->files; t->files = NULL`（保留 `list_del`），解锁后 `files_unpin(fs)`（取代直接 `deferred_files_free`）。

### 锁顺序

- `task_list_lock` **只在**定位 task + pin 表 + 置空 `t->files` 时持有，**不跨** `files_get_file`/`vfs_resolve_path`/`files_put_file`（避免全局锁持有期做可能拿其他锁的工作）。
- `fs->lock` 只在槽位 detach/bump 的临界区持有，**不跨** `file_free`（`file_free` 内部拿 pipe/pty 锁）。
- `files_unpin` 归零时触发 `deferred_files_free`（拿 `df_lock`）。deferred 路径已在 schedule 收割器内 `task_list_lock` 下调用过（`task.c:627` 现状），故 `files_unpin` 在 `task_list_lock` 内/外均安全；`do_exit` 选择在锁外调用仅因它持锁只为置空指针。

### 竞态结局

| 竞态 | 结局 |
|------|------|
| R3 file_t UAF | **消除**：`files_get_file` 在 `fs->lock` 内 ref-bump，与 `fd_close` 的 detach 同步；`file_free` 只在 refcount 0 触发 |
| R2 files_t 表 UAF | **消除**：`files_pin`/`unpin` 保证读取期间表不被释放 |
| R1 task_t UAF | **继承**（maps/status 已接受的 teardown 竞态）：`find_task_by_pid` 仍锁无关，task 释放窗口与 `gen_maps` 同型，待 rwlock（路线图 P1 #4）统一收紧 |

## 数据流

```
ls /proc/self/fd
  → getdents64 → vfs_getdents → procfs_readdir(FD_DIR, SELF)
      → resolve SELF → 遍历 fd[] → 输出 "0"/"1"/"2"...

cat /proc/self/fd/3
  → open: vfs_lookup 匹配 "3" → node->name="3", fs_data=FD_ENTRY+SELF
  → read: procfs_read(FD_ENTRY) → resolve SELF → parse_fd("3")=3
      → files_get_file(3) → gen_fd_target(f) → "/proc/meminfo\n" → files_put_file
```

## 错误处理 & 边界情况

| 场景 | 行为 |
|------|------|
| 目标 task 不存在 | 返回 0（空读） |
| fd 越界 / 空槽（open 后已 close） | `files_get_file` 返回 NULL → 空读 |
| `parse_fd` 失败（非数字/负/溢出） | 返回 0 |
| `vfs_resolve_path` 失败 | 印 `"?"` |
| 目录节点被 read | 返回 0（目录守卫） |
| self 哨兵 + 无 current | 返回 0 / -1 |
| 关闭/越界的 fd 在 open 阶段 | readdir 不枚举 → `vfs_lookup` 失败 → `open` 返回 `ENOENT` |
| fd 复用（close 后 re-open 同号） | 每次 `read` 重新查询槽位 → 展示复用后的新对象（非 open 时对象），文档化差异 |

## 测试

`systest.c` 新增 `test_proc_fd()`：

1. **FD_VFS 反解**：`fd = open("/proc/meminfo")` → 读 `/proc/self/fd/<fd>` 内容 == `"/proc/meminfo\n"`。
2. **FD_PIPE**：`pipe(fds)` → 读 `/proc/self/fd/<fds[0]>` == `"pipe:[?]\n"`。
3. **目录枚举**：`opendir("/proc/self/fd")` + `readdir`，能看到 `"0"`/`"1"`/`"2"`。
4. **关闭后 open 失败**：`close(fd)` → `open("/proc/self/fd/<fd>")` 返回 `< 0`（ENOENT）。
5. **越界 fd 失败**：`open("/proc/self/fd/9999")` 返回 `< 0`。
6. **非当前 PID**：读 `/proc/<其他 pid>/fd/0`（fork 子进程持有 fd，父进程观察），验证内容非空。
7. **socket 格式**（若网络可用）：socket → `socket:[?]\n`。

并发压力测试（close/readdir/read 竞态）不在 systest 自动化内——非确定性时序不适合回归套件；由 §并发安全 的引用协议保证，必要时用 `smp_stress` 类手工验证。

## 文件变更预估

| 文件 | 改动 | 说明 |
|------|------|------|
| `kernel/include/kernel/file.h` | `files_t` +`lock`+`refcount`；+`files_pin/unpin/get_file/put_file` 声明 | 结构体变更 |
| `kernel/fs/file.c` | `files_alloc/dup` 初始化 lock/refcount；`fd_alloc/fd_close` 走 `fs->lock`；实现 pin/unpin/get_file/put_file | 引用协议 |
| `kernel/sched/task.c` | `do_exit` 与收割器改走 `files_unpin` + `t->files=NULL`（task_list_lock 内） | 表生命周期 |
| `kernel/include/fs/procfs.h` | `PROCFS_TYPE_FD_DIR`/`FD_ENTRY` 常量 | +2 |
| `kernel/fs/procfs.c` | +`file.h`/`pty.h` include；`parse_fd`/`gen_fd_target`；read/readdir 分支 | ~+100 |
| `user/systest.c` | `test_proc_fd()` + 注册 | ~+60 |

**总计: ~+200 行，6 个文件。无 syscall / uapi 变更；有 `files_t` 结构体变更（需 `make clean`）。**

## 不在范围内

- `/proc/<pid>/fd/N` 的 **write 重定向**（`echo x > fd/N` 触发 dup2 语义）— 超出「可观测」
- **真 symlink** 语义（`ls -l` 显示 `N -> /path`）— 依赖 `symlink`/`readlink`（路线图 P1 #6）
- `fdinfo` — 记录 P1
- pipe/socket 稳定单调 inode ID — 记录 P1
- R1（task_t teardown 竞态）统一加固 — 依赖 rwlock（路线图 P1 #4）

## 已知语义变化（评审需知悉）

`do_exit` 的 fd 表释放从「同步 `files_free`」改为「`files_unpin` → refcount 0 → `deferred_files_free` 异步」。正常退出（无 /proc 读取器）时 refcount 1→0，释放延迟到 df-reaper kthread 的下一次调度——对 pipe 写端 EOF / socket close 的时序有轻微推迟，量级为一个 blocker 唤醒周期，可接受。这是统一 deferred 决策（见 §并发安全）的既定代价，非开放选项。
