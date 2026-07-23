# poll/select 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 syscall poll (Linux ABI 7)，支持 pipe 和 /dev/tty 的多 fd 同时等待及超时

**Architecture:** poll_table + 双队列（task/poll 分离）+ 级联唤醒。每个 fd 对象有两套队列——`*_wait` (wait_queue_t，挂直接阻塞的 task) + `*_poll` (list_t，挂 poll_wait_entry)。poll 调用时通过 poll_wait() 注册 poll_wait_entry 到各 fd 的 poll 队列，任一 fd 就绪时级联唤醒 polling task。

**Tech Stack:** C11, OS01 内核 (x86_64), clang, QEMU

## Global Constraints

- 所有内核头文件修改必须同步更新 `test/include/` 镜像
- 系统调用编号从 48 开始分配
- 一次 poll 最多支持 16 个 fd
- 信号检查使用 `current->signal & ~current->blocked` (POSIX 语义)
- POLLHUP 用于写端全关 (EOF)，POLLERR 用于读端全关 (EPIPE)

**Design spec:** `docs/superpowers/specs/2026-07-18-poll-select-design.md`

---

### Task 1: pipe_t 添加 4 个新字段

**Files:**
- Modify: `kernel/include/kernel/file.h:35-42`
- Modify: `test/include/kernel/file.h:35-42`

**Interfaces:**
- Produces: `pipe_t.read_wait`, `pipe_t.write_wait`, `pipe_t.read_poll`, `pipe_t.write_poll`

- [ ] **Step 1: 修改 kernel/include/kernel/file.h 中的 pipe_t**

```c
// kernel/include/kernel/file.h — 在 pipe_t 的 spinlock_T lock 之后、闭合 } 之前插入 4 行

// ── Pipe ───────────────────────────────────────────────────

typedef struct pipe {
    char      buf[PIPE_SIZE];
    int       head;           // producer writes here (ring buffer)
    int       tail;           // consumer reads here
    int       readers;        // active reader count
    int       writers;        // active writer count
    spinlock_T lock;
    wait_queue_t  read_wait;    // task 直接阻塞 (fd_read) — 挂 task_t.io_wait_node
    wait_queue_t  write_wait;   // task 直接阻塞 (fd_write)
    list_t        read_poll;    // poll entry — 挂 poll_wait_entry_t.node
    list_t        write_poll;   // poll entry
} pipe_t;
```

在文件顶部添加需要的 include：
```c
// 在 #include <kernel/arch/spinlock.h> 之后添加:
#include <list.h>
#include <kernel/wait.h>
```

- [ ] **Step 2: 同步到 test/include/kernel/file.h**

复制相同的修改到 `test/include/kernel/file.h`。

- [ ] **Step 3: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -5
```

预期: 编译通过（可能有未使用字段的 warning，忽略）

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/file.h test/include/kernel/file.h
git commit -m "feat(pipe): add wait_queue and poll_list fields to pipe_t

Four new fields for poll-supporting dual-queue architecture:
- read_wait / write_wait: wait_queue_t for direct fd_read/fd_write blocking
- read_poll / write_poll: list_t for poll_wait_entry_t cascade wake

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: pipe_alloc() 初始化新字段

**Files:**
- Modify: `kernel/fs/file.c:29-37`

**Interfaces:**
- Consumes: `pipe_t.read_wait`, `pipe_t.write_wait`, `pipe_t.read_poll`, `pipe_t.write_poll` (from Task 1)

- [ ] **Step 1: 在 pipe_alloc() 中添加初始化**

当前代码 (file.c:29-37):
```c
pipe_t *pipe_alloc(void)
{
    pipe_t *p = (pipe_t *)calloc(1, sizeof(pipe_t));
    if (!p) return NULL;
    p->readers = 1;
    p->writers = 1;
    spin_init(&p->lock);
    return p;
}
```

修改为:
```c
pipe_t *pipe_alloc(void)
{
    pipe_t *p = (pipe_t *)calloc(1, sizeof(pipe_t));
    if (!p) return NULL;
    p->readers = 1;
    p->writers = 1;
    spin_init(&p->lock);
    wait_queue_init(&p->read_wait);
    wait_queue_init(&p->write_wait);
    list_init(&p->read_poll);
    list_init(&p->write_poll);
    return p;
}
```

- [ ] **Step 2: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -5
```

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/file.c
git commit -m "feat(pipe): init wait_queue and poll_list in pipe_alloc()

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: 创建 poll.h 头文件 — poll_table_t + poll_wait_entry_t

**Files:**
- Create: `kernel/include/kernel/poll.h`
- Create: `test/include/kernel/poll.h`

**Interfaces:**
- Produces: `poll_table_t`, `poll_wait_entry_t`, `POLLIN`, `POLLOUT`, `POLLHUP`, `POLLERR`, `POLLNVAL`, `POLLRDNORM`, `POLLWRNORM`, `POLL_MAX_FDS`

- [ ] **Step 1: 创建 kernel/include/kernel/poll.h**

```c
#ifndef _KERNEL_POLL_H
#define _KERNEL_POLL_H

#include <stdint.h>
#include <stdbool.h>
#include <list.h>
#include <kernel/wait.h>
#include <kernel/arch/spinlock.h>

// ── Max fds per poll call ──────────────────────────────

#define POLL_MAX_FDS  16

// ── Poll event flags (Linux ABI) ──────────────────────

#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020
#define POLLRDNORM  0x040
#define POLLRDBAND  0x080
#define POLLWRNORM  0x100
#define POLLWRBAND  0x200

// ── Poll fd (Linux ABI — must match libc/include/poll.h) ──
// Defined here so kernel code (poll.c, trap.c) can use it
// without including a userspace header.

struct pollfd {
    int   fd;
    short events;
    short revents;
};

// ── Forward declaration ────────────────────────────────

struct poll_table;

// ── Poll wait entry ────────────────────────────────────
// Hung on an fd's poll list (e.g. pipe_t.read_poll).
// When the fd becomes ready, its wake path cascades:
//   wait_queue_wake_all(entry->poll_wq)
// The fd_lock pointer protects the list this entry lives on;
// poll_table_cleanup uses it for safe removal.

typedef struct poll_wait_entry {
    list_t       node;        // link in fd's poll list
    wait_queue_t *poll_wq;    // cascade target — pt.wq
    spinlock_T   *fd_lock;    // lock protecting the poll list
} poll_wait_entry_t;

// ── Poll table ─────────────────────────────────────────
// Per-syscall stack object.  The polling task sleeps on pt.wq;
// when any fd becomes ready it cascade-wakes pt.wq.

typedef struct poll_table {
    wait_queue_t        wq;                      // main wait queue
    poll_wait_entry_t   entries[POLL_MAX_FDS];   // static array
    int                 nent;                    // active entry count
    bool                triggered;               // short-circuit: fd ready
} poll_table_t;

// ── API ────────────────────────────────────────────────

// Reset poll table for a new scan round (nent=0, triggered=false).
// Does NOT re-init wq — call poll_table_setup once, then init per round.
void poll_table_init(poll_table_t *pt);

// One-time setup: init wq + all entry nodes.
void poll_table_setup(poll_table_t *pt);

// Register current fd as not-ready.  The entry is hung on poll_list
// (protected by fd_lock).  When the fd becomes ready, its wake path
// walks poll_list and calls wait_queue_wake_all(e->poll_wq).
void poll_wait(poll_table_t *pt, list_t *poll_list, spinlock_T *fd_lock);

// Remove all entries from their fd poll lists.  Uses entry.fd_lock
// for mutual exclusion with fd wake paths.
void poll_table_cleanup(poll_table_t *pt);

// Forward-declared in kernel/fs/file.h; implementation in kernel/fs/poll.c.
struct file;
uint32_t fd_poll(struct file *f, struct poll_table *pt);

#endif // _KERNEL_POLL_H
```

