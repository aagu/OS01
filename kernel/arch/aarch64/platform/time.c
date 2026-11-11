/* aarch64 CNTP physical-timer backend (per-CPU tick accounting,
 * Task 1: freeze the CNTP configuration + arm the local counter).
 *
 * Phase 1 (Task 2.2) had the ISR do its own TVAL rewrite + per-second
 * "[tick] N" print but DID NOT call tick_handler() — so jiffies stayed
 * 0, need_resched was never set, the watchdog counter never incremented,
 * the timer-list never scanned, and TIMER_SIRQ was never raised on
 * aarch64.  Phase 2 P2 unifies the tick semantic with x86_64 by
 * delegating to tick_handler() (kernel/time/tick.c:17) and KEEPING the
 * per-second "[tick] N" print required by the harness evidence gate
 * (R3.1 fix for R1 CRITICAL-1: qemutests/aarch64_uefi_smp.py:545/574
 * asserts >=3 [tick] N lines per case).
 *
 * Order contract (phase1 spec §2.3 + phase2 unification + the per-CPU
 * tick accounting):
 *   1. Rewrite CNTP_TVAL_EL0 = period  (FIRST; avoids losing a tick)
 *   2. On the BSP, call tick_handler() — jiffies++, poll-timeout scan,
 *      timer-list scan, set_softirq_status(TIMER_SIRQ).  The per-CPU
 *      accounting, the need_resched request and the watchdog counter
 *      are NOT here: they belong to the local scheduler tick
 *      (aarch64_sched_tick(), step 5), which charges the vruntime per
 *      DELIVERED local tick.
 *   3. Print "[tick] N" once per second (LAST; output may be slow)
 *   4. EOI is performed by gic_dev_dispatch after this returns.
 *
 * `arch_tick_start()` is called by the kernel_main boot sequence AFTER dtb_init,
 * gic_init, softirq_init() (explicit, see main.c) and the SUBSYS hook
 * (Phase 2 #1 commit bddf8eb: arch_register_subsys() +
 * subsys_init_phase(SUBSYS_PHASE_4)).  It enables the CNTP, arms it for
 * one period ahead, and registers `cntp_tick_handler` with the GIC
 * handler table so the generic dispatch can route CNTP PPI ticks to it.
 * Subsequent re-arms happen in the ISR itself.
 *
 * Per-CPU tick accounting (Task 1):
 *   - the BSP FREEZES the CNTP configuration (counter frequency,
 *     period, PPI) and publishes it; every AP acquires that frozen
 *     configuration before arming its own counter;
 *   - the GIC handler slot is GLOBAL and owned by the BSP: the
 *     per-CPU entry point (`aarch64_timer_start_local()`) never
 *     registers a handler;
 *   - a registration conflict (-2, "already registered") is a HARD
 *     failure — the slot would belong to a foreign handler;
 *   - a local counter frequency that differs from the BSP's is a
 *     HARD failure — the two CPUs' samples would not be comparable;
 *   - a failed arm leaves CNTP_CTL_EL0.ENABLE cleared, so a
 *     half-armed timer can never deliver an interrupt.
 *
 * Output policy: printing on every 100 Hz tick would flood the
 * polled PL011 (~100 characters/second), so we print one line per
 * second (every 100th tick).  The "[cntp]" and "[tick]" lines are
 * part of the QEMU harness evidence protocol and are emitted through
 * log_info() — the aarch64 log backend adds no prefix, so the bytes
 * on the wire are unchanged. */

#include <stdint.h>
#include <stdbool.h>
#include <arch/regs.h>
#include <arch/cpu.h>
#include <arch/aarch64/reg.h>
#include <arch/aarch64/gic.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/timer.h>
#include <arch/aarch64/sched_tick.h>
#include <percpu/percpu.h>     /* cpu_id() — the BSP owns the global tick */
#include <log/log.h>
#include <time/clockevent.h>   /* tick_handler() */
#ifdef OS01_SELFTEST
#include <selftest/aarch64_scheduler.h>
#endif
#ifdef AARCH64_SCHED_TICK_TEST
#include <selftest/aarch64_sched_tick.h>
#endif

