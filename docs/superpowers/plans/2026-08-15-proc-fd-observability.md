# /proc/<pid>/fd/ 可观测性 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补齐 `/proc/<pid>/fd/` 只读可观测，让 shell 能枚举 fd 号并查看每个 fd 指向的目标，同时建立并发安全的文件描述符引用协议。

**Architecture:** 给 `files_t` 增加表级 spinlock + refcount，新增 `files_pin/unpin/get_file/put_file` 与 `file_get/put` 引用协议；把 fd 槽位写入者（`fd_alloc`/`fd_close`/`fd_dup`/`fd_dup2`）收敛到 `file.c` 并统一持锁；`task.c` 导出 `task_files_pin_by_pid` 供 procfs 在 task_list_lock 内 pin 表；procfs 新增 `FD_DIR`/`FD_ENTRY` 两类节点，文件名承载 fd 号，`read` 时渲染目标路径。

**Tech Stack:** C（clang，`-target x86_64-unknown-none`）、自研 VFS/procfs、`__sync` 原子原语、`spinlock_T`、内核 selftest + 用户态 systest。

## Global Constraints

- **执行前先切 worktree**（用户要求「git commit 前，切出 worktree 来修改代码」）。spec/plan 文档在 master 提交；**代码实现一律在 worktree 内**。
- **`make clean` 强制**：`files_t` 结构体变更后必须 `make clean`（无头文件依赖追踪，陈旧 `.o` = 静默 `sizeof` 错配）。
- **锁纪律（来自 spec，任何任务不得违反）**：
  - `files_unpin`、`file_put`、`files_put_file` 归零路径可能同步 `files_free`（deferred OOM 回退，`deferred_free.c:36`）→ **禁止持 `task_list_lock`/`fs->lock`/rq lock** 调用。
  - `files_get_file`、`fd_dup`、`fd_dup2` 内部自行加 `fs->lock` → **调用者不得持 `fs->lock`**（`spinlock_T` 不可重入）。
  - `file_get` 任意锁内安全（纯原子）；但必须已持引用或持保护来源的锁。
- **不向用户态泄漏内核地址**：pipe/socket 用 `pipe:[?]`/`socket:[?]` 占位，禁止输出指针。
- **不改 uapi/syscall 号**。新增的只有内核内部 API 与 procfs 节点类型。
- 代码风格匹配现有：`__sync_*` 原子、`spin_lock_irqsave`/`spin_unlock_irqrestore`、`log_err`/`debug_fs`。

---

### Task 1: files_t 引用协议核心

**Files:**
- Modify: `kernel/include/kernel/file.h:95-98`（结构体 + 新 API 声明）
- Modify: `kernel/fs/file.c`（`files_alloc`/`files_dup`/`files_free`/`fd_alloc`/`fd_close` + 新 API 实现 + `#include <kernel/deferred_free.h>`）

**Interfaces:**
- Produces（后续任务依赖，签名必须精确）：
  ```c
  void    files_pin(files_t *fs);
  void    files_unpin(files_t *fs);          // 禁止持 task/fs/rq 锁
  file_t *files_get_file(files_t *fs, int fd); // 内部加 fs->lock
  void    files_put_file(file_t *f);          // 禁止持 fs->lock
  void    file_get(file_t *f);                // 任意锁内安全
  void    file_put(file_t *f);                // 禁止持 fs->lock
  int     fd_dup(files_t *fs, int oldfd, int minfd);   // 内部加 fs->lock
  int     fd_dup2(files_t *fs, int oldfd, int newfd);  // 内部加 fs->lock
  ```

- [ ] **Step 1: 修改 `files_t` 结构体**

`kernel/include/kernel/file.h` 中 `files_t`（第 95-98 行）替换为：

```c
typedef struct files_struct {
    spinlock_T   lock;       // 护 fd[] 槽位
    int          refcount;   // 表生命周期（__sync 原子增减）
    file_t      *fd[NOFILE];
    char        *cwd;        // heap-allocated current working directory
} files_t;
```

- [ ] **Step 2: 在 `file.h` 的 API 段（第 102-118 行 `files_dup` 声明之后）追加引用协议声明**

