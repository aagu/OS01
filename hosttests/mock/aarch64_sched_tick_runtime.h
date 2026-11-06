/* Host-only runtime surface for compiling the REAL
 *   kernel/arch/aarch64/platform/time.c   (the CNTP backend)
 *   kernel/arch/aarch64/cpu/sched_tick.c  (the per-CPU scheduler tick)
 *   kernel/sched/fair.c                    (update_curr)
 *   kernel/sched/accounting.c              (the per-CPU usage counters)
 * (aarch64 per-CPU tick accounting plan, Task 1 + Task 2).
 *
 * The mock replaces ONLY the machine / GIC / DTB / IRQ / CPU-identity
 * boundaries:
 *
 *   - the CNTP register accesses (counter-frequency read, TVAL write,
 *     CTL read/write) and the instruction barrier,
 *   - the GIC handler registration,
 *   - the DTB-provided CNTP PPI,
 *   - the global tick_handler() entry point,
 *   - the CPU identity (this_cpu() / cpu_id()), the current-task
 *     accessor, the admission mask and the per-CPU idle metadata,
 *   - the lock surface (with a nesting recorder) and the log output.
 *
 * Every piece of ALGORITHM stays in the production objects: the
 * frequency / period / PPI validation, the frozen-configuration
 * publish/acquire protocol, the TVAL -> CTL -> ISB -> read-back arm
 * order, the ENABLE-clearing failure path, the ready / started /
 * admission gates, the exact busy/idle accounting increments and the
 * tick-driven vruntime charge.  The mock never re-implements them.
 *
 * The mocked GIC registration also CAPTURES the registered handler,
 * so the fixture can deliver a real CNTP interrupt by calling the
 * production ISR itself.
 *
 * Two compilation modes:
 *   - PRODUCTION OBJECTS: this header is the only preinclude; the
 *     whole control surface below is declared extern.
 *   - TEST TU (hosttests/cases/test_aarch64_sched_tick.c): the case
 *     file defines AARCH64_SCHED_TICK_HOST_TU before including this
 *     header; the control surface is then DEFINED here.
 */
#ifndef AARCH64_SCHED_TICK_RUNTIME_H
#define AARCH64_SCHED_TICK_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Skip the real kernel headers via their include guards ─────── */
#define _ARCH_AARCH64_GIC_H      /* <arch/aarch64/reg.h>    CNTP accessors */
#define OS01_AARCH64_GIC_H       /* <arch/aarch64/gic.h>    handler table  */
#define OS01_AARCH64_DTB_H       /* <arch/aarch64/dtb.h>    CNTP PPI       */
#define OS01_AARCH64_SMP_H       /* <arch/aarch64/smp.h>    admission mask */
#define _KERNEL_CLOCKEVENT_H     /* <time/clockevent.h>     tick_handler   */
#define _KERNEL_LOG_H            /* <log/log.h>              log_*          */
#define _ARCH_CPU_H              /* <arch/cpu.h>             arch facade    */
#define _ARCH_IRQ_H              /* <arch/irq.h>             IRQ helpers    */
#define _ARCH_SPINLOCK_H         /* <arch/spinlock.h>        lock surface   */
#define _KERNEL_PERCPU_H         /* <percpu/percpu.h>        percpu_t       */
#define KERNEL_TASK_H            /* <sched/task.h>           task_t         */
#define KERNEL_SCHED_INTERNAL_H  /* <sched/internal.h>      EEVDF surface  */
#define _KERNEL_ASSERT_H         /* <core/assert.h>          ASSERT         */
#define _KERNEL_H                /* <kernel.h>               container_of   */

/* The public interfaces below are host-safe and NOT skipped: the
 * production objects and the fixture must agree on the frozen-config
 * layout, the nominal tick rate and the tick lifecycle. */
#include <arch/aarch64/timer.h>
#include <arch/aarch64/sched_tick.h>
#include <sched/accounting.h>