#define TICKS_PER_SECOND   AARCH64_TIMER_HZ

/* Counter for once-per-second print.  This is per-CPU logically but
 * phase 1 is single-core so a plain uint64 is fine.  It is written
 * by the ISR and read by nobody; visibility across IRQ entries is
 * provided by the implicit dsb ish that eret implies.
 *
 * Stays static (not exported) — the probes in irq_probe.c now drive
 * a deterministic SGI/SPI pair rather than relying on this counter,
 * so v2's "export g_ticks" requirement is retracted. */
static volatile uint64_t g_ticks;
#if AARCH64_EL0_TEST
/* Observed by SVC while IRQ is masked, on the same BSP. */
static volatile uint64_t el0_ticks;

uint64_t aarch64_el0_timer_ticks(void)
{
    return el0_ticks;
}
#endif

/* Frozen CNTP configuration.  The BSP publishes it once (release
 * store on `ready`, fields written first); every AP acquires it
 * read-only before arming its own counter.  The period is read back
 * from here by the ISR, so the re-arm value is always the frozen one
 * and never a per-CPU copy that could drift. */
static struct aarch64_timer_config g_timer_config;

/* Registered CNTP PPI tick handler.  Order contract per phase1 spec §2.3
 * + phase2 unification + the per-CPU tick accounting spec §5:
 *   1. Rewrite TVAL FIRST (avoid losing a tick) — the period comes
 *      from the FROZEN configuration, so every CPU re-arms with the
 *      same value.
 *   2. The GLOBAL time base belongs to the BSP alone: tick_handler()
 *      (jiffies, poll timeout, timer list / TIMER_SIRQ) and the
 *      per-second "[tick] N" line run on CPU0 only.  An AP never
 *      touches the global objects and never prints.
 *   3. The LOCAL scheduler tick runs on every CPU: per-CPU / per-task
 *      accounting, the tick-driven vruntime and the watchdog counter.
 *   4. EOI is performed by gic_dev_dispatch after this returns.
 *
 * Same-priority nesting is masked at the GIC anyway (PPI priority
 * unique under CNTP), so extending the active window by one EOI
 * round-trip is behaviorally equivalent to the in-handler EOI. */
static void cntp_tick_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param;
#ifdef OS01_SELFTEST
    aarch64_sched_observe_timer_irq(regs, true);
#else
    (void)regs;
#endif
    cntp_tval_el0_write(g_timer_config.period);  /* TVAL rewrite FIRST */
    if (cpu_id() == 0) {
        /* Global time base — BSP only. */
        tick_handler();
#ifdef AARCH64_SCHED_TICK_TEST
        /* The per-CPU tick observer (selftest build only): the BSP's
         * completed global tick. */
        aarch64_sched_tick_observe(0, true);
#endif
        /* GIC Phase 1 evidence gate: qemutests/aarch64_uefi_smp.py:545/574
         * requires >=3 [tick] N lines per case. */
        uint64_t t = g_ticks + 1;
        g_ticks = t;
        if ((t % TICKS_PER_SECOND) == 0) {
            log_info("[tick] %lu\n", (unsigned long)(t / TICKS_PER_SECOND));
        }
    }
    /* Local scheduler tick — every admitted CPU. */
    aarch64_sched_tick();
#if AARCH64_EL0_TEST
    if (regs && (regs->spsr_el1 & 0x1f) == 0)
        el0_ticks++;
#endif
#ifdef OS01_SELFTEST
    aarch64_sched_observe_timer_irq(regs, false);
#endif
}

