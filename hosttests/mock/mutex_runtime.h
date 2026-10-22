/* Minimal scheduler/IRQ substrate; mutex.c and wait.c remain production code. */
#ifndef MUTEX_TEST_RUNTIME_H
#define MUTEX_TEST_RUNTIME_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <list.h>
#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H
#define _ARCH_IRQ_H
#define _ARCH_ATOMIC_H
#define _ARCH_SPINLOCK_H
#define TASK_RUNNING 1
#define TASK_INTERRUPTIBLE 2
typedef struct { int held; } spinlock_T;
extern int mutex_test_irq_enabled;
static inline void spin_init(spinlock_T *l) { l->held = 0; }
static inline uint64_t arch_local_irq_save(void) {
    int old = mutex_test_irq_enabled; mutex_test_irq_enabled = 0; return old;
}
static inline void arch_local_irq_restore(uint64_t f) { mutex_test_irq_enabled = f; }
static inline void arch_local_irq_enable(void) { mutex_test_irq_enabled = 1; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) {
    uint64_t f = arch_local_irq_save();
    if (l->held) __builtin_trap(); l->held = 1; return f;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) {
    if (!l->held) __builtin_trap(); l->held = 0; arch_local_irq_restore(f);
}
static inline int arch_atomic_cas(volatile uint64_t *p, uint64_t old, uint64_t n) {
    return __atomic_compare_exchange_n(p, &old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline void arch_atomic_write(volatile uint64_t *p, uint64_t n) {
    __atomic_store_n(p, n, __ATOMIC_SEQ_CST);
}
typedef struct { int64_t pid, state, signal; list_t io_wait_node; } task_t;
extern task_t mutex_test_task;
#define current (&mutex_test_task)
void schedule(void);
void task_wake(task_t *t);
#endif