- [ ] **Step 2: 创建 test/include/kernel/poll.h**

与上完全相同的文件内容。

- [ ] **Step 3: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -10
```

预期: 编译通过（fd_poll 未定义但该符号可延迟到 poll.c 编译时解析）。

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/poll.h test/include/kernel/poll.h
git commit -m "feat(poll): add poll_table_t and poll_wait_entry_t header

Defines the poll subsystem data structures and event flags.
- poll_wait_entry_t: hung on fd poll lists for cascade wake
- poll_table_t: per-syscall stack object with local wait queue
- POLLIN/POLLOUT/POLLHUP/POLLERR/POLLNVAL per Linux ABI

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: pipe_wake_readers() / pipe_wake_writers() 实现

**Files:**
- Modify: `kernel/fs/file.c` (在 pipe_full 之后、fd_read 之前插入)

**Interfaces:**
- Produces: `pipe_wake_readers(pipe_t *p)`, `pipe_wake_writers(pipe_t *p)` — 调用方已持有 p->lock
- Consumes: `pipe_t.read_wait`, `pipe_t.write_wait`, `pipe_t.read_poll`, `pipe_t.write_poll` (from Task 1)

- [ ] **Step 1: 在文件前部添加 forward declaration 和 include**

在 file.c 顶部 `#include <errno.h>` 之后添加：
```c
#include <kernel/poll.h>    // poll_wait_entry_t, container_of for poll entries
```

- [ ] **Step 2: 在 pipe_full() 之后、fd_read() 之前插入两个函数**

当前 file.c 的 pipe_full() 在 line 143。在 `}` 闭合后、`// ── Read through a file descriptor` 注释行之前（约 line 145 之后），插入:

```c
// ── Pipe wake helpers ──────────────────────────────────────
// Caller must hold p->lock.  Wake one direct waiter + all poll
// entries.  Direct waiters use wait_queue_t (task_t.io_wait_node).
// Poll entries use a plain list_t (poll_wait_entry_t.node).
// Each poll entry cascades to wait_queue_wake_all(entry->poll_wq).

static void pipe_wake_readers(pipe_t *p)
{
    wait_queue_wake_one(&p->read_wait);

    while (!list_is_empty(&p->read_poll)) {
        list_t *node = p->read_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
}

static void pipe_wake_writers(pipe_t *p)
{
    wait_queue_wake_one(&p->write_wait);

    while (!list_is_empty(&p->write_poll)) {
        list_t *node = p->write_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
}
```

注意: `kernel/poll.h` 已在 Task 3 创建，此时编译可通过。

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/file.c
git commit -m "feat(pipe): add pipe_wake_readers/writers wake helpers

Cascade wake: direct waiter via wait_queue_t + poll entries via
poll_wait_entry_t poll_wq.  Caller holds p->lock.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: fd_read pipe 路径重构 — wait_queue_sleep 替代 schedule()

**Files:**
- Modify: `kernel/fs/file.c:164-192` (fd_read 的 FD_PIPE case)

**Interfaces:**
- Consumes: `pipe_wake_readers()` (from Task 4), `pipe_t.read_wait`

- [ ] **Step 1: 替换 fd_read 的 FD_PIPE 分支中的 busy-wait**

当前代码 (file.c:164-192):
```c
    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return -1;

        uint8_t *dst = (uint8_t *)buf;
        uint64_t total = 0;

        while (total < size) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_empty(p)) {
                dst[total++] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
            }
            spin_unlock_irqrestore(&p->lock, flags);

            if (total > 0) break;

            if (p->writers == 0)
                return 0;

            schedule();
        }

        return (int64_t)total;
    }
```

改为：
```c
    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return -1;

        uint8_t *dst = (uint8_t *)buf;
        uint64_t total = 0;

        for (;;) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_empty(p)) {
                dst[total++] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
            }

            if (total > 0) {
                // Data consumed — wake any blocked writers
                pipe_wake_writers(p);
                spin_unlock_irqrestore(&p->lock, flags);
                return (int64_t)total;
            }

            // Pipe empty — check if any writer still exists
            if (p->writers == 0) {
                spin_unlock_irqrestore(&p->lock, flags);
                return 0;  // EOF
            }

            spin_unlock_irqrestore(&p->lock, flags);

            // Block on pipe's read_wait (not busy-loop schedule)
            wait_queue_sleep(&p->read_wait);

            // Check for fatal signals after wake
            if (signal_pending_fatal())
                return -EINTR;
        }
    }
```

注意: 此时还不能编译（poll_wait_entry_t 缺失），但逻辑正确。将在 Task 7 之后一起验证。

- [ ] **Step 2: Commit**

```bash
git add kernel/fs/file.c
git commit -m "refactor(pipe): use wait_queue_sleep in fd_read pipe path

Replace busy-loop schedule() with proper blocking on pipe_t.read_wait.
Wake writers after consuming data via pipe_wake_writers().

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: fd_write pipe 路径重构 — wait_queue_sleep 替代 schedule()

**Files:**
- Modify: `kernel/fs/file.c:216-243` (fd_write 的 FD_PIPE case)

**Interfaces:**
- Consumes: `pipe_wake_readers()` (from Task 4), `pipe_t.write_wait`

- [ ] **Step 1: 替换 fd_write 的 FD_PIPE 分支中的 busy-wait**

当前代码 (file.c:216-243):
```c
    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return -1;

        const uint8_t *src = (const uint8_t *)buf;
        uint64_t total = 0;

        while (total < size) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_full(p)) {
                p->buf[p->head] = src[total++];
                p->head = (p->head + 1) % PIPE_SIZE;
            }
            spin_unlock_irqrestore(&p->lock, flags);

            if (total == size) break;

            if (p->readers == 0)
                return -EPIPE;

            schedule();
        }

        return (int64_t)total;
    }
```

改为：
```c
    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return -1;

        const uint8_t *src = (const uint8_t *)buf;
        uint64_t total = 0;

        for (;;) {
            uint64_t flags = spin_lock_irqsave(&p->lock);

            while (total < size && !pipe_full(p)) {
                p->buf[p->head] = src[total++];
                p->head = (p->head + 1) % PIPE_SIZE;
            }

            if (total > 0) {
                // Wrote some data — wake blocked readers, return
                pipe_wake_readers(p);
                spin_unlock_irqrestore(&p->lock, flags);
                return (int64_t)total;
            }

            // Pipe is full (total == 0)
            // Check if any reader still exists
            if (p->readers == 0) {
                spin_unlock_irqrestore(&p->lock, flags);
                return -EPIPE;
            }

            spin_unlock_irqrestore(&p->lock, flags);

            // Block on pipe's write_wait (not busy-loop schedule)
            wait_queue_sleep(&p->write_wait);

            if (signal_pending_fatal())
                return -EINTR;
        }
    }
```

- [ ] **Step 2: Commit**

```bash
git add kernel/fs/file.c
git commit -m "refactor(pipe): use wait_queue_sleep in fd_write pipe path

