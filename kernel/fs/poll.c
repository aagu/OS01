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
