#ifndef _KERNEL_ARCH_RTC_H
#define _KERNEL_ARCH_RTC_H

#include <stdint.h>
#include <driver/rtc.h>   // datetime_t

// ─────────────────────────────────────────────────────────
//  arch/rtc.h — arch-neutral facade for the RTC wall-clock hooks.
//
//  Each arch provides strong overrides:
//    • x86_64: kernel/arch/x86_64/rtc_cmos.c
//      (CMOS RTC, ports 0x70/0x71, BCD-encoded registers)
//
//  Weak defaults (kernel/driver/rtc.c): both return `false`,
//  signalling "no wall clock". A future aarch64 PL031 driver
//  adds kernel/arch/aarch64/rtc_pl031.c with strong overrides.
//
//  IMPORTANT: the include guard MUST differ from any per-arch
//  header (kernel/arch/<arch>/rtc_*.c uses its own guard). This
//  is the same pattern as kernel/include/arch/regs.h
//  (see the FACADE_H guard naming convention established there).
// ─────────────────────────────────────────────────────────

bool arch_rtc_read(datetime_t *out);
bool arch_rtc_write(const datetime_t *in);

#endif /* _KERNEL_ARCH_RTC_H */