Replace busy-loop schedule() with proper blocking on pipe_t.write_wait.
Wake readers after producing data via pipe_wake_readers().

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: fd_close() 添加 poll waiter 唤醒

**Files:**
- Modify: `kernel/fs/file.c:108-131`

**Interfaces:**
- Consumes: `pipe_wake_readers()`, `pipe_wake_writers()` (from Task 4)

- [ ] **Step 1: 修改 fd_close 的 pipe 路径，在关闭后唤醒 poll waiter**

当前代码 (file.c:107-131):
```c
void fd_close(files_t *fs, int fd)
{
    if (!fs || fd < 0 || fd >= NOFILE) return;

    file_t *f = fs->fd[fd];
    if (!f) return;

    fs->fd[fd] = NULL;

    // For pipes: decrement reader/writer counts
    if (f->type == FD_PIPE && f->pipe) {
        if (f->flags == O_RDONLY)
            f->pipe->readers--;
        else
            f->pipe->writers--;
    }

    f->refcount--;
    if (f->refcount == 0) {
        if (f->type == FD_PIPE && f->pipe)
            pipe_free(f->pipe);
        file_free(f);
    }
}
```

改为：
```c
void fd_close(files_t *fs, int fd)
{
    if (!fs || fd < 0 || fd >= NOFILE) return;

    file_t *f = fs->fd[fd];
    if (!f) return;

    fs->fd[fd] = NULL;

    // For pipes: decrement reader/writer counts and wake poll waiters
    if (f->type == FD_PIPE && f->pipe) {
        uint64_t flags = spin_lock_irqsave(&f->pipe->lock);

        if (f->flags == O_RDONLY) {
            f->pipe->readers--;
            if (f->pipe->readers == 0)
                pipe_wake_writers(f->pipe);
        } else {
            f->pipe->writers--;
            if (f->pipe->writers == 0)
                pipe_wake_readers(f->pipe);
        }

        spin_unlock_irqrestore(&f->pipe->lock, flags);
    }

    f->refcount--;
    if (f->refcount == 0) {
        if (f->type == FD_PIPE && f->pipe)
            pipe_free(f->pipe);
        file_free(f);
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/fs/file.c
git commit -m "feat(pipe): wake poll waiters on fd_close

When last reader/writer closes, cascade-wake any poll waiters:
- writers→0: wake poll readers (they get POLLHUP on next scan)
- readers→0: wake poll writers (they get POLLERR on next scan)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: 创建 poll.c — poll_table 操作原语

**Files:**
- Create: `kernel/fs/poll.c`

**Interfaces:**
- Produces: `poll_table_init()`, `poll_table_setup()`, `poll_wait()`, `poll_table_cleanup()`
- Consumes: `poll_table_t`, `poll_wait_entry_t` (from Task 3 header)

- [ ] **Step 1: 创建 kernel/fs/poll.c 并实现 poll_table 操作**

```c
// kernel/fs/poll.c — poll/select system call implementation
//
// Architecture:
//   Each fd object has TWO queues:
//     *_wait  (wait_queue_t): direct blocking via task_t.io_wait_node
//     *_poll  (list_t):       poll entries via poll_wait_entry_t.node
//   When an fd becomes ready, its wake path walks both queues.
//   Poll entries cascade-wake: wait_queue_wake_all(entry->poll_wq).
//
//   poll_table_cleanup uses entry.fd_lock for mutual exclusion
//   with concurrent fd wake paths.

#include <kernel/poll.h>
#include <kernel/file.h>
#include <kernel/task.h>
#include <fs/vfs.h>
#include <kernel/percpu.h>
#include <device/timer.h>    // jiffies
#include <stddef.h>

// ── poll_table_init — reset for new scan round ─────────────
// Does NOT re-init wq or entry nodes (one-time setup).

void poll_table_init(poll_table_t *pt)
{
    pt->nent = 0;
    pt->triggered = false;
}

// ── poll_table_setup — one-time init of wq + entry nodes ──

void poll_table_setup(poll_table_t *pt)
{
    wait_queue_init(&pt->wq);
    for (int i = 0; i < POLL_MAX_FDS; i++)
        list_init(&pt->entries[i].node);
}

// ── poll_wait — register on an fd's poll list ─────────────
// Called by fd_poll() when the fd is NOT ready.
// The entry will later be cascade-woken by the fd's wake path.

void poll_wait(poll_table_t *pt, list_t *poll_list, spinlock_T *fd_lock)
{
    if (pt->nent >= POLL_MAX_FDS || pt->triggered)
        return;

    poll_wait_entry_t *e = &pt->entries[pt->nent++];
    e->poll_wq = &pt->wq;
    e->fd_lock = fd_lock;

    list_add_to_before(poll_list, &e->node);
}

// ── poll_table_cleanup — remove all entries from fd lists ──
// Safe against concurrent fd wake: takes each entry's fd_lock,
// re-checks list_is_empty (fd wake may have already removed it).

void poll_table_cleanup(poll_table_t *pt)
{
    for (int i = 0; i < pt->nent; i++) {
        poll_wait_entry_t *e = &pt->entries[i];
        if (!list_is_empty(&e->node) && e->fd_lock) {
            uint64_t flags = spin_lock_irqsave(e->fd_lock);
            if (!list_is_empty(&e->node))
                list_del_init(&e->node);
            spin_unlock_irqrestore(e->fd_lock, flags);
        }
    }
    pt->nent = 0;
}
```

- [ ] **Step 2: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -10
```

预期: 编译通过（poll.c 有 forward declaration，`.c→.o` 成功）。链接阶段 `devfs_poll` 未定义，Task 15 后解决。

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/poll.c
git commit -m "feat(poll): implement poll_table init/wait/cleanup primitives

poll_table_setup/init/wait/cleanup — the four operations for
managing poll_wait_entry registration across multiple fds.
cleanup is concurrency-safe via entry.fd_lock double-check.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: 实现 fd_poll() — 单 fd 就绪检查

**Files:**
- Modify: `kernel/fs/poll.c`

**Interfaces:**
- Produces: `fd_poll(file_t *f, poll_table_t *pt) → uint32_t` (revents 掩码)
- Consumes: `poll_wait()` (from Task 8), `devfs_poll()` (will be added in Task 15; forward-declare for now)

- [ ] **Step 1: 在 poll.c 中添加文件顶部 forward declaration**

在 poll.c 的 `#include` 块之后、`poll_table_init` 之前插入:

```c
// Forward: devfs_poll lives in devfs.c (devices[] is static there)
struct vfs_node;
uint32_t devfs_poll(struct vfs_node *node, poll_table_t *pt);
```

- [ ] **Step 2: 在 poll_table_cleanup 之后、文件末尾添加 fd_poll**