```c
// ── Reference protocol (concurrency-safe fd table access) ──
// Table lifecycle: pin/unpin.  file lifecycle: get/put.
// files_unpin / file_put / files_put_file MUST NOT be called while
// holding task_list_lock, fs->lock, or an rq lock — their drop-to-zero
// path may synchronously files_free/file_free (deferred OOM fallback).
void    files_pin(files_t *fs);
void    files_unpin(files_t *fs);
file_t *files_get_file(files_t *fs, int fd);   // locks fs->lock internally
void    files_put_file(file_t *f);
void    file_get(file_t *f);                   // safe under any lock
void    file_put(file_t *f);
int     fd_dup(files_t *fs, int oldfd, int minfd);   // locks fs->lock internally
int     fd_dup2(files_t *fs, int oldfd, int newfd);  // locks fs->lock internally
```

- [ ] **Step 3: `file.c` 加 include**

`kernel/fs/file.c` 顶部 include 区（第 1-14 行）追加一行：

```c
#include <kernel/deferred_free.h>
```

- [ ] **Step 4: 实现 `file_get`/`file_put`（放在 `file_free` 定义之后）**

```c
// ── file_t reference counting ──────────────────────────────
// file_get is safe under any lock (pure atomic, never frees).
// file_put MUST NOT be called under fs->lock: a drop-to-zero enters
// file_free → pipe/pty/socket cleanup + wake.
void file_get(file_t *f)
{
    __sync_add_and_fetch(&f->refcount, 1);
}

void file_put(file_t *f)
{
    if (__sync_sub_and_fetch(&f->refcount, 1) == 0)
        file_free(f);
}
```

- [ ] **Step 5: 重写 `files_alloc`（第 169-178 行）**

```c
files_t *files_alloc(void)
{
    files_t *fs = (files_t *)calloc(1, sizeof(files_t));
    if (!fs) return NULL;
    spin_init(&fs->lock);
    fs->refcount = 1;            // caller holds the initial reference
    fs->cwd = strdup("/");
    if (!fs->cwd) { free(fs); return NULL; }
    return fs;
}
```

- [ ] **Step 6: 重写 `files_free`（第 181-194 行）— 用 `file_put` 替代裸 refcount**

```c
void files_free(files_t *fs)
{
    if (!fs) return;
    // refcount==0 here: no concurrent readers, no lock needed.
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i]) {
            file_put(fs->fd[i]);
            fs->fd[i] = NULL;
        }
    }
    kfree(fs->cwd);
    free(fs);
}
```

- [ ] **Step 7: 重写 `files_dup`（第 196-216 行）— 持源锁 + `file_get` 锁内完成**

```c
files_t *files_dup(files_t *fs)
{
    if (!fs) return NULL;

    files_t *new_fs = (files_t *)calloc(1, sizeof(files_t));
    if (!new_fs) return NULL;
    spin_init(&new_fs->lock);
    new_fs->refcount = 1;

    new_fs->cwd = strdup(fs->cwd);
    if (!new_fs->cwd) { free(new_fs); return NULL; }

    // /proc readers may be concurrently files_get_file()'ing the source
    // table, so hold src->lock; file_get() is atomic and safe in-lock.
    uint64_t fl = spin_lock_irqsave(&fs->lock);
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i]) {
            file_get(fs->fd[i]);
            new_fs->fd[i] = fs->fd[i];
        }
    }
    spin_unlock_irqrestore(&fs->lock, fl);
    return new_fs;
}
```

- [ ] **Step 8: 重写 `fd_alloc`（第 218-229 行）— 持锁**

```c
int fd_alloc(files_t *fs, file_t *f)
{
    if (!fs || !f) return -1;
    uint64_t fl = spin_lock_irqsave(&fs->lock);
    int ret = -1;
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i] == NULL) {
            fs->fd[i] = f;
            ret = i;
            break;
        }
    }
    spin_unlock_irqrestore(&fs->lock, fl);
    return ret;
}
```

- [ ] **Step 9: 重写 `fd_close`（第 232-243 行）— 锁内 detach、锁外 `file_put`**

```c
void fd_close(files_t *fs, int fd)
{
    if (!fs || fd < 0 || fd >= NOFILE) return;

    file_t *f;
    uint64_t fl = spin_lock_irqsave(&fs->lock);
    f = fs->fd[fd];
    fs->fd[fd] = NULL;
    spin_unlock_irqrestore(&fs->lock, fl);

    if (f) file_put(f);   // outside lock: drop-to-zero → file_free
}
```

- [ ] **Step 10: 实现 `files_pin`/`files_unpin`/`files_get_file`/`files_put_file`（放在 `fd_close` 之后）**

