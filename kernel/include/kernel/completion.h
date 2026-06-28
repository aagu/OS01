#ifndef _KERNEL_COMPLETION_H
#define _KERNEL_COMPLETION_H

#include <kernel/wait.h>

// ── Completion — signal-before-wait synchronisation ──────────
// Usage from process context:
//     completion_init(&c);
//     wait_for_completion(&c);     // blocks until complete()'d
//
// Usage from IRQ / process context:
//     complete(&c);                // wakes one waiter, IRQ-safe
//
// complete() may fire before wait_for_completion() — the
// internal 'done' counter absorbs the signal.  One complete()
// wakes exactly one waiter (FIFO).

typedef struct {
    volatile int done;
    wait_queue_t wq;
} completion_t;

void completion_init(completion_t *c);

// Block until at least one completed.  Not IRQ-safe.
void wait_for_completion(completion_t *c);

// Signal one waiter.  Safe from IRQ context.
void complete(completion_t *c);

#endif