```c
// ── fd_poll — check readiness of a single fd ──────────────
// Returns a mask of poll event flags.  If the fd is NOT ready
// and pt is non-NULL, calls poll_wait() to register on the fd's
// private poll list for later cascade-wake.
//
// FD types:
//   FD_VFS  — plain files: always ready
//   FD_DEV  — delegate to devfs_poll() (e.g. /dev/tty)
//   FD_PIPE — check ring buffer; if empty/full, poll_wait

static inline int pipe_empty(pipe_t *p) { return p->head == p->tail; }
static inline int pipe_full(pipe_t *p)  { return ((p->head + 1) % PIPE_SIZE) == p->tail; }

uint32_t fd_poll(file_t *f, poll_table_t *pt)
{
    if (!f) return POLLNVAL;

    switch (f->type) {

    case FD_VFS:
        // Plain files are always ready for read and write
        if (f->flags == O_RDONLY || f->flags == O_RDWR)
            return POLLIN | POLLRDNORM;
        if (f->flags == O_WRONLY || f->flags == O_RDWR)
            return POLLOUT | POLLWRNORM;
        return 0;

    case FD_DEV:
        // /dev devices — delegate to devfs_poll()
        if (!f->node) return POLLNVAL;
        return devfs_poll(f->node, pt);

    case FD_PIPE: {
        pipe_t *p = f->pipe;
        if (!p) return POLLERR;

        uint32_t mask = 0;
        uint64_t flags = spin_lock_irqsave(&p->lock);

        if (f->flags == O_RDONLY) {
            if (!pipe_empty(p))
                mask |= POLLIN;
            else if (p->writers == 0)
                mask |= POLLHUP;
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->read_poll, &p->lock);
        }

        if (f->flags == O_WRONLY) {
            if (!pipe_full(p))
                mask |= POLLOUT;
            else if (p->readers == 0)
                mask |= POLLERR;
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->write_poll, &p->lock);
        }

        if (f->flags == O_RDWR) {
            if (!pipe_empty(p))
                mask |= POLLIN;
            else if (p->writers == 0)
                mask |= POLLHUP;
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->read_poll, &p->lock);

            if (!pipe_full(p))
                mask |= POLLOUT;
            else if (p->readers == 0)
                mask |= POLLERR;
            else if (pt && !pt->triggered)
                poll_wait(pt, &p->write_poll, &p->lock);
        }

        spin_unlock_irqrestore(&p->lock, flags);
        return mask;
    }

    default:
        return POLLNVAL;
    }
}
```

注意: `pipe_empty`/`pipe_full` 在 file.c 中已定义为 `static inline`（file.c:135-143），在 poll.c 中也需一份副本（或移至头文件）。这里在 poll.c 中重新定义以保持 file.c 不变。

- [ ] **Step 3: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -10
```

预期: `devfs_poll` 未定义 → 编译通过但链接错误。这是预期的 — Task 15 会补充该函数。

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/poll.c
git commit -m "feat(poll): implement fd_poll for VFS/DEV/PIPE types

Single-fd readiness check with poll_wait registration for non-ready
fds.  O_RDWR pipes checked for both read and write readiness.

Pending: devfs_poll() in devfs.c (Task 15).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: 实现 do_poll() 主循环

**Files:**
- Modify: `kernel/fs/poll.c`

**Interfaces:**
- Produces: `do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout) → int64_t`
- Consumes: `fd_poll()`, `poll_table_setup/init/cleanup/wait()`

- [ ] **Step 1: 在 poll.c 文件底部添加 do_poll**

```c
// ── do_poll — poll syscall implementation ────────────────
//
// Linux ABI: int poll(struct pollfd *fds, nfds_t nfds, int timeout)
//   timeout: -1 = infinite, 0 = non-blocking, >0 = milliseconds
// Returns: count of ready fds, 0 = timeout, <0 = -errno
//
// Signal semantics: any unblocked signal (not just fatal) interrupts
// poll with -EINTR, per POSIX.

// ── Global poll state (single-CPU safe; will need per-CPU for SMP) ──
wait_queue_t *current_poll_wq = NULL;
uint64_t poll_deadline_jiffies = 0;

int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout_val)
{
    // ── Validate user pointer ──────────────────────────────
    if ((uint64_t)user_fds >= current->addr_limit)
        return -EFAULT;
    if (nfds == 0)
        return 0;
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;

    // ── Copy pollfd from user space ────────────────────────
    struct pollfd kfds[POLL_MAX_FDS];
    for (uint32_t i = 0; i < nfds; i++) {
        kfds[i].fd      = user_fds[i].fd;
        kfds[i].events  = user_fds[i].events;
        kfds[i].revents = 0;
    }

    // ── Signal check: any unblocked pending signal → EINTR ─
    if (current->signal & ~current->blocked)
        return -EINTR;

    // ── Setup poll table (one-time wq init) ────────────────
    poll_table_t pt;
    poll_table_setup(&pt);

    // ── Timeout setup ──────────────────────────────────────
    uint64_t deadline = 0;
    if (timeout_val > 0) {
        // Convert ms to PIT ticks (100 Hz → 10 ms/tick)
        int ticks = (timeout_val + 9) / 10;
        if (ticks < 1) ticks = 1;
        poll_deadline_jiffies = jiffies + (uint64_t)ticks;
        deadline = poll_deadline_jiffies;
        current_poll_wq = &pt.wq;
    }

    int ready_count = 0;

    for (;;) {
        poll_table_init(&pt);  // reset nent=0, triggered=false

        // ── Scan all fds ──────────────────────────────────
        for (uint32_t i = 0; i < nfds; i++) {
            if (kfds[i].fd < 0) continue;

            file_t *f = current->files->fd[kfds[i].fd];
            if (!f) {
                kfds[i].revents = POLLNVAL;
                ready_count++;
                continue;
            }

            uint32_t revents = fd_poll(f, &pt);
            if (revents & kfds[i].events) {
                kfds[i].revents = revents & kfds[i].events;
                ready_count++;
                pt.triggered = true;
            }
        }

        // ── Ready? Return ─────────────────────────────────
        if (ready_count > 0) {
            poll_table_cleanup(&pt);
            if (timeout_val > 0) current_poll_wq = NULL;  // prevent dangling ptr
            break;
        }

        // ── Non-blocking? ─────────────────────────────────
        if (timeout_val == 0) {
            poll_table_cleanup(&pt);
            break;
        }

        // ── Pre-sleep signal check ────────────────────────
        if (current->signal & ~current->blocked) {
            poll_table_cleanup(&pt);
            if (timeout_val > 0) current_poll_wq = NULL;
            return -EINTR;
        }

        // ── Block on pt.wq ────────────────────────────────
        wait_queue_sleep(&pt.wq);

        // Woken up — remove entries from fd poll lists
        if (timeout_val > 0) current_poll_wq = NULL;
        poll_table_cleanup(&pt);

        // ── Timeout check ─────────────────────────────────
        if (timeout_val > 0 && jiffies >= deadline)
            return 0;

        // ── Post-sleep signal check ───────────────────────
        if (current->signal & ~current->blocked)
            return -EINTR;

        ready_count = 0;
    }

    // ── Copy revents back to user space ────────────────────
    for (uint32_t i = 0; i < nfds; i++)
        user_fds[i].revents = kfds[i].revents;

    return ready_count;
}
```

- [ ] **Step 2: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -10
```

预期: 仍缺少 `devfs_poll`（链接错误）。syscall 注册语法应无编译错误。

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/poll.c
git commit -m "feat(poll): implement do_poll main loop

Full poll() syscall: copy_from_user, fd scan with poll_wait
registration, blocking sleep on pt.wq with timeout via
scheduler tick deadline, three signal check points (entry,
pre-sleep, post-sleep), copy_to_user revents.

Pending: devfs_poll in Task 15, timer callback in Task 16.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: 注册 syscall 编号 + do_system_call 分发

**Files:**
- Modify: `libc/include/sys/syscall.h:49` (在 SYS_futex 之后)
- Modify: `kernel/arch/x86_64/trap.c` (添加 include + syscall_names 数组 + case)