```c
// ── files_t reference protocol ─────────────────────────────
void files_pin(files_t *fs)
{
    __sync_add_and_fetch(&fs->refcount, 1);
}

// MUST NOT be called under task_list_lock/fs->lock/rq lock: drop-to-zero
// → deferred_files_free, whose OOM fallback synchronously files_free()s.
void files_unpin(files_t *fs)
{
    if (__sync_sub_and_fetch(&fs->refcount, 1) == 0)
        deferred_files_free(fs);
}

// Locks fs->lock internally.  Caller must NOT already hold fs->lock
// (spinlock is non-reentrant).  Caller must hold a live files_t ref.
file_t *files_get_file(files_t *fs, int fd)
{
    if (!fs || fd < 0 || fd >= NOFILE) return NULL;
    uint64_t fl = spin_lock_irqsave(&fs->lock);
    file_t *f = fs->fd[fd];
    if (f) file_get(f);   // ref-bump inside the critical section
    spin_unlock_irqrestore(&fs->lock, fl);
    return f;
}

void files_put_file(file_t *f)
{
    file_put(f);
}
```

- [ ] **Step 11: 实现 `fd_dup`/`fd_dup2`（放在 `files_put_file` 之后）**

```c
// ── Centralized dup (slot writers must hold fs->lock) ──────
int fd_dup(files_t *fs, int oldfd, int minfd)
{
    if (!fs || oldfd < 0 || oldfd >= NOFILE || minfd < 0)
        return -EBADF;

    uint64_t fl = spin_lock_irqsave(&fs->lock);
    file_t *f = fs->fd[oldfd];
    if (!f) { spin_unlock_irqrestore(&fs->lock, fl); return -EBADF; }

    int newfd = -1;
    for (int i = minfd; i < NOFILE; i++) {
        if (fs->fd[i] == NULL) { newfd = i; break; }
    }
    if (newfd < 0) { spin_unlock_irqrestore(&fs->lock, fl); return -ENFILE; }

    file_get(f);
    fs->fd[newfd] = f;
    spin_unlock_irqrestore(&fs->lock, fl);
    return newfd;
}

int fd_dup2(files_t *fs, int oldfd, int newfd)
{
    if (!fs || oldfd < 0 || oldfd >= NOFILE || newfd < 0 || newfd >= NOFILE)
        return -EBADF;

    if (oldfd == newfd) {
        uint64_t fl = spin_lock_irqsave(&fs->lock);
        int ok = fs->fd[oldfd] != NULL;
        spin_unlock_irqrestore(&fs->lock, fl);
        return ok ? newfd : -EBADF;
    }

    file_t *old_target = NULL;
    uint64_t fl = spin_lock_irqsave(&fs->lock);
    file_t *f = fs->fd[oldfd];
    if (!f) { spin_unlock_irqrestore(&fs->lock, fl); return -EBADF; }
    file_get(f);
    old_target = fs->fd[newfd];
    fs->fd[newfd] = f;
    spin_unlock_irqrestore(&fs->lock, fl);

    if (old_target) files_put_file(old_target);   // outside lock
    return newfd;
}
```

- [ ] **Step 12: 编译验证**

```bash
cd /home/aagu/OS01   # 或 worktree 根
make clean
make kernel.bin
```

Expected: 无编译错误。若报 `deferred_files_free`/`spin_init` 未声明，检查 Step 3 的 include 与 `file.h` 已含 `kernel/arch/spinlock.h`（它在 `file.h` 第 5 行已 include）。

- [ ] **Step 13: Commit**

```bash
git add kernel/include/kernel/file.h kernel/fs/file.c
git commit -m "feat(fd): add files_t reference protocol

Add per-table spinlock + refcount and files_pin/unpin/get_file/put_file,
file_get/put, fd_dup/fd_dup2.  fd_alloc/fd_close/files_dup/files_free
now hold fs->lock for slot access and drop refs outside the lock."
```

---

### Task 2: dup/dup2/fcntl 收敛到 fd_dup/fd_dup2

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`（`SYS_dup` 约 1347-1374、`SYS_dup2` 约 1376-1400、`SYS_fcntl` 的 `F_DUPFD`/`F_DUPFD_CLOEXEC` 约 1619-1651）

**Interfaces:**
- Consumes: `int fd_dup(files_t *fs, int oldfd, int minfd)`, `int fd_dup2(files_t *fs, int oldfd, int newfd)`（Task 1，返回值 `newfd` 或 `-errno`）。

- [ ] **Step 1: 重写 `SYS_dup` 分支（第 1347-1374 行）**

```c
    case SYS_dup: {
        // dup(int oldfd) → newfd / -errno
        int oldfd = (int)regs->rdi;
        regs->rax = fd_dup(current->files, oldfd, 0);
        break;
    }
