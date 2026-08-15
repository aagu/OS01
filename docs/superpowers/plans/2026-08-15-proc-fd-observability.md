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
// All take NULL as a no-op (defensive; failure paths are safe to call).
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
// NULL is a no-op for both (defensive: keeps failure paths and the
// procfs reader's "slot may be empty" guard from crashing).
// file_get is safe under any lock (pure atomic, never frees).
// file_put MUST NOT be called under fs->lock: a drop-to-zero enters
// file_free → pipe/pty/socket cleanup + wake.
void file_get(file_t *f)
{
    if (!f) return;
    __sync_add_and_fetch(&f->refcount, 1);
}

void file_put(file_t *f)
{
    if (!f) return;
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
// NULL is a no-op (defensive; mirrors file_get/file_put).
void files_pin(files_t *fs)
{
    if (!fs) return;
    __sync_add_and_fetch(&fs->refcount, 1);
}

// MUST NOT be called under task_list_lock/fs->lock/rq lock: drop-to-zero
// → deferred_files_free, whose OOM fallback synchronously files_free()s.
void files_unpin(files_t *fs)
{
    if (!fs) return;
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
替换为（**两阶段：task_list_lock 内 detach，锁外 unpin**——与 `task_files_pin_by_pid` 通过同一把锁串行化，否则 CPU0 读到旧 `t->files` 后 CPU1 无锁置空并释放，CPU0 再 pin 已释放的表）：

```c
    // Detach our fd-table under task_list_lock (serializes against
    // task_files_pin_by_pid), then drop the reference outside the lock
    // (files_unpin may synchronously files_free on deferred OOM).
    files_t *fs = NULL;
    {
        uint64_t fl = spin_lock_irqsave(&task_list_lock);
        fs = current->files;
        current->files = NULL;
        spin_unlock_irqrestore(&task_list_lock, fl);
    }
    if (fs)
        files_unpin(fs);
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

### Task 6: 内核 selftest（SMP 引用协议竞态）

> **依赖顺序**：本任务独立于 Task 4（procfs）与 Task 5（systest），可在 Task 3 完成后立即执行，尽早验证协议。启动顺序要求（`kernel_main → selftest_run_all() → task_init() → deferred_free_spawn() → scheduler_ok=1`）决定了它**不能**注册进早期 `selftest_run_all()`——那时 `df_queue` 未初始化，`files_unpin` 归零会向未初始化链表插入节点。

**Files:**
- Create: `kernel/test/test_fd_refcount.c`（双 kthread 竞态测试）
- Modify: `kernel/sched/task.c`（在 `task_init()` 的 `OS01_SELFTEST` 块加调用）

**Interfaces:**
- Consumes: `files_alloc`/`files_unpin`/`files_get_file`/`files_put_file`/`fd_alloc`/`fd_close`/`file_alloc`/`file_put`（Task 1）；`create_kthread`/`schedule`（已有）。

- [ ] **Step 1: 新建 `kernel/test/test_fd_refcount.c`**

```c
// kernel/test/test_fd_refcount.c
// ── fd reference-protocol SMP race tests ──────────────────
// Two scenarios, run from task_init() AFTER deferred_free_spawn()
// (files_unpin can defer-free) and scheduler_ok=1 (create_kthread +
// schedule() work).
//
// Synchronisation uses __atomic acquire/release flags, NOT volatile:
// volatile gives no cross-CPU happens-before.  The harness runs as the
// idle task and cannot wait_queue_sleep (wait.c adds current->io_wait_node
// and schedule()s), so it spins on flags via schedule(); workers are
// kthreads and use the same spin, keeping one uniform protocol.
//
// Ownership: harness holds the initial files_t ref; each SUCCESSFULLY
// created worker files_pin()s its own ref and unpins BEFORE signaling done.
// An abort flag + start flag let a partially-created worker set exit cleanly
// instead of spinning forever.  A scenario PASSes only when it provably ran
// cross-CPU and (for R2) provably observed a detach inside the reader's
// active window.  A use-after-free otherwise manifests as a crash (#PF).

#if defined(OS01_SELFTEST)

#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/file.h>
#include <kernel/percpu.h>

#define FD_RACE_ITERS  10000
#define SPIN_LIMIT     10000000

static void wait_flag(const int *flag)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
        schedule();
}

// Returns 1 if flag observed set within SPIN_LIMIT, else 0 (timeout).
static int wait_flag_timeout(const int *flag)
{
    int spins = 0;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE) && spins < SPIN_LIMIT) {
        schedule();
        spins++;
    }
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0;
}