/* Validate a CNTP configuration.
 *
 *   - the PPI must be a private peripheral interrupt (16..31);
 *   - the counter frequency must be at least the nominal tick rate;
 *   - the resulting period must be a usable TVAL value: 1 <= period
 *     <= INT32_MAX (CNTP_TVAL_EL0 is a signed 32-bit-ish window and
 *     the per-CPU accounting counts in ticks).
 *
 * Returns false and leaves *period_out untouched when the
 * configuration is rejected. */
static bool timer_config_valid(uint64_t freq, uint32_t ppi, uint64_t *period_out)
{
    if (ppi < AARCH64_TIMER_PPI_FIRST || ppi > AARCH64_TIMER_PPI_LAST)
        return false;
    if (freq < AARCH64_TIMER_HZ)
        return false;
    uint64_t period = freq / AARCH64_TIMER_HZ;
    if (period == 0 || period > (uint64_t)INT32_MAX)
        return false;
    if (period_out)
        *period_out = period;
    return true;
}

/* Disable the local counter: CNTP_CTL_EL0.ENABLE cleared, then ISB.
 *
 * The write is UNCONDITIONAL: the failure path must guarantee that the
 * counter is off even when the read-back that detected the failure is
 * itself broken (a counter that reports a stale CTL must never be
 * left enabled). */
static void timer_disable_local(void)
{
    uint64_t ctl = cntp_ctl_el0_read();
    ctl &= ~1UL;
    cntp_ctl_el0_write(ctl);
    cntp_isb();
}

/* Arm the local counter for one period.
 *
 * Order contract: TVAL -> CTL -> ISB -> read-back.
 *   - TVAL is written FIRST so the counter cannot expire between the
 *     control update and the value being programmed;
 *   - CNTP_CTL_EL0 bit 0 = ENABLE (set), bit 1 = IMASK (cleared, so
 *     the tick is actually delivered);
 *   - the read-back must show ENABLE=1 and IMASK=0, otherwise the
 *     counter is disabled again and the start fails. */
static bool timer_arm_local(uint64_t period)
{
    cntp_tval_el0_write(period);

    uint64_t ctl = cntp_ctl_el0_read();
    ctl |= 1UL;             /* bit 0 = ENABLE */
    ctl &= ~(1UL << 1);     /* bit 1 = IMASK — we want it CLEAR */
    cntp_ctl_el0_write(ctl);
    cntp_isb();

    ctl = cntp_ctl_el0_read();
    if ((ctl & 1UL) == 0 || (ctl & (1UL << 1)) != 0) {
        timer_disable_local();
        return false;
    }
    return true;
}

/* Arm the LOCAL counter of the calling CPU from the frozen
 * configuration.  Never registers a GIC handler. */
bool aarch64_timer_start_local(void)
{
    if (__atomic_load_n(&g_timer_config.ready, __ATOMIC_ACQUIRE) != 1U) {
        log_err("[timer] local start refused: configuration not published\n");
        return false;
    }

    uint64_t freq = g_timer_config.freq;
    uint64_t period = g_timer_config.period;
    uint32_t ppi = g_timer_config.ppi;

    /* Re-validate the acquired configuration: an AP must never arm
     * from a configuration the BSP itself would have rejected. */
    uint64_t check_period = 0;
    if (!timer_config_valid(freq, ppi, &check_period) || check_period != period) {
        log_err("[timer] local start refused: invalid published configuration\n");
        return false;
    }

    /* The local counter must run at the frozen frequency: otherwise
     * the local samples of the two CPUs are not comparable. */
    uint64_t local_freq = cntfrq_el0();
    if (local_freq != freq) {
        log_err("[timer] local start refused: local frequency mismatch\n");
        return false;
    }

    return timer_arm_local(period);
}

/* BSP: verify that the local counter is really running.  Read-only:
 * no rearm, no handler registration, no change to the global jiffies
 * or the per-second counter. */
bool aarch64_timer_local_running(void)
{
    uint64_t ctl = cntp_ctl_el0_read();
    return (ctl & 1UL) != 0 && (ctl & (1UL << 1)) == 0;
}

