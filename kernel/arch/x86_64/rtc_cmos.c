/* kernel/arch/x86_64/rtc_cmos.c -- x86_64 strong override for the
 * arch-neutral RTC wall-clock hooks (kernel/include/kernel/arch/rtc.h).
 *
 * Implements arch_rtc_read / arch_rtc_write on top of the legacy
 * PC-AT CMOS RTC (I/O ports 0x70 / 0x71). All register numbers,
 * the BCD-vs-binary toggle (status register B bit 2), and the
 * "update in progress" check (status register A bit 7) are
 * specific to the MC146818 family and stay local to this file.
 *
 * The weak defaults in kernel/driver/rtc.c return false; this
 * file's strong definitions are linked in instead on x86_64
 * builds via $(wildcard $(ARCHDIR)/*.c).
 */

#include <arch/rtc.h>        // arch_rtc_read / arch_rtc_write
#include <arch/io.h>         // arch_inb / arch_outb (port I/O)
#include <driver/rtc.h>             // datetime_t

// PC-AT CMOS RTC port pair. Local to this file -- never escape.
#define CMOS_ADDR 0x70u
#define CMOS_DATA 0x71u

// Bit 7 of register A is the "update in progress" flag. Polled
// before reading the date/time registers so a multi-byte read
// doesn't straddle an update cycle and yield garbage.
#define CMOS_STA_A_UIP  (1u << 7)

// Bit 2 of register B selects binary (1) vs BCD (0) encoding.
#define CMOS_STA_B_BIN  (1u << 2)

// CMOS register indices for date/time. Local to MC146818.
#define CMOS_REG_SECOND    0x00u
#define CMOS_REG_MINUTE    0x02u
#define CMOS_REG_HOUR      0x04u
#define CMOS_REG_DAY       0x07u
#define CMOS_REG_MONTH     0x08u
#define CMOS_REG_YEAR      0x09u
#define CMOS_REG_STATUS_A  0x0Au
#define CMOS_REG_STATUS_B  0x0Bu

// BCD→binary: two-digit packed BCD → 0..99.
#define BCD2BIN(v)  ((uint8_t)(((v) & 0x0fu) + (((v) >> 4) & 0x0fu) * 10u))

static inline bool cmos_is_updating(void)
{
    return (arch_inb(CMOS_ADDR) & (unsigned)CMOS_STA_A_UIP) != 0;
}

static inline uint8_t cmos_read(uint8_t reg)
{
    arch_outb(CMOS_ADDR, (uint8_t)(0x80u | reg));   // 0x80 = NMI disable bit
    return arch_inb(CMOS_DATA);
}

static inline void cmos_write(uint8_t reg, uint8_t val)
{
    arch_outb(CMOS_ADDR, (uint8_t)(0x80u | reg));
    arch_outb(CMOS_DATA, val);
}

bool arch_rtc_read(datetime_t *out)
{
    // Wait until the RTC is not in the middle of an update.
    while (cmos_is_updating()) { /* spin */ }

    uint8_t second = cmos_read(CMOS_REG_SECOND);
    uint8_t minute = cmos_read(CMOS_REG_MINUTE);
    uint8_t hour   = cmos_read(CMOS_REG_HOUR);
    uint8_t day    = cmos_read(CMOS_REG_DAY);
    uint8_t month  = cmos_read(CMOS_REG_MONTH);
    uint8_t year   = cmos_read(CMOS_REG_YEAR);
    uint8_t status_b = cmos_read(CMOS_REG_STATUS_B);

    if ((status_b & CMOS_STA_B_BIN) == 0) {
        second = BCD2BIN(second);
        minute = BCD2BIN(minute);
        hour   = BCD2BIN(hour);
        day    = BCD2BIN(day);
        month  = BCD2BIN(month);
        year   = BCD2BIN(year);
    }

    out->century = 0;   // CMOS doesn't carry century; ignored.
    out->year    = year;
    out->month   = month;
    out->day     = day;
    out->hour    = hour;
    out->minute  = minute;
    out->second  = second;
    return true;
}

bool arch_rtc_write(const datetime_t *in)
{
    while (cmos_is_updating()) { /* spin */ }

    cmos_write(CMOS_REG_SECOND, in->second);
    cmos_write(CMOS_REG_MINUTE, in->minute);
    cmos_write(CMOS_REG_HOUR,   in->hour);
    cmos_write(CMOS_REG_DAY,    in->day);
    cmos_write(CMOS_REG_MONTH,  in->month);
    cmos_write(CMOS_REG_YEAR,   in->year);
    return true;
}