```

- [ ] **Step 2: 重写 `SYS_dup2` 分支（第 1376-1400 行）**

```c
    case SYS_dup2: {
        // dup2(int oldfd, int newfd) → newfd / -errno
        int oldfd = (int)regs->rdi;
        int newfd = (int)regs->rsi;
        regs->rax = fd_dup2(current->files, oldfd, newfd);
        break;
    }
```

- [ ] **Step 3: 重写 `SYS_fcntl` 的 `F_DUPFD`（第 1619-1635 行）**

```c
        case F_DUPFD: {
            // dup to >= arg
            int start = (int)arg;
            if (start < 0) start = 0;
            regs->rax = fd_dup(current->files, fd, start);
            break;
        }
```

- [ ] **Step 4: 重写 `SYS_fcntl` 的 `F_DUPFD_CLOEXEC`（第 1636-1652 行）**

```c
        case F_DUPFD_CLOEXEC: {
            // Same as F_DUPFD for now (close-on-exec not implemented)
            int start = (int)arg;
            if (start < 0) start = 0;
            regs->rax = fd_dup(current->files, fd, start);
            break;
        }
```

- [ ] **Step 5: 编译 + 回归验证**

```bash
make clean
make kernel.bin
make OS01_SYSTEST=1 test-syscall
```

Expected: 内核编译通过；systest 仍 **126/126 passed**（`test_dup_dup2`、`test_fcntl` 继续通过，证明收敛无回归）。

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "refactor(fd): route dup/dup2/fcntl through fd_dup/fd_dup2

Remove direct refcount++/slot-write in SYS_dup/SYS_dup2/SYS_fcntl
(F_DUPFD/F_DUPFD_CLOEXEC); they now call the centralized slot-locked
fd_dup/fd_dup2 in file.c."
```

---

### Task 3: task_files_pin_by_pid + 退出路径收敛

**Files:**
- Modify: `kernel/include/kernel/task.h`（声明）
- Modify: `kernel/sched/task.c`（`task_files_pin_by_pid` 实现；`do_exit` 约 848；schedule 收割器约 627）

**Interfaces:**
- Consumes: `files_pin`/`files_unpin`（Task 1）。
- Produces: `files_t *task_files_pin_by_pid(int pid);`（Task 4 依赖）。

- [ ] **Step 1: `task.h` 加声明（放在 `task_send_signal` 声明附近，约第 343 行）**

```c
// ── SMP-safe fd-table pinning ─────────────────────────
// Finds task by pid under task_list_lock, pins its files_t, returns
// it (or NULL).  Caller owns a reference and must files_unpin() it.
// Caller does NOT touch task_t after return.
files_t *task_files_pin_by_pid(int pid);
```

- [ ] **Step 2: `task.c` 实现 `task_files_pin_by_pid`（放在 `task_send_signal` 之后，约第 1854 行前）**

```c
// ── task_files_pin_by_pid ─────────────────────────────────
// Locate a task by pid under task_list_lock and pin its fd table so a
// /proc reader can inspect it without racing do_exit / the reaper.
files_t *task_files_pin_by_pid(int pid)
{
    files_t *fs = NULL;
    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid) {
            if (t->files) {
                fs = t->files;
                files_pin(fs);   // atomic, safe under task_list_lock
            }
            break;
        }
    }
    spin_unlock_irqrestore(&task_list_lock, tl_flags);
    return fs;
}
```

- [ ] **Step 3: `do_exit` 收敛（第 847-851 行）**

原代码：
```c
    // Close all file descriptors
    if (current->files) {
        files_free(current->files);
        current->files = NULL;
    }
```
替换为：
```c
    // Drop our fd-table reference (deferred free on refcount==0).
    // No lock held here, so files_unpin is safe.
    if (current->files) {
        files_t *fs = current->files;
        current->files = NULL;
        files_unpin(fs);
    }
```

- [ ] **Step 4: schedule 收割器收敛（第 626-627 行）**

原代码（在 `task_list_lock` 内）：
```c
            if (t->thread) {deferred_kfree(t->thread); t->thread = NULL;}
            if (t->files) {deferred_files_free(t->files); t->files = NULL;}
```
替换为（**锁内只收集，锁外 unpin**）：
```c
            if (t->thread) {deferred_kfree(t->thread); t->thread = NULL;}
            if (t->files) { files_to_free[files_free_count++] = t->files; t->files = NULL; }
```

