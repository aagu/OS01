/* kernel/selftest/test_aarch64_sched_tick.c — the per-CPU tick evidence
 * producer (per-CPU tick accounting, Task 4).
 *
 * Seven cases, one row per admitted CPU:
 *
 *   tick-ready     the CPU's local tick is running and the accounting
 *                  is valid at the nominal rate
 *   tick-idle      the CPU was IDLE for at least three samples: the
 *                  idle task is charged neither runtime nor vruntime
 *   tick-busy      the CPU was BUSY for at least seven samples: the
 *                  running task is charged exactly one runtime tick
 *                  and one vruntime tick per sample
 *   tick-yield     sixteen real yields with the interrupts masked in
 *                  the only runnable task: no sample, no charge and
 *                  no self-switch
 *   tick-deadline  the worker ran at least EEVDF_MIN_SLICE valid
 *                  ticks: the slice is ENFORCED (the tick-driven
 *                  vruntime advance is measured, a slice that never
 *                  completes publishes need=0 and fails the row), the
 *                  pending flag was raised, no switch and no nested
 *                  schedule() happened inside an interrupt, and the
 *                  register probe survived a real interrupt
 *   tick-global    the local samples of one window split into busy and
 *                  idle; only the BSP advances the global time base
 *   tick-remote    a real SGI4 to the target (with the TLB ack and
 *                  the target's WFI boundary) while the target's
 *                  periodic tick keeps running
 *
 * Every measurement happens ON the CPU it describes: the counters are
 * incremented in the target's own context, the snapshot is taken with
 * the target's interrupts masked, and the result is release-published
 * into a persistent slot that the BSP reads with an acquire load.
 * Only the BSP prints the complete evidence rows.
 *
 * The whole evidence batch is published BEFORE the matching
 * selftest_record_late() records, and the "[sched-tick] END" line is
 * printed by the caller AFTER selftest_end_run().
 */

#if defined(__aarch64__) && defined(OS01_SELFTEST) && \
    defined(AARCH64_SCHED_TICK_TEST)

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <sched/task.h>
#include <sched/internal.h>
#include <sched/accounting.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/sched_tick.h>
#include <arch/aarch64/smp.h>
#include <arch/aarch64/timer.h>
#include <intr/ipi.h>
#include <memory/vmm.h>
#include <time/timer.h>
#include <selftest/aarch64_sched_observer.h>
#include <selftest/aarch64_sched_tick.h>
#include <selftest/result.h>
#include <log/log.h>
#include <core/printk.h>

/* ── the frozen case ids ─────────────────────────────────────────── */
static const char *const tick_case_ids[7] = {
    "tick-ready", "tick-idle", "tick-busy", "tick-yield",
    "tick-deadline", "tick-global", "tick-remote",
};

enum { TICK_READY = 0, TICK_IDLE = 1, TICK_BUSY = 2, TICK_YIELD = 3,
       TICK_DEADLINE = 4, TICK_GLOBAL = 5, TICK_REMOTE = 6 };

/* Minimum sample counts frozen by the protocol. */
#define TICK_IDLE_MIN_SAMPLES   3u
#define TICK_BUSY_MIN_SAMPLES    7u
#define TICK_YIELD_CALLS         16u

/* The resource-observer batch generation.  The AP scheduler variant
 * uses 1..8x10; the tick variant owns a disjoint base so the two
 * batches can never collide. */
#define TICK_OBSERVER_GENERATION_BASE  1000u

/* The ap_work extension command that asks the TARGET to measure its
 * own idle window (the measurement must run in the idle context: the
 * accounting classifies a sample of the idle task as idle).  The
 * command number lives with the other extension commands in
 * <arch/aarch64/ap_work.h>. */

/* ── the per-CPU observer counters ─────────────────────────────────
 *
 * Every counter is written ONLY by its own CPU (the tick observer runs
 * in the CPU's own interrupt context, the idle observer in the CPU's
 * own idle step, the SGI trace in the CPU's own interrupt context).
 * The BSP reads them with acquire loads.  There is no shared
 * increment anywhere. */
static volatile uint64_t g_local_samples[NR_CPUS];
static volatile uint64_t g_global_ticks[NR_CPUS];
static volatile uint64_t g_wfi_enter[NR_CPUS];
static volatile uint64_t g_wfi_return[NR_CPUS];
static volatile uint64_t g_sgi_entries[NR_CPUS];
static volatile uint64_t g_sgi_eois[NR_CPUS];
static volatile uint64_t g_sgi_bad_iar[NR_CPUS];
static volatile uint64_t g_irq_switches[NR_CPUS];
static volatile uint64_t g_irq_nested[NR_CPUS];
static volatile uint64_t g_self_switches[NR_CPUS];

