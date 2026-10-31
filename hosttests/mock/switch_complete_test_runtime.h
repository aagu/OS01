/* Host-only runtime surface for compiling the REAL
 * kernel/sched/kthread.c (+ kernel/sched/boot.c since Phase 2) —
 * aarch64 EL1 scheduler plans Phase 1 Task 2 and Phase 2 Task 1.
 *
 * Pattern follows accounting_test_runtime.h: heavyweight kernel headers
 * are skipped via their include guards and a small controlled surface is
 * provided instead, so the fixture drives the PRODUCTION
 * task_switch_complete() tail and the PRODUCTION
 * kthread_alloc_and_publish() constructor — never a mock of either.
 *
 * Two compilation modes:
 *   - PRODUCTION OBJECT (kernel/sched/kthread.c, kernel/sched/boot.c):
 *     this header is the only preinclude; it skips sched/task.h,
 *     sched/internal.h, memory/slab.h, percpu/percpu.h and
 *     arch/spinlock.h via their guards and provides a mock percpu_t +
 *     extern declarations for every spy (the spies live in the test TU).
 *   - TEST TU (test_switch_complete.c, test_kthread_ctor.c):
 *     FRAMEWORK_INC preincludes test_platform.h FIRST; this header then
 *     DEFINES the whole control/spy layer (the tail spies plus the
 *     constructor's fail-index slab, percpu array, identity/pid/enqueue
 *     spies and the arch_kthread_prepare/drop_files hooks).  Exactly one
 *     test TU is linked per binary, so the non-static definitions here
 *     never collide.
 *
 * The mock percpu_t is intentionally richer than test_platform.h's
 * compile-surface stub (it carries rq_lock / nr_running / min_vruntime /
 * idle): the constructor publishes against those fields, so BOTH TUs
 * must agree on the layout.  percpu_t is therefore macro-mapped onto
 * mock_percpu_t in both modes (test_platform.h's earlier typedef stays
 * unused for these fixtures).
 */
#ifndef SWITCH_COMPLETE_TEST_RUNTIME_H
#define SWITCH_COMPLETE_TEST_RUNTIME_H

#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

/* IRQ/scheduler spies for the production kthread_yield() wrapper. */
#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H
typedef uint64_t arch_irq_state_t;
arch_irq_state_t arch_local_irq_save(void);
void arch_local_irq_restore(arch_irq_state_t state);
#endif
void schedule(void);
void kthread_yield(void);

/* The real <sched/boot.h> is host-safe (stdbool only) and is needed by
 * both modes: the production TUs include it themselves (guard-protected)
 * and the test TU drives the REAL window state machine through it. */
#include <sched/boot.h>

/* ── Constants the skipped headers normally provide ───────────── */
#ifndef STACK_SIZE
#define STACK_SIZE (32 * 1024)
#endif
#ifndef NR_CPUS
#define NR_CPUS 8
#endif

/* Same values as sched/task.h. */
#define PF_SELF_REAP  (1 << 5)
#define PF_KTHREAD    (1 << 0)
#define TASK_RUNNING  (1 << 0)
#define TASK_ZOMBIE   (1 << 3)
#define BLOCKER_NONE  0
#define NSIG          32

/* Same value as sched/internal.h. */
#define EEVDF_MIN_SLICE 10

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

/* ── Fixture thread context ─────────────────────────────────────
 * Small but shaped like the real ones: the arch_kthread_prepare spy
 * writes cr3 / rsp / rsp0 / rip so the tests can assert the hook ran. */
typedef struct thread_struct {
    uint64_t rsp0;
    uint64_t rip;
    uint64_t rsp;
    uint64_t fs;
    uint64_t gs;
    uint64_t cr3;
} thread_t;

typedef struct {
    int type;
    void *check;              /* blocker_check_t stand-in */
    bool signal_can_wake;
} blocker_t;

/* ── Fixture task_t ─────────────────────────────────────────────
 * task_switch_complete() reads flags/thread/fpu_save/stack_alloc_base
 * and compares against current; kthread_alloc_and_publish() touches the
 * identity/metadata/affinity/publication fields below. */
