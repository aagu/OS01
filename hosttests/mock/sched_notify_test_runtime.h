/* Host-only runtime surface for compiling the REAL
 * kernel/sched/core.c + kernel/sched/balance.c (P1 Task 1: AP
 * cooperative scheduling contracts) and the REAL per-arch
 * kernel/arch/{x86_64,aarch64}/cpu/sched_notify.c backends.
 *
 * Pattern follows switch_complete_test_runtime.h: heavyweight kernel
 * headers are skipped via their include guards and a small controlled
 * surface is provided instead, so the fixture drives the PRODUCTION
 * sched_notify_remote_cpu(), the PRODUCTION blocker_wait()/schedule()
 * scan gating and the PRODUCTION arch backends — never a mock of any
 * of them.
 *
 * Two compilation modes:
 *   - PRODUCTION OBJECT (kernel/sched/core.c, kernel/sched/balance.c,
 *     kernel/arch/x86_64/cpu/sched_notify.c,
 *     kernel/arch/aarch64/cpu/sched_notify.c): this header is the only
 *     preinclude; it skips sched/task.h, sched/internal.h, sched/diag.h,
 *     percpu/percpu.h, memory/slab.h, arch/spinlock.h, arch/irq.h,
 *     arch/cpu.h, core/debug.h, core/panic.h, log/log.h, kernel.h,
 *     core/assert.h and intr/ipi.h via their guards and provides the
 *     surface below.  The REAL <arch/sched.h> facade header is small
 *     and host-safe: it is NOT skipped.
 *   - TEST TU (test_sched_notify.c and the two backend mini-TUs):
 *     FRAMEWORK_INC preincludes test_platform.h FIRST (defining
 *     TEST_PLATFORM_H); this header then DEFINES the whole control/spy
 *     layer.  Exactly one test TU is linked per binary.
 *
 * core.c DEFINES init_mm / init_task_union / init_task / task_list_lock /
 * task_list_next / task_list_add / user_init_task / user_init_pid /
 * alloc_pid / task_wake / blocker_wake / blocker_wait /
 * sched_unblock_blocked / task_finish_switch / schedule — the test TU
 * never redefines any of those.
 */
#ifndef SCHED_NOTIFY_TEST_RUNTIME_H
#define SCHED_NOTIFY_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

/* ── Skip the real kernel headers via their include guards ─────── */
#define KERNEL_TASK_H
#define KERNEL_SCHED_INTERNAL_H
#define KERNEL_SCHED_DIAG_H
#define _KERNEL_PERCPU_H
#define _KERNEL_SLAB_H
#define _ARCH_SPINLOCK_H
#define _ARCH_IRQ_H
#define _ARCH_CPU_H
#define _KERNEL_DEBUG_H
#define _KERNEL_PANIC_H
#define _KERNEL_HANG_H
#define _KERNEL_LOG_H
#define _KERNEL_H
#define _KERNEL_ASSERT_H
#define _KERNEL_IPI_H

/* ── Constants the skipped headers normally provide ───────────── */
#include <config/limits.h>
#ifndef STACK_SIZE
#define STACK_SIZE (32 * 1024)
#endif

/* Same values as sched/task.h. */
#define PF_SELF_REAP  (1 << 5)
#define PF_KTHREAD    (1 << 0)
#define TASK_RUNNING       (1 << 0)
#define TASK_INTERRUPTIBLE (1 << 1)
#define TASK_UNINTERRUPTIBLE (1 << 2)
#define TASK_ZOMBIE   (1 << 3)
#define BLOCKER_NONE      0
#define BLOCKER_WAITPID   1
#define BLOCKER_NANOSLEEP 2
#define CTTY_NONE 0
#define CTTY_PHYS 1
#define CTTY_PTY  2

/* Same values as sched/internal.h and core/panic.h. */
#define EEVDF_MIN_SLICE 10
#define EEVDF_LATENCY   40
#define HANG_THRESHOLD  50