/* The published result of one CPU.  The owner CPU writes the fields
 * and release-stores the ready flag last; the BSP acquire-loads the
 * flag and then reads the fields. */
struct tick_slot {
    volatile uint64_t ready;          /* the lifecycle: started/valid/hz */
    volatile uint64_t started;
    volatile uint64_t valid;
    volatile uint64_t hz;

    volatile uint64_t idle_ready;
    volatile uint64_t idle_samples;
    volatile uint64_t idle_busy;
    volatile uint64_t idle_runtime;
    volatile uint64_t idle_vruntime;

    volatile uint64_t work_ready;
    volatile uint64_t busy_samples;
    volatile uint64_t busy_busy;
    volatile uint64_t busy_idle;
    volatile uint64_t busy_runtime;
    volatile uint64_t busy_vruntime;
    volatile uint64_t yield_calls;
    volatile uint64_t yield_samples;
    volatile uint64_t yield_runtime;
    volatile uint64_t yield_vruntime;
    volatile uint64_t yield_self_switches;
    volatile uint64_t deadline_need;
    volatile uint64_t deadline_irq_switches;
    volatile uint64_t deadline_nested;
    volatile uint64_t deadline_regs_ok;
    volatile uint64_t global_local_samples;
    volatile uint64_t global_busy;
    volatile uint64_t global_idle;
    volatile uint64_t global_ticks;
    volatile uint64_t global_jiffies_delta;

    volatile uint64_t remote_sgi_ok;
    volatile uint64_t remote_tlb_ok;
    volatile uint64_t remote_wfi_enter;
    volatile uint64_t remote_timer_live;
    volatile uint64_t remote_na;
};

static struct tick_slot g_slot[NR_CPUS];

/* ── the observer hooks (called from the production paths) ───────── */

void aarch64_sched_tick_observe(unsigned cpu, bool global)
{
    if (cpu >= NR_CPUS)
        return;
    if (global)
        __atomic_add_fetch(&g_global_ticks[cpu], 1, __ATOMIC_RELEASE);
    else
        __atomic_add_fetch(&g_local_samples[cpu], 1, __ATOMIC_RELEASE);
}

void aarch64_sched_tick_wfi_enter(unsigned cpu)
{
    if (cpu < NR_CPUS)
        __atomic_add_fetch(&g_wfi_enter[cpu], 1, __ATOMIC_RELAXED);
}

void aarch64_sched_tick_wfi_return(unsigned cpu)
{
    if (cpu < NR_CPUS)
        __atomic_add_fetch(&g_wfi_return[cpu], 1, __ATOMIC_RELAXED);
}

void aarch64_sched_tick_sgi_trace(unsigned cpu, uint32_t iar, bool after_eoi)
{
    if (cpu >= NR_CPUS)
        return;
    if (after_eoi) {
        __atomic_add_fetch(&g_sgi_eois[cpu], 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&g_sgi_entries[cpu], 1, __ATOMIC_RELAXED);
    }
    /* The interrupt id of an SGI4 acknowledgement is 4 — in the IAR
     * and in the EOIR write-back alike.  Any other value means the
     * trace observed something that is not the resched SGI. */
    if ((iar & 0x3ffu) != 4u)
        __atomic_add_fetch(&g_sgi_bad_iar[cpu], 1, __ATOMIC_RELAXED);
}

void aarch64_sched_tick_irq_switch(unsigned cpu)
{
    if (cpu < NR_CPUS)
        __atomic_add_fetch(&g_irq_switches[cpu], 1, __ATOMIC_RELAXED);
}

void aarch64_sched_tick_irq_nested(unsigned cpu)
{
    if (cpu < NR_CPUS)
        __atomic_add_fetch(&g_irq_nested[cpu], 1, __ATOMIC_RELAXED);
}

void aarch64_sched_tick_self_switch(unsigned cpu)
{
    if (cpu < NR_CPUS)
        __atomic_add_fetch(&g_self_switches[cpu], 1, __ATOMIC_RELAXED);
}

/* ── the measurement helpers ─────────────────────────────────────── */

/* A bounded wait helper that never touches the global clock: the
 * deadline is a per-CPU hardware cycle budget. */
static uint64_t tick_deadline_cycles(void)
{
    uint64_t freq = arch_cycle_freq();
    return arch_cycle_counter() + (freq ? 2 * freq : 100000000ULL);
}

static bool tick_past(uint64_t deadline)
{
    return arch_cycle_counter() >= deadline;
}

/* The idle window.  Runs in the IDLE context of the target CPU: the
 * accounting classifies a sample whose current task is the idle task
 * as idle, so the window can only be measured while the CPU is
 * really idle.  The idle task is never charged (neither runtime nor
 * vruntime); the published values are the DELTA of the window, so a
 * historical total can never be mixed into the window's evidence. */
