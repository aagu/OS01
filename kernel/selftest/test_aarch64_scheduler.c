/* Production-path AArch64 EL1 scheduler acceptance cases.
 *
 * Guard: the x86 kernel build collects every C file under selftest/
 * into the compile, and the production aarch64 build compiles this
 * file without OS01_SELFTEST — in both shapes the TU must be empty
 * (the observer hooks and the assembly probes only exist in selftest
 * aarch64 builds). */
#if defined(__aarch64__) && defined(OS01_SELFTEST)

#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/boot/shootdown_probe.h>
#include <selftest/aarch64_scheduler.h>
#ifdef AARCH64_AP_SCHED_WAKE_TEST
#include <selftest/aarch64_ap_scheduler.h>
#endif
#include <selftest/aarch64_sched_observer.h>
#include <selftest/result.h>
#ifdef AARCH64_SCHED_TICK_TEST
#include <selftest/aarch64_sched_tick.h>
#endif
#include <time/timer.h>
#include <memory/slab.h>
#include <log/log.h>
#include <core/printk.h>
#include <errno.h>
#include <string.h>
#include <kernel.h>

enum { CASES = 9, TARGET_YIELDS = 10000, RESOURCE_STACK = 1,
       RESOURCE_THREAD = 2, RESOURCE_FPU = 3 };

static volatile uint64_t yield_counts[2];
static volatile uint64_t yield_done;
static volatile uint64_t entry_count;
static volatile uint64_t entry_arg;
static volatile uint64_t irq_checks;
static volatile uint64_t irq_peer_started;
static volatile uint64_t irq_case_done;
static volatile uint64_t ap_worker_finished;
static volatile uint64_t ap_worker_on_target;
static volatile uint64_t ap_worker_bad_cpu;
static volatile uint64_t ap_worker_bad_affinity;
static volatile uint64_t ap_worker_bad_blocker;
static volatile uint64_t blocker_duplicate_enqueue;
static volatile uint64_t blocker_preswitch_observed;
static volatile uint64_t blocker_preswitch_matches;
static volatile uint64_t blocker_preswitch_state;
static volatile uint64_t blocker_preswitch_on_rq;
static volatile uint64_t entry_arg_ok;
static volatile uint64_t entry_current_ok;
static volatile uint64_t entry_irq_ok;
static volatile uint64_t entry_oncpu_ok;
static volatile uint64_t entry_schedule_ok;
static volatile uint64_t blocker_done;
static volatile uint64_t blocker_ready;
static volatile uint64_t blocker_switch_seen;
static volatile uint64_t blocker_preswitch_armed;
static task_t *volatile blocker_preswitch_task;
static volatile uint64_t blocker_scan_switch_seen;
static task_t *volatile blocker_scan_target;
static volatile uint64_t blocker_scan_wake_seen;
static volatile uint64_t scheduler_observation_active;
static volatile uint64_t cpu0_observation_count;
static volatile uint64_t cpu0_bad_observations;
static volatile uint64_t blocker_scan_mode;
static volatile uint64_t blocker_cond;
static task_t *volatile blocker_target;
static volatile uint64_t created_count;
static volatile uint64_t next_stack_count;
static volatile uint64_t switch_counts[NR_CPUS];
static volatile uint64_t idle_observed;
static volatile uint64_t idle_tick_start;
static volatile uint64_t idle_worker_done;
static volatile uint64_t idle_case_active;
static volatile uint64_t idle_blocked_observed;
static volatile uint64_t blocker_tick_deadline;
static volatile uint64_t irq_nested_seen[NR_CPUS];
static volatile uint64_t irq_frame_seen[NR_CPUS];
static volatile uint64_t irq_frame_bad[NR_CPUS];
static volatile uint64_t timer_irq_active[NR_CPUS];
volatile uint64_t irq_switch_counts[NR_CPUS];
static volatile uint64_t frame_checksums[NR_CPUS];
static volatile uint64_t reg_done;
static volatile uint64_t reg_fail_mask;
static volatile uint64_t reg_status[2];
static volatile uint64_t timer_pressure_until;
volatile uint64_t aarch64_sched_timer_tick_count;
static volatile uint64_t timer_stop;
static volatile uint64_t timer_probe_done;
static volatile uint64_t timer_probe_status;
static volatile uint64_t timer_peer_done;
static volatile uint64_t timer_switch_baseline;
static volatile uint64_t reclaim_batch_active;
uint64_t reclaim_bad_stack;
/* Per-CPU first-entry records written by thread_entry.S (asm site
 * uses TPIDR_EL1 + PERCPU_CPU_ID_OFFSET to locate its slot). */
volatile struct aarch64_sched_entry_record
    aarch64_sched_entry_records[NR_CPUS];

static uint64_t test_yielder(uint64_t arg)
{
    unsigned slot = (unsigned)arg;
    for (uint64_t i = 0; i < TARGET_YIELDS; ++i) {
        yield_counts[slot]++;
        kthread_yield();
    }
    yield_done++;
    return 0;
}

static uint64_t test_entry(uint64_t arg)
{
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    task_t *self = current;
    const volatile struct aarch64_sched_entry_record *rec =
        &aarch64_sched_entry_records[0];
    bool pre_c_ok = self && self->thread && rec->ok &&
        rec->base == (uintptr_t)self &&
        rec->sp >= self->thread->usable_low &&
        rec->sp < self->thread->stack_top &&
        !(rec->sp & 15);
    entry_arg_ok = arg == 0x51a7;
    entry_current_ok = self && self == current && self->cpu == 0 &&
                       self->on_cpu && !self->in_schedule;
    entry_oncpu_ok = self && self->on_cpu;
    entry_schedule_ok = self && !self->in_schedule;
    entry_irq_ok = !(daif & (1u << 7));
    if (entry_arg_ok && pre_c_ok && entry_current_ok && entry_irq_ok)
        entry_arg++;
    entry_count++;
    return 0;
}

static uint64_t reg_worker(uint64_t pattern)
{
    unsigned slot = pattern == 0x13579bdf2468ace0ULL ? 0 : 1;
    int rc = aarch64_sched_reg_probe(pattern);
    reg_status[slot] = (uint64_t)(unsigned)rc;
    reg_fail_mask |= (uint64_t)(unsigned)rc;
    reg_done |= 1u << slot;
    return (uint64_t)rc;
}

