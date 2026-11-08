/* kernel/arch/x86_64/rtc_pie.c -- BOOT-2 RTC PIE calibration transaction
 * (spec §4).  Splits the TSC+LAPIC joint measurement from the old
 * kernel/driver/rtc.c so the arch-neutral rtc core stays clean.
 *
 * The transaction is the whole spec §4 sequence, in order:
 *   preconditions (outputs non-NULL, calibration window allowed, IF=0)
 *   → save A/B, register IRQ8 with IRQF_NO_AUTOEN (registration FIRST —
 *     a failure leaves no PIE/timer change behind)
 *   → B: clear PIE/AIE/UIE, program A=1024Hz, read C clear, LAPIC
 *     masked with DIV=0/INIT=max
 *   → enable PIE, unmask IRQ8, open the controlled window (sti)
 *   → foreground: acquire-wait for 256 valid PF samples using the raw
 *     TSC (never clocksource/jiffies/schedule); the >2^32-cycle guard
 *     AND BOOT2_CALIBRATION_SPIN_LIMIT pause rounds both bound the wait
 *   → close: IRQs off, mask route, clear PIE/AIE/UIE + C, restore A/B,
 *     unregister the owned route (kept masked), restore masked LAPIC
 *   → compute; outputs written only on success.
 */

#include <arch/io.h>
#include <intr/interrupt.h>   // register_irq / unregister_irq / irq_mask
#include <arch/irq.h>
#include <intr/apic.h>        // lapic_read / lapic_write / LAPIC_* / LVT_MASK
#include <arch/cpu.h>         // arch_cycle_counter / arch_cpu_pause
#include <arch/x86_64/platform/boot_irq.h>  // calibration window
#include <arch/x86_64/rtc.h>  // BOOT2_CALIBRATION_SPIN_LIMIT
#include <stddef.h>             // NULL
#ifdef X86_BOOT_IRQ_TIME_TEST
#include <selftest/boot_irq_time_fixture.h>
#define B2T_RTC_HOOK(regs)      boot2_test_rtc_pf(regs)
#define B2T_RTC_DONE(rc)        boot2_test_after_rtc(rc)
#else
#define B2T_RTC_HOOK(regs)      ((void)0)
#define B2T_RTC_DONE(rc)        ((void)0)
#endif

// ── CMOS RTC port I/O (local to this file; same as rtc_cmos.c) ──
#define RTC_CMOS_ADDR         0x70u
#define RTC_CMOS_DATA         0x71u
#define RTC_CMOS_REG_STATUS_A 0x0Au
#define RTC_CMOS_REG_STATUS_B 0x0Bu
#define RTC_CMOS_REG_STATUS_C 0x0Cu
#define RTC_PIE_TICKS         256u     // sample window (~250ms @ 1024Hz)
#define RTC_PIE_IRQ_GSI       8u       // GSI 8 = legacy RTC
#define RTC_B_PIE             0x40u
#define RTC_B_AIE             0x20u
#define RTC_B_UIE             0x10u
#define RTC_C_PF              0x40u
#define RTC_RATE_1024HZ       0x06u

static inline void cmos_write(uint8_t reg, uint8_t val)
{
    arch_outb(RTC_CMOS_ADDR, (uint8_t)(0x80u | reg));
    arch_outb(RTC_CMOS_DATA, val);
}

static inline uint8_t cmos_read(uint8_t reg)
{
    arch_outb(RTC_CMOS_ADDR, (uint8_t)(0x80u | reg));
    return arch_inb(RTC_CMOS_DATA);
}

// ── Sample state (handler-context; cross-CPU volatile) ──
static volatile uint32_t rtc_pie_count;
static volatile bool     rtc_pie_done;
static volatile uint64_t rtc_pie_tsc0;
static volatile uint64_t rtc_pie_tsc1;
// LAPIC counter sampled inside the handler so it shares the same
// N-1 PIE-tick window as the TSC samples (no INIT→handler drift).
static volatile uint32_t rtc_pie_lapic0;
static volatile uint32_t rtc_pie_lapic1;

static void rtc_pie_handler(uint64_t nr, uint64_t parameter, pt_regs_t *regs)
{
    (void)nr; (void)parameter;
    B2T_RTC_HOOK(regs);   /* BOOT-2 test fixture (variants only) */
    // Read reg 0x0C to clear the IRQ reason.
    uint8_t c = cmos_read(RTC_CMOS_REG_STATUS_C);

    /* BOOT-2: ONLY a periodic tick (C.PF) is a sample.  An IRQ with
     * C=0 or AF/UF-only reasons is spurious for this measurement and
     * must not advance the window. */
    if (!(c & RTC_C_PF))
        return;
    if (rtc_pie_done)
        return;                     /* saturated: the 257th+ never rewrites */

    if (rtc_pie_count == 0) {
        rtc_pie_tsc0 = arch_cycle_counter();
        rtc_pie_lapic0 = lapic_read(LAPIC_TIMER_CUR);
    }
    rtc_pie_count++;
    if (rtc_pie_count >= RTC_PIE_TICKS) {
        rtc_pie_tsc1 = arch_cycle_counter();
        rtc_pie_lapic1 = lapic_read(LAPIC_TIMER_CUR);
        rtc_pie_done = true;        /* release-publish the last sample */
    }
}

