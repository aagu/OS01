#include <kernel/file.h>
#include <fs/vfs.h>
#include <kernel/debug.h>
#include <kernel/task.h>
#include <kernel/arch/irq.h>
#include <kernel/slab.h>
#include <kernel.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <kernel/poll.h>
#include <kernel/pty.h>
#include <fs/devfs.h>
#include <uapi/stat.h>
#include <kernel/uaccess.h>

// ── Forward declarations ─────────────────────────────────────
void pipe_wake_readers(pipe_t *p);
void pipe_wake_writers(pipe_t *p);

// ── PTY allocation lock ─────────────────────────────────────
spinlock_T pty_lock = { 1 };

#include "lwip/err.h"     // err_t, ERR_OK, ERR_CLSD
#include "lwip/api.h"     // netconn_recv, netconn_write, netconn_delete, netbuf

// ── Allocate a file_t ──────────────────────────────────────
file_t *file_alloc(void)
{
    file_t *f = (file_t *)calloc(1, sizeof(file_t));
    if (!f) return NULL;
    f->refcount = 1;
    return f;
}

// ── Release a file_t ────────────────────────────────────────
void file_free(file_t *f)
{
    if (!f) return;

    // Pipe cleanup: when the last reference to a pipe-end
    // file descriptor goes away, decrement reader/writer count
    // and wake the other end if needed.  Defer the wake calls
    // to after p->lock is released — avoids lock-order inversion
    // with fd_read/fd_write's wq->lock → p->lock path.
    if (f->type == FD_PIPE && f->pipe) {
        pipe_t *p = f->pipe;
        int need_wake_r = 0, need_wake_w = 0;

        {
            uint64_t flags = spin_lock_irqsave(&p->lock);
            if (f->flags == O_RDONLY) {
                p->readers--;
                if (p->readers == 0) need_wake_w = 1;
            } else {
                p->writers--;
                if (p->writers == 0) need_wake_r = 1;
            }
            int last = (p->readers == 0 && p->writers == 0);
            spin_unlock_irqrestore(&p->lock, flags);

            f->pipe = NULL;  // break link to avoid UAF from the other end
            if (need_wake_r) pipe_wake_readers(p);
            if (need_wake_w) pipe_wake_writers(p);
            if (last)         pipe_free(p);
        }
    }

    // ── PTY cleanup ──────────────────────────────────────────────
    if ((f->type == FD_PTY_MASTER || f->type == FD_PTY_SLAVE) && f->pty) {
        pty_t *pty = f->pty;
        // Hold pty_lock to prevent concurrent pty_alloc reusing this slot (SMP)
        uint64_t pty_flags = spin_lock_irqsave(&pty_lock);
        int need_rd_a = 0, need_wr_a = 0, need_rd_b = 0, need_wr_b = 0;

        if (f->type == FD_PTY_MASTER) {
            // master reads slave_to_master, writes master_to_slave
            if (pty->slave_to_master) {
                uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
                pty->slave_to_master->readers--;           // master stopped reading
                need_wr_b = (pty->slave_to_master->readers == 0);
                spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
            }
            if (pty->master_to_slave) {
                uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
                pty->master_to_slave->writers--;            // master stopped writing
                need_rd_a = (pty->master_to_slave->writers == 0);
                spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
            }
        } else { // FD_PTY_SLAVE
            // slave reads master_to_slave, writes slave_to_master
            if (pty->master_to_slave) {
                uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
                pty->master_to_slave->readers--;            // slave stopped reading
                need_wr_a = (pty->master_to_slave->readers == 0);
                spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
            }
            if (pty->slave_to_master) {
                uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
                pty->slave_to_master->writers--;            // slave stopped writing
                need_rd_b = (pty->slave_to_master->writers == 0);
                spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
            }
            pty->pgrp = 0;  // reset on slave close
        }

        // Wake blocked sides
        if (need_rd_a) pipe_wake_readers(pty->master_to_slave);
        if (need_wr_a) pipe_wake_writers(pty->master_to_slave);
        if (need_rd_b) pipe_wake_readers(pty->slave_to_master);
        if (need_wr_b) pipe_wake_writers(pty->slave_to_master);

        // Check if both pipes are fully closed -> free them + reclaim slot
        int m2s_done = (!pty->master_to_slave->readers && !pty->master_to_slave->writers);
        int s2m_done = (!pty->slave_to_master->readers && !pty->slave_to_master->writers);
        if (m2s_done && s2m_done) {
            pipe_free(pty->master_to_slave);
            pipe_free(pty->slave_to_master);
            pty->master_to_slave = NULL;
            pty->slave_to_master = NULL;
            pty->allocated = false;  // slot reusable
        }

        spin_unlock_irqrestore(&pty_lock, pty_flags);
        f->pty = NULL;
    }

    if (f->node) {
        vfs_node_put(f->node);
        f->node = NULL;
    }
    if (f->type == FD_SOCKET && f->sock) {
        if (f->sock->conn)
            netconn_delete((struct netconn *)f->sock->conn);
        free(f->sock);
    }
    // Poison to catch use-after-free
    f->pipe = NULL;
    f->pty  = NULL;
    f->sock = NULL;
    free(f);
}

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