/* A waiter body used for explicit wake, the pre-switch lost-wake window,
 * scan-only wake and the production idle pump. */
static bool blocker_condition(task_t *task)
{
    (void)task;
    if (blocker_scan_mode && jiffies >= blocker_tick_deadline)
        blocker_cond = 1;
    if (idle_case_active && blocker_cond && !idle_tick_start)
        idle_tick_start = jiffies;
    return blocker_cond != 0;
}

static uint64_t blocker_waiter(uint64_t unused)
{
    (void)unused;
    if (blocker_scan_mode)
        blocker_scan_target = current;
    blocker_ready = 1;
    int rc = blocker_wait(blocker_condition, BLOCKER_NANOSLEEP, false);
    if (rc == 0 && current->blocker.type == BLOCKER_NONE && !current->on_rq)
        blocker_done++;
    return (uint64_t)rc;
}

static uint64_t blocker_waker(uint64_t unused)
{
    (void)unused;
    /* Determinism gate: blocker_ready is set by the waiter itself
     * immediately before blocker_wait, so (ready && !on_cpu) is
     * reachable ONLY after the waiter's real block switch-out — never
     * from the waiter's never-ran initial state (on_cpu==0 before its
     * first pick would otherwise fake "blocked"). on_cpu stays 1 for
     * the waiter's whole run and drops only in task_finish_switch. */
    uint64_t gate_until = jiffies + 100;
    while (!blocker_ready && jiffies < gate_until)
        kthread_yield();
    uint64_t until = jiffies + 100;
    while (!blocker_switch_seen && blocker_target && blocker_target->on_cpu &&
           jiffies < until)
        kthread_yield();
    if (blocker_target && !blocker_target->on_cpu)
        blocker_switch_seen = 1;
    if (blocker_switch_seen) {
        blocker_cond = 1;
        percpu_t *rq = &percpu_data[blocker_target->cpu];
        uint64_t f = spin_lock_irqsave(&rq->rq_lock);
        uint32_t before = rq->nr_running;
        spin_unlock_irqrestore(&rq->rq_lock, f);
        blocker_wake(blocker_target);
        f = spin_lock_irqsave(&rq->rq_lock);
        uint32_t after = rq->nr_running;
        uint64_t on_rq = blocker_target->on_rq;
        uint64_t cleared = blocker_target->blocker.type == BLOCKER_NONE;
        spin_unlock_irqrestore(&rq->rq_lock, f);
        if (!on_rq || !cleared || after != before + 1)
            blocker_duplicate_enqueue = 1;
    }
    return 0;
}

/* ── Deterministic IRQ four-state fixture ───────────────────────────
 *
 * The four states under test are:
 *
 *   on_switch    IRQs ENABLED + another runnable task -> a REAL switch
 *   off_switch   IRQs MASKED  + another runnable task -> a REAL switch
 *   on_noswitch  IRQs ENABLED + only this task runnable -> NO switch
 *   off_noswitch IRQs MASKED  + only this task runnable -> NO switch
 *
 * The fixture NEVER relies on "yield often enough until the scheduler
 * happens to pick the other task".  The EEVDF order of the two
 * runnable tasks is arranged EXPLICITLY under the runqueue lock, so
 * every pick in the fixture is decided by the arrangement and not by
 * luck:
 *
 *   - irq_order(self, other) puts @other at the runqueue's current
 *     min_vruntime F (deadline F + EEVDF_MIN_SLICE) and @self at
 *     F + 2*EEVDF_MIN_SLICE (deadline F + 3*EEVDF_MIN_SLICE), so
 *     @other is STRICTLY preferred.  A task that is on the runqueue
 *     is DEQUEUED before its vruntime is rewritten and RE-ENQUEUED
 *     afterwards, so the rbtree key (the deadline) is always
 *     recomputed by enqueue_task() — the key is never modified in
 *     place.  A task that is NOT on the runqueue (the blocked peer)
 *     only gets its vruntime rewritten; its deadline is computed when
 *     it is enqueued again.
 *
 *   - the peer SLEEPS in blocker_wait(): it is off the runqueue, so it
 *     can never win a pick by accident, and the worker wakes it
 *     explicitly through the production blocker wake path.
 *
 * The peer exits FIRST.  From the exit handshake on the worker never
 * dereferences the peer pointer again, and the peer's reap is
 * confirmed through the INDEPENDENT resource observer (the released
 * stack counter) — never through the task pointer.
 *
 * The artificial vruntime placement belongs to this fixture ONLY: it
 * is never a production charging path (aarch64 charges the vruntime
 * from the delivered local tick, see kernel/arch/aarch64/cpu/
 * sched_tick.c). */
#define IRQ_OBSERVER_GENERATION 100u

static task_t *volatile irq_peer_task;
static task_t *volatile irq_worker_task;
static volatile uint64_t irq_peer_req;
static volatile uint64_t irq_peer_ack;
static volatile uint64_t irq_peer_exit;
static volatile uint64_t irq_peer_exiting;
static volatile uint64_t irq_peer_reaped;

static bool irq_peer_condition(task_t *task)
{
    (void)task;
    uint64_t req = __atomic_load_n(&irq_peer_req, __ATOMIC_ACQUIRE);
    uint64_t ack = __atomic_load_n(&irq_peer_ack, __ATOMIC_ACQUIRE);
    uint64_t exit_req = __atomic_load_n(&irq_peer_exit, __ATOMIC_ACQUIRE);
    return req != ack || exit_req != 0;
}

/* Put @other strictly before @self on CPU0's runqueue. */
static void irq_order(task_t *self, task_t *other)
{
    percpu_t *rq = &percpu_data[0];
    uint64_t flags = spin_lock_irqsave(&rq->rq_lock);
    uint64_t f = rq->min_vruntime;
    if (other->on_rq) {
        dequeue_task(other, rq);
        other->vruntime = f;
        enqueue_task(other, rq);
    } else {
        other->vruntime = f;
    }
    if (self->on_rq) {
        dequeue_task(self, rq);
        self->vruntime = f + 2 * EEVDF_MIN_SLICE;
        enqueue_task(self, rq);
    } else {
        self->vruntime = f + 2 * EEVDF_MIN_SLICE;
    }
    spin_unlock_irqrestore(&rq->rq_lock, flags);
}

