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
#include <kernel/slab.h>
#include <fs/vfs.h>
#include <kernel/percpu.h>
#include <kernel/pty.h>
#include <device/timer.h>    // jiffies
#include <net/socket.h>     // SOCK_CONNECTED, SOCK_LISTENING
#include <string.h>          // memset
#include <stddef.h>
#include <errno.h>

// Forward: devfs_poll lives in devfs.c (devices[] is static there)
struct vfs_node;
uint32_t devfs_poll(struct vfs_node *node, poll_table_t *pt);

// ── poll_table_init — reset for new scan round ─────────────
// Does NOT re-init wq or entry nodes (one-time setup).

void poll_table_init(poll_table_t *pt)
{
    pt->nent = 0;
    pt->triggered = false;
}

// ── poll_table_setup — allocate entries + init wq + entry nodes ──
// Returns 0 on success, -ENOMEM on allocation failure.

int poll_table_setup(poll_table_t *pt, int max_entries)
{
    wait_queue_init(&pt->wq);
    pt->max_entries = max_entries;
    pt->entries = kmalloc(max_entries * sizeof(poll_wait_entry_t));
    if (!pt->entries)
        return -ENOMEM;
    for (int i = 0; i < max_entries; i++)
        list_init(&pt->entries[i].node);
    return 0;
}

// ── poll_table_destroy — free entries allocated by poll_table_setup ──

void poll_table_destroy(poll_table_t *pt)
{
    if (pt->entries) {
        // Always clean up fd list entries before freeing to prevent
        // use-after-free from concurrent fd wake paths.
        poll_table_cleanup(pt);
        kfree(pt->entries);
        pt->entries = NULL;
    }
}

// ── poll_wait — register on an fd's poll list ─────────────
// Called by fd_poll() when the fd is NOT ready.
// The entry will later be cascade-woken by the fd's wake path.

void poll_wait(poll_table_t *pt, list_t *poll_list, spinlock_T *fd_lock)
{
    if (pt->nent >= pt->max_entries || pt->triggered)
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
            if (!pipe_empty(p)) {
                mask |= POLLIN;
                if (p->writers == 0)
                    mask |= POLLHUP;
            } else if (p->writers == 0) {
                mask |= POLLIN | POLLHUP;  // EOF: read() returns 0 immediately
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->read_poll, &p->lock);
            }
        }

        if (f->flags == O_WRONLY) {
            if (!pipe_full(p)) {
                mask |= POLLOUT;
            } else if (p->readers == 0) {
                mask |= POLLOUT | POLLERR;  // EPIPE: write() errors immediately
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->write_poll, &p->lock);
            }
        }

        if (f->flags == O_RDWR) {
            if (!pipe_empty(p)) {
                mask |= POLLIN;
                if (p->writers == 0)
                    mask |= POLLHUP;
            } else if (p->writers == 0) {
                mask |= POLLIN | POLLHUP;
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->read_poll, &p->lock);
            }

            if (!pipe_full(p)) {
                mask |= POLLOUT;
            } else if (p->readers == 0) {
                mask |= POLLOUT | POLLERR;
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->write_poll, &p->lock);
            }
        }

        spin_unlock_irqrestore(&p->lock, flags);
        return mask;
    }

    case FD_PTY_MASTER: {
        pty_t *pty = f->pty;
        if (!pty) return POLLERR;
        uint32_t mask = 0;
        if (pty->slave_to_master) {
            uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
            pipe_t *p = pty->slave_to_master;
            if (!pipe_empty(p)) {
                mask |= POLLIN | POLLRDNORM;
                if (p->writers == 0)
                    mask |= POLLHUP;
            } else if (p->writers == 0) {
                mask |= POLLIN | POLLHUP;
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->read_poll, &p->lock);
            }
            spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
        }
        if (pty->master_to_slave) {
            uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
            pipe_t *p = pty->master_to_slave;
            if (!pipe_full(p)) {
                mask |= POLLOUT | POLLWRNORM;
                if (p->readers == 0)
                    mask |= POLLERR;
            } else if (p->readers == 0) {
                mask |= POLLOUT | POLLERR;
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->write_poll, &p->lock);
            }
            spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
        }
        return mask;
    }
    case FD_PTY_SLAVE: {
        // symmetric: slave reads master_to_slave, writes slave_to_master
        pty_t *pty = f->pty;
        if (!pty) return POLLERR;
        uint32_t mask = 0;
        if (pty->master_to_slave) {
            uint64_t fl = spin_lock_irqsave(&pty->master_to_slave->lock);
            pipe_t *p = pty->master_to_slave;
            if (!pipe_empty(p)) {
                mask |= POLLIN | POLLRDNORM;
                if (p->writers == 0)
                    mask |= POLLHUP;
            } else if (p->writers == 0) {
                mask |= POLLIN | POLLHUP;
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->read_poll, &p->lock);
            }
            spin_unlock_irqrestore(&pty->master_to_slave->lock, fl);
        }
        if (pty->slave_to_master) {
            uint64_t fl = spin_lock_irqsave(&pty->slave_to_master->lock);
            pipe_t *p = pty->slave_to_master;
            if (!pipe_full(p)) {
                mask |= POLLOUT | POLLWRNORM;
                if (p->readers == 0)
                    mask |= POLLERR;
            } else if (p->readers == 0) {
                mask |= POLLOUT | POLLERR;
            } else if (pt && !pt->triggered) {
                poll_wait(pt, &p->write_poll, &p->lock);
            }
            spin_unlock_irqrestore(&pty->slave_to_master->lock, fl);
        }
        return mask;
    }
    case FD_SOCKET: {
        socket_t *s = f->sock;
        if (!s) return POLLNVAL;
        uint32_t revents = 0;
        uint64_t flags = spin_lock_irqsave(&s->lock);
        if (s->state == SOCK_CONNECTED) {
            revents |= POLLOUT;
            // Data available (netconn RCVPLUS callback set rx_pending)
            // or peer closed (rx_pending stays set after ERR_CLSD):
            // readable either way.
            if (s->rx_pending)
                revents |= POLLIN | POLLRDNORM;
        }
        if (s->state == SOCK_LISTENING && s->rx_pending)
            revents |= POLLIN;
        if (revents == 0 && pt && !pt->triggered)
            poll_wait(pt, &s->poll_list, &s->lock);
        spin_unlock_irqrestore(&s->lock, flags);
        return revents;
    }

    default:
        return POLLNVAL;
    }
}

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

