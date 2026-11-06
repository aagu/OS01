/* aarch64 per-CPU scheduler tick (per-CPU tick accounting, Task 2).
 *
 * Every admitted CPU owns a private CNTP interrupt.  The BSP keeps
 * the global time base (jiffies, poll timeout, timer list /
 * TIMER_SIRQ, blocker scan); the LOCAL tick only produces the
 * per-CPU / per-task accounting and the tick-driven vruntime.
 *
 * Contract (spec §4 / §5):
 *
 *   - aarch64_sched_tick_init() runs ONCE on the BSP boot stack.  It
 *     initializes the accounting locks (sched_accounting_init) BEFORE
 *     any per-CPU start can publish a lifecycle, and release-publishes
 *     the global ready gate.  Until the gate is visible every tick
 *     returns without touching the not-yet-initialized accounting
 *     locks and without reading `current`.
 *
 *   - aarch64_sched_tick_start() runs on the CPU that owns the tick,
 *     with IRQs masked, on a legal idle stack.  It refuses when the
 *     global gate is closed, when the CPU is outside the admission
 *     mask, or when the CPU's idle metadata is not published yet —
 *     and a refusal never reads `current`.
 *
 *   - aarch64_sched_tick() runs from the CNTP interrupt handler.  It
 *     re-reads the same gates before it classifies the sample, so a
 *     tick that arrives before the CPU is admitted is inert.
 *
 * Lock discipline: the usage lock (inside sched_account_tick) and the
 * runqueue lock are NEVER nested, and the tick never takes
 * task_list_lock.
 */

#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/sched_tick.h>
#include <arch/aarch64/timer.h>
#include <arch/aarch64/smp.h>
#include <sched/accounting.h>
#include <sched/internal.h>      /* update_curr() */
#include <percpu/percpu.h>
#include <sched/task.h>
#include <arch/spinlock.h>
#include <arch/cpu.h>
#include <log/log.h>

/* Per-CPU "the local scheduler tick is really running" state.  The
 * array is static: no assembly-visible structure field is added.  The
 * owning CPU publishes its entry with a release store while IRQs are
 * masked; a cross-CPU reader uses an acquire load. */
static uint32_t g_started[NR_CPUS];

/* Global ready gate.  Set once by aarch64_sched_tick_init() after the
 * accounting locks exist (the .bss image is all zeros, which is the
 * LOCKED value of those spinlocks — they must be initialized, never
 * merely assumed). */
static uint32_t g_ready;

void aarch64_sched_tick_init(void)
{
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
        return;   /* one-shot; never re-init under live ticks */

    for (unsigned i = 0; i < NR_CPUS; i++)
        __atomic_store_n(&g_started[i], 0, __ATOMIC_RELAXED);

    /* The accounting locks must exist before any cpu_start(): this
     * also release-publishes accounting_ready inside accounting.c. */
    sched_accounting_init();

    /* RELEASE: the locks and the started array are valid before the
     * gate becomes visible. */
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
}

bool aarch64_sched_tick_start(void)
{
    /* Not initialized: touch nothing (no accounting lock, no
     * `current`, no register). */
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
        return false;

    uint32_t cpu = cpu_id();
    if (cpu >= NR_CPUS)
        return false;

    /* Idempotent: a repeated successful start must not rearm the
     * counter, reset the counters or move the accounting epoch. */
    if (__atomic_load_n(&g_started[cpu], __ATOMIC_ACQUIRE))
        return true;

    /* Admission: only an accepted CPU owns a local tick.  CPU0 is
     * always part of the accepted mask. */
    uint64_t accepted = aarch64_smp_accepted_mask();
    if (!((accepted >> cpu) & 1ULL))
        return false;

    /* The runqueue must already carry this CPU's idle task: the tick
     * classifies every sample against it.  Reading the published idle
     * pointer is safe — `current` is NOT touched here. */
    percpu_t *rq = &percpu_data[cpu];
    if (!rq->idle)
        return false;

    /* The BSP's counter is already running — the early boot tick owns
     * the global time base and must not be rearmed here.  An AP arms
     * its own local counter from the BSP's published configuration and
     * never touches the global GIC handler table. */
    bool timer_ok = (cpu == 0) ? aarch64_timer_local_running()
                               : aarch64_timer_start_local();
    if (!timer_ok) {
        /* The CPU is unavailable: publish the failed lifecycle so the
         * accounting reports valid=0 instead of a stale 0%. */
        sched_accounting_cpu_start(cpu, AARCH64_SCHED_TICK_HZ, false);
        __atomic_store_n(&g_started[cpu], 0, __ATOMIC_RELEASE);
        log_err("[sched-tick] cpu=%u local timer start failed\n",
                (unsigned)cpu);
        return false;
    }

    /* The accounting lifecycle is published only after the counter is
     * really running. */
    sched_accounting_cpu_start(cpu, AARCH64_SCHED_TICK_HZ, true);
    __atomic_store_n(&g_started[cpu], 1, __ATOMIC_RELEASE);
    return true;
}

void aarch64_sched_tick(void)
{
    /* Ready gate: before aarch64_sched_tick_init() the accounting
     * locks do not exist yet and `current` must not be read. */
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE))
        return;

    uint32_t cpu = cpu_id();
    if (cpu >= NR_CPUS)
        return;

    /* This CPU's own tick is not running (or was refused). */
    if (!__atomic_load_n(&g_started[cpu], __ATOMIC_ACQUIRE))
        return;

    percpu_t *rq = &percpu_data[cpu];
    /* The scheduler of this CPU is not up yet: the sample is
     * unclassifiable and must not be charged. */
    if (!__atomic_load_n(&rq->scheduler_ok, __ATOMIC_ACQUIRE))
        return;
    task_t *idle = rq->idle;
    if (!idle)
        return;

    /* ONE usage sample (busy/idle) plus the interrupted task's
     * runtime.  The usage lock is taken and RELEASED inside
     * sched_account_tick — the runqueue lock below is never nested
     * with it. */
    sched_account_tick();

    /* Scheduler fields: the running task's vruntime and the per-CPU
     * watchdog counter.  IRQs are masked in the interrupt handler, so
     * `current` cannot change under us. */
    uint64_t flags = spin_lock_irqsave(&rq->rq_lock);
    update_curr(current);
    rq->watchdog_counter++;
    spin_unlock_irqrestore(&rq->rq_lock, flags);

    /* The BSP requests a cooperative reschedule on EVERY valid tick —
     * including an idle tick — so an empty runqueue still reaches the
     * blocker scan through the ordinary cooperative entry point.  An
     * AP only asks for a reschedule when its busy task passed its
     * deadline (update_curr above); an AP idle tick must not request
     * a schedule unconditionally. */
    if (cpu == 0)
        __atomic_store_n(&rq->need_resched, 1, __ATOMIC_RELEASE);
}