static uint64_t irq_peer(uint64_t unused)
{
    (void)unused;
    irq_peer_started = 1;
    for (;;) {
        /* Sleep until the worker asks for something: the peer must not
         * hold the runqueue while the fixture decides the order. */
        if (blocker_wait(irq_peer_condition, BLOCKER_NANOSLEEP, false) != 0)
            break;
        if (__atomic_load_n(&irq_peer_exit, __ATOMIC_ACQUIRE)) {
            /* Exit handshake: the worker stops dereferencing the peer
             * pointer from here on. */
            __atomic_store_n(&irq_peer_exiting, 1, __ATOMIC_RELEASE);
            break;
        }
        uint64_t req = __atomic_load_n(&irq_peer_req, __ATOMIC_ACQUIRE);
        /* Symmetric arrangement: the worker must be the task that is
         * picked when this peer yields back. */
        irq_order(current, irq_worker_task);
        kthread_yield();
        __atomic_store_n(&irq_peer_ack, req, __ATOMIC_RELEASE);
    }
    return 0;
}

static uint64_t irq_worker(uint64_t unused)
{
    (void)unused;
    uint64_t before, after;
    uint64_t daif_before, daif_after;
    uint64_t deadline = jiffies + 100;

    /* The peer must have reached its blocking point: a peer that is
     * still queued could win a pick by accident. */
    while (!irq_peer_started && jiffies < deadline)
        kthread_yield();
    if (!irq_peer_started)
        return 1;

    /* Baseline of the INDEPENDENT resource observer: the peer's reap
     * is confirmed through the released-stack counter, never through
     * the peer task pointer. */
    uint64_t released_base =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);

    /* ── State 1: IRQs ENABLED, another runnable task -> REAL switch ── */
    before = switch_counts[0];
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif_before));
    irq_order(current, irq_peer_task);
    __atomic_store_n(&irq_peer_req, 1, __ATOMIC_RELEASE);
    blocker_wake(irq_peer_task);
    kthread_yield();
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif_after));
    after = switch_counts[0];
    if (after != before && !(daif_before & (1u << 7)) &&
        !(daif_after & (1u << 7)))
        irq_checks |= 1;

    /* ── State 2: IRQs MASKED, another runnable task -> REAL switch ── */
    {
        arch_irq_state_t flags = arch_local_irq_save();
        before = switch_counts[0];
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_before));
        irq_order(current, irq_peer_task);
        __atomic_store_n(&irq_peer_req, 2, __ATOMIC_RELEASE);
        blocker_wake(irq_peer_task);
        kthread_yield();
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_after));
        after = switch_counts[0];
        uint64_t off_ok = (after != before) && (daif_before & (1u << 7)) &&
                          (daif_after & (1u << 7));
        /* The masked state must be restored when the worker leaves the
         * critical section. */
        arch_local_irq_restore(flags);
        uint64_t daif_restored;
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_restored));
        if (off_ok && !(daif_restored & (1u << 7)))
            irq_checks |= 4;
    }

    /* ── The peer exits: it must be the task that is picked, so the
     *    worker pushes itself behind it before the request. ── */
    irq_order(current, irq_peer_task);
    __atomic_store_n(&irq_peer_exit, 1, __ATOMIC_RELEASE);
    blocker_wake(irq_peer_task);
    while (!irq_peer_exiting && jiffies < deadline)
        kthread_yield();
    while (aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) <
               released_base + 1 && jiffies < deadline)
        kthread_yield();
    if (__atomic_load_n(&irq_peer_exiting, __ATOMIC_ACQUIRE) &&
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) >=
            released_base + 1)
        irq_peer_reaped = 1;
    /* The peer is gone: drop the pointer BEFORE the no-switch states. */
    irq_peer_task = NULL;

    /* ── State 3: IRQs ENABLED, the worker is the ONLY runnable task
     *    -> the yield must NOT switch. ── */
    before = switch_counts[0];
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif_before));
    kthread_yield();
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif_after));
    after = switch_counts[0];
    if (after == before && !(daif_before & (1u << 7)) &&
        !(daif_after & (1u << 7)))
        irq_checks |= 2;

    /* ── State 4: IRQs MASKED, the worker is the ONLY runnable task
     *    -> the yield must NOT switch. ── */
    {
        arch_irq_state_t flags = arch_local_irq_save();
        before = switch_counts[0];
        kthread_yield();
        after = switch_counts[0];
        if (after == before)
            irq_checks |= 8;
        arch_local_irq_restore(flags);
    }

    irq_case_done = 1;
    return 0;
}

static uint64_t ap_cpu_probe_worker(uint64_t target)
{
    unsigned cpu = (unsigned)target;
    if (current->allowed_cpus != (1ULL << cpu))
        ap_worker_bad_affinity++;
    if (cpu_id() == cpu && current->cpu == cpu)
        ap_worker_on_target++;
    else
        ap_worker_bad_cpu++;
    /* P2 (spec §5.2a): the AP blocker_wait refusal is gone — the
     * worker sees the already-true condition return 0 immediately,
     * without ever blocking and with no blocker installed. */
    int rc = blocker_wait(blocker_condition, BLOCKER_NANOSLEEP, false);
    if (rc != 0 || current->state != TASK_RUNNING ||
        current->blocker.type != BLOCKER_NONE)
        ap_worker_bad_blocker++;
    ap_worker_finished++;
    return 0;
}

static uint64_t timer_pressure_worker(uint64_t unused)
{
    (void)unused;
    uint64_t deadline = timer_pressure_until + 100;
    while (jiffies < timer_pressure_until && jiffies < deadline)
        kthread_yield();
    uint64_t target = __atomic_load_n(&aarch64_sched_timer_tick_count,
                                      __ATOMIC_ACQUIRE) + 1;
    uint64_t cycles = arch_cycle_freq();
    uint64_t cycle_deadline = arch_cycle_counter() +
                              (cycles ? cycles * 2 : 100000000);
    log_info("[sched-diag] timer probe start target=%lu ticks=%lu\n",
             (unsigned long)target,
             (unsigned long)__atomic_load_n(&aarch64_sched_timer_tick_count,
                                            __ATOMIC_ACQUIRE));
    timer_probe_status = (uint64_t)(unsigned)aarch64_sched_timer_irq_probe(
        target, cycle_deadline);
    log_info("[sched-diag] timer probe done status=%lu ticks=%lu\n",
             (unsigned long)timer_probe_status,
             (unsigned long)__atomic_load_n(&aarch64_sched_timer_tick_count,
                                            __ATOMIC_ACQUIRE));
    timer_stop = 1;
    uint64_t peer_deadline = jiffies + 100;
    while (!timer_peer_done && jiffies < peer_deadline)
        kthread_yield();
    timer_probe_done = 1;
    return 0;
}