typedef struct task_struct {
    list_t list;
    volatile int64_t state;
    uint64_t flags;

    thread_t *thread;
    void *fpu_save;
    void *stack_alloc_base;
    void *files;              /* opaque — owned by the arch spies */
    void *mm;                 /* opaque */

    int64_t pid;
    int64_t pgrp;
    int64_t session;
    int64_t signal;
    int64_t blocked;
    int64_t priority;
    int64_t exit_code;
    uint64_t addr_limit;

    uint64_t vruntime;
    uint64_t deadline;
    bool on_rq;

    char comm[64];
    uint64_t birth_id;
    uint32_t cpu;
    uint64_t allowed_cpus;

    blocker_t blocker;
    list_t wait_list;
    list_t io_wait_node;

    struct task_struct *parent;

    uint32_t in_schedule;
    uint32_t on_cpu;

    uintptr_t sighand[NSIG];

    int ctty_type;
    void *ctty;
} task_t;

union task_union {
    task_t task;
    char stack[STACK_SIZE];
};

/* ── Spin surface ───────────────────────────────────────────────
 * test_platform.h already provides this in the TEST TU; the production
 * TU gets the identical (layout-compatible) copy here. */
#ifndef OS01_HOST_SPINLOCK_T_PROVIDED
#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;
/* Lock-order recorder (P5 Task 4 / spec §10.1 item 4): the production
 * constructor takes task_list_lock FIRST and the destination rq_lock
 * second; the recorder (defined in the TEST TU section below) lets the
 * cases assert the observed acquisition/release order.  Only the
 * production spin surface records — the TEST TU's spin functions come
 * from test_platform.h under this same include guard. */
void mock_record_lock_op(spinlock_T *l, int is_lock);
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l) { mock_record_lock_op(l, 1); return 0; }
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f) { (void)f; mock_record_lock_op(l, 0); }
#endif /* OS01_HOST_SPINLOCK_T_PROVIDED */

/* ── Mock percpu ──────────────────────────────────────────────── */
typedef struct mock_percpu {
    uint32_t cpu_id;
    uint32_t online;
    uint32_t scheduler_ok;
    uint32_t need_resched;
    uint32_t nr_running;
    uint64_t min_vruntime;
    spinlock_T rq_lock;
    task_t *idle;
} mock_percpu_t;

/* Both TUs must name this type `percpu_t` with the SAME layout. */
#define percpu_t mock_percpu_t

extern mock_percpu_t percpu_data[NR_CPUS];
extern uint32_t num_cpus;
extern spinlock_T task_list_lock;
extern union task_union init_task_union;

/* Production interfaces under test (from kernel/include/sched headers;
 * declared here for BOTH modes because those headers are skipped via
 * their include guards below). */
void task_switch_complete(task_t *actual_prev);
int kthread_alloc_and_publish(uint64_t (*fn)(uint64_t), uint64_t arg,
                              const char *name, unsigned target_cpu,
                              bool pinned, task_t **out_task, int *out_pid);
int kernel_thread(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t flags);
int kernel_thread_on_cpu(uint64_t (*fn)(uint64_t), uint64_t arg, unsigned cpu);
struct task_struct *create_kthread(uint64_t (*fn)(uint64_t), uint64_t arg,
                                   const char *name);

/* ── Shared control surface (both modes) ──────────────────────── */

/* The task we are "running as": on the next task's stack, current
 * identifies next; for the constructor, current identifies the creator.
 * The fixture repoints this between cases. */
extern task_t *switch_tail_current;
#define current switch_tail_current

/* Spy declarations — DEFINED in the test-TU section below, called by
 * the production objects.  kfree keeps the production <memory/slab.h>
 * signature. */
void task_finish_switch(task_t *prev);
void *kmalloc(size_t size);
size_t kfree(void *address);
int64_t alloc_pid(void);
void task_identity_init(task_t *tsk, const char *name);
void task_comm_set(task_t *tsk, const char *name);
void task_comm_inherit(task_t *child, const task_t *parent);
void enqueue_task(task_t *task, percpu_t *rq);
unsigned sched_pick_cpu_mask(uint64_t allowed);
void sched_notify_remote(task_t *tsk);
int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg);
void arch_kthread_drop_files(task_t *t);

/* Poison sentinels the mock task_finish_switch writes into prev: a tail
 * that reads prev AFTER finish (the read-after-free window) frees one of
 * these instead of the real pointers, which the cases detect. */
