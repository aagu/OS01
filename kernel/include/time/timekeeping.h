#ifndef _KERNEL_TIMEKEEPING_H
#define _KERNEL_TIMEKEEPING_H

#include <stdint.h>
#include <stdbool.h>
#include <driver/rtc.h>

/* Convert a broken-down RTC datetime (UTC) to Unix epoch seconds.
 * Returns false if any field is out of range or before 1970-01-01. */
bool timekeeping_datetime_to_epoch(const datetime_t *dt, uint64_t *epoch_sec_out);

/* Anchor wall-clock time from the platform RTC + monotonic clocksource. */
void timekeeping_init(void);

/* Current wall-clock time in nanoseconds since 1970-01-01T00:00:00Z.
 * Falls back to arch_clocksource_read_ns() when no RTC is available. */
uint64_t timekeeping_real_ns(void);

/* True if wall-clock time was anchored from an RTC at boot. */
bool timekeeping_has_rtc(void);

#endif /* _KERNEL_TIMEKEEPING_H */
