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
    // Default cwd is root
    fs->cwd = strdup("/");
    if (!fs->cwd) { free(fs); return NULL; }
    return fs;
}

// ── Release entire fd table ─────────────────────────────────
void files_free(files_t *fs)
{
    if (!fs) return;
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i]) {
            file_t *f = fs->fd[i];
            if (__sync_sub_and_fetch(&f->refcount, 1) == 0)
                file_free(f);
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

    new_fs->cwd = strdup(fs->cwd);
    if (!new_fs->cwd) { free(new_fs); return NULL; }

    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i]) {
            __sync_add_and_fetch(&fs->fd[i]->refcount, 1);
            new_fs->fd[i] = fs->fd[i];
        }
    }
    return new_fs;
}

// ── Find lowest free fd slot ────────────────────────────────
int fd_alloc(files_t *fs, file_t *f)
{
    if (!fs || !f) return -1;
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i] == NULL) {
            fs->fd[i] = f;
            return i;
        }
    }
    return -1;  // table full
}

// ── Close a single fd ───────────────────────────────────────
void fd_close(files_t *fs, int fd)
{
    if (!fs || fd < 0 || fd >= NOFILE) return;

    file_t *f = fs->fd[fd];
    if (!f) return;

    fs->fd[fd] = NULL;

    if (__sync_sub_and_fetch(&f->refcount, 1) == 0)
        file_free(f);
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
int64_t pipe_read_internal(pipe_t *p, void *buf, uint64_t size)
{
    if (!p) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t total = 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&p->lock);

        while (total < size && !pipe_empty(p)) {
            // Read one byte at a time from the ring buffer
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

        // Register on pipe's wait queue, then double-check
        // under p->lock to close the lost-wakeup race:
        //  1. check writers=1 → 2. unlock → 3. writer exits, wake
        //     (queue empty) → 4. add to queue → sleep forever.
        // Double-check after queue registration catches step 3.
        {
            wait_queue_t *wq = &p->read_wait;
            int do_eof = 0;

            uint64_t wq_flags = spin_lock_irqsave(&wq->lock);
            list_add_to_before(&wq->head, &current->io_wait_node);

            {
                uint64_t p2_flags = spin_lock_irqsave(&p->lock);
                if (p->writers == 0 && pipe_empty(p)) {
                    // No more data will ever arrive → EOF
                    list_del_init(&current->io_wait_node);
                    do_eof = 1;
                } else if (p->writers == 0 && !pipe_empty(p)) {
                    // Last writer gone but data still in buffer —
                    // don't sleep, go back and read it
                    list_del_init(&current->io_wait_node);
                }
                spin_unlock_irqrestore(&p->lock, p2_flags);
            }

            if (do_eof) {
                spin_unlock_irqrestore(&wq->lock, wq_flags);
                return 0;
            }

            current->state = TASK_INTERRUPTIBLE;
            int was_queued = !list_is_empty(&current->io_wait_node);
            spin_unlock_irqrestore(&wq->lock, wq_flags);

            if (!was_queued) {
                // Dequeued by double-check (buffer has data) — skip sleep
                current->state = TASK_RUNNING;
            } else {
                schedule();
                arch_local_irq_enable();
                if (!list_is_empty(&current->io_wait_node))
                    list_del_init(&current->io_wait_node);
                current->state = TASK_RUNNING;
            }
        }

        if (signal_pending_fatal())
            return -EINTR;
    }
}

// ── Read through a file descriptor ──────────────────────────
// Returns bytes read, 0 for EOF (pipe with no writers), or
// negative on error.
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
        int64_t n = vfs_read(f->node, f->offset, size, buf);
        if (n > 0)
            f->offset += (uint64_t)n;
        return n;
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
        // Drain a partially-consumed netbuf first (a 1-byte fgets
        // read must not lose the rest of the 370-byte response).
        if (s->rx_nb) {
                struct netbuf *nb = (struct netbuf *)s->rx_nb;
                void *data; u16_t data_len;
                netbuf_data(nb, &data, &data_len);
                u16_t avail = (data_len > (u16_t)s->rx_off) ? (u16_t)(data_len - s->rx_off) : 0;
                size_t copy = (avail < size) ? avail : size;
                if (copy > 0) memcpy(buf, (uint8_t *)data + s->rx_off, copy);
                s->rx_off += (int)copy;
                if (s->rx_off >= data_len) {
                    s->rx_off = 0;
                    if (netbuf_next(nb) < 0) {
                        netbuf_delete(nb);
                        s->rx_nb = NULL;
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
                if (copy > 0) memcpy(buf, data, copy);
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
            if (err == ERR_CLSD) return 0;
            if (err == ERR_WOULDBLOCK) return -EAGAIN;
            return -EIO;
        }
    default:
        return -1;
    }
}

// ── Pipe write internal (blocking, exported for PTY) ────────
int64_t pipe_write_internal(pipe_t *p, const void *buf, uint64_t size)
{
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

        if (signal_pending_fatal())
            return -EINTR;
    }
}

// ── Write through a file descriptor ─────────────────────────
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
        int64_t n = vfs_write(f->node, f->offset, size, (void *)buf);
        if (n > 0)
            f->offset += (uint64_t)n;
        return n;
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
        err_t err = netconn_write((struct netconn *)s->conn, buf,
                                  (u16_t)size, 0x01);  // NETCONN_COPY
        if (err == ERR_OK) { f->offset += size; return (int64_t)size; }
        return -EIO;
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
            if (!arg) return -EINVAL;
            memcpy(arg, &pty->term, sizeof(struct termios));
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

    // Write fd numbers to user space
    int fds[2] = { rfd, wfd };
    if ((uint64_t)user_fds >= current->addr_limit)
        return -EFAULT;
    memcpy((void *)user_fds, fds, sizeof(fds));

    debug_fs("pipe: pid=%d fds=[%d,%d]\n",
                  (int)current->pid, rfd, wfd);
    return 0;
}