#define SWITCH_TAIL_POISON_THREAD ((thread_t *)0xbaad000000000001ULL)
#define SWITCH_TAIL_POISON_FPU    ((void *)0xbaad000000000002ULL)
#define SWITCH_TAIL_POISON_STACK  ((void *)0xbaad000000000003ULL)

#ifdef TEST_PLATFORM_H
/* ══ Test TU: the whole control/spy layer is DEFINED here ════════ */

/* ── Switch-tail spies (Phase 1) ── */
task_t *switch_tail_current;
int spy_finish_calls;        /* task_finish_switch invocations */
task_t *spy_finish_last;     /* its last argument */
int spy_seq_finish;          /* op counter value at finish */
int spy_kfree_calls;         /* kfree invocations (incl. overflow) */
void *spy_kfreed[8];         /* the first 8 freed pointers */
int spy_seq_kfree[8];        /* op counter value at each kfree */
static int op_counter;

/* ── Constructor-harness state (Phase 2) ── */

/* Fail-index slab: kmalloc returns NULL on the mock_kmalloc_fail_at-th
 * call (1-based; 0 = never fail).  Live bytes are tracked through a
 * small pointer table so kfree can decrement exactly what kmalloc
 * handed out — "zero live bytes" is the rollback assertion. */
int mock_kmalloc_calls;
int mock_kmalloc_fail_at;
size_t mock_kmalloc_live_bytes;
void *mock_kmalloc_live[16];
size_t mock_kmalloc_live_size[16];

/* percpu / globals the production objects reference. */
mock_percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = 1;
spinlock_T task_list_lock;
union task_union init_task_union;

/* Window bookkeeping through the REAL sched/boot.c state machine is
 * driven by calling sched_boot_creation_begin()/sched_boot_start(). */

/* arch hook control: mock_arch_prepare_rc is returned by the
 * arch_kthread_prepare spy; mock_arch_files_live emulates the hook-owned
 * file-table resource so the tests can assert it is not leaked. */
int mock_arch_prepare_rc;
int mock_arch_prepare_calls;
int mock_arch_drop_files_calls;
int mock_arch_files_live;
void *mock_arch_files_token = (void *)0xfeed0001ULL;
int mock_arch_fpu_live;
void *mock_arch_fpu_token;

/* Enqueue / placement spies. */
int mock_enqueue_calls;
int mock_enqueue_refused;    /* enqueues refused by the affinity mask */
int mock_pick_calls;
int mock_notify_remote_calls;
int64_t mock_alloc_pid_next = 1;

/* Lock-order recorder state (filled from the PRODUCTION spin surface). */
spinlock_T *mock_lock_log_addr[16];
int mock_lock_log_is_lock[16];
int mock_lock_log_count;

void mock_record_lock_op(spinlock_T *l, int is_lock)
{
    if (mock_lock_log_count < 16) {
        mock_lock_log_addr[mock_lock_log_count] = l;
        mock_lock_log_is_lock[mock_lock_log_count] = is_lock;
    }
    mock_lock_log_count++;
}

/* Creator fixture: `current` while the constructor runs. */
static task_t creator_task;

void task_finish_switch(task_t *prev)
{
    spy_finish_calls++;
    spy_finish_last = prev;
    spy_seq_finish = ++op_counter;
    /* Release: after on_cpu clears, a waiter may free prev and the slab
     * may hand the memory to anyone.  Anything the tail reads from prev
     * past this point is a use-after-free — poison the fields so a late
     * read is detectable in the freed-pointer log. */
    prev->flags = 0;
    prev->thread = SWITCH_TAIL_POISON_THREAD;
    prev->fpu_save = SWITCH_TAIL_POISON_FPU;
    prev->stack_alloc_base = SWITCH_TAIL_POISON_STACK;
}

void *kmalloc(size_t size)
{
    mock_kmalloc_calls++;
    if (mock_kmalloc_fail_at == mock_kmalloc_calls)
        return NULL;
    void *p = malloc(size);
    if (!p)
        return NULL;
    for (int i = 0; i < 16; i++) {
        if (!mock_kmalloc_live[i]) {
            mock_kmalloc_live[i] = p;
            mock_kmalloc_live_size[i] = size;
            mock_kmalloc_live_bytes += size;
            return p;
        }
    }
    free(p);            /* table full — harness bug, not a leak path */
    return NULL;
}