并在 `task_list_lock` 临界区（第 553 行 `{` 之后）声明收集数组，在 `spin_unlock_irqrestore(&task_list_lock, reap_flags);`（第 637 行）之后补 unpin 循环。

第 552-553 行 `// ── 3. Zombie reaper` 块开头，`task_t *reap_list[64];` 之前加：

```c
        files_t *files_to_free[64];
        int      files_free_count = 0;
```

第 637 行 `spin_unlock_irqrestore(&task_list_lock, reap_flags);` 之后（第 638 行 `}` 之前，即仍在 `{ ... }` 作用域内）加：

```c
        for (int i = 0; i < files_free_count; i++)
            files_unpin(files_to_free[i]);   // outside task_list_lock
```

> 注意：`files_to_free`/`files_free_count` 的作用域必须与 `reap_list` 一致（同在第 552 行开始的 `{ ... }` 块内），否则锁外 unpin 循环看不到它们。

- [ ] **Step 5: 编译 + 回归验证**

```bash
make clean
make kernel.bin
make OS01_SYSTEST=1 test-syscall
```

Expected: 编译通过；systest 126/126。重点确认 `fork+exec+waitpid`、`deferred_free` 相关测试通过（退出路径改动无回归）。

- [ ] **Step 6: Commit**

```bash
git add kernel/include/kernel/task.h kernel/sched/task.c
git commit -m "feat(fd): export task_files_pin_by_pid, pin tables in exit paths

do_exit and the schedule reaper now drop their files_t reference via
files_unpin (collecting under task_list_lock, unpinning after unlock),
and task_files_pin_by_pid lets /proc pin a table by pid."
```

---

### Task 4: procfs /proc/<pid>/fd/ 节点

**Files:**
- Modify: `kernel/include/fs/procfs.h`（常量）
- Modify: `kernel/fs/procfs.c`（include、`parse_fd`、`gen_fd_target`、readdir/read 分支）

**Interfaces:**
- Consumes: `files_t *task_files_pin_by_pid(int pid)`, `file_t *files_get_file(files_t*,int)`, `void files_put_file(file_t*)`, `void files_unpin(files_t*)`（Task 1、3）。
- Produces: `PROCFS_TYPE_FD_DIR`（=7）、`PROCFS_TYPE_FD_ENTRY`（=8）。

- [ ] **Step 1: `procfs.h` 加常量（第 19 行 `PROCFS_TYPE_MAPS` 之后）**

```c
#define PROCFS_TYPE_MAPS      5   // /proc/<pid>/maps
#define PROCFS_TYPE_FD_DIR    7   // /proc/<pid>/fd/   (directory)
#define PROCFS_TYPE_FD_ENTRY  8   // /proc/<pid>/fd/<N> (synthetic file)
```

- [ ] **Step 2: `procfs.c` 加 include（第 1-12 行 include 区）**

```c
#include <kernel/file.h>   // file_t / files_t full definition
#include <kernel/pty.h>    // pty_struct full definition (file.h only fwd-declares)
```

- [ ] **Step 3: 加 `parse_fd`（放在 `gen_status` 之前，约第 47 行）**

```c
// ── Strict decimal fd parse ────────────────────────────────
// Rejects negative, leading garbage, trailing chars, and overflow.
// NOT atoi(): we own the name (from readdir) but parse defensively.
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

- [ ] **Step 4: 加 `gen_fd_target`（放在 `parse_fd` 之后）**

```c
// ── Render an fd's target as a text line ──────────────────
// f must be a stable reference (from files_get_file).
// Returns bytes written (excluding NUL).
static int gen_fd_target(file_t *f, char *buf, int bufsz)
{
    if (!f || !buf || bufsz <= 0) return 0;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        if (!f->node) return snprintf(buf, bufsz, "?\n");
        char path_buf[280];
        int n = vfs_resolve_path(f->node, path_buf, sizeof(path_buf) - 4);
        return snprintf(buf, bufsz, "%s\n", n < 0 ? "?" : path_buf);
    }
    case FD_PIPE:
        return snprintf(buf, bufsz, "pipe:[?]\n");
    case FD_PTY_MASTER:
        return snprintf(buf, bufsz, "/dev/ptmx\n");
    case FD_PTY_SLAVE: {
        int idx = f->pty ? f->pty->index : -1;
        return snprintf(buf, bufsz, "/dev/pts%d\n", idx);
    }
    case FD_SOCKET:
        return snprintf(buf, bufsz, "socket:[?]\n");
    default:
        return 0;
    }
}
```

- [ ] **Step 5: `procfs_read` 目录守卫加 `FD_DIR`（第 290-292 行）**

```c
    if (type == PROCFS_TYPE_PID_DIR || type == PROCFS_TYPE_ROOT ||
        type == PROCFS_TYPE_SELF_DIR || type == PROCFS_TYPE_FD_DIR)
        return 0;