**Interfaces:**
- Produces: `SYS_poll(48)`, `SYS_ppoll(49)`, `SYS_select(50)`

- [ ] **Step 1: 在 trap.c 顶部添加 include**

在 `#include <kernel/file.h>` 之后添加：
```c
#include <kernel/poll.h>   // struct pollfd, do_poll()
```

- [ ] **Step 2: 修改 libc/include/sys/syscall.h**

在 `#define SYS_futex 47` 之后添加：
```c
#define SYS_poll     48
#define SYS_ppoll    49   // v1: stub (returns -ENOSYS)
#define SYS_select   50   // v1: adaptor on top of do_poll
```

- [ ] **Step 3: 修改 syscall_names 数组 (trap.c:895-896)**

在 `[47] = "futex",` 之后、`};` 之前添加：
```c
        [48] = "poll",
        [49] = "ppoll",
        [50] = "select",
```

- [ ] **Step 4: 在 SYS_futex case 之后、default 之前添加 syscall 分发**

在 trap.c 的 `case SYS_futex: { ... } break;` (约 line 2061) 之后、`default:` 之前插入：

```c
    case SYS_poll: {
        int64_t nfds64 = (int64_t)regs->rsi;  // nfds_t is unsigned long (8 bytes)
        regs->rax = do_poll((struct pollfd *)regs->rdi,
                            (uint64_t)nfds64,
                            (int)regs->rdx);
        break;
    }
    case SYS_ppoll: {
        // v1: not implemented
        regs->rax = -ENOSYS;
        break;
    }
    case SYS_select: {
        // v1: not implemented (will use select→poll adaptor in Task 19)
        regs->rax = -ENOSYS;
        break;
    }
```

- [ ] **Step 5: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -10
```

预期: 仍缺少 `devfs_poll`（链接错误）。`SYS_poll` case 中 `struct pollfd` 通过 `#include <kernel/poll.h>` 可解析。

- [ ] **Step 6: Commit**

```bash
git add libc/include/sys/syscall.h kernel/arch/x86_64/trap.c
git commit -m "feat(poll): register SYS_poll(48)/ppoll(49)/select(50)

Add syscall numbers, syscall_names entries, and do_system_call
dispatch.  SYS_ppoll and SYS_select return -ENOSYS (v1 stubs).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 12: tty_t 添加 read_poll 字段 + tty_alloc 初始化

**Files:**
- Modify: `kernel/include/kernel/tty.h:39` (tty_t 结构体，在 read_wait 之后)
- Modify: `kernel/tty/tty.c:90-108` (tty_alloc)

**Interfaces:**
- Produces: `tty_t.read_poll` (list_t)

- [ ] **Step 1: 修改 tty_t 结构体**

在 `tty.h:39` 的 `list_t read_wait;` 之后添加：
```c
    // ── Poll wait list ──────────────────────────
    // Poll entries (poll_wait_entry_t.node) wait here.
    // Also protected by cooked_lock.
    list_t      read_poll;
```

- [ ] **Step 2: 修改 tty_alloc() 初始化**

在 `tty.c:101` `list_init(&tty->read_wait);` 之后添加：
```c
    list_init(&tty->read_poll);
```

- [ ] **Step 3: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -5
```

预期: 编译通过（只有 header change + init）。

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/tty.h kernel/tty/tty.c
git commit -m "feat(tty): add read_poll list to tty_t for poll support

Dual-queue architecture: read_wait for direct tty_read blocking,
read_poll for poll_wait_entry cascade-wake.  Both protected by
cooked_lock.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 13: 实现 tty_poll() + 扩展 tty_wake_waiters()

**Files:**
- Modify: `kernel/tty/tty.c:63-74` (tty_wake_waiters)
- Modify: `kernel/tty/tty.c` (在 tty_ioctl 之前插入 tty_poll)
- Modify: `kernel/include/kernel/tty.h:76` (在声明区添加 tty_poll 原型)

**Interfaces:**
- Produces: `tty_poll(tty_t *tty, poll_table_t *pt) → uint32_t`
- Consumes: `tty_t.read_poll`, `poll_table_t`, `poll_wait()`

- [ ] **Step 1: 在 tty.c 顶部添加 poll.h include**

在 tty.c 的 `#include` 块（约 line 11）添加：
```c
#include <kernel/poll.h>
```

- [ ] **Step 2: 扩展 tty_wake_waiters()**

将 tty.c:63-74 的现有 `tty_wake_waiters`:
```c
static void tty_wake_waiters(tty_t *tty)
{
    while (!list_is_empty(&tty->read_wait)) {
        list_t *node = tty->read_wait.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }
    this_cpu()->need_resched = 1;
}
```

改为：
```c
static void tty_wake_waiters(tty_t *tty)
{
    // 1. Wake direct blocking reader tasks (tty_read path)
    while (!list_is_empty(&tty->read_wait)) {
        list_t *node = tty->read_wait.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }

    // 2. Cascade-wake all poll waiters (fd_poll path)
    while (!list_is_empty(&tty->read_poll)) {
        list_t *node = tty->read_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }

    this_cpu()->need_resched = 1;
}
```

- [ ] **Step 3: 添加 tty_poll() 函数**

在 `tty_ioctl()` (约 line 304) 之前插入：

```c
// ── tty_poll — check TTY readiness ───────────────────────
// TTY is always writable.  Readable if cooked ring buffer
// has data.  If not ready and pt is provided, register a
// poll_wait_entry on tty->read_poll for cascade wake when
// tty_push_input() → tty_wake_waiters() fires.

uint32_t tty_poll(tty_t *tty, poll_table_t *pt)
{
    uint32_t mask = 0;

    // TTY output is always ready
    mask |= POLLOUT | POLLWRNORM;

    // Check cooked ring buffer
    uint64_t flags = spin_lock_irqsave(&tty->cooked_lock);
    if (tty->head != tty->tail) {
        mask |= POLLIN | POLLRDNORM;
    } else if (pt && !pt->triggered) {
        poll_wait(pt, &tty->read_poll, &tty->cooked_lock);
    }
    spin_unlock_irqrestore(&tty->cooked_lock, flags);

    return mask;
}
```

- [ ] **Step 4: 在 tty.h 中添加 tty_poll 原型**

在 `kernel/include/kernel/tty.h:76` (tty_ioctl 声明之后) 添加：
```c
// TTY poll — check if input is available.  Returns POLLIN/POLLOUT mask.
// If not ready, registers a poll_wait_entry on read_poll.
uint32_t tty_poll(tty_t *tty, struct poll_table *pt);
```

- [ ] **Step 5: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -5
```

预期: 编译通过（仍缺 devfs_poll，但 tty_poll 独立可编译）。

- [ ] **Step 6: Commit**

```bash
git add kernel/tty/tty.c kernel/include/kernel/tty.h
git commit -m "feat(tty): implement tty_poll and dual-queue wake

tty_wake_waiters now walks both read_wait (direct tasks) and
read_poll (poll entries).  tty_poll returns POLLOUT always +
POLLIN when cooked ring buffer has data; registers poll_wait
entry on read_poll when empty.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 14: devfs_device_t 添加 poll 回调 + 修改注册 API

**Files:**
- Modify: `kernel/fs/devfs.c:14-21` (devfs_device_t)
- Modify: `kernel/include/fs/devfs.h:13-15` (devfs_register_chrdev 签名)

