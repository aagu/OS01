/* Host-only runtime surface for compiling the REAL
 * kernel/selftest/aarch64_sched_observer.c (P1 Task 2).
 *
 * The observer's only kernel dependency is the IRQ-safe spin surface;
 * the host harness maps it onto a REAL pthread mutex so the
 * concurrency cases exercise genuine mutual exclusion, and the
 * "IRQ-disabled region" is simply the mutex-held region (the host
 * runner is preemptive; the lock contract is what the cases prove).
 */
#ifndef AARCH64_SCHED_OBSERVER_RUNTIME_H
#define AARCH64_SCHED_OBSERVER_RUNTIME_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Skip the real kernel spin surface. */
#define _ARCH_SPINLOCK_H
#define _ARCH_IRQ_H

typedef uint64_t arch_irq_state_t;
arch_irq_state_t arch_local_irq_save(void);
void arch_local_irq_restore(arch_irq_state_t state);

#ifndef OS01_HOST_SPINLOCK_T_PROVIDED
#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    extern void aarch64_sched_observer_host_lock(void);
    aarch64_sched_observer_host_lock();
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{
    extern void aarch64_sched_observer_host_unlock(void);
    (void)l;
    (void)f;
    aarch64_sched_observer_host_unlock();
}
#endif

#endif /* AARCH64_SCHED_OBSERVER_RUNTIME_H */