// ── do_poll_core — core polling loop (no user memory access) ──
// Caller provides kfds and pt (already setup via poll_table_setup).
// Caller is responsible for poll_table_destroy.
// Returns: ready count (>=0), or -EINTR.

int64_t do_poll_core(struct pollfd *kfds, uint64_t nfds, int64_t timeout_val, poll_table_t *pt)
{
    // ── Timeout setup ──────────────────────────────────────
    uint64_t deadline = 0;
    if (timeout_val > 0) {
        // Convert ms to PIT ticks (100 Hz → 10 ms/tick)
        int64_t ticks = (timeout_val + 9) / 10;
        if (ticks < 1) ticks = 1;
        poll_deadline_jiffies = jiffies + (uint64_t)ticks;
        deadline = poll_deadline_jiffies;
        current_poll_wq = &pt->wq;
    }

    int ready_count = 0;

    for (;;) {
        poll_table_init(pt);  // reset nent=0, triggered=false

        // ── Scan all fds ──────────────────────────────────
        for (uint32_t i = 0; i < nfds; i++) {
            if (kfds[i].fd < 0) continue;

            file_t *f = current->files->fd[kfds[i].fd];
            if (!f) {
                kfds[i].revents = POLLNVAL;
                ready_count++;
                continue;
            }

            uint32_t revents = fd_poll(f, pt);
            // fd is ready if it matches requested events OR has error/HUP
            if ((revents & kfds[i].events) || (revents & (POLLHUP | POLLERR))) {
                // Mask to requested events, but always include POLLHUP/POLLERR
                // even if not requested (POSIX: output-only error flags).
                kfds[i].revents = (revents & kfds[i].events)
                                | (revents & (POLLHUP | POLLERR | POLLNVAL));
                ready_count++;
                pt->triggered = true;
            }
        }

        // ── Ready? Return ─────────────────────────────────
        if (ready_count > 0) {
            poll_table_cleanup(pt);
            if (timeout_val > 0) current_poll_wq = NULL;
            break;
        }

        // ── Non-blocking? ─────────────────────────────────
        if (timeout_val == 0) {
            poll_table_cleanup(pt);
            break;
        }

        // ── Pre-sleep signal check ────────────────────────
        if (current->signal & ~current->blocked) {
            poll_table_cleanup(pt);
            if (timeout_val > 0) current_poll_wq = NULL;
            return -EINTR;
        }

        // ── Block on pt.wq ────────────────────────────────
        wait_queue_sleep(&pt->wq);

        // Woken up — remove entries from fd poll lists
        if (timeout_val > 0) current_poll_wq = NULL;
        poll_table_cleanup(pt);

        // ── Timeout check ─────────────────────────────────
        if (timeout_val > 0 && jiffies >= deadline) {
            return 0;
        }

        // ── Post-sleep signal check ───────────────────────
        if (current->signal & ~current->blocked) {
            return -EINTR;
        }

        ready_count = 0;
    }

    return ready_count;
}

// ── do_poll — poll syscall implementation (thin wrapper) ──
//
// Linux ABI: int poll(struct pollfd *fds, nfds_t nfds, int timeout)
//   timeout: -1 = infinite, 0 = non-blocking, >0 = milliseconds
// Returns: count of ready fds, 0 = timeout, <0 = -errno
//
// Signal semantics: any unblocked signal (not just fatal) interrupts
// poll with -EINTR, per POSIX.

int64_t do_poll(struct pollfd *user_fds, uint64_t nfds, int timeout_val)
{
    // ── Validate user pointer ──────────────────────────────
    if ((uint64_t)user_fds >= current->addr_limit)
        return -EFAULT;
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;

    // nfds==0 with timeout<=0 returns immediately.
    // nfds==0 with timeout>0 goes through do_poll_core to sleep.
    if (nfds == 0 && timeout_val <= 0)
        return 0;

    // ── Copy pollfd from user space ────────────────────────
    struct pollfd kfds[POLL_MAX_FDS];
    if (nfds > 0) {
        for (uint32_t i = 0; i < nfds; i++) {
            kfds[i].fd      = user_fds[i].fd;
            kfds[i].events  = user_fds[i].events;
            kfds[i].revents = 0;
        }
    }

    // ── Signal check: any unblocked pending signal → EINTR ─
    if (current->signal & ~current->blocked)
        return -EINTR;

    // ── Setup poll table (dynamic entries allocation) ─────────
    poll_table_t pt;
    if (poll_table_setup(&pt, (nfds == 0) ? 1 : POLL_MAX_FDS) != 0)
        return -ENOMEM;

    int64_t ret = do_poll_core(kfds, nfds, timeout_val, &pt);

    // ── Single-point cleanup ────────────────────────────────
    if (ret >= 0 && nfds > 0) {
        for (uint32_t i = 0; i < nfds; i++)
            user_fds[i].revents = kfds[i].revents;
    }
    poll_table_destroy(&pt);
    return ret;
}