**Interfaces:**
- Produces: `devfs_device_t.poll`, updated `devfs_register_chrdev` signature
- Consumes: `poll_table_t` (forward declared)

- [ ] **Step 1: 修改 devfs_device_t 结构体**

当前 devfs.c:14-21:
```c
typedef struct devfs_device {
    char name[DEVFS_NAME_MAX];
    uint8_t type;       // VFS_CHRDEV or VFS_BLKDEV
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
    void *private_data;
    int registered;
} devfs_device_t;
```

改为：
```c
typedef struct devfs_device {
    char name[DEVFS_NAME_MAX];
    uint8_t type;       // VFS_CHRDEV or VFS_BLKDEV
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
    // Poll: check device readiness.  pt is poll_table for non-ready registration.
    // NULL means device is always ready (default for most devices).
    uint32_t (*poll)(void *priv, poll_table_t *pt);
    void *private_data;
    int registered;
} devfs_device_t;
```

在 devfs.c 顶部 `#include` 块添加:
```c
#include <kernel/poll.h>
```

- [ ] **Step 2: 修改 devfs_register_chrdev 签名和实现**

`devfs.h:13-15` 改为:
```c
// Register a character device that will appear under /dev/
// poll callback (nullable): if non-NULL, called by fd_poll(FD_DEV)
// to check device readiness.
int devfs_register_chrdev(const char *name, void *private_data,
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *),
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *),
    uint32_t (*poll)(void *priv, struct poll_table *pt));
```

`devfs.c:240-261` 中的函数签名和实现更新：
```c
int devfs_register_chrdev(const char *name, void *private_data,
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *),
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *),
    uint32_t (*poll)(void *priv, poll_table_t *pt))
{
    if (device_count >= DEVFS_MAX_DEVICES)
        return -1;

    int idx = device_count;
    size_t nlen = strlen(name);
    if (nlen >= DEVFS_NAME_MAX) nlen = DEVFS_NAME_MAX - 1;
    memcpy(devices[idx].name, name, nlen);
    devices[idx].name[nlen] = '\0';

    devices[idx].type = VFS_CHRDEV;
    devices[idx].read = read;
    devices[idx].write = write;
    devices[idx].poll = poll;        // ← new
    devices[idx].private_data = private_data;
    devices[idx].registered = 1;
    device_count++;

    debug_fs("devfs: registered '%s' (chrdev)\n", name);
    return 0;
}
```

- [ ] **Step 3: 验证编译 — 会失败（调用点尚未更新）**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -15
```

预期: 编译错误 — `devfs_register_chrdev` 调用参数不足。这是预期的，下一步修复。

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/devfs.c kernel/include/fs/devfs.h
git commit -m "feat(devfs): add poll callback to devfs_device_t and register API

devfs_device_t gains a poll function pointer.  devfs_register_chrdev
gains a poll parameter (nullable — NULL means always ready).

Compile break expected until call sites are updated in next task.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 15: 实现 devfs_poll() + 设备 poll 回调 + devfs_init 更新

**Files:**
- Modify: `kernel/fs/devfs.c`

**Interfaces:**
- Produces: `devfs_poll()`, `dev_tty_poll()`, `dev_keyboard_poll()`
- Consumes: `devfs_device_t.poll`, `tty_poll()`, `get_dev_tty()`

- [ ] **Step 1: 添加 dev_tty_poll() 和 dev_keyboard_poll()**

在 devfs.c 中 `dev_tty_write()` 之后 (约 line 67)、`serial_read()` 之前插入:

```c
// /dev/tty poll — delegates to tty_poll via global TTY singleton
static uint32_t dev_tty_poll(void *priv, poll_table_t *pt)
{
    (void)priv;
    tty_t *tty = get_dev_tty();
    if (!tty)
        return POLLERR;
    return tty_poll(tty, pt);
}

// /dev/keyboard poll — check scancode ring buffer (v1: no cascade wake)
// For v1, keyboard poll always returns readable (ring buffer state
// can't be queried from here cleanly).  TTY path is the primary poll target.
static uint32_t dev_keyboard_poll(void *priv, poll_table_t *pt)
{
    (void)priv;
    (void)pt;
    // Always return readable + writable
    return POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
}
```

- [ ] **Step 2: 添加 devfs_poll() — 供 fd_poll(FD_DEV) 调用的封装**

在 `devfs_write()` 之后 (约 line 175)、`devfs_readdir()` 之前插入:

```c
// ── devfs_poll — dispatch poll for fd_poll(FD_DEV) ─────────
// node->fs_data holds the device index.  Resolve it and call
// the device's poll callback, or return always-ready if none.
uint32_t devfs_poll(vfs_node_t *node, poll_table_t *pt)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return POLLNVAL;

    devfs_device_t *dev = &devices[idx];

    if (dev->poll)
        return dev->poll(dev->private_data, pt);

    // No poll callback: default to always ready
    return POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
}
```

- [ ] **Step 3: 更新 devfs_init() 中的 devfs_register_chrdev 调用**

当前 devfs.c:231-235:
```c
    devfs_register_chrdev("null",   NULL, null_read,   null_write);
    devfs_register_chrdev("zero",   NULL, zero_read,   null_write);
    devfs_register_chrdev("random", NULL, random_read, random_write);
    devfs_register_chrdev("serial", NULL, serial_read, serial_write);
    devfs_register_chrdev("tty",    NULL, dev_tty_read, dev_tty_write);
```

改为 (最后参数加 poll):
```c
    devfs_register_chrdev("null",   NULL, null_read,   null_write,   NULL);
    devfs_register_chrdev("zero",   NULL, zero_read,   null_write,   NULL);
    devfs_register_chrdev("random", NULL, random_read, random_write, NULL);
    devfs_register_chrdev("serial", NULL, serial_read, serial_write, NULL);
    devfs_register_chrdev("tty",    NULL, dev_tty_read, dev_tty_write, dev_tty_poll);
```

- [ ] **Step 4: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -10
```

预期: 编译通过！devfs_poll 现在已定义，poll.c 的 forward declaration 可以被解析。

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/devfs.c
git commit -m "feat(devfs): implement devfs_poll + tty/keyboard poll callbacks

devfs_poll resolves node->fs_data index to devfs_device_t and
dispatches to the device's poll callback.  dev_tty_poll delegates
to tty_poll via get_dev_tty().  dev_keyboard_poll returns
always-ready for v1.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 16: 更新 main.c 中的 devfs_register_chrdev 调用点

**Files:**
- Modify: `kernel/kernel/main.c`

- [ ] **Step 1: 更新 main.c 中的 devfs_register_chrdev 调用**

找到 main.c 中的 `devfs_register_chrdev` 调用行。当前约 line 172-173:
```c
    devfs_register_chrdev("keyboard", NULL, keyboard_devfs_read, NULL);
    devfs_register_chrdev("fb", NULL, NULL, fb_dev_write);
```

改为（每个调用增加第五个参数 NULL）:
```c
    devfs_register_chrdev("keyboard", NULL, keyboard_devfs_read, NULL, NULL);
    devfs_register_chrdev("fb", NULL, NULL, fb_dev_write, NULL);
```