```

- [ ] **Step 6: `procfs_read` 加 `FD_ENTRY` case（第 313 行 `default:` 之前）**

```c
    case PROCFS_TYPE_FD_ENTRY: {
        uint32_t p = pid;
        if (p == PROCFS_PID_SELF) { if (!current) return 0; p = (uint32_t)current->pid; }
        int fd = parse_fd(node->name);
        if (fd < 0) return 0;

        files_t *fs = task_files_pin_by_pid((int)p);
        if (!fs) return 0;
        file_t *f = files_get_file(fs, fd);
        files_unpin(fs);                 // drop table ref early
        if (!f) return 0;

        len = gen_fd_target(f, local, sizeof(local));
        files_put_file(f);
        break;
    }
```

- [ ] **Step 7: `procfs_readdir` — `SELF_DIR` case 加 `fd` 条目（第 417-419 行 `default:` 之前）**

```c
        case 2:
            strcpy(entry->name, "fd");
            entry->type = VFS_DIR;
            entry->size = 0;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_FD_DIR, PROCFS_PID_SELF);
            return 0;
```

- [ ] **Step 8: `procfs_readdir` — `PID_DIR` case 加 `fd` 条目（第 439-440 行 `entry->name[0] = '\0';` 之前）**

```c
        if (index == 2) {
            strcpy(entry->name, "fd");
            entry->type = VFS_DIR;
            entry->size = 0;
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_FD_DIR, pid);
            return 0;
        }
```

- [ ] **Step 9: `procfs_readdir` 加 `FD_DIR` case（第 443 行 `default:` 之前）**

```c
    // ── /proc/<pid>/fd/ ──────────────────────────────────
    case PROCFS_TYPE_FD_DIR: {
        uint32_t p = pid;
        if (p == PROCFS_PID_SELF) { if (!current) return -1; p = (uint32_t)current->pid; }

        files_t *fs = task_files_pin_by_pid((int)p);   // pin under task_list_lock
        if (!fs) return -1;

        uint64_t k = index;
        int found = -1;
        {
            uint64_t fl = spin_lock_irqsave(&fs->lock); // single scan, held
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
            entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                PROCFS_TYPE_FD_ENTRY, pid);
        } else {
            entry->name[0] = '\0';
        }

        files_unpin(fs);   // outside any lock
        return 0;
    }
```

- [ ] **Step 10: 编译 + 手动冒烟**

```bash
make clean
make kernel.bin
make run
```

Expected: 编译通过，boot 到 shell。在 shell 里手敲验证：

```
# cat /proc/self/fd/3      → 输出 /proc/meminfo（或其他已打开路径）
# ls /proc/self/fd         → 列出 0 1 2 ...
```

- [ ] **Step 11: Commit**

```bash
git add kernel/include/fs/procfs.h kernel/fs/procfs.c
git commit -m "feat(procfs): add /proc/<pid>/fd/ read-only observability