// ── Allocate a pipe ─────────────────────────────────────────
pipe_t *pipe_alloc(void)
{
    pipe_t *p = (pipe_t *)calloc(1, sizeof(pipe_t));
    if (!p) return NULL;
    p->buf = (char *)kmalloc(PIPE_SIZE);
    if (!p->buf) { free(p); return NULL; }
    p->readers = 1;
    p->writers = 1;
    spin_init(&p->lock);
    wait_queue_init(&p->read_wait);
    wait_queue_init(&p->write_wait);
    list_init(&p->read_poll);
    list_init(&p->write_poll);
    // Task 8: read_busy reservation fields.  calloc zeros memory so
    // read_busy==0 == "available" already.  Spinlocks/wait_queues must
    // still be initialized explicitly (calloc gives lock=0 == "locked").
    spin_init(&p->read_busy_lock);
    wait_queue_init(&p->read_busy_wq);
    return p;
}

// ── Release a pipe ──────────────────────────────────────────
void pipe_free(pipe_t *p)
{
    if (!p) return;
    kfree(p->buf);
    p->buf = NULL;
    free(p);
}

// ── Allocate a files_struct ─────────────────────────────────
files_t *files_alloc(void)
{
    files_t *fs = (files_t *)calloc(1, sizeof(files_t));
    if (!fs) return NULL;
    spin_init(&fs->lock);
    fs->refcount = 1;            // caller holds the initial reference
    // Default cwd is root
    fs->cwd = strdup("/");
    if (!fs->cwd) { free(fs); return NULL; }
    return fs;
}

// ── Release entire fd table ─────────────────────────────────
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

// ── Deep-copy fd table (for fork) ───────────────────────────
// Each file->refcount is incremented so the child shares the
// same open file descriptions with the parent.
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

// ── Find lowest free fd slot ────────────────────────────────
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

// ── Close a single fd ───────────────────────────────────────
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

// ── files_t reference protocol ─────────────────────────────
// NULL is a no-op (defensive; mirrors file_get/file_put).
void files_pin(files_t *fs)
{
    if (!fs) return;
    __sync_add_and_fetch(&fs->refcount, 1);
}

