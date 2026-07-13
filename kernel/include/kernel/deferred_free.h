#ifndef _KERNEL_DEFERRED_FREE_H
#define _KERNEL_DEFERRED_FREE_H

#include <list.h>
#include <stdint.h>
#include <kernel/task.h>

// Forward-declare files_t (full definition in kernel/file.h — not
// included here to keep this header light for consumers).
struct files_struct;

// Callback type for deferred-free work items.
typedef void (*deferred_fn_t)(void *ptr);

typedef struct deferred_work {
    list_t        node;
    deferred_fn_t fn;
    void         *ptr;
} deferred_work_t;

// Enqueue a deferred free. Safe from any context including IRQs-off
// (only takes a spinlock, never sleeps).
void deferred_free(deferred_fn_t fn, void *ptr);

// Convenience wrappers that avoid UB from casting kfree/files_free
// to deferred_fn_t. All three functions enqueue the free onto the
// reaper kthread's work queue.
void deferred_kfree(void *ptr);                           // kfree a slab allocation
void deferred_files_free(struct files_struct *fs);        // free an fd table

// Spawn the reaper kthread. Called once from task_init() before
// activating the scheduler. Returns the kthread's task_t* or NULL.
task_t *deferred_free_spawn(void);

#endif // _KERNEL_DEFERRED_FREE_H