/* ── Constants the skipped headers normally provide ───────────── */
#include <config/limits.h>

#define EEVDF_MIN_SLICE  10   /* time slice = 10 ticks = 100ms   */
#define EEVDF_LATENCY    40   /* eligibility window = 40 ticks   */

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* ── Red-black tree surface (fair.c's runqueue helpers) ──────────
 * The declarations mirror libc/include/rbtree.h exactly; the real
 * implementation is linked into the binary. */
typedef struct rbtree_node {
    struct rbtree_node *left;
    struct rbtree_node *right;
    struct rbtree_node *parent;
    unsigned long color;
} rbtree_node_t;

typedef struct rbtree_root {
    rbtree_node_t *rb_node;
} rbtree_root_t;

static inline int rbtree_empty(rbtree_root_t *root)
{
    return root->rb_node == NULL;
}

rbtree_node_t *rbtree_insert(rbtree_root_t *root, rbtree_node_t *node,
                             int (*cmp)(rbtree_node_t *a, rbtree_node_t *b));
void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node);
rbtree_node_t *rbtree_first(rbtree_root_t *root);

/* ── Reduced task_t (the fields the exercised code touches) ────── */
typedef struct task_struct {
    rbtree_node_t rb_node;
    uint64_t vruntime;
    uint64_t deadline;
    uint64_t runtime_ticks;
    uint64_t allowed_cpus;
    int64_t  pid;
    uint32_t on_rq;
    uint32_t state;
    uint32_t cpu;
    uint32_t flags;
} task_t;

/* ── Lock surface with a nesting recorder ────────────────────────
 * The production objects never nest the usage lock with the runqueue
 * lock; the recorder proves it. */
typedef struct { unsigned long lock; } spinlock_T;

void mock_lock_record(spinlock_T *l, int is_lock);
void mock_assert_failed(const char *expr, const char *file, int line);

static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { mock_lock_record(l, 1); }
static inline void spin_unlock(spinlock_T *l) { mock_lock_record(l, 0); }
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    mock_lock_record(l, 1);
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t flags)
{
    (void)flags;
    mock_lock_record(l, 0);
}

#define ASSERT(x) do { if (!(x)) mock_assert_failed(#x, __FILE__, __LINE__); } while (0)

/* ── Reduced percpu_t ───────────────────────────────────────────── */
typedef struct mock_percpu {
    uint64_t self, need_resched;
    uint32_t cpu_id, arch_processor_id, online, scheduler_ok;
    void *tss;
    rbtree_root_t run_queue;
    task_t *idle;
    uint64_t schedule_count;
    uint64_t watchdog_counter;
    uint64_t min_vruntime;
    uint32_t nr_running;
    spinlock_T rq_lock;
} mock_percpu_t;

#define percpu_t mock_percpu_t

extern mock_percpu_t percpu_data[NR_CPUS];
extern uint32_t num_cpus;

/* ── CPU identity / current task ───────────────────────────────── */
extern mock_percpu_t *mock_sched_this_cpu;
extern task_t *mock_sched_current;
extern int mock_current_reads;
extern int mock_current_poisoned;

task_t *mock_current_read(void);

static inline mock_percpu_t *mock_this_cpu(void) { return mock_sched_this_cpu; }
#define this_cpu() mock_this_cpu()
#define cpu_id() (mock_sched_this_cpu ? mock_sched_this_cpu->cpu_id : 0)
#define current mock_current_read()

/* ── Admission mask (the CPU boundary) ─────────────────────────── */
extern uint64_t mock_accepted_mask;
uint64_t aarch64_smp_accepted_mask(void);

/* ── Production interfaces under test ──────────────────────────── */
void update_curr(task_t *task);

/* BSP boot start of the arch tick source (the <arch/cpu.h> facade
 * entry point that the real backend implements). */
bool arch_tick_start(void);

/* ── Machine boundary ──────────────────────────────────────────── */
struct pt_regs;