// ── Scenario 1: get-vs-detach (R3) ────────────────────────
static files_t *race_fs;
static int      r3_start;
static int      r3_abort;
static int      r3_reader_done;
static int      r3_writer_done;
static int      r3_reader_cpu = -1;
static int      r3_writer_cpu = -1;
static int      r3_writer_err;

static uint64_t race_reader(uint64_t arg)
{
    (void)arg;
    wait_flag(&r3_start);
    if (__atomic_load_n(&r3_abort, __ATOMIC_ACQUIRE))
        goto out;                       // partial-create: exit without racing
    r3_reader_cpu = (int)cpu_id();
    for (int i = 0; i < FD_RACE_ITERS; i++) {
        file_t *g = files_get_file(race_fs, 0);
        if (g) files_put_file(g);       // slot may be empty (writer detached)
    }
out:
    files_unpin(race_fs);               // drop reader's own ref BEFORE signaling
    __atomic_store_n(&r3_reader_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static uint64_t race_writer(uint64_t arg)
{
    (void)arg;
    wait_flag(&r3_start);
    if (__atomic_load_n(&r3_abort, __ATOMIC_ACQUIRE))
        goto out;
    r3_writer_cpu = (int)cpu_id();
    for (int i = 0; i < FD_RACE_ITERS; i++) {
        file_t *f = file_alloc();
        if (!f) { r3_writer_err++; continue; }
        int fd = fd_alloc(race_fs, f);
        if (fd < 0) { file_put(f); r3_writer_err++; continue; }
        fd_close(race_fs, fd);
    }
out:
    files_unpin(race_fs);
    __atomic_store_n(&r3_writer_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static void run_get_detach_race(void)
{
    race_fs = files_alloc();            // harness ref: refcount == 1
    if (!race_fs) { serial_printk("FAIL (files_alloc)\n"); return; }

    r3_start = 0; r3_abort = 0; r3_reader_done = 0; r3_writer_done = 0;
    r3_reader_cpu = -1; r3_writer_cpu = -1; r3_writer_err = 0;

    // Pin a worker ref ONLY when its kthread is successfully created.
    int reader_ok = 0, writer_ok = 0;
    task_t *r = create_kthread(race_reader, 0, "fd-race-reader");
    if (r) { files_pin(race_fs); reader_ok = 1; }
    task_t *w = create_kthread(race_writer, 0, "fd-race-writer");
    if (w) { files_pin(race_fs); writer_ok = 1; }

    if (!reader_ok || !writer_ok) {
        // Release any created worker with abort set so it exits (not spin
        // forever on r3_start); wait for it to drop its ref, then drop ours.
        __atomic_store_n(&r3_abort, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&r3_start, 1, __ATOMIC_RELEASE);
        if (reader_ok) wait_flag_timeout(&r3_reader_done);
        if (writer_ok) wait_flag_timeout(&r3_writer_done);
        files_unpin(race_fs);
        serial_printk("FAIL (kthread create)\n");
        return;
    }

    __atomic_store_n(&r3_start, 1, __ATOMIC_RELEASE);   // release both workers

    int rd = wait_flag_timeout(&r3_reader_done);
    int wd = wait_flag_timeout(&r3_writer_done);

    if (!rd || !wd) {
        // Timeout: a worker may still be running (finite loop → will exit on
        // its own and drop its ref).  Deliberately LEAK the harness ref rather
        // than free a table a live worker may touch.
        serial_printk("FAIL (timeout reader=%d writer=%d)\n", rd, wd);
        return;
    }

    if (r3_writer_err)
        serial_printk("FAIL (writer_err=%d)\n", r3_writer_err);
    else if (r3_reader_cpu != r3_writer_cpu)
        serial_printk("PASS (cross-CPU %d/%d)\n", r3_reader_cpu, r3_writer_cpu);
    else
        serial_printk("FAIL (same-CPU %d, no SMP coverage)\n", r3_reader_cpu);

    files_unpin(race_fs);               // harness ref → refcount 0 → deferred free
}

// ── Scenario 2: pin-vs-detach (R2) ────────────────────────
// holder inherits its files_t via create_kthread (do_fork files_dup's init's
// table); we never create or override a table, so there is no leaked ref.
// Orchestration forces the detach to happen INSIDE the reader's active
// window: reader pins successfully (present), signals reader_started; holder
// waits for that, records its CPU, signals holder_entered, then do_exit().
// reader keeps looping until it sees a NULL pin (absent).  PASS requires
// present && absent && holder_entered && cross-CPU.
static int r2_reader_go;       // harness → reader: start
static int r2_reader_started;  // reader → holder: "I pinned at least once"
static int r2_holder_entered;  // holder → harness: "about to do_exit"
static int r2_reader_done;
static int r2_reader_cpu = -1;
static int r2_holder_cpu = -1;
static int r2_saw_present;
static int r2_saw_absent;

static uint64_t race_holder(uint64_t arg)
{
    (void)arg;
    wait_flag(&r2_reader_started);      // wait until reader has pinned once
    r2_holder_cpu = (int)cpu_id();
    __atomic_store_n(&r2_holder_entered, 1, __ATOMIC_RELEASE);
    do_exit(0);   // detaches inherited files under task_list_lock; no return
    return 0;
}

static uint64_t pin_reader(uint64_t arg)
{
    int pid = (int)arg;
    wait_flag(&r2_reader_go);
    r2_reader_cpu = (int)cpu_id();
    for (int i = 0; i < FD_RACE_ITERS && !(r2_saw_present && r2_saw_absent); i++) {
        files_t *fs = task_files_pin_by_pid(pid);
        if (fs) {
            r2_saw_present = 1;
            files_unpin(fs);
            // Signal holder on the FIRST successful pin (may fire repeatedly;
            // holder's wait_flag absorbs it harmlessly).
            __atomic_store_n(&r2_reader_started, 1, __ATOMIC_RELEASE);
        } else {
            r2_saw_absent = 1;          // holder detached while we were active
        }
    }
    __atomic_store_n(&r2_reader_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static void run_pin_detach_race(void)
{
    r2_reader_go = 0; r2_reader_started = 0; r2_holder_entered = 0;
    r2_reader_done = 0;
    r2_reader_cpu = -1; r2_holder_cpu = -1;
    r2_saw_present = 0; r2_saw_absent = 0;

    task_t *holder = create_kthread(race_holder, 0, "fd-pin-holder");
    if (!holder) { serial_printk("FAIL (holder create)\n"); return; }
    int holder_pid = (int)holder->pid;  // copy value while holder is alive

    task_t *reader = create_kthread(pin_reader, holder_pid, "fd-pin-reader");
    if (!reader) {
        // Let holder proceed to exit (its inherited table frees on do_exit).
        // We never touch holder task_t* again.
        __atomic_store_n(&r2_reader_started, 1, __ATOMIC_RELEASE);
        serial_printk("FAIL (reader create)\n");
        return;
    }

    __atomic_store_n(&r2_reader_go, 1, __ATOMIC_RELEASE);   // release reader

    if (!wait_flag_timeout(&r2_reader_done)) {
        serial_printk("FAIL (timeout)\n");
        return;
    }

    int holder_entered = __atomic_load_n(&r2_holder_entered, __ATOMIC_ACQUIRE);

    if (!(r2_saw_present && r2_saw_absent))
        serial_printk("FAIL (no detach observed present=%d absent=%d)\n",
                      r2_saw_present, r2_saw_absent);
    else if (!holder_entered)
        serial_printk("FAIL (holder never entered)\n");
    else if (r2_reader_cpu != r2_holder_cpu)
        serial_printk("PASS (cross-CPU %d/%d)\n", r2_reader_cpu, r2_holder_cpu);
    else
        serial_printk("FAIL (same-CPU %d, no SMP coverage)\n", r2_reader_cpu);
}

void test_fd_refcount(void)
{
    serial_printk("[selftest] fd_refcount get-vs-detach... ");
    run_get_detach_race();

    serial_printk("[selftest] fd_refcount pin-vs-detach... ");
    run_pin_detach_race();
}

#endif // OS01_SELFTEST
```

- [ ] **Step 2: `task.c` 的 `OS01_SELFTEST` 块加调用（`test_deferred_free()` 块之后，约第 1944 行）**

```c
#ifdef OS01_SELFTEST
    // ── fd reference-protocol race test ─────────────────────
    // After deferred_free_spawn() (files_unpin defers) and scheduler_ok
    // (create_kthread + schedule() work).
    {
        extern void test_fd_refcount(void);
        test_fd_refcount();
    }
#endif
```

- [ ] **Step 3: 运行验证（至少 -smp 2）**

```bash
make clean
make KERNEL_SELFTEST=1 run          # 默认 SMP=2；可 make SMP=4 KERNEL_SELFTEST=1 run 加重跨 CPU 窗口
```

Expected: boot 串口输出两条 PASS（无崩溃），且都必须是 **cross-CPU**。same-CPU 判 FAIL（coverage 未达成，需重跑；`sched_pick_cpu` 选最少负载 CPU，两个 kthread 先后创建、第二个创建时第一个已 enqueue 使 `nr_running` 更新，故通常分到不同 CPU）：
```
[selftest] fd_refcount get-vs-detach... PASS (cross-CPU 0/1)
[selftest] fd_refcount pin-vs-detach... PASS (cross-CPU 0/1)
```

R2 的 PASS 还要求 `saw_present && saw_absent && holder_entered` —— 证明 detach 确实发生在 reader 活跃区间内，而非 reader 跑完 holder 才退出。

漏 `fs->lock` 时，get-vs-detach 的 `files_get_file` 会在 writer `fd_close` 释放 `file_t` 后对已 free 的 `f->refcount` 原子自增 → #PF 崩溃；漏 `task_list_lock` 内 detach 时，pin-vs-detach 的 `task_files_pin_by_pid` 会 pin 已释放的表 → 崩溃。测试自身不制造 UAF：创建失败路径用 abort+start 释放已创建 worker（不再永久自旋），worker 先 `files_unpin` 自己再 signal done；超时路径有意泄漏。

- [ ] **Step 4: Commit**

```bash
git add kernel/test/test_fd_refcount.c kernel/sched/task.c
git commit -m "test(selftest): add SMP fd reference-protocol race tests

get-vs-detach (R3): files_get_file/put vs fd_close/fd_alloc on one slot.
pin-vs-detach (R2): task_files_pin_by_pid vs a kthread do_exit() detaching
its inherited table; PASS requires cross-CPU and observed detach (present &&
absent) inside the reader's window.  __atomic acquire/release + abort/start
protocol so a partial create never leaves a spinning worker.  Both run after
deferred_free_spawn + scheduler_ok."
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
make KERNEL_SELFTEST=1 run            # selftest fd_refcount PASS (SMP race)
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

- **Spec 覆盖**：引用协议（Task 1）、dup/fcntl 收敛（Task 2）、task_files_pin_by_pid + 退出路径（Task 3）、procfs 节点（Task 4）、systest（Task 5）、SMP 竞态 selftest（Task 6，两个场景：get-vs-detach=R3 + pin-vs-detach=R2）、roadmap（Task 7）——spec 各节均有对应任务。
- **类型一致性**：`files_pin/unpin/get_file/put_file`、`file_get/put`、`fd_dup/dup2`、`task_files_pin_by_pid` 在 Task 1/3 定义、Task 4/6 消费，签名一致。
- **锁纪律**：所有 `files_unpin`/`file_put`/`files_put_file` 调用点均在锁外——Task 3 Step 3 的 do_exit **锁内 detach + 锁外 unpin**（两阶段，与 `task_files_pin_by_pid` 经 `task_list_lock` 串行化）、Task 3 Step 4 收割器的锁外 unpin 循环、Task 4 的 `files_unpin(fs)` 在 `task_files_pin_by_pid` 返回后、Task 6 的 `files_unpin` 在测试尾部（无锁上下文）。
- **启动顺序**：Task 6 的 selftest 明确放在 `task_init()` 内 `deferred_free_spawn()` + `scheduler_ok=1` 之后，不注册进早期 `selftest_run_all()`（`df_queue` 未初始化会导致 `files_unpin` 归零时向未初始化链表插入节点）。
- **NULL 语义**：`file_get/put`/`files_pin/unpin` 均以 NULL 为 no-op，Task 6 reader 的 `if (g) files_put_file(g)` 与 Task 1 实现判空一致，消除 `file_put(NULL)` 崩溃路径。
- **SMP 同步**：Task 6 用 `__atomic_store_n/__atomic_load_n`（acquire/release）替代 volatile；harness 是 idle task、不能 `wait_queue_sleep`，故用 `schedule()` spin 检查标志。worker 记录 `cpu_id()`；**same-CPU 判 FAIL（coverage 未达成）**，不误报 PASS。
- **测试生命周期**：所有权模型 = harness 初始 ref + 每**成功创建**的 worker 各 `files_pin` 一个 ref，worker 先 `files_unpin` 自己再 `__atomic_store_n` signal done；harness 等两个 done 后才 `files_unpin` 自己。
- **abort/start 协议**：R3 创建失败时 set `r3_abort` + release `r3_start`，已创建 worker 检查 abort 后退出（不永久自旋），harness 等其 done 再 `files_unpin` 自己。超时路径仍有意泄漏（worker 有限循环会自退，其 ref 保活表）。
- **R2 观察证明**：编排 `reader → (pin 成功) → reader_started → holder → holder_entered → do_exit`，reader 循环直到 `saw_present && saw_absent`；PASS 要求 `saw_present && saw_absent && holder_entered && cross-CPU`，杜绝「reader 跑完 holder 才 detach」的假 PASS。holder 完成 detach 的确认来自 reader 观察到 `task_files_pin_by_pid(pid)==NULL`，而非轮询可被收割的 task_t*。




