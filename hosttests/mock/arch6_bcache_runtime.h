/* hosttests/mock/arch6_bcache_runtime.h — host substrate for the ARCH-6
 * bcache tests.  Compiles the production kernel/block/bcache.c together
 * with the production kernel/sync/wait.c: provides the minimal task /
 * IRQ / schedule surface (modeled on mutex_runtime.h) plus the allocator
 * and debug surface the block tests already use (modeled on
 * arch9/block_runtime.h).
 *
 * Every object compiled with -include this header sees the SAME
 * spinlock_T / task_t layout, which is what lets bcache.o and wait.o
 * share the wait queue.
 */
#ifndef ARCH6_BCACHE_RUNTIME_H
#define ARCH6_BCACHE_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <list.h>

/* Header pre-emptions: the host harness does not link the kernel's
 * debug/slab/irq/percpu/task modules; provide the minimal surface. */
#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H 1
#endif
#ifndef debug_block
#define debug_block(...) do {} while (0)
#endif
#ifndef debug_fs
#define debug_fs(...) do {} while (0)
#endif
#ifndef _KERNEL_SLAB_H
#define _KERNEL_SLAB_H 1
#endif
void *kmalloc(size_t size);
size_t kfree(void *ptr);

#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H 1
#endif
#ifndef _ARCH_ATOMIC_H
#define _ARCH_ATOMIC_H 1
#endif
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H 1
#endif
#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H 1
#endif
#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H 1
#endif

#define TASK_RUNNING        1
#define TASK_INTERRUPTIBLE  2

typedef struct { int held; } spinlock_T;

extern int arch6_bcache_irq_enabled;

static inline void spin_init(spinlock_T *l) { l->held = 0; }
static inline uint64_t arch_local_irq_save(void) {
    int old = arch6_bcache_irq_enabled; arch6_bcache_irq_enabled = 0; return old;
}
static inline void arch_local_irq_restore(uint64_t f) { arch6_bcache_irq_enabled = (int)f; }
static inline void arch_local_irq_enable(void) { arch6_bcache_irq_enabled = 1; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) {
    uint64_t f = arch_local_irq_save();
    if (l->held) __builtin_trap(); l->held = 1; return f;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) {
    if (!l->held) __builtin_trap(); l->held = 0; arch_local_irq_restore(f);
}
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline int arch_atomic_cas(volatile uint64_t *p, uint64_t old, uint64_t n) {
    return __atomic_compare_exchange_n(p, &old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline void arch_atomic_write(volatile uint64_t *p, uint64_t n) {
    __atomic_store_n(p, n, __ATOMIC_SEQ_CST);
}

typedef struct { int64_t pid, state, signal; list_t io_wait_node; } task_t;
extern task_t arch6_bcache_task;
#define current (&arch6_bcache_task)

extern int arch6_bcache_schedule_count;

/* schedule() is provided by arch6_bcache_runtime.c: by default an
 * unexpected wait (no future producer scripted) aborts the test; the
 * case file installs a completion script for the windows it wants. */
void schedule(void);
void task_wake(task_t *t);

/* Script hooks the case file can install (NULL = default abort). */
void arch6_bcache_set_schedule_script(void (*fn)(void));

#endif /* ARCH6_BCACHE_RUNTIME_H */
