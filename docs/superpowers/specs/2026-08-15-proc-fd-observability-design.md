# /proc/<pid>/fd/ — 进程文件描述符只读可观测

> **日期**: 2026-08-15
> **状态**: design (v3 — 修复并发 API 边界：task 锁封装、readdir 协议、dup/fcntl 覆盖)
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
- **并发安全：不引入 use-after-free**（本迭代的硬约束）
- **不向用户态泄漏内核地址**

## 关键设计决策

### 1. 文件名即 fd 号（不用 fs_data 存 fd）

`fs_data` 现有编码为 32 位（`procfs.h:10-12`）：8 位 type + 24 位 pid，装不下第三个字段 fd。

`vfs_lookup` 已在解析 `fd/3` 时把条目名 `"3"` `strdup` 进 `node->name`（`vfs.c:239`）。因此 **fd 号由文件名承载**：`readdir` 输出 `"0"/"1"/"2"` 作为条目名，`read` 时从 `node->name` 严格解析取回 fd。这与 Linux 语义一致（fd 目录里条目名就是 fd 号），零编码改动。

### 2. 魔法文件而非 symlink

`fd/N` 做成 `PROCFS_TYPE_FD_ENTRY` 合成文件，`read` 直接渲染目标路径文本。绕开 symlink 基础设施（路线图 P1 #6），把 `readlink` 语义折叠进普通 `read`。

### 3. pipe/socket 不暴露内核地址

Linux 用 `pipe:[12345]`/`socket:[12345]`。OS01 无稳定 inode 概念，若用 `pipe_t *`/`socket_t *` 地址当伪 inode，会把内核堆地址直接暴露给用户态。故用占位符 `pipe:[?]` / `socket:[?]`，只区分对象类型、不暴露身份。稳定单调 ID 计数器列为 P1 增强。

### 4. FD_ENTRY 而非 FD_LINK

`fd/N` 是普通合成文件，不是链接。节点类型命名 `PROCFS_TYPE_FD_ENTRY`，避免「LINK」误导。

## 新增 procfs 节点类型

```c
// kernel/include/fs/procfs.h
#define PROCFS_TYPE_FD_DIR    7   // /proc/<pid>/fd/  目录
#define PROCFS_TYPE_FD_ENTRY  8   // /proc/<pid>/fd/<N>  只读合成文件
```

`FD_DIR` 的 `fs_data = ENCODE(FD_DIR, pid)`；`FD_ENTRY` 同编码，**fd 号走 `node->name`**。

## 并发安全（本迭代核心）

### 问题定性

现有 fd 数据路径**没有 fd 表锁**，且 `task_list_lock`（`task.c:37`）是 **static**，procfs 无法直接访问：

- **R3（file_t UAF，阻断）**：读取器 `f = fd[fd]` 后、解引用前，另一 CPU 的 `fd_close`（`file.c:232`）无锁 detach + `refcount--` 归零即 `file_free`（释放 `file_t` 及其 `node`/`pipe`/`sock`）。
- **R2（files_t 表 UAF）**：读取器取得 `t->files` 后，`do_exit`（`task.c:848` 同步 `files_free`）或 schedule 收割器（`task.c:627` `deferred_files_free` 异步）可释放整个表。
- **R1（task_t UAF）**：`find_task_by_pid` 返回裸指针，收割器可 `list_del`+释放 task。

**关键事实（决定边界）**：`do_fork` 完全忽略 `clone_flags`（`task.c:1656` 标 unused），`files_t` per-task 独占，**无 CLONE_FILES 线程共享**。故 syscall 内部对 `current->files->fd[]` 的读是 `current` 独占的（同一 task 不会同时在两 CPU 上跑，`on_cpu` 保证），无外部 detach 者。唯一新增的外部并发者是 /proc 读取器。

### API 边界：procfs 不直接碰锁

`task_list_lock` 是 task.c 的 static，procfs 无法访问。故在 task.c 导出边界函数：

```c
// kernel/sched/task.c 实现，task.h 声明
files_t *task_files_pin_by_pid(int pid);
```

**语义**：`task_list_lock` 内 `find_task_by_pid(pid)` → 若 `t && t->files` 则 `files_pin(t->files)` 并返回之，否则返回 NULL；解锁后返回。procfs 拿到的是**已 pin 的 files_t**，之后不再解引用 task_t。

### 引用协议（file.c 实现并导出）

`files_t` 加两字段（`file.h`）：

```c
typedef struct files_struct {
    spinlock_T   lock;       // 护 fd[] 槽位
    int          refcount;   // 表生命周期（原子增减）
    file_t      *fd[NOFILE];
    char        *cwd;
} files_t;
```