/* Same values as intr/ipi.h. */
#define IPI_VECTOR_TLB     0x40
#define IPI_VECTOR_RESCHED 0x41

typedef int pid_t;

/* ── Fixture list (mirrors libc/include/list.h semantics) ─────── */
typedef struct mock_list {
    struct mock_list *next, *prev;
} list_t;

static inline void list_init(list_t *l)
{
    l->next = l;
    l->prev = l;
}

static inline void list_add_to_before(list_t *entry, list_t *new_entry)
{
    new_entry->next = entry;
    new_entry->prev = entry->prev;
    entry->prev->next = new_entry;
    entry->prev = new_entry;
}

static inline void list_del(list_t *entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
}

/* ── rbtree surface: stubbed (the notify/scan cases never enqueue for
 *    real — pick_eevdf/dequeue/enqueue are test spies). ─────────── */
typedef struct { uint64_t stub; } rbtree_node_t;
typedef struct { uint64_t stub; } rbtree_root_t;
void *rbtree_last(rbtree_root_t *root);
void *rbtree_prev(void *node);
void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node);

/* ── Spin surface ───────────────────────────────────────────────
 * Production objects get the recording copy (mock_record_lock_op is
 * DEFINED in the test TU); the TEST TU's spin functions come from
 * test_platform.h under the same guard. */
#ifndef OS01_HOST_SPINLOCK_T_PROVIDED
#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;
void mock_record_lock_op(spinlock_T *l, int is_lock);
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { mock_record_lock_op(l, 1); }
static inline void spin_unlock(spinlock_T *l) { mock_record_lock_op(l, 0); }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) { mock_record_lock_op(l, 1); return 0; }
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) { (void)f; mock_record_lock_op(l, 0); }
#endif

typedef struct mm_struct {
    spinlock_T lock;
} mm_t;

/* ── Fixture thread context / blocker / task_t ──────────────────
 * Field-for-field the names core.c's schedule()/task_wake()/
 * blocker_wait() paths touch, plus everything INIT_TASK initialises. */
typedef struct thread_struct {
    uint64_t sched_flags;
} thread_t;

/* Forward declaration REQUIRED before the callback typedef: a struct
 * tag first named inside a function prototype would create a
 * prototype-scope type incompatible with the file-scope task_t below
 * (the real sched/task.h forward-declares for the same reason). */
struct task_struct;
typedef bool (*blocker_check_t)(struct task_struct *waiter);

typedef struct {
    int type;
    blocker_check_t check;
    bool signal_can_wake;
} blocker_t;

typedef struct task_struct {
    list_t list;
    volatile int64_t state;
    uint64_t flags;

    mm_t *mm;
    thread_t *thread;
    uint64_t addr_limit;

    int64_t pid;
    int64_t pgrp;
    int64_t session;
    int64_t counter;
    int64_t signal;
    int64_t blocked;
    int64_t priority;

    rbtree_node_t rb_node;
    uint64_t vruntime;
    uint64_t deadline;
    bool on_rq;

    char comm[64];
    uint32_t cpu;
    uint64_t allowed_cpus;

    blocker_t blocker;

    struct task_struct *parent;
    int64_t exit_code;
    list_t wait_list;
    list_t io_wait_node;

    uint32_t in_schedule;
    uint32_t on_cpu;

    void *ctty;
    int ctty_type;
} task_t;

typedef union task_union {
    task_t task;
    char stack[STACK_SIZE];
} task_union_t;

