#include <driver/rtc.h>
#include <hw.h>

bool is_updating_rtc()
{
    outb(CMOS_ADDR, 0x0a);
    uint32_t status = inb(CMOS_DATA);
    return (status & 0x80);
}

uint8_t get_rtc_register(uint8_t nr)
{
    outb(CMOS_ADDR, 0x80 | nr);
    return inb(CMOS_DATA);
}

void set_rtc_register(uint8_t nr, uint8_t val)
{
    outb(CMOS_ADDR, 0x80 | nr);
    outb(CMOS_DATA,val);
}

void rtc_read_datetime(datetime_t * dt)
{
    while (is_updating_rtc());
    
    dt->year = get_rtc_register(0x09);
    dt->month = get_rtc_register(0x08);
    dt->day = get_rtc_register(0x07);
    dt->hour = get_rtc_register(0x04);
    dt->minute = get_rtc_register(0x02);
    dt->second = get_rtc_register(0x00);
    
    uint8_t Use_BCD = get_rtc_register(0x0b);
    if (!(Use_BCD & 0x04))
    {
        dt->year = BCD2BIN(dt->year);
        dt->month = BCD2BIN(dt->month);
        dt->day = BCD2BIN(dt->day);
        dt->hour = BCD2BIN(dt->hour);
        dt->minute = BCD2BIN(dt->minute);
        dt->second = BCD2BIN(dt->second);
    }
    
}

void rtc_write_datetime(datetime_t * dt)
{
    while (is_updating_rtc());

    set_rtc_register(0x00, dt->second);
    set_rtc_register(0x02, dt->minute);
    set_rtc_register(0x04, dt->hour);
    set_rtc_register(0x07, dt->day);
    set_rtc_register(0x08, dt->month);
    set_rtc_register(0x09, dt->year);
}