#ifndef _KERNEL_RTC_H
#define _KERNEL_RTC_H

#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────
//  RTC — arch-neutral public API
//
//  OS01's RTC abstraction covers "read/write wall-clock datetime".
//  The hardware-specific register layout (CMOS indices, BCD packing,
//  PL031 MMIO, etc.) lives in per-arch overrides of `arch_rtc_read`
//  and `arch_rtc_write` and is invisible to kernel code outside the
//  per-arch rtc_*.c file.
//
//  Return convention: `true` if the read/write succeeded; `false`
//  if the arch has no RTC wired up (e.g. aarch64 phase 1, before
//  PL031 lands). Callers should treat `false` as "no wall clock
//  available" and either fall back to TSC uptime or simply log.
//
//  Per-arch hooks (declared in kernel/include/arch/rtc.h):
//
//    bool arch_rtc_read(datetime_t *out);
//    bool arch_rtc_write(const datetime_t *in);
//
//  Default: both return false (kernel/include/arch/rtc.h
//  weak defaults). Override: kernel/arch/<arch>/rtc_*.c provides
//  the strong definition.
// ─────────────────────────────────────────────────────────

typedef struct datetime
{
    uint8_t century;
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} datetime_t;

bool rtc_read_datetime(datetime_t *dt);
bool rtc_write_datetime(const datetime_t *dt);

#endif /* _KERNEL_RTC_H */