static uint64_t timer_yield_peer(uint64_t unused)
{
    (void)unused;
    while (!timer_stop)
        kthread_yield();
    timer_peer_done = 1;
    return 0;
}

void aarch64_sched_before_block_switch(task_t *task)
{
    if (blocker_preswitch_armed) {
        blocker_preswitch_armed = 0;
        blocker_cond = 1;
        blocker_preswitch_task = task;
        blocker_wake(task);
    }
    if (idle_case_active && task && task->blocker.type != BLOCKER_NONE &&
        task->state == TASK_INTERRUPTIBLE && task->on_cpu)
        idle_blocked_observed = 1;
}

void aarch64_sched_observe_scan_wake(task_t *task)
{
    if (task && task == blocker_scan_target)
        blocker_scan_wake_seen = 1;
}

void aarch64_sched_observe_block_enqueue(task_t *task)
{
    if (!task || task != blocker_preswitch_task || !blocker_cond)
        return;
    unsigned matches = 0;
    percpu_t *rq = &percpu_data[task->cpu];
    uint64_t f = spin_lock_irqsave(&rq->rq_lock);
    for (rbtree_node_t *n = rbtree_first(&rq->run_queue); n; n = rbtree_next(n))
        if (container_of(n, task_t, rb_node) == task)
            matches++;
    if (!blocker_preswitch_observed && task->state == TASK_RUNNING &&
        task->blocker.type == BLOCKER_NONE && task->on_rq && matches == 1) {
        blocker_preswitch_observed = 1;
        blocker_preswitch_matches = matches;
        blocker_preswitch_state = task->state;
        blocker_preswitch_on_rq = task->on_rq;
    }
    if (matches > 1) blocker_duplicate_enqueue = 1;
    spin_unlock_irqrestore(&rq->rq_lock, f);
}

void aarch64_sched_observe_switch_tasks(task_t *prev, task_t *next)
{
    aarch64_sched_observe_switch();
    if (scheduler_observation_active) {
        /* CPU0-target aggregation only (P1 Task 2): a switch whose
         * tasks all belong to CPU0 advances the good counter; work on
         * other CPUs (AP idles, AP-created tasks) is neither counted
         * nor treated as bad, and a CPU0-target switch observed from
         * another CPU is bad. */
        if (cpu_id() == 0 && (!current || current->cpu == 0) &&
            (!prev || prev->cpu == 0) && (!next || next->cpu == 0)) {
            cpu0_observation_count++;
        } else if ((prev && prev->cpu == 0) || (next && next->cpu == 0) ||
                   (current && current->cpu == 0)) {
            cpu0_bad_observations++;
        }
    }
    if (prev == blocker_target && next != prev &&
        prev->state == TASK_INTERRUPTIBLE && prev->blocker.type != BLOCKER_NONE)
        blocker_switch_seen = 1;
    if (prev == blocker_scan_target && next != prev && blocker_scan_wake_seen)
        blocker_scan_switch_seen = 1;
}

static aarch64_sched_reap_observer_t reap_test_observer;

void aarch64_sched_set_reap_observer(aarch64_sched_reap_observer_t cb)
{
    reap_test_observer = cb;
}

void aarch64_sched_observe_reap(task_t *prev, bool next_stack)
{
    /* M4 acceptance hook: runs at the entry, BEFORE the reclaim-batch
     * gate, so the case can check a specific task while it is still
     * alive and still carries PF_SELF_REAP / exit_code. */
    if (reap_test_observer)
        reap_test_observer(prev);

    if (!reclaim_batch_active || !prev)
        return;
    void *raw = prev->stack_alloc_base;
    if (!raw)
        return;
    uintptr_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    bool valid_next = next_stack && current && current != prev && current->thread &&
        (sp & ~(uintptr_t)(STACK_SIZE - 1)) == (uintptr_t)current &&
        sp >= current->thread->usable_low && sp < current->thread->stack_top;
    if (valid_next)
        next_stack_count++;
    else
        reclaim_bad_stack++;
}

void aarch64_sched_observe_switch(void)
{
    /* Per-CPU counters (P1 Task 2): a switch observed on an AP is
     * never folded into a CPU0 case's baseline. */
    unsigned cpu = cpu_id();
    if (cpu < NR_CPUS) {
        switch_counts[cpu]++;
        if (timer_irq_active[cpu]
#ifdef AARCH64_AP_SCHED_WAKE_TEST
            || __atomic_load_n(&aarch64_ap_sched_sgi_active[cpu], __ATOMIC_ACQUIRE)
#endif
            )
            irq_switch_counts[cpu]++;
#ifdef AARCH64_SCHED_TICK_TEST
        /* The per-CPU tick observer: a switch that happened while an
         * interrupt was active must never happen (the tick never
         * switches). */
        if (timer_irq_active[cpu])
            aarch64_sched_tick_irq_switch(cpu);
#endif
    }
}

void aarch64_sched_observe_nested_schedule(void)
{
    unsigned cpu = cpu_id();
    if (cpu < NR_CPUS && timer_irq_active[cpu]) {
        irq_nested_seen[cpu]++;
#ifdef AARCH64_SCHED_TICK_TEST
        /* The per-CPU tick observer: a nested schedule() inside an
         * interrupt must never happen. */
        aarch64_sched_tick_irq_nested(cpu);
#endif
    }
}

