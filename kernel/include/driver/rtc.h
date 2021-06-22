#ifndef _KERNEL_RTC_H
#define _KERNEL_RTC_H

#include <stdint.h>
#include <stdbool.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define BCD2BIN(value)  (((value) & 0xf) + ((value) >> 4) * 10);

typedef struct datetime
{
    uint8_t century;
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
}datetime_t;

bool is_updating_rtc();

uint8_t get_rtc_register(uint8_t nr);

void set_rtc_register(uint8_t nr, uint8_t val);

void rtc_read_datetime(datetime_t * dt);

void rtc_write_datetime(datetime_t * dt);

#endif