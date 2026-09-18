/* aarch64 CNTP physical-timer tick ISR (Phase 2 P2 follow-up #2).
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
 * Order contract (phase1 spec §2.3 + phase2 unification):
 *   1. Rewrite CNTP_TVAL_EL0 = period  (FIRST; avoids losing a tick)
 *   2. Call tick_handler() — jiffies++, need_resched=1, watchdog++,
 *      timer-list scan, set_softirq_status(TIMER_SIRQ)
 *   3. Print "[tick] N" once per second (LAST; output may be slow)
 *   4. EOI is performed by gic_dev_dispatch after this returns.
 *
 * `arch_tick_start()` is called by aarch64_main AFTER dtb_init,
 * gic_init, softirq_init() (explicit, see main.c) and the SUBSYS hook
 * (Phase 2 #1 commit bddf8eb: arch_register_subsys() +
 * subsys_init_phase(SUBSYS_PHASE_4)).  It enables the CNTP, arms it for
 * one period ahead, and registers `cntp_tick_handler` with the GIC
 * handler table so the generic dispatch can route CNTP PPI ticks to it.
 * Subsequent re-arms happen in the ISR itself.
 *
 * Output policy: printing on every 100 Hz tick would flood the
 * polled PL011 (~100 characters/second), so we print one line per
 * second (every 100th tick). */

#include <stdint.h>
#include <stdbool.h>
#include <arch/regs.h>
#include "reg.h"
#include <arch/aarch64/gic.h>
#include <arch/aarch64/dtb.h>
#include <time/clockevent.h>   /* tick_handler() */

/* Forward from pl011.c. */
void kputs(const char *s);
void kputu(uint64_t v);
void kputx(uint64_t v);

#define HZ                 100U
#define TICKS_PER_SECOND   HZ

/* Counter for once-per-second print.  This is per-CPU logically but
 * phase 1 is single-core so a plain uint64 is fine.  It is written
 * by the ISR and read by nobody; visibility across IRQ entries is
 * provided by the implicit dsb ish that eret implies.
 *
 * Stays static (not exported) — the probes in irq_probe.c now drive
 * a deterministic SGI/SPI pair rather than relying on this counter,
 * so v2's "export g_ticks" requirement is retracted. */
static volatile uint64_t g_ticks;

/* Cached "current period in ticks" — set by arch_tick_start(),
 * re-read by the ISR from CNTP_TVAL_EL0.  We keep the integer so
 * the ISR doesn't have to re-issue the mrs every entry. */
static uint64_t g_period;

/* Registered CNTP PPI tick handler.  Order contract per phase1 spec §2.3
 * + phase2 unification:
 *   1. Rewrite TVAL FIRST (avoid losing a tick).
 *   2. Call tick_handler() — unified semantic with x86_64.
 *   3. Per-second "[tick] N" debug print (KEEP per R3.1).
 *   4. EOI is performed by gic_dev_dispatch after this returns.
 *
 * Same-priority nesting is masked at the GIC anyway (PPI priority
 * unique under CNTP), so extending the active window by one EOI
 * round-trip is behaviorally equivalent to the in-handler EOI. */
static void cntp_tick_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
    cntp_tval_el0_write(g_period);  /* TVAL rewrite FIRST (phase1 spec §2.3) */
    tick_handler();                   /* unified tick semantic */
    /* GIC Phase 1 evidence gate: qemutests/aarch64_uefi_smp.py:545/574
     * requires >=3 [tick] N lines per case. */
    uint64_t t = g_ticks + 1;
    g_ticks = t;
    if ((t % TICKS_PER_SECOND) == 0) {
        kputs("[tick] ");
        kputu(t / TICKS_PER_SECOND);
        kputs("\n");
    }
}

bool arch_tick_start(void)
{
    uint64_t freq = cntfrq_el0();
    if (freq == 0) {
        return false;
    }
    g_period = freq / HZ;
    if (g_period == 0) {
        g_period = 1;
    }

    /* Register the tick handler with the GIC handler table BEFORE we
     * arm the timer.  If the registration fails (e.g. unexpected -2
     * "already registered" from a re-init), we still proceed — the
     * earlier registration is just as valid. */
    int rc_reg = gic_register_handler(dtb_cntp_ppi(), cntp_tick_handler, 0,
                                      "cntp-tick");
    if (rc_reg != 0 && rc_reg != -2) {
        return false;
    }

    /* Arm and enable.  CNTP_CTL_EL0 bit 0 = EN (enable).  bit 1 =
     * IMASK (interrupt mask).  We leave IMASK=0 (unmasked) so the
     * tick fires when TVAL reaches 0. */
    cntp_tval_el0_write(g_period);
    /* Read-modify-write to preserve reserved bits. */
    uint64_t ctl = cntp_ctl_el0_read();
    ctl |= 1UL;  /* bit 0 = ENABLE */
    /* IMASK is bit 1; we want it CLEAR. */
    ctl &= ~(1UL << 1);
    cntp_ctl_el0_write(ctl);

    /* Read-back sanity: ENABLE must be set.  If not, the timer is
     * not actually running and the GIC will never see a tick. */
    ctl = cntp_ctl_el0_read();
    if ((ctl & 1UL) == 0) {
        return false;
    }

    /* Period value matches the spec for QEMU virt (62500000 / 100). */
    kputs("[cntp] freq=");
    kputu(freq);
    kputs(" Hz, period=");
    kputu(g_period);
    kputs(" ticks (");
    kputu(HZ);
    kputs(" Hz)\n");
    return true;
}