void aarch64_sched_observe_timer_irq(struct pt_regs *regs, bool entering)
{
    unsigned cpu = cpu_id();
    if (cpu >= NR_CPUS)
        return;
    if (entering) {
        timer_irq_active[cpu]++;
        /* The GLOBAL tick counter belongs to the BSP alone.  Every
         * admitted CPU now delivers its own CNTP interrupt, so a
         * shared increment would be a data race and would mix the
         * CPUs' samples.  The BSP publishes it with a release RMW;
         * probe.S reads it with LDAR (acquire) and the C readers use
         * an acquire load. */
        if (cpu == 0)
            __atomic_add_fetch(&aarch64_sched_timer_tick_count, 1,
                               __ATOMIC_RELEASE);
        if (regs && !((uintptr_t)regs & 15)) {
            uint64_t *words = (uint64_t *)regs;
            uint64_t sum = 0;
            for (unsigned i = 0; i < 34; ++i) sum ^= words[i] + i;
            frame_checksums[cpu] = sum;
            irq_frame_seen[cpu] = 1;
        }
    } else if (timer_irq_active[cpu]) {
        if (regs && !((uintptr_t)regs & 15)) {
            uint64_t *words = (uint64_t *)regs;
            uint64_t sum = 0;
            for (unsigned i = 0; i < 34; ++i) sum ^= words[i] + i;
            if (sum != frame_checksums[cpu]) {
                irq_frame_bad[cpu] = 1;
                irq_frame_seen[cpu] = 0;
            }
        }
        timer_irq_active[cpu]--;
    }
}

static uint64_t idle_blocker_worker(uint64_t unused)
{
    (void)unused;
    blocker_scan_mode = 1;
    blocker_tick_deadline = jiffies + 2;
    idle_case_active = 1;
    int rc = blocker_wait(blocker_condition, BLOCKER_NANOSLEEP, false);
    idle_worker_done = (rc == 0);
    return (uint64_t)rc;
}

static void idle_pump_until_worker(void)
{
    uint64_t deadline = jiffies + 10;
    while (!idle_worker_done && jiffies < deadline) {
        if (percpu_data[0].idle == current && !current->on_rq &&
            rbtree_empty(&percpu_data[0].run_queue)) {
            if (idle_blocked_observed)
                idle_observed = 1;
        }
        arch_aarch64_idle_step();
    }
}

static unsigned task_list_count(void)
{
    unsigned count = 0;
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    for (list_t *p = init_task_union.task.list.next;
         p != &init_task_union.task.list; p = p->next)
        ++count;
    spin_unlock_irqrestore(&task_list_lock, flags);
    return count;
}

static int case_yield(void)
{
    yield_counts[0] = yield_counts[1] = 0;
    yield_done = 0;
    uint64_t before = switch_counts[0];
    if (!create_kthread(test_yielder, 0, "sched-yield-a") ||
        !create_kthread(test_yielder, 1, "sched-yield-b"))
        return -1;
    uint64_t deadline = jiffies + 500;
    while ((yield_done < 2 || yield_counts[0] < TARGET_YIELDS ||
            yield_counts[1] < TARGET_YIELDS) && jiffies < deadline)
        kthread_yield();
    uint64_t switches = switch_counts[0] - before;
    serial_printk("[sched-evidence] id=aarch64_sched_yield a_yields=%lu b_yields=%lu switches=%lu\n",
                  (unsigned long)yield_counts[0], (unsigned long)yield_counts[1],
                  (unsigned long)switches);
    return yield_done == 2 && yield_counts[0] >= TARGET_YIELDS && yield_counts[1] >= TARGET_YIELDS &&
           switches > 0 ? 0 : -1;
}

static int case_regs(void)
{
    /* C stack sentinels are distinct for each worker and checked after
     * production switches by the assembly probe. */
    reg_done = reg_fail_mask = 0;
    reg_status[0] = reg_status[1] = UINT64_MAX;
    if (!create_kthread(reg_worker, 0x13579bdf2468ace0ULL, "sched-reg-a") ||
        !create_kthread(reg_worker, 0xfedcba9876543210ULL, "sched-reg-b"))
        return -1;
    uint64_t deadline = jiffies + 100;
    while (reg_done != 3 && jiffies < deadline)
        kthread_yield();
    uint64_t reg_fail = ((reg_status[0] | reg_status[1]) & 1) != 0;
    uint64_t stack_fail = ((reg_status[0] | reg_status[1]) & 2) != 0;
    uint64_t sp_fail = ((reg_status[0] | reg_status[1]) & 4) != 0;
    serial_printk("[sched-evidence] id=aarch64_sched_regs reg_fail=%lu stack_fail=%lu sp_fail=%lu\n",
                  (unsigned long)reg_fail, (unsigned long)stack_fail,
                  (unsigned long)sp_fail);
    return reg_done == 3 && reg_status[0] == 0 && reg_status[1] == 0 ? 0 : -1;
}

static int case_entry(void)
{
    entry_count = entry_arg = 0;
    memset((void *)&aarch64_sched_entry_records[0], 0,
           sizeof(aarch64_sched_entry_records[0]));
    if (!create_kthread(test_entry, 0x51a7, "sched-entry"))
        return -1;
    uint64_t deadline = jiffies + 100;
    while (!entry_count && jiffies < deadline)
        kthread_yield();
    uint64_t pre_c_sp_ok = aarch64_sched_entry_records[0].ok &&
        aarch64_sched_entry_records[0].sp >= aarch64_sched_entry_records[0].base &&
        aarch64_sched_entry_records[0].sp <
            aarch64_sched_entry_records[0].base + STACK_SIZE &&
        !(aarch64_sched_entry_records[0].sp & 15);
    uint64_t current_ok = entry_current_ok;
    uint64_t irq_enabled = entry_irq_ok;
    serial_printk("[sched-evidence] id=aarch64_sched_entry entries=%lu arg_ok=%lu pre_c_sp_ok=%lu irq_enabled=%lu current_ok=%lu\n",
                  (unsigned long)entry_count, (unsigned long)entry_arg_ok,
                  (unsigned long)pre_c_sp_ok, (unsigned long)irq_enabled,
                  (unsigned long)current_ok);
    return entry_count == 1 && entry_arg == 1 && pre_c_sp_ok &&
           current_ok && irq_enabled ? 0 : -1;
}