/* INIT_TASK: same field set the real sched/task.h macro initialises. */
#define INIT_TASK(task)               \
{                                     \
    .state = TASK_UNINTERRUPTIBLE,    \
    .flags = PF_KTHREAD,              \
    .list = {&(task).list, &(task).list}, \
    .mm = &mock_init_mm,              \
    .thread = &init_thread,           \
    .addr_limit = 0xffff800000000000, \
    .blocked = 0,                     \
    .pid = 0,                         \
    .pgrp = 1,                        \
    .session = 1,                     \
    .counter = 0,                     \
    .signal = 0,                      \
    .priority = 2,                    \
    .ctty_type = CTTY_NONE,           \
    .ctty = NULL,                     \
    .comm = "idle",                   \
    .allowed_cpus = 1ULL,             \
}

/* ── Mock percpu ────────────────────────────────────────────────
 * Field-for-field the names the production objects touch, including
 * arch_processor_id (the x86 backend's IPI target).  Both modes name
 * the type percpu_t with the SAME layout. */
typedef struct mock_percpu {
    uint32_t cpu_id;
    uint32_t arch_processor_id;
    uint32_t online;
    uint32_t scheduler_ok;
    uint32_t ipi_ready;      /* P2: the kick backend's admission gate */
    uint64_t need_resched;
    rbtree_root_t run_queue;
    task_t *idle;
    uint64_t schedule_count;
    uint64_t min_vruntime;
    spinlock_T rq_lock;
    uint32_t nr_running;
    uint64_t watchdog_counter;
} mock_percpu_t;

#define percpu_t mock_percpu_t

extern mock_percpu_t percpu_data[NR_CPUS];
extern uint32_t num_cpus;
extern spinlock_T task_list_lock;
extern task_union_t init_task_union;
extern mm_t mock_init_mm;
extern thread_t init_thread;
extern task_t *user_init_task;
extern int64_t user_init_pid;

/* this_cpu/cpu_id resolve through a repointable fixture pointer.
 * Macro-mapped (not redefined as functions): test_platform.h already
 * defines inert static inlines with those names in the TEST TU, and a
 * macro makes every production call site — including the REAL backend
 * objects — resolve through the fixture. */
extern percpu_t *mock_sched_this_cpu;
static inline percpu_t *mock_sched_this_cpu_ptr(void) { return mock_sched_this_cpu; }
static inline uint32_t mock_sched_cpu_id(void)
{
    return mock_sched_this_cpu ? mock_sched_this_cpu->cpu_id : 0;
}
#define this_cpu() mock_sched_this_cpu_ptr()
#define cpu_id() mock_sched_cpu_id()

/* ── IRQ surface (spies defined in the test TU) ───────────────── */
typedef uint64_t arch_irq_state_t;
arch_irq_state_t arch_local_irq_save(void);
void arch_local_irq_restore(arch_irq_state_t state);
void arch_local_irq_enable(void);

/* idle_task_resume's halt (skipped <arch/cpu.h>); the P2 kick backend
 * fail-stops through it and NEVER RETURNS in production.  On the host
 * an armed halt longjmps back to the test point (the production loop
 * would otherwise park the runner). */
extern int mock_halt_armed;
extern void (*mock_halt_escape)(void);
static inline void arch_cpu_halt(void)
{
    if (mock_halt_armed) {
        mock_halt_armed = -1;   /* observed stop */
        if (mock_halt_escape)
            mock_halt_escape();
    }
    for (;;)
        ;   /* production semantics: the CPU never resumes */
}

/* ── Production interfaces under test ─────────────────────────── */
void schedule(void);
int blocker_wait(blocker_check_t check, int type, bool signal_can_wake);
void blocker_wake(task_t *task);
void task_wake(task_t *t);
void task_finish_switch(task_t *prev);
void sched_notify_remote_cpu(unsigned cpu);
void sched_notify_remote(task_t *tsk);
unsigned sched_pick_cpu_mask(uint64_t allowed);
void sched_balance(percpu_t *rq);

/* fair.c stand-ins (NOT linked in this fixture — the spies are the
 * test's observation points; no scheduler algorithm is copied). */
void update_curr(task_t *task);
void enqueue_task(task_t *task, percpu_t *rq);
void dequeue_task(task_t *task, percpu_t *rq);
task_t *pick_eevdf(percpu_t *rq);

