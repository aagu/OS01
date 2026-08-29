/* aarch64 phase 1: CNTP physical-timer tick ISR (Task 3).
 *
 * Spec §2.3 — strict ISR order:
 *   1. Rewrite CNTP_TVAL_EL0 = period  (FIRST; avoids losing a tick)
 *   2. Write GICC_EOIR = PPI intid      (THEN EOI)
 *   3. printk "+tick"                   (LAST; output may be slow)
 *
 * `arch_tick_start()` is called by aarch64_main AFTER dtb_init and
 * gic_init.  It enables the CNTP and arms it for one period ahead.
 * Subsequent re-arms happen in the ISR itself.
 *
 * Output policy: printing on every 100 Hz tick would flood the
 * polled PL011 (~100 characters/second), so we print one line per
 * second (every 100th tick).  The spec's exit B says "≈1000 times
 * +tick" but also allows "a per-second counter is equally valid
 * evidence; prefer whatever is clean" — the printed counter is
 * observable and easy to read.
 */

#include <stdint.h>
#include <stdbool.h>
#include "reg.h"

/* Forward from pl011.c. */
void kputs(const char *s);
void kputu(uint64_t v);
void kputx(uint64_t v);

/* From dtb.c.  The CNTP PPI defaults to 30 in dtb_init(). */
extern uint32_t dtb_cntp_ppi(void);

#define HZ                 100U
#define TICKS_PER_SECOND   HZ

/* Counter for once-per-second print.  This is per-CPU logically but
 * phase 1 is single-core so a plain uint64 is fine.  It is written
 * by the ISR and read by nobody; visibility across IRQ entries is
 * provided by the implicit dsb ish that eret implies. */
static volatile uint64_t g_ticks;

/* Cached "current period in ticks" — set by arch_tick_start(),
 * re-read by the ISR from CNTP_TVAL_EL0.  We keep the integer so
 * the ISR doesn't have to re-issue the mrs every entry. */
static uint64_t g_period;

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

/* C-side EL1 IRQ handler.  Called by the EL1h IRQ vector slot in
 * entry.S after reading GICC_IAR.
 *
 * In phase 1 we hard-code the expected intid (= the CNTP PPI from
 * dtb.c).  Spurious INTID 1023 is treated as a benign "no IRQ
 * actually pending" and returns 0 so the caller does an eret
 * without EOI.
 *
 * Returns 1 if the IRQ was handled (caller should EOI + eret), 0
 * if spurious. */
int el1_irq_dispatch(void)
{
    uint32_t iar = gicc_read32(GICC_IAR);
    uint32_t intid = iar & 0x3FFU;          /* bits[9:0] */
    uint32_t expected = dtb_cntp_ppi();     /* PPI 30 by default */

    if (intid == GICC_INTID_SPURIOUS) {
        return 0;
    }
    if (intid != expected) {
        /* Phase 1 only enables the CNTP PPI.  Any other INTID is a
         * misconfiguration (we did not enable SPI 33, so PL011
         * never raises an IRQ).  EOI it anyway so the GIC doesn't
         * lock up. */
        gicc_write32(GICC_EOIR, intid);
        kputs("[gic] unexpected IRQ intid=");
        kputu(intid);
        kputs("\n");
        return 0;
    }

    /* Spec §2.3 order: rewrite TVAL FIRST, then EOI, then print. */
    cntp_tval_el0_write(g_period);
    gicc_write32(GICC_EOIR, intid);

    /* Per-second print: every 100th tick.  The PL011 polled output
     * is slow, so we don't print on every tick.  To make a
     * "≈1000 times +tick" verification possible with this policy,
     * we print the tick count once a second. */
    uint64_t t = g_ticks + 1;
    g_ticks = t;
    if ((t % TICKS_PER_SECOND) == 0) {
        kputs("[tick] ");
        kputu(t / TICKS_PER_SECOND);
        kputs("\n");
    }
    return 1;
}