FD_DIR enumerates open fds (name = fd number); FD_ENTRY reads the
target path as a synthetic file, resolved via the reference protocol.
pipe/socket render as pipe:[?]/socket:[?] to avoid kernel addr leaks."
```

---

### Task 5: systest 覆盖

**Files:**
- Modify: `user/systest.c`（`test_proc_fd` + 注册）

**Interfaces:**
- Consumes: `/proc/self/fd/<N>`、`/proc/<pid>/fd/<N>`（Task 4）；`nanosleep`（已有）。

- [ ] **Step 1: 加 `test_proc_fd`（放在 `test_proc_maps` 之后，约第 1177 行）**

```c
// ── Test /proc/self/fd ─────────────────────────────────────
static void test_proc_fd(void)
{
    char buf[512], path[64];
    int n, fd, fdfd, r;

    // 1. FD_VFS: open /proc/meminfo, read /proc/self/fd/<fd> → resolved path
    fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) { FAIL("proc_fd", "open /proc/meminfo failed"); return; }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    fdfd = open(path, O_RDONLY);
    if (fdfd < 0) { FAIL("proc_fd", "open /proc/self/fd/N failed"); close(fd); return; }
    n = (int)read(fdfd, buf, sizeof(buf) - 1);
    close(fdfd);
    if (n <= 0) { FAIL("proc_fd", "read fd link empty"); close(fd); return; }
    buf[n] = '\0';
    CHECK3(strcmp(buf, "/proc/meminfo\n") == 0, "proc_fd", "FD_VFS resolves path");

    // 2. FD_PIPE: pipe, read read-end target
    int fds[2];
    if (pipe(fds) < 0) { FAIL("proc_fd", "pipe failed"); close(fd); return; }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fds[0]);
    fdfd = open(path, O_RDONLY);
    if (fdfd < 0) { FAIL("proc_fd", "open pipe fd failed"); close(fd); close(fds[0]); close(fds[1]); return; }
    n = (int)read(fdfd, buf, sizeof(buf) - 1);
    close(fdfd);
    if (n <= 0) { FAIL("proc_fd", "read pipe link empty"); close(fd); close(fds[0]); close(fds[1]); return; }
    buf[n] = '\0';
    CHECK3(strncmp(buf, "pipe:[?]\n", 9) == 0, "proc_fd", "FD_PIPE format");

    // 3. Directory enumeration: see 0, 1, 2
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) { FAIL("proc_fd", "opendir /proc/self/fd failed"); close(fd); close(fds[0]); close(fds[1]); return; }
    int has0 = 0, has1 = 0, has2 = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, "0") == 0) has0 = 1;
        if (strcmp(de->d_name, "1") == 0) has1 = 1;
        if (strcmp(de->d_name, "2") == 0) has2 = 1;
    }
    closedir(dir);
    CHECK3(has0 && has1 && has2, "proc_fd", "enum 0/1/2");

    // 4. Close then open fails (ENOENT)
    int saved = fds[1];
    close(saved);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", saved);
    r = open(path, O_RDONLY);
    CHECK3(r < 0, "proc_fd", "closed fd open fails");

    // 5. Out-of-range fd fails
    r = open("/proc/self/fd/9999", O_RDONLY);
    CHECK3(r < 0, "proc_fd", "out-of-range fd fails");

    // 6. Non-current PID: child holds fds, parent reads /proc/<child>/fd/0
    int cpid = fork();
    if (cpid == 0) {
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, NULL);   // hold fds open while parent inspects
        _exit(0);
    } else if (cpid > 0) {
        snprintf(path, sizeof(path), "/proc/%d/fd/0", cpid);
        fdfd = open(path, O_RDONLY);
        if (fdfd >= 0) {
            n = (int)read(fdfd, buf, sizeof(buf) - 1);
            close(fdfd);
            CHECK3(n > 0, "proc_fd", "non-current pid fd readable");
        } else {
            FAIL("proc_fd", "open /proc/<pid>/fd/0 failed");
        }
        waitpid(cpid, NULL, 0);
    } else {
        FAIL("proc_fd", "fork failed");
    }

    close(fd);
    close(fds[0]);
}
```

- [ ] **Step 2: 注册进 `tests[]`（第 1236 行 `{"proc_maps", test_proc_maps}` 之后）**

```c
    {"proc_maps",           test_proc_maps},
    {"proc_fd",             test_proc_fd},