/* Switch plumbing spies. */
void mock_arch_task_switch(task_t *prev, task_t *next);
void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void));
void arch_task_check_prev(task_t *t);
#define switch_to(prev, next) mock_arch_task_switch((prev), (next))

/* `current` resolves through a repointable fixture pointer. */
extern task_t *mock_sched_current;
#define current mock_sched_current

/* ── Arch facade hooks (P1 spec §4): declared for both modes; the
 *    common binary's test TU defines controllable spies, the backend
 *    binaries link the REAL per-arch backends. ──────────────────── */
uint64_t arch_kthread_default_mask(void);
void arch_sched_notify_remote(unsigned cpu);
bool arch_sched_scan_blockers(void);

/* x86 IPI surface (spy in the test TU). */
void ipi_send(uint32_t dest_apic_id, uint8_t vector);

/* aarch64 broadcast surface (spy in the test TU; the REAL ipi.c is
 * exercised by the resched-sgi transport test). */
void ipi_broadcast(uint32_t vector, uint64_t target_mask);

/* ── Log surface (counting spies in the test TU) ──────────────── */
void mock_log_err_impl(const char *fmt, ...);
void mock_log_warn_impl(const char *fmt, ...);
void mock_log_info_impl(const char *fmt, ...);
#define log_err(...)  mock_log_err_impl(__VA_ARGS__)
#define log_warn(...) mock_log_warn_impl(__VA_ARGS__)
#define log_info(...) mock_log_info_impl(__VA_ARGS__)
#ifndef OS01_HOST_DEBUG_SCHED
#define OS01_HOST_DEBUG_SCHED
#define debug_sched(...) do { } while (0)
#endif
#define ASSERT(x) do { (void)(x); } while (0)
#define hang_dump_all() mock_hang_dump_all()
void mock_hang_dump_all(void);

#ifndef container_of
#define container_of(ptr, type, member) ({ \
    const typeof( ((type *)0)->member ) *__mptr = (ptr); \
    (type *)( (char *)__mptr - offsetof(type,member) );})
#endif

#ifdef TEST_PLATFORM_H
/* ══ Test TU: the whole control/spy layer is DEFINED here ════════ */

mock_percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = 1;
mm_t mock_init_mm = { .lock = { .lock = 1L } };
thread_t init_thread;
percpu_t *mock_sched_this_cpu;
task_t *mock_sched_current;

/* ── Lock-order recorder ── */
spinlock_T *mock_lock_log_addr[32];
int mock_lock_log_is_lock[32];
int mock_lock_log_count;
void mock_record_lock_op(spinlock_T *l, int is_lock)
{
    if (mock_lock_log_count < 32) {
        mock_lock_log_addr[mock_lock_log_count] = l;
        mock_lock_log_is_lock[mock_lock_log_count] = is_lock;
    }
    mock_lock_log_count++;
}

/* ── IRQ spies ── */
int mock_irq_save_calls, mock_irq_restore_calls, mock_irq_enable_calls;
arch_irq_state_t mock_irq_state;
arch_irq_state_t arch_local_irq_save(void)
{
    mock_irq_save_calls++;
    arch_irq_state_t old = mock_irq_state;
    mock_irq_state = 1;
    return old;
}
void arch_local_irq_restore(arch_irq_state_t state)
{
    mock_irq_restore_calls++;
    mock_irq_state = state;
}
void arch_local_irq_enable(void) { mock_irq_enable_calls++; mock_irq_state = 0; }

/* ── Log counters ── */
int mock_log_err_calls, mock_log_warn_calls, mock_log_info_calls;
int mock_halt_armed;
void (*mock_halt_escape)(void);
void mock_log_err_impl(const char *fmt, ...)  { (void)fmt; mock_log_err_calls++; }
void mock_log_warn_impl(const char *fmt, ...) { (void)fmt; mock_log_warn_calls++; }
void mock_log_info_impl(const char *fmt, ...) { (void)fmt; mock_log_info_calls++; }
void mock_hang_dump_all(void) { }