static int case_irq(void)
{
    uint64_t on_switch, on_noswitch, off_switch, off_noswitch;
    irq_checks = 0;
    irq_peer_req = irq_peer_ack = 0;
    irq_peer_exit = irq_peer_exiting = irq_peer_reaped = 0;
    irq_peer_started = 0;
    irq_case_done = 0;
    irq_peer_task = NULL;
    irq_worker_task = NULL;

    /* The reap of the two fixture tasks is confirmed through the
     * INDEPENDENT resource observer: a task pointer is never used as
     * a liveness proof. */
    uint64_t released_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);
    aarch64_sched_observer_begin(0, IRQ_OBSERVER_GENERATION);
    irq_peer_task = create_kthread(irq_peer, 0, "sched-irq-peer");
    if (!irq_peer_task) {
        aarch64_sched_observer_end(0, IRQ_OBSERVER_GENERATION);
        return -1;
    }
    irq_worker_task = create_kthread(irq_worker, 0, "sched-irq");
    if (!irq_worker_task) {
        aarch64_sched_observer_end(0, IRQ_OBSERVER_GENERATION);
        return -1;
    }
    uint64_t deadline = jiffies + 100;
    while (!irq_case_done && jiffies < deadline)
        arch_aarch64_idle_step();
    /* Both fixture tasks must be RELEASED before the batch can end. */
    while (aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) <
               released_before + 2 && jiffies < deadline)
        arch_aarch64_idle_step();
    int observer_failed = aarch64_sched_observer_end(0, IRQ_OBSERVER_GENERATION);
    on_switch = (irq_checks & 1) != 0;
    on_noswitch = (irq_checks & 2) != 0;
    off_switch = (irq_checks & 4) != 0;
    off_noswitch = (irq_checks & 8) != 0;
    log_info("[sched-diag] irq raw observations=%lu done=%lu peer_reaped=%lu observer_failed=%lu idle_state=%lu idle_on_rq=%lu\n",
             (unsigned long)irq_checks, (unsigned long)irq_case_done,
             (unsigned long)irq_peer_reaped, (unsigned long)observer_failed,
             (unsigned long)(current == percpu_data[0].idle &&
                             current->state == TASK_RUNNING),
             (unsigned long)current->on_rq);
    serial_printk("[sched-evidence] id=aarch64_sched_irq on_switch=%lu off_switch=%lu on_noswitch=%lu off_noswitch=%lu\n",
                  (unsigned long)on_switch, (unsigned long)off_switch,
                  (unsigned long)on_noswitch, (unsigned long)off_noswitch);
    return irq_case_done && current == percpu_data[0].idle &&
           current->state == TASK_RUNNING && !current->on_rq &&
           on_switch && off_switch && on_noswitch && off_noswitch &&
           irq_peer_reaped && !observer_failed ? 0 : -1;
}

static int case_blocker(void)
{
    /* Already-true timing is directly checked. The remaining three
     * timings use production blocker_wait/wake and schedule scan paths. */
    blocker_cond = 1;
    uint64_t already = blocker_wait(blocker_condition, BLOCKER_NANOSLEEP,
                                    false) == 0;
    blocker_duplicate_enqueue = blocker_preswitch_observed = 0;
    blocker_preswitch_matches = blocker_preswitch_state = blocker_preswitch_on_rq = 0;
    blocker_cond = 0; blocker_done = blocker_ready = blocker_switch_seen = 0;
    blocker_target = create_kthread(blocker_waiter, 0, "sched-block-waiter");
    if (!blocker_target || !create_kthread(blocker_waker, 0, "sched-block-waker"))
        return -1;
    uint64_t deadline = jiffies + 300;
    while (!blocker_done && jiffies < deadline) kthread_yield();
    /* The waiting task's switch-out is observable through the hook below. */
    uint64_t explicit_wake = blocker_done != 0;
    /* The explicit-subrun waiter has exited and been reaped by the time
     * the loop above observes blocker_done; the pointer is dangling
     * value-only from here on, so drop it before creating the next
     * waiter (observe_switch_tasks compares against blocker_target). */
    blocker_target = NULL;
    blocker_cond = 0; blocker_done = 0; blocker_ready = 0;
    blocker_preswitch_armed = 1;
    if (!create_kthread(blocker_waiter, 0, "sched-block-preswitch")) return -1;
    deadline = jiffies + 100;
    while (!blocker_done && jiffies < deadline) kthread_yield();
    uint64_t pre_switch = blocker_done != 0 && blocker_preswitch_observed;
    blocker_scan_mode = 1; blocker_cond = 0; blocker_done = 0;
    blocker_scan_target = NULL;
    blocker_scan_wake_seen = blocker_scan_switch_seen = 0;
    blocker_tick_deadline = jiffies + 2;
    if (!create_kthread(blocker_waiter, 0, "sched-block-scan")) return -1;
    deadline = jiffies + 100;
    while (!blocker_done && jiffies < deadline) kthread_yield();
    uint64_t scan = blocker_done != 0 && blocker_scan_wake_seen &&
                    blocker_scan_switch_seen;
    uint64_t duplicate_enqueue = blocker_duplicate_enqueue;
    serial_printk("[sched-evidence] id=aarch64_sched_blocker already=%lu explicit=%lu pre_switch=%lu scan=%lu duplicate_enqueue=%lu\n",
                  (unsigned long)already, (unsigned long)explicit_wake,
                  (unsigned long)pre_switch, (unsigned long)scan,
                  (unsigned long)duplicate_enqueue);
    log_info("[sched-diag] blocker requeue observed=%lu matches=%lu state=%lu on_rq=%lu\n",
             (unsigned long)blocker_preswitch_observed,
             (unsigned long)blocker_preswitch_matches,
             (unsigned long)blocker_preswitch_state,
             (unsigned long)blocker_preswitch_on_rq);
    return already && explicit_wake && pre_switch && scan &&
           !duplicate_enqueue ? 0 : -1;
}