```

- [ ] **Step 3: 运行验证**

```bash
make clean
make OS01_SYSTEST=1 test-syscall
```

Expected: systest 全部通过，新增 `proc_fd` 测试组全 PASS（总数 126 → 变为更多）。

- [ ] **Step 4: Commit**

```bash
git add user/systest.c
git commit -m "test(systest): add proc_fd coverage for /proc/<pid>/fd"
```

---

### Task 6: 内核 selftest（引用协议不变量）

**Files:**
- Modify: `kernel/test/selftest.c`（`test_fd_refcount_basic` + 注册）

**Interfaces:**
- Consumes: `files_alloc`/`files_pin`/`files_unpin`/`files_get_file`/`files_put_file`/`fd_alloc`/`fd_close`/`fd_dup`/`fd_dup2`/`file_alloc`/`file_put`（Task 1、2）。

- [ ] **Step 1: 加 `test_fd_refcount_basic`（放在 `test_pipe_basic` 之后，约第 110 行）**

```c
// ── fd reference-protocol invariants ──────────────────────
// Single-threaded protocol exercise (SMP stress is not feasible in the
// pre-idle selftest window; correctness rests on lock ordering, which
// these invariants verify for the drop-to-zero/dup2-detach paths).
static int test_fd_refcount_basic(void)
{
    files_t *fs = files_alloc();
    if (!fs) return -1;
    if (fs->refcount != 1) { files_unpin(fs); return -1; }

    // pin/unpin round-trip keeps table alive
    files_pin(fs);
    if (fs->refcount != 2) { files_unpin(fs); files_unpin(fs); return -1; }
    files_unpin(fs);
    if (fs->refcount != 1) { files_unpin(fs); return -1; }

    // get/put: put a file in a slot, get it back with a stable ref
    file_t *f = file_alloc();          // refcount==1
    if (!f) { files_unpin(fs); return -1; }
    int fd = fd_alloc(fs, f);          // table holds the initial ref
    if (fd < 0) { file_put(f); files_unpin(fs); return -1; }

    file_t *g = files_get_file(fs, fd);
    if (g != f) { files_put_file(g); fd_close(fs, fd); files_unpin(fs); return -1; }
    files_put_file(g);                 // back to refcount 1

    // dup: fd and fd2 both reference f
    int fd2 = fd_dup(fs, fd, 0);
    if (fd2 != fd + 1) { fd_close(fs, fd); files_unpin(fs); return -1; }

    // dup2 onto existing target detaches old ref without UAF
    if (fd_dup2(fs, fd, fd2) != fd2) { fd_close(fs, fd); fd_close(fs, fd2); files_unpin(fs); return -1; }

    // close both → drop-to-zero → file_free (no crash = pass)
    fd_close(fs, fd);
    fd_close(fs, fd2);
    files_unpin(fs);                   // deferred free; reaper drains later
    return 0;
}
```

- [ ] **Step 2: 注册进 `selftest_run_all`（第 135 行 `pipe_basic` 之后）**

```c
    selftest_register("pipe_basic",        test_pipe_basic);
    selftest_register("fd_refcount_basic", test_fd_refcount_basic);
```

- [ ] **Step 3: 运行验证**

```bash
make clean
make KERNEL_SELFTEST=1 run
```

Expected: boot 串口输出 `[selftest] fd_refcount_basic... PASS`，且无 `FAIL`。若 `files_alloc` 在 selftest 阶段不可用（`calloc` 未就绪），把测试移到 `task.c` 的 `OS01_SELFTEST` 块（`scheduler_ok` 之后）改用 `extern void test_fd_refcount(void)` 模式（参照 `test_deferred_free`），并在 Step 2 相应改注册位置。

- [ ] **Step 4: Commit**

```bash
git add kernel/test/selftest.c
git commit -m "test(selftest): add fd reference-protocol invariants"
```

---

### Task 7: roadmap 标记 + 全量回归

**Files:**
- Modify: `docs/roadmap.md`（P0 #2 标 ✅）

- [ ] **Step 1: 更新 roadmap**

`docs/roadmap.md` 第 30 行：
```
 2. /proc/<pid>/fd/           — 补齐进程文件描述符可观测性
```
改为：
```
 2. /proc/<pid>/fd/           ✅ — 只读 fd 目录 + fd→目标路径合成文件；files_t 引用协议（pin/unpin/get/put）消除 UAF
```

- [ ] **Step 2: 全量回归（三种测试全跑）**

```bash
make clean
make OS01_SYSTEST=1 test-syscall      # systest 全绿
make KERNEL_SELFTEST=1 run            # selftest fd_refcount_basic PASS
```

Expected: systest 全 PASS；selftest 无 FAIL。

- [ ] **Step 3: Commit + 合并回 master**

```bash
git add docs/roadmap.md
git commit -m "docs: mark /proc/<pid>/fd observability complete"
# 之后在 worktree 分支跑完整测试，再合并回 master（按你的工作流）
```

---

## Self-Review 记录

- **Spec 覆盖**：引用协议（Task 1）、dup/fcntl 收敛（Task 2）、task_files_pin_by_pid + 退出路径（Task 3）、procfs 节点（Task 4）、systest（Task 5）、selftest（Task 6）、roadmap（Task 7）——spec 各节均有对应任务。
- **类型一致性**：`files_pin/unpin/get_file/put_file`、`file_get/put`、`fd_dup/dup2`、`task_files_pin_by_pid` 在 Task 1/3 定义、Task 4 消费，签名一致。
- **锁纪律**：所有 `files_unpin`/`file_put`/`files_put_file` 调用点均在锁外（Task 3 Step 4 的锁外 unpin 循环、Task 4 的 `files_unpin(fs)` 在 `task_files_pin_by_pid` 返回后）。
