/*
 * kernel/sched/accounting.c — per-CPU tick accounting
 *
 * Monitor plan Task 2 / monitor design spec §2
 * (docs/superpowers/specs/2026-10-09-monitor-design.md).
 *
 * A separate static array (instead of percpu_t fields) keeps the
 * assembly-visible percpu_t layout untouched.  Each entry holds the
 * "timer observed a non-idle task" counters for one CPU:
 *
 *   - busy_ticks: sampled ticks whose current task != the CPU's idle
 *     task (kernel threads count as busy; a user task running inside a
 *     syscall counts for that task);
 *   - idle_ticks: sampled ticks whose current task == the idle task
 *     (compared by POINTER, never by PID or task state);
 *   - epoch: bumped on every explicitly new tick-source lifecycle so
 *     user-space delta math can discard counters that were reset or
 *     that came from a re-onlined CPU;
 *   - tick_hz / valid: sample rate and "this CPU's timer really
 *     started" flag (a core whose lapic_timer_start() failed stays
 *     valid=0 = unavailable; it must never display 0%).
 *
 * Concurrency (spec §2.4): every writer and reader takes the per-CPU
 * usage lock with IRQs saved, so busy/idle/epoch/hz are always copied
 * or updated as a consistent set.  The hot path (sched_account_tick)
 * and the read path perform no formatting and no allocation.
 *
 * aarch64: no per-CPU tick source is wired up yet, so nothing calls
 * sched_accounting_cpu_start() there and every core reads valid=0
 * (unavailable).  No x86-specific clocksource helpers are used here —
 * this file is arch-neutral.
 */

#include <sched/accounting.h>
#include <percpu/percpu.h>
#include <sched/task.h>
#include <errno.h>

struct cpu_usage_account {
    spinlock_T lock;         /* irqsave; guards the whole entry */
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint64_t epoch;          /* changes on reset or re-onlining */
    uint32_t tick_hz;
    uint32_t valid;          /* timer really started on this CPU */
};

static struct cpu_usage_account usage[NR_CPUS];

/* Release publication: written once by sched_accounting_init() after
 * every usage lock holds its unlocked value.  Until then tick
 * interrupts (e.g. the PIT before the handoff) must return without
 * touching the zero-initialized (== LOCKED) spinlocks. */
static uint32_t accounting_ready;

void sched_accounting_init(void)
{
    if (__atomic_load_n(&accounting_ready, __ATOMIC_ACQUIRE))
        return;   /* one-time init; never re-init under live ticks */

    for (unsigned i = 0; i < NR_CPUS; i++) {
        usage[i].busy_ticks = 0;
        usage[i].idle_ticks = 0;
        usage[i].epoch = 0;
        usage[i].tick_hz = 0;
        usage[i].valid = 0;
        spin_init(&usage[i].lock);
    }

    /* RELEASE: the locks are valid before ready becomes visible. */
    __atomic_store_n(&accounting_ready, 1, __ATOMIC_RELEASE);
}

void sched_accounting_cpu_start(unsigned cpu, unsigned hz, bool started)
{
    if (cpu >= NR_CPUS)
        return;

    struct cpu_usage_account *u = &usage[cpu];
    uint64_t flags = spin_lock_irqsave(&u->lock);

    if (started) {
        /* New lifecycle only: first start, or a restart after a stop
         * (or after a failed start).  Re-publishing the same running
         * parameters is idempotent — the epoch must NOT move, or user
         * space would drop an in-flight baseline for no reason. */
        if (!u->valid || u->tick_hz != hz) {
            u->busy_ticks = 0;
            u->idle_ticks = 0;
            u->epoch++;
            u->tick_hz = hz;
        }
        u->valid = 1;
    } else {
        if (u->valid) {
            /* A previously running timer went down (AP start failed,
             * CPU offlined): new lifecycle pending, counters restart
             * when — if — it comes back. */
            u->epoch++;
            u->valid = 0;
        }
    }

    spin_unlock_irqrestore(&u->lock, flags);
}

void sched_account_tick(void)
{
    /* Early PIT interrupts (before x86_64_boot_aps runs) land here and
     * must stay inert: the usage locks are still zero-initialized. */
    if (!__atomic_load_n(&accounting_ready, __ATOMIC_ACQUIRE))
        return;

    uint32_t cpu = cpu_id();
    if (cpu >= NR_CPUS)
        return;

    percpu_t *rq = &percpu_data[cpu];
    if (!rq->scheduler_ok)
        return;

    task_t *cur = current;
    task_t *idle = rq->idle;

    struct cpu_usage_account *u = &usage[cpu];
    uint64_t flags = spin_lock_irqsave(&u->lock);
    if (!u->valid) {
        spin_unlock_irqrestore(&u->lock, flags);
        return;
    }
    if (idle && cur == idle)
        u->idle_ticks++;
    else
        u->busy_ticks++;
    spin_unlock_irqrestore(&u->lock, flags);

    /* Runtime accounting for the interrupted task itself — atomic
     * fetch-add because task_list_lock protects the task lifetime, not
     * cross-CPU concurrent runtime reads/writes (spec §2.4). */
    if (cur && cur != idle)
        __atomic_fetch_add(&cur->runtime_ticks, 1, __ATOMIC_RELAXED);
}

int sched_accounting_read(unsigned cpu, struct cpu_usage_snapshot *out)
{
    if (!out || cpu >= NR_CPUS)
        return -EINVAL;

    if (!__atomic_load_n(&accounting_ready, __ATOMIC_ACQUIRE)) {
        /* Pre-init reads stay off the (still zeroed) locks. */
        out->busy_ticks = 0;
        out->idle_ticks = 0;
        out->epoch = 0;
        out->tick_hz = 0;
        out->online = 0;
        out->valid = 0;
        return 0;
    }

    struct cpu_usage_account *u = &usage[cpu];

    /* Paired copy under the per-CPU lock, unlocked immediately; no
     * other lock is nested here (spec §2.4). */
    uint64_t flags = spin_lock_irqsave(&u->lock);
    out->busy_ticks = u->busy_ticks;
    out->idle_ticks = u->idle_ticks;
    out->epoch = u->epoch;
    out->tick_hz = u->tick_hz;
    uint32_t src_valid = u->valid;
    spin_unlock_irqrestore(&u->lock, flags);

    /* online comes from the percpu actual online state, not from the
     * accounting bookkeeping. */
    out->online = (cpu < num_cpus) ? percpu_data[cpu].online : 0;

    /* valid requires the global publish, this CPU's scheduler being
     * up, and a really-started tick source on this CPU. */
    out->valid = (src_valid && percpu_data[cpu].scheduler_ok) ? 1 : 0;
    return 0;
}