// MUST NOT be called under task_list_lock/fs->lock/rq lock: drop-to-zero
// → synchronous files_free, which may block (netconn_delete → lwIP) and
// take file/pipe/pty locks.
void files_unpin(files_t *fs)
{
    if (!fs) return;
    if (__sync_sub_and_fetch(&fs->refcount, 1) == 0)
        files_free(fs);
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

// ── Pipe helpers ──────────────────────────────────────────

static inline int pipe_empty(pipe_t *p)
{
    return p->head == p->tail;
}

static inline int pipe_full(pipe_t *p)
{
    return ((p->head + 1) % PIPE_SIZE) == p->tail;
}

// ── Pipe wake helpers ──────────────────────────────────────
// Caller must hold p->lock.  Wake one direct waiter + all poll
// entries.  Direct waiters use wait_queue_t (task_t.io_wait_node).
// Poll entries use a plain list_t (poll_wait_entry_t.node).
// Each poll entry cascades to wait_queue_wake_all(entry->poll_wq).

void pipe_wake_readers(pipe_t *p)
{
    wait_queue_wake_one(&p->read_wait);
    while (!list_is_empty(&p->read_poll)) {
        list_t *node = p->read_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
}

void pipe_wake_writers(pipe_t *p)
{
    wait_queue_wake_one(&p->write_wait);

    while (!list_is_empty(&p->write_poll)) {
        list_t *node = p->write_poll.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
}

// ── Pipe read internal (blocking, exported for PTY) ─────────
//
// Task 8 (Cat C): three-phase + read_busy reservation + copy_to_user_ft_res.
//
// Concurrency contract:
//   The pipe is a 1-producer / 1-consumer ring buffer.  fork()/dup()
//   can share the read end across two tasks, so the "single reader"
//   guarantee is NOT structural.  We enforce it KERNEL-side: the
//   peek→copy→commit window is serialized by p->read_busy (Task 8a).
//
// Three phases:
//   (1) reserve   — acquire read_busy (blocks if another reader holds
//                   it; second reader blocks instead of racing tail)
//   (2) wait data — block on read_wait until pipe has data or writers
//                   reach 0; reservation held throughout
//   (3) peek      — under p->lock, copy ring→kernel bounce WITHOUT
//                   advancing tail (no consume; concurrent peek would
//                   corrupt tail)
//   (4) _ft_res   — copy_to_user_ft_res with on_fault callback that
//                   releases read_busy (NO p->lock held here, so fault
//                   safe; on fault tail is unchanged → data preserved)
//   (5) commit    — under p->lock, advance tail + wake writers
//   (6) release   — every exit path (EOF / -EINTR / -EFAULT / success)
//                   routes through out_release; the `released` flag
//                   guards against double-release when the fault cb
//                   already ran (pipe_read_release is itself
//                   idempotent — see below — but the flag is the
//                   contract that makes it auditable)
//
// pipe_read_release is lock-protected IDEMPOTENT: only clears+wakes
// when read_busy==1.  This is the by-construction safety against
// double-release (fault cb + out_release), not a comment.

// Bytes available in pipe (head != tail mod PIPE_SIZE).  Used by the
// peek phase to know how much to copy into the kernel bounce.
static inline size_t pipe_avail(pipe_t *p)
{
    return (p->head - p->tail + PIPE_SIZE) % PIPE_SIZE;
}

// Block until read_busy clears, then atomically set it.
// wait_queue_sleep uses current->io_wait_node; that node is FREE for
// re-use here because pipe_read_internal hasn't enqueued itself on
// read_wait yet (reservation is acquired FIRST).  The reserve loop
// wakes when pipe_read_release wakes read_busy_wq.
static void pipe_read_reserve(pipe_t *p)
{
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&p->read_busy_lock);
        if (!p->read_busy) {
            p->read_busy = 1;
            spin_unlock_irqrestore(&p->read_busy_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&p->read_busy_lock, flags);
        wait_queue_sleep(&p->read_busy_wq);
        // loop and re-check read_busy
    }
}

// Lock-protected idempotent release.  Safe to call from both the
// fault-cleanup path (longjmp from copy_to_user_ft_res) and the
// normal out_release path — only clears+wakes when read_busy==1.
// The `released` flag in pipe_read_rsv is the AUDITABLE contract on
// top of this; we still keep idempotency because it makes the
// callback safe even if someone adds a future release site.
static void pipe_read_release(pipe_t *p)
{
    uint64_t flags = spin_lock_irqsave(&p->read_busy_lock);
    if (p->read_busy) {
        p->read_busy = 0;
        spin_unlock_irqrestore(&p->read_busy_lock, flags);
        wait_queue_wake_all(&p->read_busy_wq);   // wake outside the lock
    } else {
        spin_unlock_irqrestore(&p->read_busy_lock, flags);
    }
}

// Forward declared here so pipe_read_release_cb can use it.  Full
// struct is defined inside pipe_read_internal below.
struct pipe_read_rsv {
    pipe_t *p;
    int    *released;
};

// on_fault callback for copy_to_user_ft_res — matches the
// void (*)(void *) signature the primitive requires.
static void pipe_read_release_cb(void *arg)
{
    struct pipe_read_rsv *r = (struct pipe_read_rsv *)arg;
    *r->released = 1;                  // out_release must NOT re-release
    pipe_read_release(r->p);           // idempotent: only clears when set
}

int64_t pipe_read_internal(pipe_t *p, void *buf, uint64_t size)
{
    if (!p) return -1;
    if (!buf || size == 0) return -1;

    int released = 0;
    struct pipe_read_rsv rsv = { p, &released };

    pipe_read_reserve(p);

    int64_t result = 0;

    for (;;) {
        // ── Phase 2: wait for data ──────────────────────────
        // (Equivalent to the old block-for-data loop, but with the
        // read_busy reservation held so a concurrent reader cannot
        // race tail during peek→commit.)
        uint64_t flags = spin_lock_irqsave(&p->lock);

        if (!pipe_empty(p)) {
            spin_unlock_irqrestore(&p->lock, flags);
            break;
        }

        // Pipe empty — check if any writer still exists
        if (p->writers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            result = 0;                       // EOF
            goto out_release;
        }

        spin_unlock_irqrestore(&p->lock, flags);

        // Double-check after queue registration to close lost-wakeup
        {
            wait_queue_t *wq = &p->read_wait;
            int do_eof = 0;

            uint64_t wq_flags = spin_lock_irqsave(&wq->lock);
            list_add_to_before(&wq->head, &current->io_wait_node);

            {
                uint64_t p2_flags = spin_lock_irqsave(&p->lock);
                if (p->writers == 0 && pipe_empty(p)) {
                    list_del_init(&current->io_wait_node);
                    do_eof = 1;
                } else if (p->writers == 0 && !pipe_empty(p)) {
                    // Last writer gone but data still in buffer —
                    // don't sleep, go back and read it
                    list_del_init(&current->io_wait_node);
                } else if (!pipe_empty(p)) {
                    // Race: writer arrived between the outer check
                    // and queue registration — don't sleep, retry.
                    list_del_init(&current->io_wait_node);
                }
                spin_unlock_irqrestore(&p->lock, p2_flags);
            }

            if (do_eof) {
                spin_unlock_irqrestore(&wq->lock, wq_flags);
                result = 0;
                goto out_release;
            }

            current->state = TASK_INTERRUPTIBLE;
            int was_queued = !list_is_empty(&current->io_wait_node);
            spin_unlock_irqrestore(&wq->lock, wq_flags);

            if (!was_queued) {
                current->state = TASK_RUNNING;
            } else {
                schedule();
                arch_local_irq_enable();
                if (!list_is_empty(&current->io_wait_node))
                    list_del_init(&current->io_wait_node);
                current->state = TASK_RUNNING;
            }
        }

        if (arch_signal_pending_fatal()) {
            result = -EINTR;
            goto out_release;
        }
    }

    // ── Phase 3: peek — copy ring→bounce WITHOUT advancing tail ──
    uint8_t bounce[PIPE_SIZE];
    size_t avail;
    {
        uint64_t flags = spin_lock_irqsave(&p->lock);
        avail = pipe_avail(p);
        if (avail > size) avail = size;
        // Linear copy if tail+avail <= PIPE_SIZE, else two halves
        if (p->tail + avail <= PIPE_SIZE) {
            memcpy(bounce, p->buf + p->tail, avail);
        } else {
            size_t first = PIPE_SIZE - p->tail;
            memcpy(bounce, p->buf + p->tail, first);
            memcpy(bounce + first, p->buf, avail - first);
        }
        spin_unlock_irqrestore(&p->lock, flags);
    }

    // ── Phase 4: _ft_res copy bounce→user with fault cleanup ──
    // NO pipe lock held here.  on_fault releases read_busy (tail
    // unchanged → no data lost).  If _ft succeeds, we commit.
    ssize_t rc = copy_to_user_ft_res(buf, bounce, avail,
                                     pipe_read_release_cb, &rsv);
    if (rc < 0) {
        // Fault cb already set *released=1 AND called pipe_read_release
        // (idempotent: cleared read_busy if set).  Do NOT release again.
        result = -EFAULT;
        goto out_release;
    }

    // ── Phase 5: commit — advance tail + wake writers ────────
    {
        uint64_t flags = spin_lock_irqsave(&p->lock);
        p->tail = (p->tail + avail) % PIPE_SIZE;
        pipe_wake_writers(p);
        spin_unlock_irqrestore(&p->lock, flags);
    }
    result = (int64_t)avail;

out_release:
    if (!released) {
        pipe_read_release(p);
    }
    return result;
}

// ── Read through a file descriptor ──────────────────────────
//
// Task 8 (Cat C): all read paths route user-space destinations through
// the kernel bounce (UACCESS_BOUNCE_SIZE) so the FS / lwIP callback
// never touches a user pointer.  Offset advances ONLY after a
// successful copy_to_user_ft; first-chunk fault returns -EFAULT,
// later-chunk fault returns the short count already committed.
//
//   FD_VFS / FD_DEV   — kmalloc(BOUNCE), vfs_read into kbuf, _ft to
//                       user; loop until vfs_read returns 0 or all
//                       bytes consumed
//   FD_PIPE / PTY     — pipe_read_internal (Task 8d three-phase +
//                       read_busy + _ft_res)
//   FD_SOCKET         — drain rx_nb cache OR netconn_recv into
//                       kbuf, _ft to user, advance rx_off only on
//                       success (per brief: "rx_off/netbuf 原样保留
//                       可重试" on _ft fault)
//
// Returns bytes read, 0 for EOF (pipe with no writers), or negative
// on error.
int64_t fd_read(file_t *f, void *buf, uint64_t size)
{
    if (!f || !buf || size == 0) return -1;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        if (!f->node || !f->node->ops ||
            (uint64_t)f->node->ops < 0xffff800000000000ULL ||
            !f->node->ops->read)
            return -1;
        // Check access mode (low 2 bits) — ignore O_CREAT etc.
        int acc = f->flags & 3;
        if (!(acc == O_RDONLY || acc == O_RDWR))
            return -1;

        // Task 8: chunk bounce through kernel buffer.  vfs_read
        // never sees a user pointer (writes into kbuf); _ft copies
        // to user; offset advances ONLY after a successful _ft copy.
        // submitted==0 → -EFAULT (block never made it past user).
        void *kbuf = kmalloc(UACCESS_BOUNCE_SIZE);
        if (!kbuf) return -1;

        uint64_t committed = 0;
        for (;;) {
            uint64_t remaining = size - committed;
            if (remaining == 0) break;
            uint64_t chunk = remaining < UACCESS_BOUNCE_SIZE
                             ? remaining : UACCESS_BOUNCE_SIZE;

            int64_t n = vfs_read(f->node, f->offset, chunk, kbuf);
            if (n <= 0) break;            // 0 = EOF, <0 = error

            ssize_t rc = copy_to_user_ft(
                (uint8_t *)buf + committed, kbuf, (size_t)n);
            if (rc < 0) {
                // User buffer fault: stop here.  If we already
                // committed bytes, return the short count; else -EFAULT.
                if (committed == 0) {
                    kfree(kbuf);
                    return -EFAULT;
                }
                break;
            }
            f->offset += (uint64_t)n;
            committed += (uint64_t)n;
            if ((uint64_t)n < chunk) break;   // short read (EOF)
        }
        kfree(kbuf);
        if (committed == 0) return -1;        // vfs_read error
        return (int64_t)committed;
    }
    case FD_PIPE:
        return pipe_read_internal(f->pipe, buf, size);
    case FD_PTY_MASTER:
        if (!f->pty || !f->pty->slave_to_master) return -1;
        return pipe_read_internal(f->pty->slave_to_master, buf, size);
    case FD_PTY_SLAVE:
        if (!f->pty || !f->pty->master_to_slave) return -1;
        return pipe_read_internal(f->pty->master_to_slave, buf, size);
    case FD_SOCKET: {
        socket_t *s = f->sock;
        if (!s || !s->conn) return -1;
        if (arch_signal_pending_fatal()) return -EINTR;

        // Drain a partially-consumed netbuf first (a 1-byte fgets
        // read must not lose the rest of the 370-byte response).
        if (s->rx_nb) {
            struct netbuf *nb = (struct netbuf *)s->rx_nb;
            void *data; u16_t data_len;
            netbuf_data(nb, &data, &data_len);
            u16_t avail = (data_len > (u16_t)s->rx_off)
                          ? (u16_t)(data_len - s->rx_off) : 0;
            size_t copy = (avail < size) ? avail : size;
            if (copy > 0) {
                // Task 8: bounce through kernel, _ft to user, advance
                // rx_off only on _ft success.
                ssize_t rc = copy_to_user_ft(
                    buf, (uint8_t *)data + s->rx_off, copy);
                if (rc < 0) return -EFAULT;
                s->rx_off += (int)copy;
                if (s->rx_off >= data_len) {
                    s->rx_off = 0;
                    if (netbuf_next(nb) < 0) {
                        netbuf_delete(nb);
                        s->rx_nb = NULL;
                    }
                }
            }
            if (copy > 0) return (int64_t)copy;
            return -EAGAIN;
        }
        struct netbuf *nb;
        err_t err = netconn_recv((struct netconn *)s->conn, &nb);
        if (err == ERR_OK) {
            void *data; u16_t data_len;
            netbuf_data(nb, &data, &data_len);
            size_t copy = (data_len < size) ? data_len : size;
            if (copy > 0) {
                // Task 8: same _ft-first commit-later discipline.
                ssize_t rc = copy_to_user_ft(buf, data, copy);
                if (rc < 0) {
                    // Drop nb (one-shot netbuf, can't safely cache
                    // here without state inflation).  POSIX recv on
                    // bad user ptr is a documented loss.
                    netbuf_delete(nb);
                    return -EFAULT;
                }
            }
            if (copy < data_len) {
                // Keep the rest for the next read.
                s->rx_nb = nb;
                s->rx_off = (int)copy;
            } else {
                if (netbuf_next(nb) >= 0) {
                    s->rx_nb = nb;
                    s->rx_off = 0;
                } else {
                    netbuf_delete(nb);
                }
            }
            if (copy > 0) return (int64_t)copy;
            return -EAGAIN;
        }
        if (arch_signal_pending_fatal()) return -EINTR;
        if (err == ERR_CLSD) return 0;
        if (err == ERR_WOULDBLOCK) return -EAGAIN;
        return -EIO;
    }
    default:
        return -1;
    }
}