/* ── Switch plumbing spies ── */
int mock_switch_calls;
task_t *mock_switch_prev, *mock_switch_next;
void mock_arch_task_switch(task_t *prev, task_t *next)
{
    mock_switch_calls++;
    mock_switch_prev = prev;
    mock_switch_next = next;
}
void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void))
{
    (void)t;
    (void)idle_entry;
}
void arch_task_check_prev(task_t *t) { (void)t; }

/* ── fair.c stand-in spies ── */
int mock_update_curr_calls;
void update_curr(task_t *task) { (void)task; mock_update_curr_calls++; }

int mock_enqueue_calls;
task_t *mock_enqueue_last;
percpu_t *mock_enqueue_rq;
void enqueue_task(task_t *task, percpu_t *rq)
{
    mock_enqueue_calls++;
    mock_enqueue_last = task;
    mock_enqueue_rq = rq;
    task->on_rq = true;
    rq->nr_running++;
}
int mock_dequeue_calls;
void dequeue_task(task_t *task, percpu_t *rq)
{
    mock_dequeue_calls++;
    (void)task;
    (void)rq;
    if (rq->nr_running > 0)
        rq->nr_running--;
}
task_t *mock_pick_next;   /* NULL = idle */
task_t *mock_idle_task;
task_t *pick_eevdf(percpu_t *rq)
{
    (void)rq;
    return mock_pick_next ? mock_pick_next : mock_idle_task;
}

/* ── rbtree stubs (balance.c's migration path stays unreachable:
 *    every fixture nr_running is 0, so rbtree_last is never hit) ── */
void *rbtree_last(rbtree_root_t *root) { (void)root; return NULL; }
void *rbtree_prev(void *node) { (void)node; return NULL; }
void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node)
{
    (void)root;
    (void)node;
}

/* ── aarch64 broadcast spy (P2 kick contract) ── */
int mock_ipi_broadcast_calls;
uint32_t mock_broadcast_vector;
uint64_t mock_broadcast_mask;
uint64_t mock_broadcast_need_at_call;   /* dst need_resched snapshot */
void ipi_broadcast(uint32_t vector, uint64_t target_mask)
{
    mock_ipi_broadcast_calls++;
    mock_broadcast_vector = vector;
    mock_broadcast_mask = target_mask;
    if (target_mask && (target_mask & (target_mask - 1)) == 0) {
        unsigned c = __builtin_ctzll(target_mask);
        if (c < NR_CPUS)
            mock_broadcast_need_at_call = percpu_data[c].need_resched;
    }
}

/* ── x86 IPI spy ── */
int mock_ipi_send_calls;
uint32_t mock_ipi_dest_apic_id;
uint8_t mock_ipi_vector;
uint64_t mock_ipi_need_resched_at_send;   /* dst need_resched at call time */
void ipi_send(uint32_t dest_apic_id, uint8_t vector)
{
    mock_ipi_send_calls++;
    mock_ipi_dest_apic_id = dest_apic_id;
    mock_ipi_vector = vector;
}

/* ── Arch facade-hook spies (common binary only; the backend binaries
 *    link the REAL backends and must NOT define these) ──────────── */
#ifdef SCHED_NOTIFY_TEST_COMMON_SPIES
int mock_arch_notify_calls;
unsigned mock_arch_notified_cpu;
uint64_t mock_arch_notify_need_at_hook;
int mock_scan_blockers_calls;
int mock_arch_scan_blockers;   /* <0: derive from cpu_id()==0 */
void arch_sched_notify_remote(unsigned cpu)
{
    mock_arch_notify_calls++;
    mock_arch_notified_cpu = cpu;
    if (cpu < NR_CPUS)
        mock_arch_notify_need_at_hook = percpu_data[cpu].need_resched;
}
bool arch_sched_scan_blockers(void)
{
    mock_scan_blockers_calls++;
    if (mock_arch_scan_blockers >= 0)
        return mock_arch_scan_blockers != 0;
    return cpu_id() == 0;
}
#endif /* SCHED_NOTIFY_TEST_COMMON_SPIES */