static int case_reclaim(void)
{
    /* P1 Task 2: the 1000-task acceptance now runs through the
     * concurrency-safe observer in FOUR batches of 250 (one active
     * batch at a time, separate generations); the original >=1000
     * created, cross-stack reap and zero-leak acceptance is unchanged.
     * The old 1000-slot capacity tables and the allocation_active
     * double truth source are gone. */
    unsigned list_before = task_list_count();
    uint32_t rq_before = percpu_data[0].nr_running;
    created_count = next_stack_count = 0;
    reclaim_bad_stack = 0;
    reclaim_batch_active = 1;
    uint64_t created_before = aarch64_sched_observer_created();
    uint64_t stack_before = aarch64_sched_observer_released(RESOURCE_STACK);
    uint64_t ctx_before = aarch64_sched_observer_released(RESOURCE_THREAD);
    int failed = 0;
    for (unsigned batch = 0; batch < 4 && !failed; ++batch) {
        aarch64_sched_observer_begin(0, batch + 1);
        uint64_t batch_stack_before =
            aarch64_sched_observer_released(RESOURCE_STACK);
        uint64_t batch_created = 0;
        for (unsigned i = 0; i < 250; ++i) {
            if (!create_kthread(test_entry, i, "sched-reclaim")) {
                failed = 1;
                break;
            }
            created_count++;
            batch_created++;
            if ((i & 31) == 31) kthread_yield();
        }
        /* Wait for the REAL cross-stack reaps: the observer's released
         * counters only advance after the actual kfree completed, so
         * "the thread function returned" can never fake this.  The
         * baseline is THIS batch's snapshot (released counters are
         * cumulative across batches). */
        uint64_t deadline = jiffies + 1000;
        while ((aarch64_sched_observer_released(RESOURCE_STACK) -
                batch_stack_before) < batch_created &&
               percpu_data[0].nr_running > rq_before && jiffies < deadline)
            kthread_yield();
        if (aarch64_sched_observer_end(0, batch + 1))
            failed = 1;
    }
    reclaim_batch_active = 0;
    uint64_t stack_freed = aarch64_sched_observer_released(RESOURCE_STACK) -
                           stack_before;
    uint64_t ctx_freed = aarch64_sched_observer_released(RESOURCE_THREAD) -
                         ctx_before;
    uint64_t created_observed = aarch64_sched_observer_created() -
                                created_before;
    unsigned list_after = task_list_count();
    int64_t list_delta_signed = (int64_t)list_after - (int64_t)list_before;
    int64_t rq_delta_signed = (int64_t)percpu_data[0].nr_running - (int64_t)rq_before;
    uint64_t list_delta = list_delta_signed < 0 ? UINT64_MAX : (uint64_t)list_delta_signed;
    uint64_t rq_delta = rq_delta_signed < 0 ? UINT64_MAX : (uint64_t)rq_delta_signed;
    /* Zero-leak leg: every observed allocation was really released.
     * obs_created counts stack AND thread records (two per task). */
    uint64_t stack_live = stack_freed != created_count;
    uint64_t thread_live = ctx_freed != created_count;
    uint64_t fpu_live = 0;   /* aarch64 kthreads allocate no FPU area */
    uint64_t next_stack = stack_freed == created_count &&
                          ctx_freed == created_count &&
                          next_stack_count == created_count &&
                          !reclaim_bad_stack;
    serial_printk("[sched-evidence] id=aarch64_sched_reclaim created=%lu stack_live=%lu thread_live=%lu fpu_live=%lu next_stack=%lu list_delta=%lu rq_delta=%lu\n",
                  (unsigned long)created_count, (unsigned long)stack_live,
                  (unsigned long)thread_live, (unsigned long)fpu_live,
                  (unsigned long)next_stack, (unsigned long)list_delta,
                  (unsigned long)rq_delta);
    return created_count >= 1000 && created_observed == 2 * created_count &&
           stack_freed == created_count && ctx_freed == created_count &&
           !stack_live && !thread_live && !fpu_live &&
           next_stack && !list_delta && !rq_delta ? 0 : -1;
}

static int case_idle(void)
{
    idle_observed = idle_tick_start = idle_worker_done = 0;
    idle_blocked_observed = 0;
    blocker_scan_mode = 0; blocker_cond = 0; idle_case_active = 1;
    if (!create_kthread(idle_blocker_worker, 0, "sched-idle-waiter")) return -1;
    /* Schedule the worker once. It blocks and returns control to this
     * idle continuation, which pumps the same production idle step. */
    log_info("[sched-diag] idle case: worker created jiffies=%lu\n",
             (unsigned long)jiffies);
    schedule();
    /* schedule() returns with IRQs masked by contract; the polling
     * idle step requires IRQs ENABLED at entry (the old WFI idle used
     * to re-enable on its sleep path, which masked this). */
    arch_local_irq_enable();
    idle_pump_until_worker();
    uint64_t wake_ticks = jiffies - idle_tick_start;
    idle_case_active = 0;
    uint64_t idle_in_tree = current->on_rq;
    serial_printk("[sched-evidence] id=aarch64_sched_idle idle_in_tree=%lu idle_observed=%lu wake_ticks=%lu\n",
                  (unsigned long)idle_in_tree, (unsigned long)idle_observed,
                  (unsigned long)wake_ticks);
    return !idle_in_tree && idle_observed && idle_worker_done && wake_ticks <= 10
           ? 0 : -1;
}

static int case_timer(void)
{
    uint64_t before = jiffies;
    /* Only CPU0's OWN slots are cleared here, and only with the BSP's
     * interrupts masked: the AP slots belong to the APs, which keep
     * observing their own CNTP interrupts while this case runs (a BSP
     * memset of a live AP slot would be a data race on a slot the AP
     * is still writing). */
    arch_irq_state_t irq_flags = arch_local_irq_save();
    irq_frame_seen[0] = irq_frame_bad[0] = irq_nested_seen[0] = 0;
    irq_switch_counts[0] = 0;
    timer_irq_active[0] = 0;
    __atomic_store_n(&aarch64_sched_timer_tick_count, 0, __ATOMIC_RELEASE);
    arch_local_irq_restore(irq_flags);
    timer_pressure_until = before + 3;
    timer_stop = timer_probe_done = timer_peer_done = 0;
    timer_probe_status = 0;
    timer_switch_baseline = switch_counts[0];
    if (!create_kthread(timer_yield_peer, 0, "sched-timer-yield-peer")) return -1;
    if (!create_kthread(timer_pressure_worker, 0, "sched-timer-pressure")) return -1;
    uint64_t deadline = before + 100;
    while (!timer_probe_done && jiffies < deadline) kthread_yield();
    timer_stop = 1;
    while (!timer_peer_done && jiffies < deadline) kthread_yield();
    uint64_t tick_delta = jiffies - before;
    uint64_t irq_frame_ok = irq_frame_seen[0] && !irq_frame_bad[0] &&
                            timer_probe_done && timer_probe_status == 0;
    uint64_t nested_schedule = irq_nested_seen[0];
    uint64_t irq_switches = irq_switch_counts[0];
    uint64_t pressure_switches = switch_counts[0] - timer_switch_baseline;
    serial_printk("[sched-evidence] id=aarch64_sched_timer tick_delta=%lu irq_frame_ok=%lu nested_schedule=%lu irq_switches=%lu\n",
                  (unsigned long)tick_delta, (unsigned long)irq_frame_ok,
                  (unsigned long)nested_schedule, (unsigned long)irq_switches);
    return tick_delta >= 3 &&
           __atomic_load_n(&aarch64_sched_timer_tick_count, __ATOMIC_ACQUIRE) >= 3 &&
           pressure_switches > 0 && irq_frame_ok && !nested_schedule && !irq_switches
           ? 0 : -1;
}