size_t kfree(void *address)
{
    if (spy_kfree_calls < 8) {
        spy_kfreed[spy_kfree_calls] = address;
        spy_seq_kfree[spy_kfree_calls] = ++op_counter;
    }
    spy_kfree_calls++;
    if (address && address == mock_arch_fpu_token) {
        mock_arch_fpu_token = NULL;
        mock_arch_fpu_live--;
    }
    for (int i = 0; i < 16; i++) {
        if (mock_kmalloc_live[i] == address) {
            mock_kmalloc_live_bytes -= mock_kmalloc_live_size[i];
            mock_kmalloc_live[i] = NULL;
            mock_kmalloc_live_size[i] = 0;
            free(address);
            return 0;
        }
    }
    /* Not slab-tracked (e.g. a static fixture pointer): record only —
     * the Phase 1 tail cases deliberately free static fixtures. */
    return 0;
}

int64_t alloc_pid(void)
{
    return mock_alloc_pid_next++;
}

void task_comm_set(task_t *tsk, const char *name)
{
    if (!tsk)
        return;
    if (!name)
        name = "";
    size_t i;
    for (i = 0; i + 1 < sizeof(tsk->comm) && name[i]; i++)
        tsk->comm[i] = name[i];
    tsk->comm[i] = '\0';
}

void task_comm_inherit(task_t *child, const task_t *parent)
{
    memcpy(child->comm, parent->comm, sizeof(child->comm));
    child->comm[sizeof(child->comm) - 1] = '\0';
}

void task_identity_init(task_t *tsk, const char *name)
{
    static uint64_t birth;
    tsk->birth_id = ++birth;
    task_comm_set(tsk, name);
}

/* Mirrors fair.c's enqueue_task contract: the affinity mask gates the
 * runqueue, then on_rq is published and nr_running advances. */
void enqueue_task(task_t *task, percpu_t *rq)
{
    mock_enqueue_calls++;
    if (rq->cpu_id < NR_CPUS &&
        ((task->allowed_cpus >> rq->cpu_id) & 1ULL) == 0) {
        mock_enqueue_refused++;
        return;
    }
    task->on_rq = true;
    rq->nr_running++;
}

/* Mirrors balance.c's sched_pick_cpu_mask: online && scheduler_ok &&
 * allowed bit; fewest nr_running wins; deterministic on ties (lowest
 * cpu index).  Returns NR_CPUS when nothing matches. */
unsigned sched_pick_cpu_mask(uint64_t allowed)
{
    mock_pick_calls++;
    if (allowed == 0)
        return NR_CPUS;
    unsigned best = NR_CPUS;
    uint32_t min_nr = UINT32_MAX;
    for (uint32_t i = 0; i < NR_CPUS; i++) {
        if (((allowed >> i) & 1ULL) == 0)
            continue;
        if (i >= num_cpus)
            break;
        if (!percpu_data[i].online || !percpu_data[i].scheduler_ok)
            continue;
        if (percpu_data[i].nr_running < min_nr) {
            min_nr = percpu_data[i].nr_running;
            best = i;
        }
    }
    return best;
}

void sched_notify_remote(task_t *tsk)
{
    (void)tsk;
    mock_notify_remote_calls++;
}

int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg)
{
    mock_arch_prepare_calls++;
    /* Success: acquire the hook-owned file-table resource and build the
     * first-entry context (x86's real hook writes the same fields). */
    mock_arch_files_live++;
    t->files = mock_arch_files_token;
    t->fpu_save = kmalloc(256);
    if (!t->fpu_save) {
        t->files = NULL;
        mock_arch_files_live--;
        return -ENOMEM;
    }
    mock_arch_fpu_token = t->fpu_save;
    mock_arch_fpu_live++;
    if (mock_arch_prepare_rc != 0) {
        /* Model a later preparation failure after acquiring both hook
         * resources; the hook owns cleanup on this path. */
        kfree(t->fpu_save);
        t->fpu_save = NULL;
        t->files = NULL;
        mock_arch_files_live--;
        return mock_arch_prepare_rc;
    }
    t->thread->cr3 = 0x1000;
    t->thread->rip = 0x2000;
    t->thread->rsp = 0x3000;
    t->thread->rsp0 = 0x3000;
    (void)fn;
    (void)arg;
    return 0;
}