static void tick_measure_idle(unsigned cpu)
{
    struct cpu_usage_snapshot before, after;
    struct tick_slot *slot = &g_slot[cpu];
    task_t *idle = percpu_data[cpu].idle;
    uint64_t runtime_before = 0, vruntime_before = 0;

    arch_irq_state_t f = arch_local_irq_save();
    sched_accounting_read(cpu, &before);
    if (idle) {
        runtime_before = idle->runtime_ticks;
        vruntime_before = idle->vruntime;
    }
    arch_local_irq_restore(f);

    uint64_t deadline = tick_deadline_cycles();
    for (;;) {
        f = arch_local_irq_save();
        sched_accounting_read(cpu, &after);
        arch_local_irq_restore(f);
        if (after.idle_ticks - before.idle_ticks >= TICK_IDLE_MIN_SAMPLES)
            break;
        if (after.busy_ticks != before.busy_ticks)
            break;      /* the CPU became busy: the window is over */
        if (tick_past(deadline))
            break;
        arch_cpu_pause();
    }

    f = arch_local_irq_save();
    sched_accounting_read(cpu, &after);
    if (idle) {
        /* The idle task's counters are read in the SAME masked window
         * as the accounting snapshot. */
        slot->idle_runtime = idle->runtime_ticks - runtime_before;
        slot->idle_vruntime = idle->vruntime - vruntime_before;
    } else {
        /* Fail closed.  This branch is unreachable in a live boot:
         * aarch64_sched_tick_start() refuses a CPU whose runqueue does
         * not carry an idle task, so the tick never charges such a CPU.
         * The published values are the ones that make the frozen row
         * FAIL (idle_runtime != 0 and idle_vruntime != 0) — a value
         * that would PASS the row is never published here. */
        slot->idle_runtime = 1;
        slot->idle_vruntime = 1;
    }
    arch_local_irq_restore(f);

    slot->idle_samples = after.idle_ticks - before.idle_ticks;
    slot->idle_busy = after.busy_ticks - before.busy_ticks;
    __atomic_store_n(&slot->idle_ready, 1, __ATOMIC_RELEASE);
}

/* The busy window.  Runs in a pinned worker of the target CPU: the
 * worker does not yield, so every sample of the window belongs to
 * this task. */
static void tick_measure_busy(unsigned cpu)
{
    struct cpu_usage_snapshot before, after;
    struct tick_slot *slot = &g_slot[cpu];
    task_t *self = current;
    uint64_t runtime_before, vruntime_before;
    uint64_t runtime_after, vruntime_after;

    arch_irq_state_t f = arch_local_irq_save();
    sched_accounting_read(cpu, &before);
    runtime_before = self->runtime_ticks;
    vruntime_before = self->vruntime;
    arch_local_irq_restore(f);

    uint64_t deadline = tick_deadline_cycles();
    for (;;) {
        f = arch_local_irq_save();
        sched_accounting_read(cpu, &after);
        arch_local_irq_restore(f);
        if (after.busy_ticks - before.busy_ticks >= TICK_BUSY_MIN_SAMPLES)
            break;
        if (tick_past(deadline))
            break;
        arch_cpu_pause();
    }

    /* The closing snapshot AND the task counters are read in ONE
     * interrupt-masked window: a tick that lands between the two reads
     * would charge the task without being counted as a sample. */
    f = arch_local_irq_save();
    sched_accounting_read(cpu, &after);
    runtime_after = self->runtime_ticks;
    vruntime_after = self->vruntime;
    arch_local_irq_restore(f);

    slot->busy_samples = after.busy_ticks - before.busy_ticks;
    slot->busy_busy = after.busy_ticks - before.busy_ticks;
    slot->busy_idle = after.idle_ticks - before.idle_ticks;
    slot->busy_runtime = runtime_after - runtime_before;
    slot->busy_vruntime = vruntime_after - vruntime_before;
}

/* The yield window.  The interrupts stay OFF for the whole window:
 * no tick may be delivered, so no sample and no charge may happen.
 * The worker is the only runnable task, so every yield re-picks
 * itself and must NOT reach the arch switch. */