// ── Pipe write internal (blocking, exported for PTY) ────────
//
// Task 8 (Cat C): the user source is staged into a kernel bounce
// buffer BEFORE we ever touch p->lock.  The write side is naturally
// copy-then-consume: once a byte is in the ring we never have to
// re-fetch it from user memory, so a fault on the user side just
// truncates the chunk and returns a short count — no data loss, no
// memcpy under lock.
//
// Pipe is small (PIPE_SIZE = 512B), so we can stack-allocate the
// bounce and stage at most one pipe-buffer's worth per call to the
// ring (the rest stays in the user pointer loop and is staged on
// retry).  This keeps the lock-held section minimal.
int64_t pipe_write_internal(pipe_t *p, const void *buf, uint64_t size)
{
    if (!p) return -1;
    if (!buf || size == 0) return -1;

    uint64_t total = 0;

    for (;;) {
        // ── Stage: copy user→kernel bounce BEFORE taking p->lock ──
        // We copy at most one pipe buffer's worth per iteration
        // (the rest stays in the user pointer for the next loop).
        // copy_from_user_ft is fault-safe — longjmp on bad user ptr
        // returns -EFAULT without any state corruption here.
        uint64_t remaining = size - total;
        uint64_t want = remaining < PIPE_SIZE ? remaining : PIPE_SIZE;

        uint8_t bounce[PIPE_SIZE];
        ssize_t rc = copy_from_user_ft(bounce,
                                       (const uint8_t *)buf + total,
                                       (size_t)want);
        if (rc < 0) {
            // Fault on the user source.  Whatever we already wrote
            // in earlier iterations stays in the pipe; return the
            // short count (or -EFAULT if nothing was written yet).
            if (total == 0) return -EFAULT;
            return (int64_t)total;
        }
        uint64_t staged = want;

        // ── Locked section: drain bounce into ring ─────────────
        uint64_t written = 0;
        uint64_t flags = spin_lock_irqsave(&p->lock);

        while (written < staged && !pipe_full(p)) {
            p->buf[p->head] = bounce[written++];
            p->head = (p->head + 1) % PIPE_SIZE;
        }

        total += written;

        if (written > 0) {
            // Wrote some data — wake blocked readers, return or loop
            pipe_wake_readers(p);
            spin_unlock_irqrestore(&p->lock, flags);
            if (total == size) return (int64_t)total;   // all consumed
            continue;
        }

        // Pipe is full (written == 0).  Check if any reader still exists.
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -EPIPE;
        }

        spin_unlock_irqrestore(&p->lock, flags);

        // Double-check after queue registration to
        // close the lost-wakeup race (same pattern as fd_read).
        {
            wait_queue_t *wq = &p->write_wait;
            int do_epipe = 0;

            uint64_t wq_flags = spin_lock_irqsave(&wq->lock);
            list_add_to_before(&wq->head, &current->io_wait_node);

            {
                uint64_t p2_flags = spin_lock_irqsave(&p->lock);
                if (p->readers == 0) {
                    list_del_init(&current->io_wait_node);
                    do_epipe = 1;
                } else if (!pipe_full(p)) {
                    // Reader consumed data between the outer
                    // pipe_full check and queue registration —
                    // don't sleep, loop back and write
                    list_del_init(&current->io_wait_node);
                }
                spin_unlock_irqrestore(&p->lock, p2_flags);
            }

            if (do_epipe) {
                spin_unlock_irqrestore(&wq->lock, wq_flags);
                return -EPIPE;
            }

            current->state = TASK_INTERRUPTIBLE;
            int was_queued = !list_is_empty(&current->io_wait_node);
            spin_unlock_irqrestore(&wq->lock, wq_flags);

            if (!was_queued) {
                current->state = TASK_RUNNING;
            } else {
                schedule();
                arch_local_irq_enable();
                if (!list_is_empty(&current->io_wait_node))
                    list_del_init(&current->io_wait_node);
                current->state = TASK_RUNNING;
            }
        }

        if (arch_signal_pending_fatal())
            return -EINTR;
    }
}