typedef void (*gic_handler_fn)(uint32_t intid, uint64_t param,
                               struct pt_regs *regs);

uint64_t cntfrq_el0(void);
void     cntp_tval_el0_write(uint64_t v);
uint64_t cntp_ctl_el0_read(void);
void     cntp_ctl_el0_write(uint64_t v);
void     cntp_isb(void);

int      gic_register_handler(uint32_t intid, gic_handler_fn fn,
                              uint64_t param, const char *name);
uint32_t dtb_cntp_ppi(void);
void     tick_handler(void);

/* ── Log surface ───────────────────────────────────────────────── */
void aarch64_timer_mock_log(const char *level, const char *fmt, ...);
#define log_err(...)  aarch64_timer_mock_log("err", __VA_ARGS__)
#define log_warn(...) aarch64_timer_mock_log("warn", __VA_ARGS__)
#define log_info(...) aarch64_timer_mock_log("info", __VA_ARGS__)

/* ── Register trace ────────────────────────────────────────────── */
enum aarch64_timer_op {
    AARCH64_TIMER_OP_FREQ_READ = 0,
    AARCH64_TIMER_OP_TVAL_WRITE,
    AARCH64_TIMER_OP_CTL_READ,
    AARCH64_TIMER_OP_CTL_WRITE,
    AARCH64_TIMER_OP_ISB,
};

#define AARCH64_TIMER_TRACE_MAX 64

struct aarch64_timer_trace_entry {
    enum aarch64_timer_op op;
    uint64_t value;
};

extern struct aarch64_timer_trace_entry
    aarch64_timer_trace[AARCH64_TIMER_TRACE_MAX];
extern int aarch64_timer_trace_count;

/* ── Fixture control surface ───────────────────────────────────── */
extern uint64_t aarch64_timer_mock_freq;      /* cntfrq_el0() result         */
extern uint64_t aarch64_timer_mock_ctl;       /* CNTP_CTL_EL0 register value  */
extern uint64_t aarch64_timer_mock_tval;      /* CNTP_TVAL_EL0 register value */
extern int      aarch64_timer_mock_readback_fails; /* read-back hides ENABLE    */
extern int      aarch64_timer_mock_imask_sticky;  /* CTL write keeps IMASK set */
extern int      aarch64_timer_mock_register_rc;   /* forced gic_register rc    */
extern int      aarch64_timer_mock_register_calls;
extern uint32_t aarch64_timer_mock_register_intid;
extern uint32_t aarch64_timer_mock_ppi;       /* dtb_cntp_ppi() result       */
extern int      aarch64_timer_mock_tick_calls; /* global tick_handler() calls */

/* The registered CNTP handler — the fixture delivers a REAL interrupt
 * by calling it. */
extern gic_handler_fn aarch64_timer_mock_handler;
extern uint64_t      aarch64_timer_mock_handler_param;

#define AARCH64_TIMER_MOCK_MAX_INTIDS 8
extern uint32_t aarch64_timer_mock_registered[AARCH64_TIMER_MOCK_MAX_INTIDS];
extern int      aarch64_timer_mock_registered_count;

/* ── Lock recorder state ───────────────────────────────────────── */
extern int mock_lock_depth;
extern int mock_lock_max_depth;
extern int mock_lock_nesting_violations;
extern int mock_lock_ops;
extern int mock_assert_failures;

/* ── Captured log lines ────────────────────────────────────────── */
#define AARCH64_TIMER_LOG_MAX 32
#define AARCH64_TIMER_LOG_LEN 256

extern char aarch64_timer_log_buf[AARCH64_TIMER_LOG_MAX][AARCH64_TIMER_LOG_LEN];
extern int  aarch64_timer_log_count;
extern int  aarch64_timer_log_info_count;
extern int  aarch64_timer_log_err_count;