| 函数 | 语义 | 调用约束（锁）与生命周期前置条件 |
|------|------|----------------------------------|
| `files_pin(fs)` | `refcount++`（`__sync_add_and_fetch`，原子） | 前置条件：调用者已持有一个 `files_t` 引用，或持保护该指针来源的 `task_list_lock`。**不得对未受保护的裸指针 pin**。可持 `task_list_lock`（仅原子操作） |
| `files_unpin(fs)` | `refcount--`（`__sync_sub_and_fetch`）；归零则 `deferred_files_free(fs)` | **禁止持 `task_list_lock`/`fs->lock`/rq lock**：归零路径在 deferred 队列 OOM 时同步 `files_free`（`deferred_free.c:36`）→ `file_free → pipe/pty/socket 清理 + wake` |
| `files_get_file(fs, fd)` | 内部自行获取 `fs->lock`，锁内取槽位、非空则 `file_get(f)`，返回稳定 `file_t*` | **禁止调用者持 `fs->lock`**（`spinlock_T` 不可重入，内部自行加锁会自旋死锁）。调用者须持有效的 `files_t` 引用（如 `task_files_pin_by_pid` 返回的） |
| `files_put_file(f)` | `file_put(f)`（refcount--，归零则 `file_free`） | **禁止持 `fs->lock`/`task_list_lock`/rq lock**（归零进 `file_free` → pipe/pty/socket 锁 + wake → `task_wake` 触碰调度状态） |
| `file_get(f)` | `file_t` refcount++（原子，唯一封装） | 前置条件：调用者已持有一个 `file_t` 引用，或持保护该槽位的 `fs->lock`。**不得对锁外取得的裸槽位指针 get**。任意锁内安全（不释放对象、不拿其他锁） |
| `file_put(f)` | `file_t` refcount--（原子，唯一封装）；归零则 `file_free(f)` | **禁止持 `fs->lock`/`task_list_lock`/rq lock**：归零进 pipe/pty/socket 清理 + wake → `task_wake` |
| `fd_dup(fs, oldfd, minfd)` | 集中 dup 语义：内部自行获取 `fs->lock`，锁内找 `>= minfd` 空槽、`file_get`、写槽位 | **调用者不得持 `fs->lock`**（内部自行加锁）；须持有效 `files_t` 引用。锁内只 `file_get`（原子），无归零动作 |
| `fd_dup2(fs, oldfd, newfd)` | 集中 dup2 语义：内部自行获取 `fs->lock`，锁内 `file_get(old)`、detach 旧目标、写槽位；**锁外** `files_put_file(旧目标)` | **调用者不得持 `fs->lock`**（内部自行加锁）；须持有效 `files_t` 引用。detach 的旧目标移出临界区后再 `file_put` |

> **前置条件总则**：原子自增（`files_pin`/`file_get`）只能保护**已经存活**的对象，不能让悬空指针复活。调用者必须在「已持有引用」或「持有保护指针来源的锁」二选一的前提下调用。这正是 `task_files_pin_by_pid`（task_list_lock 内取 `t->files` 并 pin）和 `files_get_file`（fs->lock 内取槽位并 file_get）存在的原因——它们把「取得稳定引用」的临界区封装在 API 内，调用者无需也不应自行加锁。

**决策：`files_unpin` 归零时统一走 `deferred_files_free`（deferred）。** 理由：schedule 收割器在 schedule() 内，同步 `files_free` 会触发 `file_free → pipe wake → task_wake` 重入调度器；deferred 路径规避之。代价是 `do_exit` 的 fd 关闭时序轻微推迟（见 §已知语义变化）。**但 deferred 的 OOM 回退会同步 `files_free`（`deferred_free.c:36`），故 `files_unpin` 的锁约束必须按上表执行，不能宣称锁内安全。**

> 若未来需要锁内版本，可拆 `files_get_file_locked()`（要求调用者已持 `fs->lock`）+ `files_get_file()`（对外 API，自行加锁）。本方案不需要前者，故不引入。

### 槽位访问规则

**所有槽位写入者**（`fd_alloc`、`fd_close`、`fd_dup`、`fd_dup2`）持 `fs->lock`；**所有外部读取者**（`files_get_file`、procfs readdir）持 `fs->lock`。唯一免锁的是 `files_free`（refcount 0 时 teardown，无并发读取者）。

**file_t refcount 统一封装**：`files_dup`、`dup`、`close`、`do_pipe` 等现有路径直接 `__sync_add/sub_and_fetch(&f->refcount, 1)` 的，全部改为 `file_get(f)`/`file_put(f)`，杜绝绕过协议。

