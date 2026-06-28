#ifndef _KERNEL_WAIT_H
#define _KERNEL_WAIT_H

#include <list.h>
#include <kernel/task.h>
#include <kernel/arch/x86_64/spinlock.h>

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

// Wake one waiter from wq (FIFO).  Safe from IRQ context.
void wait_queue_wake_one(wait_queue_t *wq);

// Wake all waiters from wq.  Safe from IRQ context.
void wait_queue_wake_all(wait_queue_t *wq);

#endif