/* Reset the fixture to a pristine state (registers, injected faults,
 * GIC registration table, trace, lock recorder and captured log
 * lines).  The PRODUCTION state published by the real backend is NOT
 * resettable — see the case-ordering note in the test TU. */
void aarch64_timer_mock_reset(void);

#ifdef AARCH64_SCHED_TICK_HOST_TU
/* ══ Test TU: the whole control/spy layer is DEFINED here ════════ */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

mock_percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = 1;
mock_percpu_t *mock_sched_this_cpu;
task_t *mock_sched_current;
int mock_current_reads;
int mock_current_poisoned;
uint64_t mock_accepted_mask = 1ULL;

struct aarch64_timer_trace_entry
    aarch64_timer_trace[AARCH64_TIMER_TRACE_MAX];
int aarch64_timer_trace_count;

uint64_t aarch64_timer_mock_freq = 62500000ULL;
uint64_t aarch64_timer_mock_ctl;
uint64_t aarch64_timer_mock_tval;
int      aarch64_timer_mock_readback_fails;
int      aarch64_timer_mock_imask_sticky;
int      aarch64_timer_mock_register_rc;
int      aarch64_timer_mock_register_calls;
uint32_t aarch64_timer_mock_register_intid;
uint32_t aarch64_timer_mock_ppi = 30U;
int      aarch64_timer_mock_tick_calls;
gic_handler_fn aarch64_timer_mock_handler;
uint64_t      aarch64_timer_mock_handler_param;

uint32_t aarch64_timer_mock_registered[AARCH64_TIMER_MOCK_MAX_INTIDS];
int      aarch64_timer_mock_registered_count;

int mock_lock_depth;
int mock_lock_max_depth;
int mock_lock_nesting_violations;
int mock_lock_ops;
int mock_assert_failures;

char aarch64_timer_log_buf[AARCH64_TIMER_LOG_MAX][AARCH64_TIMER_LOG_LEN];
int  aarch64_timer_log_count;
int  aarch64_timer_log_info_count;
int  aarch64_timer_log_err_count;

/* ── Current-task accessor (poisonable) ────────────────────────── */
task_t *mock_current_read(void)
{
    mock_current_reads++;
    if (mock_current_poisoned)
        return NULL;   /* a poisoned accessor must never be reached */
    return mock_sched_current;
}

void mock_lock_record(spinlock_T *l, int is_lock)
{
    (void)l;
    mock_lock_ops++;
    if (is_lock) {
        if (mock_lock_depth > 0)
            mock_lock_nesting_violations++;
        mock_lock_depth++;
        if (mock_lock_depth > mock_lock_max_depth)
            mock_lock_max_depth = mock_lock_depth;
    } else {
        if (mock_lock_depth > 0)
            mock_lock_depth--;
    }
}

void mock_assert_failed(const char *expr, const char *file, int line)
{
    mock_assert_failures++;
    printf("  [ASSERT] %s:%d: %s\n", file, line, expr ? expr : "?");
}

uint64_t aarch64_smp_accepted_mask(void)
{
    return mock_accepted_mask;
}

/* ── Machine boundary ──────────────────────────────────────────── */
static void aarch64_timer_trace_record(enum aarch64_timer_op op, uint64_t value)
{
    if (aarch64_timer_trace_count < AARCH64_TIMER_TRACE_MAX) {
        aarch64_timer_trace[aarch64_timer_trace_count].op = op;
        aarch64_timer_trace[aarch64_timer_trace_count].value = value;
    }
    aarch64_timer_trace_count++;
}

uint64_t cntfrq_el0(void)
{
    aarch64_timer_trace_record(AARCH64_TIMER_OP_FREQ_READ,
                                aarch64_timer_mock_freq);
    return aarch64_timer_mock_freq;
}

void cntp_tval_el0_write(uint64_t v)
{
    aarch64_timer_mock_tval = v;
    aarch64_timer_trace_record(AARCH64_TIMER_OP_TVAL_WRITE, v);
}