// ── Write through a file descriptor ─────────────────────────
//
// Task 8 (Cat C): all write paths read user data into a kernel
// bounce (UACCESS_BOUNCE_SIZE) before handing it to the FS / lwIP
// callback.  The user pointer never reaches a callback that might
// dereference it.  For socket TX, lwIP never touches user memory
// at all — we hand it pure kernel memory via netconn_write_partly.
//
//   FD_VFS / FD_DEV   — kmalloc(BOUNCE), copy_from_user_ft → kbuf,
//                       vfs_write, loop until all bytes consumed
//   FD_PIPE / PTY     — pipe_write_internal (Task 8e staged _ft)
//   FD_SOCKET         — kmalloc(16KB), copy_from_user_ft → kbuf,
//                       netconn_write_partly (Task 8g chunked bounce)
//
// Returns bytes written or negative on error.
int64_t fd_write(file_t *f, const void *buf, uint64_t size)
{
    if (!f || !buf || size == 0) return -1;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        if (!f->node || !f->node->ops ||
            (uint64_t)f->node->ops < 0xffff800000000000ULL ||
            !f->node->ops->write)
            return -1;
        // Check access mode (low 2 bits) — ignore O_CREAT etc.
        int acc = f->flags & 3;
        if (!(acc == O_WRONLY || acc == O_RDWR))
            return -1;

        // Task 8: chunk bounce through kernel buffer.  We _ft-copy
        // a chunk into kbuf, then vfs_write it; offset advances
        // after the _ft succeeds (so user-fault on chunk N leaves
        // chunks 0..N-1 committed).  submitted==0 → -EFAULT.
        void *kbuf = kmalloc(UACCESS_BOUNCE_SIZE);
        if (!kbuf) return -1;

        uint64_t committed = 0;
        for (;;) {
            uint64_t remaining = size - committed;
            if (remaining == 0) break;
            uint64_t chunk = remaining < UACCESS_BOUNCE_SIZE
                             ? remaining : UACCESS_BOUNCE_SIZE;

            ssize_t rc = copy_from_user_ft(
                kbuf, (const uint8_t *)buf + committed, (size_t)chunk);
            if (rc < 0) {
                if (committed == 0) {
                    kfree(kbuf);
                    return -EFAULT;
                }
                break;          // short count
            }

            int64_t n = vfs_write(f->node, f->offset, chunk, kbuf);
            if (n <= 0) {
                if (committed == 0) {
                    kfree(kbuf);
                    return (n < 0) ? n : -1;   // propagate error
                }
                break;
            }
            f->offset += (uint64_t)n;
            committed += (uint64_t)n;
            if ((uint64_t)n < chunk) break;    // short write
        }
        kfree(kbuf);
        if (committed == 0) return -1;
        return (int64_t)committed;
    }
    case FD_PIPE:
        return pipe_write_internal(f->pipe, buf, size);
    case FD_PTY_MASTER:
        if (!f->pty || !f->pty->master_to_slave) return -1;
        return pipe_write_internal(f->pty->master_to_slave, buf, size);
    case FD_PTY_SLAVE:
        if (!f->pty || !f->pty->slave_to_master) return -1;
        return pipe_write_internal(f->pty->slave_to_master, buf, size);
    case FD_SOCKET: {
        socket_t *s = f->sock;
        if (!s || !s->conn) return -EIO;
        if (arch_signal_pending_fatal()) return -EINTR;

        // Task 8g: chunked bounce.  lwIP never touches user memory.
        // 16 KiB chunk keeps TCP segmentation efficient without
        // ballooning kernel memory.  netconn_write_partly blocks
        // internally on its own mbox; we loop until the user
        // buffer is exhausted or a fatal error occurs.
        uint64_t committed = 0;
        while (committed < size) {
            uint64_t remaining = size - committed;
            uint64_t chunk = remaining < (16 * 1024)
                             ? remaining : (16 * 1024);
            uint8_t kbuf[16 * 1024];

            ssize_t rc = copy_from_user_ft(
                kbuf, (const uint8_t *)buf + committed, (size_t)chunk);
            if (rc < 0) {
                if (committed == 0) return -EFAULT;
                return (int64_t)committed;     // short count
            }

            err_t err = netconn_write_partly(
                (struct netconn *)s->conn, kbuf, (size_t)chunk,
                NETCONN_COPY, NULL);
            if (err != ERR_OK) {
                if (committed == 0) return -EIO;
                return (int64_t)committed;     // short count
            }
            committed += chunk;
            f->offset += chunk;
            if (arch_signal_pending_fatal()) {
                return (committed == 0) ? -EINTR : (int64_t)committed;
            }
        }
        return (int64_t)committed;
    }
    default:
        return -1;
    }
}