/* ── Reset (common binary only: the backend binaries do not link
 *    core.c, so this must not reference init_task_union there) ── */
#ifdef SCHED_NOTIFY_TEST_COMMON_SPIES
void sched_notify_reset(void)
{
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; i++) {
        percpu_data[i].cpu_id = i;
        percpu_data[i].arch_processor_id = i;
        spin_init(&percpu_data[i].rq_lock);
    }
    num_cpus = 1;
    mock_sched_this_cpu = &percpu_data[0];
    mock_lock_log_count = 0;
    memset(mock_lock_log_addr, 0, sizeof(mock_lock_log_addr));
    memset(mock_lock_log_is_lock, 0, sizeof(mock_lock_log_is_lock));
    mock_irq_save_calls = mock_irq_restore_calls = mock_irq_enable_calls = 0;
    mock_irq_state = 0;
    mock_log_err_calls = mock_log_warn_calls = mock_log_info_calls = 0;
    mock_halt_armed = 0;
    mock_halt_escape = NULL;
    mock_switch_calls = 0;
    mock_switch_prev = mock_switch_next = NULL;
    mock_update_curr_calls = 0;
    mock_enqueue_calls = 0;
    mock_enqueue_last = NULL;
    mock_enqueue_rq = NULL;
    mock_dequeue_calls = 0;
    mock_pick_next = NULL;
    mock_idle_task = NULL;
    mock_ipi_send_calls = 0;
    mock_ipi_dest_apic_id = 0;
    mock_ipi_vector = 0;
    mock_ipi_need_resched_at_send = 0;
    mock_ipi_broadcast_calls = 0;
    mock_broadcast_vector = 0;
    mock_broadcast_mask = 0;
    mock_broadcast_need_at_call = 0;
#ifdef SCHED_NOTIFY_TEST_COMMON_SPIES
    mock_arch_notify_calls = 0;
    mock_arch_notified_cpu = 0;
    mock_arch_notify_need_at_hook = 0;
    mock_scan_blockers_calls = 0;
    mock_arch_scan_blockers = -1;
#endif

    /* Fresh idle fixture: init_task_union.task is the idle task on CPU0
     * (core.c's INIT_TASK initialiser already ran via its own definition;
     * the fields below re-publish the scheduling-visible surface). */
    init_task_union.task.state = TASK_RUNNING;
    init_task_union.task.pid = 0;
    init_task_union.task.cpu = 0;
    init_task_union.task.allowed_cpus = 1ULL;
    init_task_union.task.on_rq = false;
    init_task_union.task.on_cpu = 1;
    init_task_union.task.in_schedule = 0;
    init_task_union.task.blocker.type = BLOCKER_NONE;
    init_task_union.task.blocker.check = NULL;
    init_task_union.task.thread = &init_thread;
    list_init(&init_task_union.task.list);
    percpu_data[0].idle = &init_task_union.task;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 1;
    mock_sched_current = &init_task_union.task;
    mock_idle_task = &init_task_union.task;
}
#endif /* SCHED_NOTIFY_TEST_COMMON_SPIES */

#else /* ══ Production object: the surface above is declarations ══ */

/* core.c defines mm_t itself is NOT true — the real mm_t lives in the
 * skipped sched/task.h.  The mock's mm_t (shared section) is the one
 * both the fixture and core.c's `mm_t init_mm = {...}` definition use;
 * nothing further is needed in this branch. */

#endif /* TEST_PLATFORM_H */

#endif /* SCHED_NOTIFY_TEST_RUNTIME_H */
