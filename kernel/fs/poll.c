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