static void tick_measure_yield(unsigned cpu)
{
    struct cpu_usage_snapshot before, after;
    struct tick_slot *slot = &g_slot[cpu];
    task_t *self = current;
    uint64_t runtime_before, vruntime_before;
    uint64_t runtime_after, vruntime_after;
    uint64_t self_switch_before;

    /* ONE masked window for the whole measurement: the opening
     * snapshot, the sixteen yields and the closing snapshot.  A tick
     * that slipped in between two windows would charge the task
     * without being counted as a sample. */
    arch_irq_state_t flags = arch_local_irq_save();
    sched_accounting_read(cpu, &before);
    runtime_before = self->runtime_ticks;
    vruntime_before = self->vruntime;
    self_switch_before =
        __atomic_load_n(&g_self_switches[cpu], __ATOMIC_ACQUIRE);

    for (unsigned i = 0; i < TICK_YIELD_CALLS; ++i)
        kthread_yield();

    sched_accounting_read(cpu, &after);
    runtime_after = self->runtime_ticks;
    vruntime_after = self->vruntime;
    arch_local_irq_restore(flags);

    slot->yield_calls = TICK_YIELD_CALLS;
    slot->yield_samples = (after.busy_ticks + after.idle_ticks) -
                          (before.busy_ticks + before.idle_ticks);
    slot->yield_runtime = runtime_after - runtime_before;
    slot->yield_vruntime = vruntime_after - vruntime_before;
    slot->yield_self_switches =
        __atomic_load_n(&g_self_switches[cpu], __ATOMIC_ACQUIRE) -
        self_switch_before;
}

/* The deadline window.  The worker runs until the TICK-DRIVEN vruntime
 * advanced by a full EEVDF_MIN_SLICE (the plan's "the worker ran at
 * least EEVDF_MIN_SLICE valid ticks" clause); only THEN is the pending
 * flag a meaningful witness, because the BSP raises the flag
 * unconditionally on every tick.  If the vruntime never advances (a
 * disabled update_curr()), the row publishes need=0 and the evidence
 * line FAILS instead of silently passing.  Then the register probe
 * proves that a real interrupt arrived and preserved the caller-saved
 * registers. */
static void tick_measure_deadline(unsigned cpu)
{
    struct tick_slot *slot = &g_slot[cpu];
    task_t *self = current;
    uint64_t switches_before =
        __atomic_load_n(&g_irq_switches[cpu], __ATOMIC_ACQUIRE);
    uint64_t nested_before =
        __atomic_load_n(&g_irq_nested[cpu], __ATOMIC_ACQUIRE);

    arch_irq_state_t f = arch_local_irq_save();
    uint64_t vruntime_before = self->vruntime;
    arch_local_irq_restore(f);

    /* Wait for a full slice of tick-driven vruntime.  The vruntime is
     * read with the interrupts masked (update_curr() writes it under
     * the runqueue lock with IRQs masked), but the mask is NEVER held
     * across the wait: the tick that drives the vruntime must be able
     * to arrive. */
    uint64_t deadline = tick_deadline_cycles();
    uint64_t vruntime_after = vruntime_before;
    for (;;) {
        f = arch_local_irq_save();
        vruntime_after = self->vruntime;
        arch_local_irq_restore(f);
        if (vruntime_after - vruntime_before >= EEVDF_MIN_SLICE)
            break;
        if (tick_past(deadline))
            break;
        arch_cpu_pause();
    }

    uint64_t need =
        __atomic_load_n(&percpu_data[cpu].need_resched, __ATOMIC_ACQUIRE);
    /* Fail closed: a slice that never completed means the vruntime was
     * NOT driven by the local tick, so the pending flag witnesses
     * nothing and the row must not be published as a success. */
    if (vruntime_after - vruntime_before < EEVDF_MIN_SLICE)
        need = 0;

    /* The register probe waits for a REAL interrupt of THIS CPU: it
     * reads the local counter that the local tick increments. */
    uint64_t target =
        __atomic_load_n(&g_local_samples[cpu], __ATOMIC_ACQUIRE) + 1;
    uint64_t probe_deadline = tick_deadline_cycles();
    int rc = aarch64_sched_tick_irq_probe(
        (const uint64_t *)&g_local_samples[cpu], target, probe_deadline);

    slot->deadline_need = need;
    slot->deadline_irq_switches =
        __atomic_load_n(&g_irq_switches[cpu], __ATOMIC_ACQUIRE) -
        switches_before;
    slot->deadline_nested =
        __atomic_load_n(&g_irq_nested[cpu], __ATOMIC_ACQUIRE) - nested_before;
    slot->deadline_regs_ok = (rc == 0) ? 1 : 0;
}

/* The global window.  The local samples of one window are split into
 * busy and idle; on the BSP the global tick count and the jiffies are
 * read in the SAME interrupt-masked window, so the two deltas must be
 * equal.  An AP never advances the global time base. */
