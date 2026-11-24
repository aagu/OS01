/* kernel/arch/x86_64/rtc_cmos.c -- x86_64 strong override for the
 * arch-neutral RTC wall-clock hooks (kernel/include/arch/rtc.h).
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
#include <arch/irq.h>        // arch_local_irq_save / arch_local_irq_restore
#include <driver/rtc.h>      // datetime_t

// PC-AT CMOS RTC port pair. Local to this file -- never escape.
#define CMOS_ADDR 0x70u
#define CMOS_DATA 0x71u

// Bit 7 of register A is the "update in progress" flag. Polled
// before reading the date/time registers so a multi-byte read
// doesn't straddle an update cycle and yield garbage.
#define CMOS_STA_A_UIP  (1u << 7)

// Bit 1 of register B selects 24-hour (1) vs 12-hour (0) mode.
// Bit 2 of register B selects binary (1) vs BCD (0) encoding.
#define CMOS_STA_B_24H  (1u << 1)
#define CMOS_STA_B_BIN  (1u << 2)
#define CMOS_HOUR_PM    0x80u

// CMOS register indices for date/time. Local to MC146818.
#define CMOS_REG_SECOND    0x00u
#define CMOS_REG_MINUTE    0x02u
#define CMOS_REG_HOUR      0x04u
#define CMOS_REG_DAY       0x07u
#define CMOS_REG_MONTH     0x08u
#define CMOS_REG_YEAR      0x09u
#define CMOS_REG_STATUS_A  0x0Au
#define CMOS_REG_STATUS_B  0x0Bu
#define CMOS_REG_CENTURY   0x32u

// BCD→binary: two-digit packed BCD → 0..99.
#define BCD2BIN(v)  ((uint8_t)(((v) & 0x0fu) + (((v) >> 4) & 0x0fu) * 10u))

static inline uint8_t cmos_read(uint8_t reg)
{
    arch_irq_state_t flags = arch_local_irq_save();
    arch_outb(CMOS_ADDR, (uint8_t)(0x80u | reg));   // 0x80 = NMI disable bit
    uint8_t val = arch_inb(CMOS_DATA);
    arch_local_irq_restore(flags);
    return val;
}

static inline void cmos_write(uint8_t reg, uint8_t val)
{
    arch_irq_state_t flags = arch_local_irq_save();
    arch_outb(CMOS_ADDR, (uint8_t)(0x80u | reg));
    arch_outb(CMOS_DATA, val);
    arch_local_irq_restore(flags);
}

static inline bool cmos_is_updating(void)
{
    return (cmos_read(CMOS_REG_STATUS_A) & CMOS_STA_A_UIP) != 0;
}

static void cmos_sample_raw(datetime_t *raw, uint8_t *status_b)
{
    while (cmos_is_updating()) { /* spin */ }

    raw->second  = cmos_read(CMOS_REG_SECOND);
    raw->minute  = cmos_read(CMOS_REG_MINUTE);
    raw->hour    = cmos_read(CMOS_REG_HOUR);
    raw->day     = cmos_read(CMOS_REG_DAY);
    raw->month   = cmos_read(CMOS_REG_MONTH);
    raw->year    = cmos_read(CMOS_REG_YEAR);
    raw->century = cmos_read(CMOS_REG_CENTURY);
    *status_b    = cmos_read(CMOS_REG_STATUS_B);
}

static bool cmos_raw_equal(const datetime_t *a, const datetime_t *b)
{
    return a->second  == b->second  &&
           a->minute  == b->minute  &&
           a->hour    == b->hour    &&
           a->day     == b->day     &&
           a->month   == b->month   &&
           a->year    == b->year    &&
           a->century == b->century;
}

bool arch_rtc_read(datetime_t *out)
{
    datetime_t cur, prev;
    uint8_t status_b = 0;

    cmos_sample_raw(&cur, &status_b);
    for (int tries = 0; tries < 8; tries++) {
        prev = cur;
        cmos_sample_raw(&cur, &status_b);
        if (cmos_raw_equal(&cur, &prev))
            break;
    }

    uint8_t hour_pm = cur.hour & CMOS_HOUR_PM;
    uint8_t hour_val = (status_b & CMOS_STA_B_24H) ? cur.hour
                                                   : (uint8_t)(cur.hour & ~CMOS_HOUR_PM);

    if ((status_b & CMOS_STA_B_BIN) == 0) {
        cur.second  = BCD2BIN(cur.second);
        cur.minute  = BCD2BIN(cur.minute);
        hour_val    = BCD2BIN(hour_val);
        cur.day     = BCD2BIN(cur.day);
        cur.month   = BCD2BIN(cur.month);
        cur.year    = BCD2BIN(cur.year);
        cur.century = BCD2BIN(cur.century);
    }

    if ((status_b & CMOS_STA_B_24H) == 0) {
        if (hour_pm && hour_val < 12u)
            hour_val = (uint8_t)(hour_val + 12u);
        else if (!hour_pm && hour_val == 12u)
            hour_val = 0u;
    }

    out->century = (cur.century >= 19u && cur.century <= 21u) ? cur.century : 0u;
    out->year    = cur.year;
    out->month   = cur.month;
    out->day     = cur.day;
    out->hour    = hour_val;
    out->minute  = cur.minute;
    out->second  = cur.second;
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
