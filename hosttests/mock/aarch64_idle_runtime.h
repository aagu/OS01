/* Host-only runtime surface for compiling the REAL
 * kernel/arch/aarch64/cpu/idle.c (P2 Task 3: masked-WFI idle).
 *
 * Short-circuits the heavyweight kernel headers and provides the
 * event-recording spy surface: the tests assert the frozen §5.3
 * ordering (rq-lock check → unlock → wait helper with IRQs masked, no
 * lock held across the WFI, IRQs enabled while ap_work runs) by
 * replaying arrival timings around the wait helper.
 */
#ifndef AARCH64_IDLE_RUNTIME_H
#define AARCH64_IDLE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/ap_work.h>

/* Skip the real kernel headers. */
#define KERNEL_TASK_H
#define KERNEL_SCHED_INTERNAL_H
#define _KERNEL_PERCPU_H
#define _ARCH_SPINLOCK_H
#define _ARCH_IRQ_H
#define _ARCH_CPU_H
#define _KERNEL_LOG_H
#define _KERNEL_DEBUG_H
/* The REAL ap_work protocol header (states + slot array) is
 * lightweight and included unchanged. */

#ifndef NR_CPUS
#define NR_CPUS 8
#endif
#ifndef STACK_SIZE
#define STACK_SIZE (32 * 1024)
#endif

/* ap_work protocol surface comes from the real header; the slot array
 * lives in the test TU. */

typedef struct { uint64_t stub; } rbtree_node_t;
typedef struct { uint64_t stub; } rbtree_root_t;
/* stub == 0 means the (mocked) runqueue is EMPTY; a non-zero stub is
 * the stand-in for queued work. */
static inline bool rbtree_empty(const rbtree_root_t *r)
{ return r->stub == 0; }

/* Macro-mapped (not re-typedef'd): test_platform.h may already have
 * defined these names for the TEST TU, and the macro mapping shadows
 * its (inert) layout with the idle mock — same pattern as
 * switch_complete_test_runtime.h.
 *
 * The TEST TU additionally macro-shadows test_platform's inert inline
 * spin surface with RECORDING versions (a macro wins at call sites),
 * so the rq_lock section is visible in the event stream.  The
 * production TU (no TEST_PLATFORM_H) gets its own recording spin
 * functions. */
#ifndef TEST_PLATFORM_H
typedef struct { unsigned long lock; } spinlock_T;
#endif

#ifdef TEST_PLATFORM_H
uint64_t idle_spin_lock(spinlock_T *l);
void idle_spin_unlock(spinlock_T *l, uint64_t f);
#define spin_init(l) ((void)0)
#define spin_lock_irqsave(l) idle_spin_lock((spinlock_T *)(l))
#define spin_unlock_irqrestore(l, f) idle_spin_unlock((spinlock_T *)(l), (f))
#else
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
uint64_t idle_spin_lock(spinlock_T *l);
void idle_spin_unlock(spinlock_T *l, uint64_t f);
#define spin_lock_irqsave(l) idle_spin_lock((l))
#define spin_unlock_irqrestore(l, f) idle_spin_unlock((l), (f))
#endif

typedef struct idle_mock_percpu {
    uint32_t cpu_id;
    uint32_t online;
    uint32_t scheduler_ok;
    uint64_t need_resched;
    rbtree_root_t run_queue;
    uint64_t nr_running;
    spinlock_T rq_lock;
} idle_mock_percpu_t;
#define percpu_t idle_mock_percpu_t

extern idle_mock_percpu_t percpu_data[NR_CPUS];
extern uint32_t num_cpus;

extern percpu_t *idle_this_cpu;
#define this_cpu() idle_this_cpu
#define cpu_id() (idle_this_cpu ? idle_this_cpu->cpu_id : 0)

/* IRQ surface with event recording. */
typedef uint64_t arch_irq_state_t;
arch_irq_state_t arch_local_irq_save(void);
void arch_local_irq_restore(arch_irq_state_t state);
void arch_local_irq_enable(void);

/* Production interfaces under test / provided by the harness. */
static inline void arch_cpu_pause(void) { }
void schedule(void);
void ap_work_run_one(uint32_t cpu);
void aarch64_idle_wait_irq(void);
void arch_aarch64_idle_step(void);
void idle_resume(void);

/* ── Event recording (defined in the test TU) ────────────────────── */
enum idle_event {
    EV_RUN_ONE,          /* ap_work_run_one entered            */
    EV_IRQ_SAVE,         /* irq_save called                    */
    EV_RQ_LOCK,          /* rq_lock acquired                   */
    EV_RQ_UNLOCK,        /* rq_lock released                   */
    EV_SCHEDULE,         /* schedule() called                  */
    EV_IRQ_RESTORE,      /* irq_restore called                 */
    EV_WAIT_ENTER,       /* wait helper entered (IRQs state in arg) */
    EV_WAIT_RETURN,      /* wait helper returned               */
    EV_IRQ_ENABLE,       /* irq_enable after the wait          */
};
void idle_event_record(int ev, uint64_t arg);
extern int idle_event_count;
extern int idle_irq_state_at_wait;   /* 1 = masked at wait enter */

#endif /* AARCH64_IDLE_RUNTIME_H */