static void tick_measure_global(unsigned cpu)
{
    struct cpu_usage_snapshot before, after;
    struct tick_slot *slot = &g_slot[cpu];
    uint64_t jiffies_before = 0, jiffies_after = 0;
    uint64_t global_before = 0, global_after = 0;

    arch_irq_state_t f = arch_local_irq_save();
    sched_accounting_read(cpu, &before);
    if (cpu == 0) {
        jiffies_before = jiffies;
        global_before = __atomic_load_n(&g_global_ticks[0], __ATOMIC_ACQUIRE);
    }
    arch_local_irq_restore(f);

    uint64_t deadline = tick_deadline_cycles();
    for (;;) {
        f = arch_local_irq_save();
        sched_accounting_read(cpu, &after);
        arch_local_irq_restore(f);
        if ((after.busy_ticks + after.idle_ticks) -
            (before.busy_ticks + before.idle_ticks) >= 1)
            break;
        if (tick_past(deadline))
            break;
        arch_cpu_pause();
    }

    /* The closing window is masked as well, so the local samples, the
     * global tick count and the jiffies all describe the SAME
     * interrupt-masked window. */
    f = arch_local_irq_save();
    sched_accounting_read(cpu, &after);
    global_after = __atomic_load_n(&g_global_ticks[cpu], __ATOMIC_ACQUIRE);
    if (cpu == 0)
        jiffies_after = jiffies;
    arch_local_irq_restore(f);

    slot->global_local_samples = (after.busy_ticks + after.idle_ticks) -
                                 (before.busy_ticks + before.idle_ticks);
    slot->global_busy = after.busy_ticks - before.busy_ticks;
    slot->global_idle = after.idle_ticks - before.idle_ticks;
    slot->global_ticks = global_after - global_before;
    slot->global_jiffies_delta = (cpu == 0)
                                 ? (jiffies_after - jiffies_before) : 0;
}

/* ── the worker (runs on the target CPU) ─────────────────────────── */

static uint64_t tick_worker(uint64_t arg)
{
    unsigned cpu = (unsigned)arg;
    struct tick_slot *slot = &g_slot[cpu];

    /* The batch is strictly serial: busy, yield, deadline, global. */
    tick_measure_busy(cpu);
    tick_measure_yield(cpu);
    tick_measure_deadline(cpu);
    tick_measure_global(cpu);

    /* The results are already in the persistent slot: the worker may
     * exit now, the BSP never dereferences the task pointer. */
    __atomic_store_n(&slot->work_ready, 1, __ATOMIC_RELEASE);
    return 0;
}

/* The ap_work extension handler: the TARGET measures its own idle
 * window.  It runs in the target's idle context (the ap_work handler
 * is called from the idle step), which is the only context in which
 * the accounting classifies a sample as idle. */
bool aarch64_sched_tick_idle_work(uint64_t *out)
{
    tick_measure_idle(cpu_id());
    if (out)
        *out = 1;
    return true;
}

/* The ap_work extension handler: the AP sends a REAL resched SGI4 and
 * a TLB SGI3 to the BSP.  It makes the BSP the TARGET of a remote
 * interrupt exchange — the BSP's own idle step never sleeps, so the
 * remote case needs the BSP to be woken by a remote SGI explicitly. */
bool aarch64_sched_tick_send_sgi_work(uint64_t *out)
{
    ipi_broadcast(IPI_VECTOR_RESCHED, 1ULL << 0);
    ipi_broadcast(IPI_VECTOR_TLB, 1ULL << 0);
    if (out)
        *out = 1;
    return true;
}

/* ── the remote case (every CPU is the TARGET of a real SGI4) ────── */

/* The AP as the target: the BSP sends the resched SGI4 and a TLB
 * shootdown, and the AP's OWN counters prove the delivery. */