**files_dup 持源锁**：`/proc` 引入外部并发读取者后，「源表仅 current 使用」的假设不再成立（另一 CPU 可能正在 `files_get_file` 读同一 `current->files`）。`files_dup` 复制 `fd[]` 时，**槽位指针取出后必须在同一 `src->lock` 临界区内完成 `file_get`**（`file_get` 仅原子计数，不释放对象、不拿其他锁，锁内安全），再写入 `dst->fd[fd]`：

```c
lock(src->lock);
for (fd = 0; fd < NOFILE; fd++) {
    file_t *f = src->fd[fd];
    if (f) {
        file_get(f);      // 必须仍在 src->lock 内 → 稳定引用
        dst->fd[fd] = f;
    }
}
unlock(src->lock);
```

> 更正：v3 原句「不跨 `file_get` 的 ref-bump」措辞矛盾——它暗示「先读指针、解锁、再 ref-bump」，违反「槽位指针从锁中取出后必须在同一临界区内 ref-bump 才成为稳定引用」的核心规则。正确语义是：**ref-bump 在 `src->lock` 内完成**；锁内不能做的是可能归零的 `file_put`，而非 `file_get`。

### 锁顺序

```
（锁内仅原子操作）
  task_list_lock  →  find_task + files_pin(t->files)   [仅 files_pin 原子]
  fs->lock        →  槽位 detach / file_get(原子) / 槽位扫描

（释放上述锁之后才可）
  files_unpin(fs)  →  deferred_files_free → df_lock；或 OOM 同步 files_free
  file_put(f)      →  file_free → pipe/pty/socket locks + wake
```

- `task_list_lock`：只在 `task_files_pin_by_pid` 内 find + pin；`do_exit`/收割器仅 `fs = t->files; t->files = NULL`（收集指针），**不跨** `files_get_file`/`vfs_resolve_path`/`files_put_file`/`files_unpin`。
- `do_exit`/收割器：**锁内只收集要 unpin 的 `files_t*`，释放 `task_list_lock` 后再逐个 `files_unpin()`**。
- `fd_close`/`fd_dup2`：detach 出的旧 `file_t*` 移出 `fs->lock` 临界区后，再 `file_put()`。
- `fs->lock`：只在槽位 detach/bump/scan 的临界区，**不跨** `file_put`/`file_free`（其内部拿 pipe/pty 锁）。
- `files_unpin` 归零触发 `deferred_files_free`（拿 `df_lock`）；df_reaper 执行回调前已释放 `df_lock`（`deferred_free.c:76`）。**`files_unpin` 必须在此链路的锁之外调用**（见上表），因其 OOM 回退会同步 `files_free`。

### 竞态结局

| 竞态 | 结局 |
|------|------|
| R3 file_t UAF | **消除**：`files_get_file` 在 `fs->lock` 内 `file_get`，与 `fd_close` detach 同步；`file_free` 只在 refcount 0 |
| R2 files_t 表 UAF | **消除**：`files_pin`/`unpin` 保证读取期表存活 |
| R1 task_t UAF | **消除**：`task_files_pin_by_pid` 在 `task_list_lock` 内 pin，解锁后不再碰 task_t；task_t 释放与否不影响已 pin 的 files_t |

> 与 v2 的差异：v2 将 R1 描述为「继承 maps/status 的既有风险」，但 `task_files_pin_by_pid` 使 fd 路径在解锁后完全脱离 task_t，故 R1 实际消除，而非继承。

## 组件

### `gen_fd_target(file_t *f, char *buf, int bufsz)`

唯一新增的核心渲染函数。入参是 `files_get_file` 返回的**已持有稳定引用**的 `file_t*`：

| `file_t.type` | 输出 | 说明 |
|---------------|------|------|
| `FD_VFS` / `FD_DEV` | `<绝对路径>\n` | `vfs_resolve_path(f->node)`；失败 `"?\n"` |
| `FD_PIPE` | `pipe:[?]\n` | 不暴露地址 |
| `FD_PTY_MASTER` | `/dev/ptmx\n` | |
| `FD_PTY_SLAVE` | `/dev/pts<index>\n` | `f->pty->index` |
| `FD_SOCKET` | `socket:[?]\n` | 不暴露地址 |
| 其他 | 返回 0 | |

### `parse_fd(const char *s)`

严格十进制解析（**不用 `atoi`**），拒绝负号、前导垃圾、尾随字符、溢出：