int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out)
{
    /* ── Preconditions: touch nothing on failure. ── */
    if (tsc_hz_out == NULL || lapic_hz_out == NULL)
        return -1;
    if (!x86_boot_irq_calibration_allowed())
        return -1;
    /* Entry IF must be 0: the window API opens/closes it.  (The saved
     * word is whole RFLAGS on x86 — test the IF bit.) */
    arch_irq_state_t entry_flags = arch_local_irq_save();
    arch_local_irq_restore(entry_flags);
    if (entry_flags & (1UL << 9))
        return -1;

    x86_boot_irq_calibration_begin();

    int rc = -1;
    uint8_t save_a = 0, save_b = 0;
    uint32_t rtc_gsi = arch_irq_isa_to_gsi(RTC_PIE_IRQ_GSI);

    /* Mask the LAPIC timer through the whole transaction. */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);

    /* ── 1. Register FIRST (IRQF_NO_AUTOEN): a failure leaves no
     *       PIE/timer change behind. ── */
    if (!register_irq(rtc_gsi, NULL, rtc_pie_handler, 0,
                      IRQF_TRIGGER_LEVEL | IRQF_NO_AUTOEN, "rtc-pie"))
        goto out;

    save_a = cmos_read(RTC_CMOS_REG_STATUS_A);
    save_b = cmos_read(RTC_CMOS_REG_STATUS_B);

    /* ── 2. Configure with everything still masked. ── */
    cmos_write(RTC_CMOS_REG_STATUS_B,
               (uint8_t)(save_b & (uint8_t)~(RTC_B_PIE | RTC_B_AIE | RTC_B_UIE)));
    cmos_write(RTC_CMOS_REG_STATUS_A,
               (uint8_t)((save_a & 0xF0) | RTC_RATE_1024HZ));
    (void)cmos_read(RTC_CMOS_REG_STATUS_C);   /* clear stale pending */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);
    lapic_write(LAPIC_TIMER_DIV, 0);          /* divisor=0 means ÷2 */
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    /* ── 3. Arm: PIE last, route unmask, THEN the controlled window. ── */
    rtc_pie_count = 0;
    rtc_pie_done = false;
    cmos_write(RTC_CMOS_REG_STATUS_B, (uint8_t)((save_b & (uint8_t)~(RTC_B_AIE | RTC_B_UIE)) | RTC_B_PIE));
#ifndef X86_BOOT_IRQ_TIME_RTC_TIMEOUT
    irq_unmask(rtc_gsi);   /* rtc-timeout keeps the route masked */
#endif
    arch_local_irq_enable();          /* controlled window: IF=1 */

    /* ── 4. Foreground acquire wait: raw TSC + bounded pause budget. ── */
    uint64_t tsc_start = arch_cycle_counter();
    uint32_t rounds = 0;
    while (!rtc_pie_done) {
        if (arch_cycle_counter() - tsc_start > 0x100000000ULL)
            break;                    /* original failsafe */
        if (++rounds > BOOT2_CALIBRATION_SPIN_LIMIT)
            break;                    /* BOOT-2 bounded budget */
        arch_cpu_pause();
    }

    /* ── 5. Close: IRQs off first, then unwind everything we own. ── */
    arch_local_irq_disable();         /* window closed; exit IF=0 */
    irq_mask(rtc_gsi);                /* route masked (also re-closes IRQ2) */
    cmos_write(RTC_CMOS_REG_STATUS_B,
               (uint8_t)(cmos_read(RTC_CMOS_REG_STATUS_B)
                         & (uint8_t)~(RTC_B_PIE | RTC_B_AIE | RTC_B_UIE)));
    (void)cmos_read(RTC_CMOS_REG_STATUS_C);   /* clear pending */
    cmos_write(RTC_CMOS_REG_STATUS_A, save_a); /* restore A ... */
    cmos_write(RTC_CMOS_REG_STATUS_B, save_b); /* ... and B: restore is NOT
                                                  a route re-enable */
    unregister_irq(rtc_gsi);          /* owned route stays masked */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);   /* restore masked LAPIC */
    lapic_write(LAPIC_TIMER_DIV, 0);
    lapic_write(LAPIC_TIMER_INIT, 0);

    /* ── 6. Compute: both elapses positive, results representable. ── */
    if (rtc_pie_done && rtc_pie_count >= RTC_PIE_TICKS) {
        uint64_t elapsed_lapic = (uint64_t)rtc_pie_lapic0 - (uint64_t)rtc_pie_lapic1;
        uint64_t tsc_elapsed   = rtc_pie_tsc1 - rtc_pie_tsc0;
        uint64_t n = RTC_PIE_TICKS - 1;
        if (tsc_elapsed > 0 && elapsed_lapic > 0 &&
            tsc_elapsed <= UINT64_MAX / (1024u * RTC_PIE_TICKS) &&
            elapsed_lapic <= UINT64_MAX / (1024u * RTC_PIE_TICKS)) {
            *tsc_hz_out   = tsc_elapsed   * 1024u / n;
            *lapic_hz_out = elapsed_lapic * 1024u / n;
            if (*tsc_hz_out != 0 && *lapic_hz_out != 0)
                rc = 0;               /* success: outputs written ONCE */
        }
    }

out:
    B2T_RTC_DONE(rc);
    x86_boot_irq_calibration_end();
    /* All paths return IF=0. */
    arch_local_irq_disable();
    return rc;
}