- [ ] **Step 2: 验证编译**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -10
```

预期: 编译+链接完全通过！内核可以启动。

- [ ] **Step 3: 启动测试**

```bash
cd /home/aagu/OS01 && timeout 15 make qemu 2>&1 || true
```

预期: 内核正常启动到 shell。poll syscall 可用但尚未测试。

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel/main.c
git commit -m "fix: update devfs_register_chrdev calls in main.c for new poll API

All call sites now pass NULL for the new poll parameter.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 17: PIT timer 回调 — poll 超时支持

**Files:**
- Modify: `kernel/driver/pit.c:24-44` (pit_handler)
- Modify: `kernel/fs/poll.c` (使 current_poll_wq poll_deadline_jiffies 为 extern)

- [ ] **Step 1: 在 pit_handler 的 jiffies++ 之后插入 poll 超时检查**

文件: `kernel/driver/pit.c`，在 `jiffies++;` (line 26) 之后插入:

```c
    // Poll timeout check: if a poll syscall is active and its
    // deadline has passed, wake the polling task.
    extern wait_queue_t *current_poll_wq;
    extern uint64_t poll_deadline_jiffies;
    if (current_poll_wq && jiffies >= poll_deadline_jiffies) {
        wait_queue_wake_all(current_poll_wq);
        current_poll_wq = NULL;
    }
```

在 `pit.c` 顶部添加 include:
```c
#include <kernel/wait.h>
```

- [ ] **Step 2: 更新 poll.c 中的全局变量为 extern（去掉 static）**

`poll.c` 中已有（Task 10 定义）:
```c
wait_queue_t *current_poll_wq = NULL;
uint64_t poll_deadline_jiffies = 0;
```

这些变量已是非 static（Task 10 已修复），无需修改。

- [ ] **Step 3: 验证编译 + 提交**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -5
```

```bash
git add kernel/driver/pit.c kernel/fs/poll.c
git commit -m "feat(poll): add PIT timer callback for poll timeout

pit_handler checks current_poll_wq and wakes it when
poll_deadline_jiffies are reached, providing precise
ms-level poll timeout.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

**备选**: 如果 pit.c 的外键引用引起编译错误，跳过此 Task — poll 的超时通过 `do_poll` 循环中每次 `schedule()` 返回后检查 `jiffies >= deadline` 已可工作。

---

### Task 18: libc poll() 真实现

**Files:**
- Create: `libc/unistd/poll.c`
- Modify: `libc/include/poll.h` (已有 struct pollfd，确认无误)

- [ ] **Step 1: 创建 libc/unistd/poll.c**

```c
#include <poll.h>
#include <errno.h>
#include <sys/syscall.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    int64_t ret = syscall(SYS_poll,
                          (uint64_t)fds,
                          (uint64_t)nfds,
                          (uint64_t)(int64_t)timeout);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}
```

- [ ] **Step 2: 修改 libc/include/poll.h — 确保 POLL 值与内核一致**

当前 `libc/include/poll.h`:
```c
#ifndef _POLL_H
#define _POLL_H 1
#include <sys/cdefs.h>
#include <stddef.h>
typedef unsigned long nfds_t;
struct pollfd { int fd; short events; short revents; };
#define POLLIN  1
#define POLLOUT 2
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
#endif
```

必须修改 POLLIN/POLLOUT 值与内核 poll.h 一致（0x001/0x004），否则用户态 `events=POLLOUT` 传入内核后 `revents & POLLOUT` 永远为 0。覆盖为完整的 event flags 定义：

```c
#ifndef _POLL_H
#define _POLL_H 1
#include <sys/cdefs.h>
#include <stddef.h>
typedef unsigned long nfds_t;
struct pollfd { int fd; short events; short revents; };

// Poll event flags — MUST match kernel/include/kernel/poll.h
#define POLLIN      0x001
#define POLLPRI     0x002
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020
#define POLLRDNORM  0x040
#define POLLRDBAND  0x080
#define POLLWRNORM  0x100
#define POLLWRBAND  0x200

int poll(struct pollfd *fds, nfds_t nfds, int timeout);
#endif
```

- [ ] **Step 3: 验证用户态程序链接**

```bash
cd /home/aagu/OS01 && make lib 2>&1 | tail -5
```

预期: libc 编译通过，poll.o 被链接进 libc。

- [ ] **Step 4: Commit**

```bash
git add libc/unistd/poll.c
git commit -m "feat(libc): replace poll stub with real syscall wrapper

libc/unistd/poll.c calls SYS_poll via int $0x80.  Replaces the
fake poll() in busybox_stubs.c (removed in next task).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 19: 删除 busybox_stubs.c 中的假 poll 实现 + systest poll 用例

**Files:**
- Modify: `libc/unistd/busybox_stubs.c:42-80`
- Modify: `user/systest.c` (新增 test_poll 函数)

- [ ] **Step 1: 删除 busybox_stubs.c 中的假 poll**

删除 `busybox_stubs.c` lines 42-80:
```c
/* ── Poll ── */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    ...  (entire stale implementation)
}
```

- [ ] **Step 2: 在 systest.c 中添加 test_poll**

在 `user/systest.c` 中合适位置添加（可在 test_pipe_dup2_inherit 之后）:

```c
// ── 48: poll ───────────────────────────────────────────────
static void test_poll(void)
{
    // Test 1: poll on pipe — readability
    int fds[2];
    if (pipe(fds) < 0) {
        FAIL("poll", "pipe failed");
        return;
    }

    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    // Pipe should NOT be readable yet (nothing written)
    int ret = poll(&pfd, 1, 0);  // timeout=0 → non-blocking
    CHECK3(ret == 0, "poll", "empty pipe timeout=0 yields 0");

    // Write data to pipe
    write(fds[1], "x", 1);

    // Now pipe should be readable
    ret = poll(&pfd, 1, 0);
    CHECK3(ret == 1 && (pfd.revents & POLLIN), "poll", "pipe with data readable");

    // Consume the byte
    char c;
    read(fds[0], &c, 1);

    // Pipe empty again
    pfd.revents = 0;
    ret = poll(&pfd, 1, 0);
    CHECK3(ret == 0, "poll", "pipe empty again");

    close(fds[0]);
    close(fds[1]);

    // Test 2: poll with multiple fds
    int p1[2], p2[2];
    if (pipe(p1) < 0 || pipe(p2) < 0) {
        PASS("poll", "skipped (multi-pipe alloc failed)");
        return;
    }

    struct pollfd pfds[2];
    pfds[0].fd = p1[0]; pfds[0].events = POLLIN; pfds[0].revents = 0;
    pfds[1].fd = p2[0]; pfds[1].events = POLLIN; pfds[1].revents = 0;

    // Neither pipe has data
    ret = poll(pfds, 2, 0);
    CHECK3(ret == 0, "poll", "two empty pipes timeout=0");

    // Write to second pipe only
    write(p2[1], "y", 1);

    ret = poll(pfds, 2, 0);
    CHECKF(ret == 1, "poll", "multi: ret=%d", "multi: ret=%d", ret);
    CHECK3(pfds[0].revents == 0, "poll", "multi: pipe0 not ready");
    CHECK3(pfds[1].revents & POLLIN, "poll", "multi: pipe1 ready");

    // Cleanup
    read(p2[0], &c, 1);
    close(p1[0]); close(p1[1]);
    close(p2[0]); close(p2[1]);

    // Test 3: POLLHUP when writer closes
    int hfds[2];
    if (pipe(hfds) == 0) {
        write(hfds[1], "data", 4);
        close(hfds[1]);  // close writer

        struct pollfd hpfd;
        hpfd.fd = hfds[0]; hpfd.events = POLLIN; hpfd.revents = 0;

        ret = poll(&hpfd, 1, 0);
        // Should be readable (POLLIN) — data still in buffer
        CHECK3(ret == 1 && (hpfd.revents & POLLIN), "poll", "pipe data+closed writer → POLLIN");

        // Drain data
        char buf[8];
        read(hfds[0], buf, 4);

        // Now pipe is empty and writer is closed → POLLHUP
        hpfd.revents = 0;
        ret = poll(&hpfd, 1, 0);
        CHECK3(ret == 1 && (hpfd.revents & POLLHUP), "poll", "pipe empty+closed writer → POLLHUP");

        close(hfds[0]);
    }
}
```