bool aarch64_timer_config_ready(void)
{
    return __atomic_load_n(&g_timer_config.ready, __ATOMIC_ACQUIRE) == 1U;
}

#ifdef OS01_SELFTEST
/* OS01_SELFTEST ONLY: mask the calling CPU's local CNTP interrupt.
 *
 * The saved control word is returned so the caller can restore the
 * exact state later; 0 means "the counter is not running" or "IMASK
 * did not stick" and is a hard failure for the caller.  Only the
 * CALLING CPU's counter is touched — the BSP's global time source is
 * never masked by this helper. */
uint64_t aarch64_sched_tick_test_mask_local(void)
{
    uint64_t ctl = cntp_ctl_el0_read();
    if ((ctl & 1UL) == 0)
        return 0;               /* the counter is not running */
    cntp_ctl_el0_write(ctl | (1UL << 1));   /* set IMASK */
    cntp_isb();
    uint64_t readback = cntp_ctl_el0_read();
    if ((readback & (1UL << 1)) == 0)
        return 0;               /* IMASK did not stick */
    return ctl;
}

/* OS01_SELFTEST ONLY: restore the calling CPU's local CNTP interrupt.
 *
 * Order contract (same as the arm path): TVAL (rearm) -> CTL -> ISB
 * -> read-back.  The counter keeps running while it is masked, so the
 * value is re-armed before IMASK is cleared; otherwise an expired
 * counter would deliver a burst of pending interrupts. */
void aarch64_sched_tick_test_restore_local(uint64_t saved_ctl)
{
    if (!saved_ctl)
        return;
    cntp_tval_el0_write(g_timer_config.period);
    cntp_ctl_el0_write(saved_ctl);
    cntp_isb();
    (void)cntp_ctl_el0_read();
}
#endif /* OS01_SELFTEST */

/* BSP: register the CNTP IRQ handler and publish the frozen
 * configuration.  The GIC handler table is GLOBAL — this is the only
 * registration of the CNTP PPI in the system. */
bool aarch64_timer_publish_config(void)
{
    uint64_t freq = cntfrq_el0();
    uint32_t ppi = dtb_cntp_ppi();
    uint64_t period = 0;

    if (!timer_config_valid(freq, ppi, &period)) {
        log_err("[cntp] FATAL: invalid CNTP configuration\n");
        return false;
    }

    /* Register the tick handler with the GIC handler table BEFORE we
     * arm the timer.  A non-zero return is a hard failure: -2 means
     * the slot is already owned by another handler, and the tick would
     * be delivered to the wrong code. */
    int rc_reg = gic_register_handler(ppi, cntp_tick_handler, 0, "cntp-tick");
    if (rc_reg != 0) {
        log_err("[cntp] FATAL: CNTP handler registration failed\n");
        return false;
    }

    /* Publish the frozen configuration: fields first, `ready` last
     * (release store).  Every AP acquires it before arming its own
     * counter. */
    g_timer_config.freq = freq;
    g_timer_config.period = period;
    g_timer_config.ppi = ppi;
    __atomic_store_n(&g_timer_config.ready, 1U, __ATOMIC_RELEASE);
    return true;
}

bool arch_tick_start(void)
{
    /* BSP boot start.  Any failure shuts the local counter down again
     * before returning, so a half-armed timer can never deliver an
     * interrupt after the caller decided to fail-stop. */
    if (!aarch64_timer_publish_config()) {
        timer_disable_local();
        return false;
    }
    if (!aarch64_timer_start_local()) {
        log_err("[cntp] FATAL: local timer start failed\n");
        timer_disable_local();
        return false;
    }

    /* Period value matches the spec for QEMU virt (62500000 / 100). */
    log_info("[cntp] freq=%lu Hz, period=%lu ticks (%u Hz)\n",
             (unsigned long)g_timer_config.freq,
             (unsigned long)g_timer_config.period,
             (unsigned)AARCH64_TIMER_HZ);
    return true;
}