uint64_t cntp_ctl_el0_read(void)
{
    uint64_t v = aarch64_timer_mock_ctl;
    if (aarch64_timer_mock_readback_fails)
        v &= ~1UL;   /* the counter refuses to report ENABLE */
    aarch64_timer_trace_record(AARCH64_TIMER_OP_CTL_READ, v);
    return v;
}

void cntp_ctl_el0_write(uint64_t v)
{
    if (aarch64_timer_mock_imask_sticky)
        v |= (1UL << 1);   /* the register refuses to clear IMASK */
    aarch64_timer_mock_ctl = v;
    aarch64_timer_trace_record(AARCH64_TIMER_OP_CTL_WRITE, v);
}

void cntp_isb(void)
{
    aarch64_timer_trace_record(AARCH64_TIMER_OP_ISB, 0);
}

int gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param,
                         const char *name)
{
    (void)name;
    aarch64_timer_mock_register_calls++;
    aarch64_timer_mock_register_intid = intid;
    if (aarch64_timer_mock_register_rc != 0)
        return aarch64_timer_mock_register_rc;
    for (int i = 0; i < aarch64_timer_mock_registered_count; i++) {
        if (aarch64_timer_mock_registered[i] == intid)
            return -2;   /* already registered */
    }
    if (aarch64_timer_mock_registered_count < AARCH64_TIMER_MOCK_MAX_INTIDS) {
        aarch64_timer_mock_registered[aarch64_timer_mock_registered_count++] =
            intid;
    }
    aarch64_timer_mock_handler = fn;
    aarch64_timer_mock_handler_param = param;
    return 0;
}

uint32_t dtb_cntp_ppi(void)
{
    return aarch64_timer_mock_ppi;
}

void tick_handler(void)
{
    aarch64_timer_mock_tick_calls++;
}

void aarch64_timer_mock_log(const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (aarch64_timer_log_count < AARCH64_TIMER_LOG_MAX) {
        vsnprintf(aarch64_timer_log_buf[aarch64_timer_log_count],
                  AARCH64_TIMER_LOG_LEN, fmt, ap);
    }
    aarch64_timer_log_count++;
    va_end(ap);
    if (level[0] == 'i')
        aarch64_timer_log_info_count++;
    else if (level[0] == 'e')
        aarch64_timer_log_err_count++;
}

void aarch64_timer_mock_reset(void)
{
    memset(aarch64_timer_mock_registered, 0,
           sizeof(aarch64_timer_mock_registered));
    aarch64_timer_mock_registered_count = 0;
    aarch64_timer_mock_register_calls = 0;
    aarch64_timer_mock_register_intid = 0;
    aarch64_timer_mock_register_rc = 0;
    aarch64_timer_mock_handler = NULL;
    aarch64_timer_mock_handler_param = 0;
    aarch64_timer_mock_freq = 62500000ULL;
    aarch64_timer_mock_ctl = 0;
    aarch64_timer_mock_tval = 0;
    aarch64_timer_mock_readback_fails = 0;
    aarch64_timer_mock_imask_sticky = 0;
    aarch64_timer_mock_ppi = 30U;
    aarch64_timer_mock_tick_calls = 0;
    aarch64_timer_trace_count = 0;
    memset(aarch64_timer_trace, 0, sizeof(aarch64_timer_trace));
    aarch64_timer_log_count = 0;
    aarch64_timer_log_info_count = 0;
    aarch64_timer_log_err_count = 0;
    memset(aarch64_timer_log_buf, 0, sizeof(aarch64_timer_log_buf));
    mock_lock_depth = 0;
    mock_lock_max_depth = 0;
    mock_lock_nesting_violations = 0;
    mock_lock_ops = 0;
    mock_assert_failures = 0;
    mock_current_reads = 0;
    mock_current_poisoned = 0;
}

#endif /* AARCH64_SCHED_TICK_HOST_TU */

#endif /* AARCH64_SCHED_TICK_RUNTIME_H */
