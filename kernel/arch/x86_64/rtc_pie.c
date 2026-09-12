/* kernel/arch/x86_64/rtc_pie.c -- x86_64 platform-specific RTC PIE
 * (Periodic Interrupt Enable) calibration. Splits out the TSC+LAPIC
 * frequency measurement from the old kernel/driver/rtc.c so the
 * arch-neutral rtc core (kernel/driver/rtc.c) stays clean.
 *
 * This code combines:
 *   - CMOS RTC: 0x70/0x71 port I/O, registers A and B for PIE
 *     configuration (PC-AT MC146818 specifics)
 *   - LAPIC:    LVT_TIMER / TIMER_DIV / TIMER_INIT (LAPIC specifics)
 *   - IRQ8:     via arch_irq_select_controller / register_irq
 *               (arch-neutral API; resolves to IOAPIC or PIC on x86_64)
 *   - TSC:      arch_cycle_counter (arch-neutral)
 *
 * All four pieces are x86_64 platform glue -- hence this file lives
 * under kernel/arch/x86_64 and is only compiled on x86_64.
 *
 * Triggered by kernel/arch/x86_64/time.c when CPUID 0x15 returns 0
 * (no nominal TSC frequency). On success, both TSC and LAPIC
 * frequencies are reported; the LAPIC result is fed into
 * lapic_timer_set_premeasured for the LAPIC period-mode init.
 */

#include <arch/io.h>
#include <intr/interrupt.h>   // register_irq / unregister_irq
#include <intr/apic.h>        // lapic_read / lapic_write / LAPIC_* / LVT_MASK
#include <arch/cpu.h>    // arch_cycle_counter / arch_cpu_pause
#include <stddef.h>             // NULL

// ── CMOS RTC port I/O (local to this file; same as rtc_cmos.c) ──
#define RTC_CMOS_ADDR         0x70u
#define RTC_CMOS_DATA         0x71u
#define RTC_CMOS_REG_STATUS_A 0x0Au
#define RTC_CMOS_REG_STATUS_B 0x0Bu
#define RTC_CMOS_REG_STATUS_C 0x0Cu
#define RTC_PIE_TICKS         256u     // sample window (~250ms @ 1024Hz)
#define RTC_PIE_IRQ_GSI       8u       // GSI 8 = legacy RTC

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

// ── LAPIC sample state (handler-context; cross-CPU volatile) ──
static volatile uint32_t rtc_pie_count;
static volatile uint64_t rtc_pie_tsc0;
static volatile uint64_t rtc_pie_tsc1;
// LAPIC counter sampled inside the handler so it shares the same
// N-1 PIE-tick window as the TSC samples (no INIT→handler drift).
static volatile uint32_t rtc_pie_lapic0;
static volatile uint32_t rtc_pie_lapic1;

static void rtc_pie_handler(uint64_t nr, uint64_t parameter, pt_regs_t *regs)
{
    (void)nr; (void)parameter; (void)regs;
    // Read reg 0x0C to clear the PIE interrupt flag.
    (void)cmos_read(RTC_CMOS_REG_STATUS_C);

    if (rtc_pie_count == 0) {
        rtc_pie_tsc0 = arch_cycle_counter();
        rtc_pie_lapic0 = lapic_read(LAPIC_TIMER_CUR);
    }
    rtc_pie_count++;
    if (rtc_pie_count >= RTC_PIE_TICKS) {
        rtc_pie_tsc1 = arch_cycle_counter();
        rtc_pie_lapic1 = lapic_read(LAPIC_TIMER_CUR);
    }
}

int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out)
{
    // 1. Mask LAPIC timer to avoid countdown-to-zero firing on an
    //    unregistered vector (would GP# → triple fault).
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);
    // divisor=0 means ÷2 (SDM 000b). Must be explicit.
    lapic_write(LAPIC_TIMER_DIV, 0);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // 2. Register IRQ8 (gsi=8) at level-triggered.
    if (!register_irq(RTC_PIE_IRQ_GSI, NULL, rtc_pie_handler, 0,
                      IRQF_TRIGGER_LEVEL, "rtc-pie")) {
        return -1;
    }

    // 3. Enable PIE: reg B bit 6 = PIE; reg A low 4 bits = 1024Hz (0b0110 = 6).
    uint8_t b = cmos_read(RTC_CMOS_REG_STATUS_B);
    cmos_write(RTC_CMOS_REG_STATUS_B, b | 0x40);
    uint8_t a = cmos_read(RTC_CMOS_REG_STATUS_A);
    cmos_write(RTC_CMOS_REG_STATUS_A, (uint8_t)((a & 0xF0) | 0x06));

    // 4. Loop until either N ticks have fired or TSC has advanced
    //    > 2^32 cycles (≈500ms @ 8.6GHz; failsafe in case IRQ8
    //    never arrives so the boot doesn't hang).
    rtc_pie_count = 0;
    uint64_t tsc_start = arch_cycle_counter();
    while (rtc_pie_count < RTC_PIE_TICKS) {
        if (arch_cycle_counter() - tsc_start > 0x100000000ULL) {
            break;
        }
        arch_cpu_pause();
    }

    // 5. Disable PIE, unregister IRQ8.
    b = cmos_read(RTC_CMOS_REG_STATUS_B);
    cmos_write(RTC_CMOS_REG_STATUS_B, b & (uint8_t)~0x40);
    unregister_irq(RTC_PIE_IRQ_GSI);

    if (rtc_pie_count < RTC_PIE_TICKS) return -1;

    // 6. Compute frequencies. TSC elapsed covers N-1 PIE cycles;
    //    LAPIC elapsed covers the same window (decrementing, so
    //    lapic0 > lapic1). `elapsed_lapic` is already divided by 2
    //    (LAPIC_TIMER_DIV=0), so the rate we compute is the
    //    decrement rate, which is what lapic_timer_start expects.
    uint64_t elapsed_lapic = (uint64_t)rtc_pie_lapic0 - (uint64_t)rtc_pie_lapic1;
    uint64_t tsc_elapsed   = rtc_pie_tsc1 - rtc_pie_tsc0;
    uint64_t n = RTC_PIE_TICKS - 1;
    *tsc_hz_out   = tsc_elapsed   * 1024 / n;
    *lapic_hz_out = elapsed_lapic * 1024 / n;
    return 0;
}