- [ ] **Step 3: 在 systest.c 的 main 函数中添加调用**

在 `main()` 中已有 `test_pipe_dup2_inherit();` 之后添加：
```c
    test_poll();
```

- [ ] **Step 4: 编译并运行 systest**

```bash
cd /home/aagu/OS01 && make user 2>&1 | tail -5
```

启动 QEMU 并手动运行 `/systest.elf`。

- [ ] **Step 5: Commit**

```bash
git add libc/unistd/busybox_stubs.c user/systest.c
git commit -m "feat(poll): remove old poll stub, add systest poll cases

Tests cover: empty pipe timeout=0, pipe with data, multi-fd poll
with one pipe ready, POLLHUP when writer closes after drain.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 20: 端到端验证 — busybox ash

**Files:** (无代码修改，纯验证)

- [ ] **Step 1: 重新完整构建**

```bash
cd /home/aagu/OS01 && make clean && make all 2>&1 | tail -20
```

预期: 全量编译+链接通过。

- [ ] **Step 2: 启动 QEMU 并交互测试**

```bash
make qemu
```

在 QEMU 中:
1. 等待 boot 完成进入 ash shell
2. 验证键盘输入、回显正常
3. 运行 `/systest.elf` — 确认所有 poll 测试 PASS
4. 运行 `ls /dev` — 确认 `/dev/tty` 仍正常
5. 运行 `echo hello | cat` — pipe 功能正常
6. Ctrl-C 中断 — 信号仍正常

- [ ] **Step 3: Commit (如有微调)**

```bash
git add -A
git commit -m "chore: final integration validation for poll syscall

All systest poll cases pass.  busybox ash + pipe + Ctrl-C verified.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 21 (可选): select() 包装实现

**Files:**
- Modify: `kernel/fs/poll.c`
- Modify: `kernel/arch/x86_64/trap.c` (替换 SYS_select stub)

**Interfaces:**
- Produces: `do_select()` — converts fd_set to pollfd[], calls do_poll(), converts back

- [ ] **Step 1: 实现 do_select**

在 poll.c 底部添加:
```c
// ── do_select — select syscall adaptor on top of do_poll ──
//
// Linux ABI: int select(int nfds, fd_set *readfds, fd_set *writefds,
//                        fd_set *exceptfds, struct timeval *timeout)

// fd_set helpers (OS01 libc uses an array of longs)
typedef unsigned long fd_mask_t;
#define FD_MASK_BITS  (sizeof(fd_mask_t) * 8)
#define FD_SET_COUNT  ((FD_SETSIZE + FD_MASK_BITS - 1) / FD_MASK_BITS)

static inline int fd_isset(int fd, fd_mask_t *set)
{
    return (set[fd / FD_MASK_BITS] >> (fd % FD_MASK_BITS)) & 1;
}

static inline void fd_set_bit(int fd, fd_mask_t *set)
{
    set[fd / FD_MASK_BITS] |= ((fd_mask_t)1 << (fd % FD_MASK_BITS));
}

int64_t do_select(int nfds, fd_mask_t *readfds, fd_mask_t *writefds,
                  fd_mask_t *exceptfds, struct timeval *timeout)
{
    if (nfds < 0 || nfds > FD_SETSIZE)
        return -EINVAL;

    // fd_set is an array of unsigned long; FD_SETSIZE = 1024 per POSIX
    // We cap at POLL_MAX_FDS (16) to keep pollfd[] on the kernel stack small
    struct pollfd pfds[POLL_MAX_FDS];
    int count = 0;

    for (int fd = 0; fd < nfds; fd++) {
        short events = 0;
        if (readfds   && fd_isset(fd, readfds))   events |= POLLIN;
        if (writefds  && fd_isset(fd, writefds))  events |= POLLOUT;
        if (exceptfds && fd_isset(fd, exceptfds)) events |= POLLPRI;
        if (events && count < POLL_MAX_FDS) {
            pfds[count].fd      = fd;
            pfds[count].events  = events;
            pfds[count].revents = 0;
            count++;
        }
    }

    int timeout_ms = -1;
    if (timeout) {
        timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    }

    int ready = (int)do_poll(pfds, (uint64_t)count, timeout_ms);
    if (ready < 0) return ready;

    // Write back fd_sets
    int total = 0;
    for (int i = 0; i < count; i++) {
        int fd = pfds[i].fd;
        if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (readfds) { fd_set_bit(fd, readfds); total++; }
        }
        if (pfds[i].revents & (POLLOUT | POLLERR)) {
            if (writefds) { fd_set_bit(fd, writefds); total++; }
        }
        if (pfds[i].revents & POLLPRI) {
            if (exceptfds) { fd_set_bit(fd, exceptfds); total++; }
        }
    }
    return total;
}
```

- [ ] **Step 2: 在 trap.c 中替换 SYS_select stub**

将 `case SYS_select: regs->rax = -ENOSYS; break;` 替换为:
```c
    case SYS_select: {
        int nfds = (int)regs->rdi;
        regs->rax = do_select(nfds,
                              (fd_mask_t *)regs->rsi,
                              (fd_mask_t *)regs->rdx,
                              (fd_mask_t *)regs->r10,
                              (struct timeval *)regs->r8);
        break;
    }
```

- [ ] **Step 3: 验证编译 + 提交**

```bash
cd /home/aagu/OS01 && make kernel/kernel.bin 2>&1 | tail -5
git add kernel/fs/poll.c kernel/arch/x86_64/trap.c
git commit -m "feat(select): implement select() on top of do_poll

Converts fd_set bitmaps to pollfd array, calls do_poll, converts
revents back to fd_set.  Uses standard fd_set helpers.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## 任务依赖图

```
Task 1 ──→ Task 2 ──→ Task 7 ──→ Task 3 ──→ Task 4 ──→ Task 5 ──→ Task 6
  │                                          │          │          │
  │                                          └──────────┴──────────┘
  │                                               (all need poll.h)
  │
  └──→ Task 8 ──→ Task 9 ──→ Task 10 ──→ Task 11
                   │
                   ├──→ Task 12 ──→ Task 13
                   │
                   └──→ Task 14 ──→ Task 15 ──→ Task 16
                                                   │
                                Task 10 ──────────┼──→ Task 17 (optional)
                                                   │
                                                   └──→ Task 18 ──→ Task 19
                                                                       │
                                                                       └──→ Task 20
                                                                             │
                                                                             └──→ Task 21 (optional)
```

**关键里程碑**:
- Task 7 完成后: poll.h 就绪，pipe 相关代码可编译
- Task 6 完成后: pipe 阻塞语义全部正确（可独立测试）
- Task 13 完成后: TTY poll 可工作
- Task 16 完成后: 内核编译链接通过，poll syscall 可用
- Task 19 完成后: systest poll 用例全部通过