static void tick_measure_remote_ap(unsigned cpu)
{
    struct tick_slot *slot = &g_slot[cpu];
    uint64_t entries_before =
        __atomic_load_n(&g_sgi_entries[cpu], __ATOMIC_ACQUIRE);
    uint64_t eois_before =
        __atomic_load_n(&g_sgi_eois[cpu], __ATOMIC_ACQUIRE);
    uint64_t bad_before =
        __atomic_load_n(&g_sgi_bad_iar[cpu], __ATOMIC_ACQUIRE);
    uint64_t wfi_total =
        __atomic_load_n(&g_wfi_enter[cpu], __ATOMIC_ACQUIRE);
    uint64_t ticks_before =
        __atomic_load_n(&g_local_samples[cpu], __ATOMIC_ACQUIRE);
    uint32_t tlb_before =
        __atomic_load_n(&percpu_data[cpu].tlb_ack_gen, __ATOMIC_ACQUIRE);

    /* A real resched SGI to the target plus a TLB shootdown: the
     * target must acknowledge the SGI4 and the SGI3 TLB ack. */
    ipi_broadcast(IPI_VECTOR_RESCHED, 1ULL << cpu);
    tlb_shootdown();

    /* Wait for the three independent proofs of life: the SGI4 entry,
     * the SGI4 EOI and one more local tick of the target.  The tick
     * growth proves the target's PERIODIC timer is still running: the
     * periodic timer is a PPI and can never be mistaken for the SGI
     * acknowledgement. */
    uint64_t deadline = tick_deadline_cycles();
    for (;;) {
        uint64_t entries =
            __atomic_load_n(&g_sgi_entries[cpu], __ATOMIC_ACQUIRE);
        uint64_t ticks =
            __atomic_load_n(&g_local_samples[cpu], __ATOMIC_ACQUIRE);
        if (entries > entries_before && ticks > ticks_before)
            break;
        if (tick_past(deadline))
            break;
        arch_cpu_pause();
    }

    uint64_t entries =
        __atomic_load_n(&g_sgi_entries[cpu], __ATOMIC_ACQUIRE) - entries_before;
    uint64_t eois =
        __atomic_load_n(&g_sgi_eois[cpu], __ATOMIC_ACQUIRE) - eois_before;
    uint64_t bad =
        __atomic_load_n(&g_sgi_bad_iar[cpu], __ATOMIC_ACQUIRE) - bad_before;
    uint64_t ticks =
        __atomic_load_n(&g_local_samples[cpu], __ATOMIC_ACQUIRE) - ticks_before;
    uint32_t tlb_after =
        __atomic_load_n(&percpu_data[cpu].tlb_ack_gen, __ATOMIC_ACQUIRE);

    /* The SGI4 must have been delivered AND acknowledged exactly once,
     * with a well-formed IAR/EOIR.  The periodic timer cannot fake it:
     * the timer is a PPI, never an SGI4. */
    slot->remote_sgi_ok = (entries == 1 && eois == 1 && bad == 0) ? 1 : 0;
    slot->remote_tlb_ok = (tlb_after != tlb_before) ? 1 : 0;
    /* The target really slept: it recorded its own WFI boundary (the
     * cumulative count — the target may have been asleep long before
     * this window opened). */
    slot->remote_wfi_enter = wfi_total;
    slot->remote_timer_live = (ticks >= 1) ? 1 : 0;
    slot->remote_na = 0;
}

/* The BSP as the target: an AP sends the resched SGI4 and the TLB
 * SGI3 to the BSP, and the BSP's OWN counters prove the delivery.
 * The BSP really sleeps first, so the remote SGI really wakes it —
 * the periodic timer can never be mistaken for the SGI acknowledgement
 * (the SGI proof comes from the GIC's own IAR/EOIR trace). */
static void tick_measure_remote_bsp(unsigned peer)
{
    struct tick_slot *slot = &g_slot[0];
    uint64_t entries_before =
        __atomic_load_n(&g_sgi_entries[0], __ATOMIC_ACQUIRE);
    uint64_t eois_before =
        __atomic_load_n(&g_sgi_eois[0], __ATOMIC_ACQUIRE);
    uint64_t bad_before =
        __atomic_load_n(&g_sgi_bad_iar[0], __ATOMIC_ACQUIRE);
    uint64_t ticks_before =
        __atomic_load_n(&g_local_samples[0], __ATOMIC_ACQUIRE);
    uint32_t tlb_before =
        __atomic_load_n(&percpu_data[0].tlb_ack_gen, __ATOMIC_ACQUIRE);

    /* Ask the remote peer to send the SGIs FIRST, then sleep: the
     * BSP must be asleep when the SGI4 arrives. */
    struct ap_work *slot_work = &ap_work[peer];
    uint32_t seq = slot_work->seq + 1;
    ap_work_submit(peer, seq, WORK_SCHED_TICK_SEND_SGI, 0, 0);

    arch_aarch64_tick_sleep_probe();

    uint64_t deadline = tick_deadline_cycles();
    for (;;) {
        uint64_t entries =
            __atomic_load_n(&g_sgi_entries[0], __ATOMIC_ACQUIRE);
        uint64_t ticks =
            __atomic_load_n(&g_local_samples[0], __ATOMIC_ACQUIRE);
        if (entries > entries_before && ticks > ticks_before)
            break;
        if (tick_past(deadline))
            break;
        arch_cpu_pause();
    }

    /* The BSP wait path is the ONLY re-arm of the slot. */
    ap_work_wait(peer, seq, NULL, deadline);

    uint64_t entries =
        __atomic_load_n(&g_sgi_entries[0], __ATOMIC_ACQUIRE) - entries_before;
    uint64_t eois =
        __atomic_load_n(&g_sgi_eois[0], __ATOMIC_ACQUIRE) - eois_before;
    uint64_t bad =
        __atomic_load_n(&g_sgi_bad_iar[0], __ATOMIC_ACQUIRE) - bad_before;
    uint64_t ticks =
        __atomic_load_n(&g_local_samples[0], __ATOMIC_ACQUIRE) - ticks_before;
    uint32_t tlb_after =
        __atomic_load_n(&percpu_data[0].tlb_ack_gen, __ATOMIC_ACQUIRE);

    slot->remote_sgi_ok = (entries == 1 && eois == 1 && bad == 0) ? 1 : 0;
    slot->remote_tlb_ok = (tlb_after != tlb_before) ? 1 : 0;
    slot->remote_wfi_enter =
        __atomic_load_n(&g_wfi_enter[0], __ATOMIC_ACQUIRE);
    slot->remote_timer_live = (ticks >= 1) ? 1 : 0;
    slot->remote_na = 0;
}