```c
static int parse_fd(const char *s)
{
    if (!s || *s < '0' || *s > '9') return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v >= NOFILE) return -1;
    }
    return v;
}
```

### `procfs_readdir` 扩展（完整 pin/lock 流程）

1. `SELF_DIR`/`PID_DIR` 各加 index=2 条目 `"fd"`（`VFS_DIR`，`ino = ENCODE(FD_DIR, pid/SELF)`）。
2. 新增 `PROCFS_TYPE_FD_DIR` case，**按引用协议完整走**：

```c
case PROCFS_TYPE_FD_DIR: {
    uint32_t p = pid;
    if (p == PROCFS_PID_SELF) { if (!current) return -1; p = (uint32_t)current->pid; }

    files_t *fs = task_files_pin_by_pid((int)p);   // task_list_lock 内 find + pin
    if (!fs) return -1;

    uint64_t k = index;
    int found = -1;
    {
        uint64_t fl = spin_lock_irqsave(&fs->lock); // 单次扫描内持锁
        for (int fd = 0; fd < NOFILE; fd++) {
            if (!fs->fd[fd]) continue;
            if (k == 0) { found = fd; break; }
            k--;
        }
        spin_unlock_irqrestore(&fs->lock, fl);
    }

    if (found >= 0) {
        snprintf(entry->name, VFS_NAME_MAX, "%d", found);
        entry->type = VFS_FILE;
        entry->size = 4096;
        entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(PROCFS_TYPE_FD_ENTRY, pid);
    } else {
        entry->name[0] = '\0';
    }

    files_unpin(fs);                                 // 锁外 unpin
    return 0;
}
```

> 注意：**不在多次 `readdir(index)` 调用之间保留锁**。动态目录允许条目变化/重复（fd 在两次调用间 close/reuse 会导致条目漂移），但单次扫描在 `fs->lock` 内，安全。

### `procfs_read` 扩展

1. 目录守卫加 `PROCFS_TYPE_FD_DIR`。
2. 新增 `PROCFS_TYPE_FD_ENTRY` case：

```c
case PROCFS_TYPE_FD_ENTRY: {
    uint32_t p = pid;
    if (p == PROCFS_PID_SELF) { if (!current) return 0; p = (uint32_t)current->pid; }
    int fd = parse_fd(node->name);
    if (fd < 0) return 0;

    files_t *fs = task_files_pin_by_pid((int)p);
    if (!fs) return 0;
    file_t *f = files_get_file(fs, fd);   // fs->lock 内 file_get
    files_unpin(fs);                      // 表引用不再需要，尽早释放
    if (!f) return 0;

    len = gen_fd_target(f, local, sizeof(local));
    files_put_file(f);                    // file_put
    break;
}
```

### `#include` 集合

```c
#include <kernel/file.h>   // file_t / files_t 完整定义
#include <kernel/pty.h>    // pty_struct 完整定义（file.h 只前置声明）
```

## 数据流

```
ls /proc/self/fd
  → getdents64 → vfs_getdents → procfs_readdir(FD_DIR, SELF)
      → task_files_pin_by_pid → fs->lock 内扫描 → 输出 "0"/"1"/"2" → files_unpin

cat /proc/self/fd/3
  → open: vfs_lookup 匹配 "3" → node->name="3", fs_data=FD_ENTRY+SELF
  → read: procfs_read(FD_ENTRY) → parse_fd("3")=3
      → task_files_pin_by_pid → files_get_file(3) → gen_fd_target(f)
      → "/proc/meminfo\n" → files_put_file
```

## 错误处理 & 边界情况

| 场景 | 行为 |
|------|------|
| 目标 task 不存在 | `task_files_pin_by_pid` 返回 NULL → 空读 / -1 |
| fd 越界 / 空槽（open 后已 close） | `files_get_file` 返回 NULL → 空读 |
| `parse_fd` 失败（非数字/负/溢出） | 返回 0 |
| `vfs_resolve_path` 失败 | 印 `"?"` |
| 目录节点被 read | 返回 0（目录守卫） |
| self 哨兵 + 无 current | 返回 0 / -1 |
| 关闭/越界的 fd 在 open 阶段 | readdir 不枚举 → `vfs_lookup` 失败 → `open` 返回 ENOENT |
| fd 复用（close 后 re-open 同号） | 每次 read 重新查询槽位 → 展示复用后新对象，文档化差异 |

## 测试

### systest（`test_proc_fd()`）

