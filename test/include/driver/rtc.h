#ifndef _KERNEL_RTC_H
#define _KERNEL_RTC_H

#include <stdint.h>
#include <stdbool.h>

// Host-side mirror of kernel/include/driver/rtc.h. Kept in sync
// with the in-kernel public API: arch-neutral datetime struct and
// rtc_read/write_datetime entry points.

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
