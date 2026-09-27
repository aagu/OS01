#ifndef _KERNEL_WAIT_H
#define _KERNEL_WAIT_H

#include <list.h>
#include <arch/spinlock.h>

// ── Generic wait queue ───────────────────────────────────────
// Pattern extracted from tty_read's ad-hoc read_wait list.
// Tasks sleep via wait_queue_sleep() and are woken by
// wait_queue_wake_one() (FIFO) or wait_queue_wake_all().
//
// wait_queue_wake_one/all are IRQ-safe (internal spin_lock_irqsave).

typedef struct {
    list_t      head;       // task_t.io_wait_node list
    spinlock_T  lock;       // protects head
} wait_queue_t;

void wait_queue_init(wait_queue_t *wq);

// Block current on wq.  Caller should re-check its condition
// after return — the wake may be spurious or the condition may
// already be consumed by another waiter.
void wait_queue_sleep(wait_queue_t *wq);

// Enqueue current on wq under wq->lock and set state to
// TASK_INTERRUPTIBLE.  Does NOT call schedule(); the caller is
// responsible for invoking schedule() and wait_queue_disarm() after
// wake up.  Used by callers (e.g. do_poll_core) that need the
// arm-then-recheck-then-schedule handshake to avoid lost wake-ups
// where a producer wakes pt.wq between an fd scan and our entry
// being enqueued (which would silently drop the wake on an empty
// queue).
void wait_queue_arm(wait_queue_t *wq);

// Remove current from any wait queue it is on (via io_wait_node)
// and restore state to TASK_RUNNING.  Safe to call when current is
// not on any wq (idempotent no-op).  Pair with wait_queue_arm, or
// with wait_queue_sleep (which already disarms on return).
void wait_queue_disarm(void);

// Wake one waiter from wq (FIFO).  Safe from IRQ context.
void wait_queue_wake_one(wait_queue_t *wq);

// Wake all waiters from wq.  Safe from IRQ context.
void wait_queue_wake_all(wait_queue_t *wq);

#endif