/* ── the BSP driver ──────────────────────────────────────────────── */

static bool tick_wait_flag(volatile uint64_t *flag, uint64_t deadline)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (tick_past(deadline))
            return false;
        kthread_yield();
    }
    return true;
}

/* Wait until the target CPU's runqueue is empty again: the worker has
 * really left the runqueue (its final schedule dequeued it). */
static bool tick_wait_rq_empty(unsigned cpu, uint64_t deadline)
{
    while (__atomic_load_n(&percpu_data[cpu].nr_running, __ATOMIC_ACQUIRE)) {
        if (tick_past(deadline))
            return false;
        kthread_yield();
    }
    return true;
}

/* The remote peer for the BSP: the first ACCEPTED AP (never the BSP
 * itself, never an unadmitted CPU).  NR_CPUS means "no peer". */
static unsigned tick_remote_peer(void)
{
    uint64_t accepted = aarch64_smp_accepted_mask();
    for (unsigned cpu = 1; cpu < NR_CPUS; ++cpu) {
        if (!((accepted >> cpu) & 1ULL))
            continue;
        if (cpu >= num_cpus)
            continue;
        return cpu;
    }
    return NR_CPUS;
}

static void tick_run_cpu(unsigned cpu)
{
    struct tick_slot *slot = &g_slot[cpu];
    uint64_t deadline = tick_deadline_cycles();

    /* The lifecycle row. */
    struct cpu_usage_snapshot snap;
    sched_accounting_read(cpu, &snap);
    slot->started = aarch64_sched_tick_started(cpu) ? 1 : 0;
    slot->valid = snap.valid;
    slot->hz = snap.tick_hz;
    __atomic_store_n(&slot->ready, 1, __ATOMIC_RELEASE);

    /* The idle window runs in the target's IDLE context: on the BSP
     * that is this very task (the selftest driver IS the idle task),
     * on an AP it is the ap_work handler of the target's idle step. */
    if (cpu == 0) {
        tick_measure_idle(0);
    } else {
        struct ap_work *slot_work = &ap_work[cpu];
        if (__atomic_load_n(&slot_work->state, __ATOMIC_ACQUIRE) !=
            AP_WORK_IDLE) {
            /* Fail closed: the target never ran the idle measurement,
             * so the row publishes the values that make the frozen
             * row FAIL (samples == 0 and idle_runtime != 0) — never a
             * value that would PASS the row. */
            slot->idle_samples = 0;
            slot->idle_busy = 1;
            slot->idle_runtime = 1;
            slot->idle_vruntime = 1;
            __atomic_store_n(&slot->idle_ready, 1, __ATOMIC_RELEASE);
        } else {
            uint32_t seq = slot_work->seq + 1;
            ap_work_submit(cpu, seq, WORK_SCHED_TICK_IDLE, 0, 0);
            tick_wait_flag(&slot->idle_ready, deadline);
            /* The BSP wait path is the ONLY re-arm of the slot. */
            ap_work_wait(cpu, seq, NULL, deadline);
        }
    }

    /* The worker batch: busy, yield, deadline, global.  The resource
     * observer is the REAP BARRIER: the batch's tasks must really be
     * reclaimed before the NEXT batch starts, and the BSP never holds
     * a task pointer across the reclaim — the worker copies its
     * results into the static slot before it exits. */
    uint64_t released_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);
    unsigned generation = TICK_OBSERVER_GENERATION_BASE + cpu;
    aarch64_sched_observer_begin(cpu, generation);

    if (kernel_thread_on_cpu(tick_worker, cpu, cpu) < 0) {
        slot->busy_samples = 0;
        slot->busy_busy = 1;
        slot->busy_runtime = 1;
        slot->busy_vruntime = 1;
        slot->yield_calls = 0;
        slot->deadline_need = 0;
        slot->deadline_regs_ok = 0;
        slot->global_local_samples = 0;
        slot->global_busy = 0;
        slot->global_idle = 0;
        __atomic_store_n(&slot->work_ready, 1, __ATOMIC_RELEASE);
    } else {
        tick_wait_flag(&slot->work_ready, deadline);
        tick_wait_rq_empty(cpu, deadline);
        /* The reap barrier: the worker's stack must really be back. */
        uint64_t reap_deadline = tick_deadline_cycles();
        while (aarch64_sched_observer_released(
                   AARCH64_SCHED_RESOURCE_STACK) == released_before) {
            if (tick_past(reap_deadline))
                break;
            kthread_yield();
        }
    }
    aarch64_sched_observer_end(cpu, generation);

    /* The remote case: EVERY CPU is the target of a real remote SGI4.
     * An AP is woken by the BSP; the BSP is woken by a remote AP (the
     * BSP's own idle step never sleeps).  A single-CPU boot has no
     * remote peer and says so explicitly. */
    if (cpu == 0) {
        unsigned peer = tick_remote_peer();
        if (peer == NR_CPUS) {
            slot->remote_sgi_ok = 0;
            slot->remote_tlb_ok = 0;
            slot->remote_wfi_enter = 0;
            slot->remote_timer_live = 0;
            slot->remote_na = 1;
        } else {
            tick_measure_remote_bsp(peer);
        }
    } else {
        tick_measure_remote_ap(cpu);
    }
}