1. **FD_VFS 反解**：`fd = open("/proc/meminfo")` → 读 `/proc/self/fd/<fd>` == `"/proc/meminfo\n"`。
2. **FD_PIPE**：`pipe(fds)` → 读 `/proc/self/fd/<fds[0]>` == `"pipe:[?]\n"`。
3. **目录枚举**：`opendir("/proc/self/fd")` + `readdir`，能看到 `"0"`/`"1"`/`"2"`。
4. **关闭后 open 失败**：`close(fd)` → `open("/proc/self/fd/<fd>")` 返回 `< 0`。
5. **越界 fd 失败**：`open("/proc/self/fd/9999")` 返回 `< 0`。
6. **非当前 PID**：fork 子进程持有 fd，父进程读 `/proc/<child>/fd/0` 非空。
7. **socket 格式**（若网络可用）：`socket:[?]\n`。

### SMP 引用协议验证（本迭代必做）

并发引用协议是本修改的核心，仅靠功能断言不够。加**内核 selftest**（`KERNEL_SELFTEST=1` 下运行，`kernel/test/`）：

- 单 reader 线程循环 `files_get_file`/`files_put_file`，另单 writer 循环 `fd_close`+`fd_alloc` 同一槽位，迭代 N 次无 UAF（依赖 slab poison / `file_free` 的 `f->pipe=NULL` 毒化 + 崩溃即失败）。
- `files_pin`/`unpin` 与 `do_exit` 竞态：reader 持 pin 时退出路径不释放表（refcount 不归零）。

> 非确定性时序不适合放 systest 回归；用内核 selftest（可重复、确定性迭代）覆盖。若实现阶段发现 selftest 框架不便注入，退化为 `user/smp_fd_stress.c` 专用压力测试（`-smp 2` 手工跑）。

## 文件变更预估

| 文件 | 改动 | 说明 |
|------|------|------|
| `kernel/include/kernel/file.h` | `files_t` +`lock`+`refcount`；+`files_pin/unpin/get_file/put_file`、`file_get/put`、`fd_dup/dup2` 声明 | 结构体变更 |
| `kernel/fs/file.c` | `files_alloc/dup` 初始化 lock/refcount；`fd_alloc/fd_close` 走锁；实现全部新 API；`do_pipe` 等改 `file_get/put` | 引用协议 |
| `kernel/include/kernel/task.h` | +`task_files_pin_by_pid` 声明 | |
| `kernel/sched/task.c` | `task_files_pin_by_pid` 实现；`do_exit`/收割器改为 `task_list_lock` 内 `fs = t->files; t->files = NULL`，**锁外** `files_unpin(fs)` | 表生命周期 |
| `kernel/arch/x86_64/trap.c` | `SYS_dup`/`SYS_dup2`/`SYS_fcntl(F_DUPFD/F_DUPFD_CLOEXEC)` 改调 `fd_dup`/`fd_dup2` | 槽位写入者收敛 |
| `kernel/include/fs/procfs.h` | `PROCFS_TYPE_FD_DIR`/`FD_ENTRY` 常量 | +2 |
| `kernel/fs/procfs.c` | +`file.h`/`pty.h` include；`parse_fd`/`gen_fd_target`；read/readdir 分支 | ~+110 |
| `kernel/test/`（selftest） | fd 引用协议并发 selftest | ~+60 |
| `user/systest.c` | `test_proc_fd()` + 注册 | ~+60 |

**总计: ~+300 行，9 个文件。无 syscall / uapi 变更；有 `files_t` 结构体变更（需 `make clean`）。**

## 不在范围内

- `/proc/<pid>/fd/N` 的 **write 重定向**（dup2 语义）— 超出「可观测」
- **真 symlink** 语义（`ls -l` 显示 `N -> /path`）— 依赖 `symlink`/`readlink`（路线图 P1 #6）
- `fdinfo` — 记录 P1
- pipe/socket 稳定单调 inode ID — 记录 P1
- syscall 读取者（read/write/lseek/fstat/ioctl）的锁加固 — 无 CLONE_FILES，current 独占，无此竞态；未来引入线程时一并加固（记录）
- R1 task_t teardown 的 rwlock 统一加固 — 本 fd 路径已消除，其余路径待路线图 P1 #4

## 已知语义变化（评审需知悉）

`do_exit` 的 fd 表释放从「同步 `files_free`」改为「`files_unpin` → refcount 0 → `deferred_files_free` 异步」。正常退出（无 /proc 读取器）时 refcount 1→0，释放延迟到 df-reaper kthread 下一次调度——pipe 写端 EOF / socket close 时序轻微推迟，量级为一个 blocker 唤醒周期，可接受。这是统一 deferred 决策的既定代价。