// ── Weak stub for pty_slave_ioctl (real impl in pty.c, Task 7) ─
__attribute__((weak)) int pty_slave_ioctl(pty_t *pty, int cmd, void *arg)
{
    (void)pty; (void)cmd; (void)arg;
    return -ENOTTY;
}

// ── ioctl through a file descriptor ───────────────────────
int64_t fd_ioctl(file_t *f, int cmd, void *arg)
{
    if (!f) return -EBADF;

    switch (f->type) {
    case FD_VFS:
    case FD_DEV: {
        if (!f->node) return -ENOTTY;
        return devfs_ioctl_node(f->node, cmd, arg);
    }
    case FD_PTY_MASTER: {
        pty_t *pty = f->pty;
        if (!pty) return -ENOTTY;
        if (cmd == TCGETS) {
            if (!arg) return -EFAULT;
            if (!syscall_check_user_range((uint64_t)arg,
                                          sizeof(struct termios), true))
                return -EFAULT;
            if (copy_to_user_ft(arg, &pty->term, sizeof(struct termios)) < 0)
                return -EFAULT;
            return 0;
        }
        return -ENOTTY;
    }
    case FD_PTY_SLAVE: {
        pty_t *pty = f->pty;
        if (!pty) return -ENOTTY;
        return pty_slave_ioctl(pty, cmd, arg);
    }
    case FD_PIPE:
    default:
        return -ENOTTY;
    }
}