static int case_ap(void)
{
    /* P1 Task 3: the default create still lands on CPU0, an explicit
     * pin to a ready AP runs there (UP pins its only ready CPU), and
     * every accepted AP publishes scheduler_ok=1 with a quiet
     * runqueue.  Completion waits on worker counters with a jiffies
     * deadline — never on saved self-reaping task pointers. */
    uint32_t count = num_cpus;
    ap_worker_finished = ap_worker_on_target = 0;
    ap_worker_bad_cpu = ap_worker_bad_affinity = ap_worker_bad_blocker = 0;
    blocker_cond = 1;   /* the BSP worker's check returns immediately */
    uint64_t expected = 1;
    if (!create_kthread(ap_cpu_probe_worker, 0, "sched-ap-cpu-proof"))
        return -1;
    if (count >= 2) {
        if (kernel_thread_on_cpu(ap_cpu_probe_worker, 1, 1) < 0)
            return -1;
    } else if (kernel_thread_on_cpu(ap_cpu_probe_worker, 0, 0) < 0) {
        return -1;
    }
    expected++;   /* the explicit pin worker (SMP: an AP; UP: CPU0) */
    uint64_t deadline = jiffies + 200;
    while (ap_worker_finished < expected && jiffies < deadline)
        kthread_yield();
    uint64_t default_cpu0 = ap_worker_finished >= 1 &&
                            ap_worker_on_target >= 1 &&
                            !ap_worker_bad_cpu && !ap_worker_bad_affinity;
    uint64_t pinned_ap_ok = ap_worker_finished == expected &&
                            ap_worker_on_target == expected &&
                            !ap_worker_bad_blocker && !ap_worker_bad_cpu &&
                            !ap_worker_bad_affinity;
    if (count >= 2)
        aarch64_shootdown_probe_run_late();
    uint64_t ap_ready = 1;
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        if (cpu == 0) continue;
        percpu_t *p = &percpu_data[cpu];
        uint32_t sched_ok = __atomic_load_n(&p->scheduler_ok, __ATOMIC_ACQUIRE);
        uint32_t nr = __atomic_load_n(&p->nr_running, __ATOMIC_ACQUIRE);
        uint64_t work_ok = aarch64_shootdown_probe_late_ap_ok(cpu);
        serial_printk("[sched-evidence] id=aarch64_sched_ap cpu=%u scheduler_ok=%u nr_running=%u work_ok=%lu\n",
                      cpu, sched_ok, nr, (unsigned long)work_ok);
        if (!sched_ok || nr || !work_ok) ap_ready = 0;
    }
    log_info("[sched-diag] cpu0 scheduler observations=%lu bad=%lu\n",
             (unsigned long)cpu0_observation_count,
             (unsigned long)cpu0_bad_observations);
    ap_ready = ap_ready && cpu0_observation_count > 0 &&
               cpu0_bad_observations == 0;
    serial_printk("[sched-evidence] id=aarch64_sched_ap cpu_count=%u cpu0_only=%lu default_cpu0=%lu pinned_ap_ok=%lu\n",
                  count, (unsigned long)ap_ready,
                  (unsigned long)default_cpu0,
                  (unsigned long)pinned_ap_ok);
    return count == num_cpus && ap_ready && default_cpu0 && pinned_ap_ok
           ? 0 : -1;
}

void test_aarch64_scheduler_run(void)
{
    scheduler_observation_active = 1;
    cpu0_observation_count = cpu0_bad_observations = 0;
    static const char *const ids[CASES] = {
        "aarch64_sched_yield", "aarch64_sched_regs", "aarch64_sched_entry",
        "aarch64_sched_irq", "aarch64_sched_blocker", "aarch64_sched_reclaim",
        "aarch64_sched_idle", "aarch64_sched_timer", "aarch64_sched_ap"
    };
    int (*const bodies[CASES])(void) = {
        case_yield, case_regs, case_entry, case_irq, case_blocker,
        case_reclaim, case_idle, case_timer, case_ap
    };
    for (unsigned i = 0; i < CASES; ++i) {
        int rc = bodies[i]();
        if (rc)
            log_err("[sched] selftest case %s failed rc=%u\n", ids[i],
                    (unsigned)(rc < 0 ? -rc : rc));
        selftest_record_late(ids[i], rc);
    }
#if AARCH64_EL0_TEST
    extern int test_aarch64_el0_task(void);
    selftest_record_late("aarch64_el0_task", test_aarch64_el0_task());
    extern int test_aarch64_user_mm(void);
    selftest_record_late("aarch64_user_mm", test_aarch64_user_mm());
    extern int test_aarch64_user_task(void);
    selftest_record_late("aarch64_user_task_lifecycle",
                         test_aarch64_user_task());
    extern int test_aarch64_user_svc_uaccess(void);
    selftest_record_late("aarch64_user_svc_uaccess",
                         test_aarch64_user_svc_uaccess());
    extern int test_aarch64_user_vm(void);
    selftest_record_late("aarch64_user_vm", test_aarch64_user_vm());
    extern int test_aarch64_user_fork_cow(void);
    selftest_record_late("aarch64_user_fork_cow",
                         test_aarch64_user_fork_cow());
    extern int test_aarch64_user_waitpid(void);
    selftest_record_late("aarch64_user_waitpid",
                         test_aarch64_user_waitpid());
    extern int test_aarch64_user_exec(void);
    selftest_record_late("aarch64_user_exec",
                         test_aarch64_user_exec());
#endif
}

#endif /* __aarch64__ && OS01_SELFTEST */
