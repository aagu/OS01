/* Host-only runtime surface for compiling the REAL
 * kernel/sched/accounting.c (monitor plan Task 2).
 *
 * Pattern follows gfx_test_runtime.h / tlb_test_runtime.h: heavyweight
 * kernel headers are skipped via their include guards and a small
 * controlled surface is provided instead, so the fixture drives the
 * production accounting code, not a mock of it.
 *
 * Two compilation modes:
 *   - PRODUCTION OBJECT (kernel/sched/accounting.c): this header is the
 *     only preinclude; it provides the whole platform surface with a
 *     test-controlled cpu_id() / percpu_data[] / current.
 *   - TEST TU (test_monitor_accounting.c): FRAMEWORK_INC preincludes
 *     test_platform.h FIRST (its TEST_PLATFORM_H guard is then set and
 *     its NR_CPUS / spinlock stubs with the poll_test_spinlock_acquire
 *     observation hook / percpu_t are reused); this header then only
 *     adds the task_t fixture type and the test-control externs.
 */
#ifndef ACCOUNTING_TEST_RUNTIME_H
#define ACCOUNTING_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Minimal task surface (test_platform.h does not define task_t):
 * accounting.c only ever touches runtime_ticks and compares the task
 * pointer against the CPU's idle pointer. */
typedef struct task_struct {
    uint64_t runtime_ticks;
} task_t;

#ifdef TEST_PLATFORM_H
/* ── Test TU: platform surface already provided by test_platform.h ──
 * (NR_CPUS, spinlock_T + spin_* stubs, percpu_t with scheduler_ok /
 * online / idle fields, extern uint32_t num_cpus).  Only the control
 * externs are needed here. */
extern volatile uint32_t accounting_test_cpu;
#else
/* ── Production object: provide the whole surface ── */

/* Skip the real kernel headers via their include guards. */
#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H

#ifndef NR_CPUS
#define NR_CPUS 8
#endif

/* Spinlock stub — same shape/guard contract as test_platform.h. */
#ifndef OS01_HOST_SPINLOCK_T_PROVIDED
#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;
void poll_test_spinlock_acquire(spinlock_T *lock) __attribute__((weak));
void poll_test_spinlock_release(spinlock_T *lock) __attribute__((weak));
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    if (poll_test_spinlock_acquire) poll_test_spinlock_acquire(l);
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{
    if (poll_test_spinlock_release) poll_test_spinlock_release(l);
    (void)f;
}
#endif /* OS01_HOST_SPINLOCK_T_PROVIDED */

typedef struct {
    /* MUST stay byte-identical to test_platform.h's percpu_t: the
     * percpu_data[] array is DEFINED in the test TU and indexed by the
     * production object, so both TUs must agree on the stride (this is
     * why the tail fields tsc_boot/tsc_offset, unused by accounting.c,
     * are still listed here). */
    uint64_t self, need_resched;
    uint32_t cpu_id, arch_processor_id, online, scheduler_ok;
    void *tss;
    void *run_queue, *idle;
    uint64_t schedule_count, tsc_boot;
    int64_t  tsc_offset;
} percpu_t;

extern percpu_t percpu_data[NR_CPUS];
extern uint32_t num_cpus;

/* Which CPU the production code believes it is running on. */
extern volatile uint32_t accounting_test_cpu;
#define cpu_id() accounting_test_cpu

#endif /* TEST_PLATFORM_H */

/* ── Shared control surface (both modes) ── */

/* The "interrupted task": the fixture repoints this between ticks to
 * simulate idle vs busy samples. */
extern task_t *accounting_test_current;
#define current accounting_test_current

#endif /* ACCOUNTING_TEST_RUNTIME_H */