// ── Create a pipe ──────────────────────────────────────────
// Returns 0 on success, fills fds[0] = read end, fds[1] = write end.
int64_t do_pipe(int *user_fds)
{
    if (!current->files) return -ENFILE;

    pipe_t *p = pipe_alloc();
    if (!p) return -ENOMEM;

    // Reader file
    file_t *rf = file_alloc();
    if (!rf) { pipe_free(p); return -ENOMEM; }
    rf->type = FD_PIPE;
    rf->pipe = p;
    rf->flags = O_RDONLY;
    rf->refcount = 1;

    // Writer file
    file_t *wf = file_alloc();
    if (!wf) { file_free(rf); pipe_free(p); return -ENOMEM; }
    wf->type = FD_PIPE;
    wf->pipe = p;
    wf->flags = O_WRONLY;
    wf->refcount = 1;

    int rfd = fd_alloc(current->files, rf);
    int wfd = fd_alloc(current->files, wf);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(current->files, rfd);
        if (wfd >= 0) fd_close(current->files, wfd);
        return -ENFILE;
    }

    // Cat B write-back: copy fds to user via _ft.  On _ft failure,
    // H10 rollback: close both fds only — the last close auto-frees
    // the pipe via file_free (file.c:64).  Do NOT pipe_free() explicitly
    // (would double-free: the file_t for the LAST-closed end owns the
    // pipe refcount, and pipe_free()'s kfree(p->buf)/free(p) runs from
    // inside file_free's refcount==0 path).  The first fd_close drops
    // the reader ref to 0; the second drops the writer ref to 0 and
    // triggers file_free → kfree(pipe->buf) → kfree(pipe).  After
    // rollback, the pipe is gone and both fds are gone — clean.
    int fds[2] = { rfd, wfd };
    if (copy_to_user_ft(user_fds, fds, sizeof(fds)) < 0) {
        fd_close(current->files, rfd);
        fd_close(current->files, wfd);
        return -EFAULT;
    }

    debug_fs("pipe: pid=%d fds=[%d,%d]\n",
                  (int)current->pid, rfd, wfd);
    return 0;
}