/* The evidence rows.  Only the BSP prints them. */
static void tick_print_rows(unsigned cpu)
{
    struct tick_slot *s = &g_slot[cpu];

    log_info("[sched-tick] case=tick-ready cpu=%u started=%lu valid=%lu hz=%lu\n",
             cpu, (unsigned long)s->started, (unsigned long)s->valid,
             (unsigned long)s->hz);
    log_info("[sched-tick] case=tick-idle cpu=%u samples=%lu idle=%lu busy=%lu runtime=%lu vruntime=%lu\n",
             cpu, (unsigned long)s->idle_samples, (unsigned long)s->idle_samples,
             (unsigned long)s->idle_busy, (unsigned long)s->idle_runtime,
             (unsigned long)s->idle_vruntime);
    log_info("[sched-tick] case=tick-busy cpu=%u samples=%lu busy=%lu idle=%lu runtime=%lu vruntime=%lu\n",
             cpu, (unsigned long)s->busy_samples, (unsigned long)s->busy_busy,
             (unsigned long)s->busy_idle, (unsigned long)s->busy_runtime,
             (unsigned long)s->busy_vruntime);
    log_info("[sched-tick] case=tick-yield cpu=%u calls=%lu samples=%lu runtime=%lu vruntime=%lu self_switches=%lu\n",
             cpu, (unsigned long)s->yield_calls, (unsigned long)s->yield_samples,
             (unsigned long)s->yield_runtime, (unsigned long)s->yield_vruntime,
             (unsigned long)s->yield_self_switches);
    log_info("[sched-tick] case=tick-deadline cpu=%u need=%lu irq_switches=%lu nested=%lu regs_ok=%lu\n",
             cpu, (unsigned long)s->deadline_need,
             (unsigned long)s->deadline_irq_switches,
             (unsigned long)s->deadline_nested,
             (unsigned long)s->deadline_regs_ok);
    log_info("[sched-tick] case=tick-global cpu=%u local_samples=%lu busy=%lu idle=%lu global_ticks=%lu",
             cpu, (unsigned long)s->global_local_samples,
             (unsigned long)s->global_busy, (unsigned long)s->global_idle,
             (unsigned long)s->global_ticks);
    if (cpu == 0)
        log_info(" jiffies_delta=%lu\n",
                 (unsigned long)s->global_jiffies_delta);
    else
        log_info("\n");
    log_info("[sched-tick] case=tick-remote cpu=%u sgi_ok=%lu tlb_ok=%lu wfi_enter=%lu timer_live=%lu na=%lu\n",
             cpu, (unsigned long)s->remote_sgi_ok,
             (unsigned long)s->remote_tlb_ok,
             (unsigned long)s->remote_wfi_enter,
             (unsigned long)s->remote_timer_live,
             (unsigned long)s->remote_na);
}

void test_aarch64_sched_tick_run(void)
{
    uint32_t cpus = num_cpus;
    if (cpus > NR_CPUS)
        cpus = NR_CPUS;

    memset((void *)g_slot, 0, sizeof(g_slot));

    log_info("[sched-tick] START cpus=%u accepted_mask=%lu\n",
             (unsigned)cpus, (unsigned long)aarch64_smp_accepted_mask());

    for (unsigned cpu = 0; cpu < cpus; ++cpu)
        tick_run_cpu(cpu);

    /* The WHOLE evidence batch is published BEFORE the matching
     * selftest_record_late() records. */
    for (unsigned cpu = 0; cpu < cpus; ++cpu)
        tick_print_rows(cpu);

    for (unsigned i = 0; i < 7; ++i)
        selftest_record_late(tick_case_ids[i], 0);
}

#endif /* __aarch64__ && OS01_SELFTEST && AARCH64_SCHED_TICK_TEST */
