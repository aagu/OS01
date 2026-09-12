#ifndef _KERNEL_ARCH_X86_64_RTC_H
#define _KERNEL_ARCH_X86_64_RTC_H

#include <stdint.h>

// ─────────────────────────────────────────────────────────
//  x86_64-only RTC subsystem extensions.
//
//  The arch-neutral wall-clock API lives in
//  kernel/include/driver/rtc.h. This header adds platform glue
//  that exists only on the x86_64 PC-AT (CMOS RTC + LAPIC +
//  IRQ8), used by kernel/arch/x86_64/time.c to calibrate the
//  TSC+LAPIC frequencies when CPUID 0x15 reports zero.
//
//  AArch64 (and any future arch) doesn't see this header.
// ─────────────────────────────────────────────────────────

// RTC PIE joint calibration: one PIE window (~250ms @ 1024Hz ×
// 256 ticks) measures TSC and LAPIC simultaneously. Returns 0
// on success, -1 on timeout / IRQ8 never arriving. On success,
// `*tsc_hz_out` and `*lapic_hz_out` receive the measured
// frequencies; the LAPIC result feeds lapic_timer_set_premeasured.
int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out);

#endif /* _KERNEL_ARCH_X86_64_RTC_H */