void arch_kthread_drop_files(task_t *t)
{
    mock_arch_drop_files_calls++;
    if (t->files == mock_arch_files_token) {
        t->files = NULL;
        mock_arch_files_live--;
    }
}

/* kthread_exit() references schedule(); the exit path itself is a
 * Phase 4 QEMU case, so this is an inert stub. */
int mock_schedule_calls;
int mock_irq_restore_calls;
int mock_restore_after_schedule;
void schedule(void) { mock_schedule_calls++; }

arch_irq_state_t mock_irq_state;
arch_irq_state_t arch_local_irq_save(void)
{
    arch_irq_state_t old = mock_irq_state;
    mock_irq_state = 1;
    return old;
}
void arch_local_irq_restore(arch_irq_state_t state)
{
    mock_irq_state = state;
    mock_irq_restore_calls++;
    if (mock_schedule_calls)
        mock_restore_after_schedule = 1;
}

void switch_tail_reset(void)
{
    spy_finish_calls = 0;
    spy_finish_last = NULL;
    spy_seq_finish = 0;
    spy_kfree_calls = 0;
    op_counter = 0;
    for (int i = 0; i < 8; i++) {
        spy_kfreed[i] = NULL;
        spy_seq_kfree[i] = 0;
    }

    /* Constructor harness: destroy any task still on the fixture list
     * (a completed case leaves its created task published) and reset
     * every counter/gate.  The task_t lives INSIDE the raw task-union
     * allocation (stack_alloc_base), so only the raw base and the
     * thread struct are freed — never the interior task pointer. */
    if (init_task_union.task.list.next == NULL)
        list_init(&init_task_union.task.list);
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        list_t *nxt = pos->next;
        task_t *t = (task_t *)((char *)pos - __builtin_offsetof(task_t, list));
        list_del(pos);
        if (t->thread)
            free(t->thread);
        free(t->stack_alloc_base);
        pos = nxt;
    }
    memset(percpu_data, 0, sizeof(percpu_data));
    for (int i = 0; i < NR_CPUS; i++)
        percpu_data[i].cpu_id = (uint32_t)i;

    mock_kmalloc_calls = 0;
    mock_kmalloc_fail_at = 0;
    mock_kmalloc_live_bytes = 0;
    memset(mock_kmalloc_live, 0, sizeof(mock_kmalloc_live));
    memset(mock_kmalloc_live_size, 0, sizeof(mock_kmalloc_live_size));

    mock_arch_prepare_rc = 0;
    mock_arch_prepare_calls = 0;
    mock_arch_drop_files_calls = 0;
    mock_arch_files_live = 0;
    mock_arch_fpu_live = 0;
    mock_arch_fpu_token = NULL;

    mock_enqueue_calls = 0;
    mock_enqueue_refused = 0;
    mock_pick_calls = 0;
    mock_notify_remote_calls = 0;

    mock_lock_log_count = 0;
    memset(mock_lock_log_addr, 0, sizeof(mock_lock_log_addr));
    memset(mock_lock_log_is_lock, 0, sizeof(mock_lock_log_is_lock));

    /* creator fixture: a plain running task on CPU0. */
    memset(&creator_task, 0, sizeof(creator_task));
    creator_task.state = TASK_RUNNING;
    creator_task.pid = 77;
    creator_task.pgrp = 1;
    creator_task.session = 1;
    creator_task.vruntime = 5;
    creator_task.cpu = 0;
    task_comm_set(&creator_task, "creator");
    switch_tail_current = &creator_task;
}

#else /* ══ Production object: skip the heavyweight headers ════════ */

/* Skip the real kernel headers via their include guards.  sched/boot.h
 * is intentionally NOT skipped: it is host-safe and is the real window
 * interface. */
#define KERNEL_TASK_H
#define KERNEL_SCHED_INTERNAL_H
#define _KERNEL_SLAB_H
#define _KERNEL_PERCPU_H
#define _ARCH_SPINLOCK_H

/* kthread_exit() (compiled into the production object, exercised only
 * in Phase 4) references schedule(); the one inline the exit path
 * needs beyond that is arch_cpu_halt. */
void schedule(void);
static inline void arch_cpu_halt(void) { }

#endif /* TEST_PLATFORM_H */

#endif /* SWITCH_COMPLETE_TEST_RUNTIME_H */
