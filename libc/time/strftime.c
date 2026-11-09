/*
 * libc/time/strftime.c — the time formatter (T13).
 *
 * The C locale only: OS01 ships no locale database, so the day and
 * month names, the date/time representations (%c, %x, %X) and the
 * AM/PM marker are the plain C ones.  The conversions are the ones the
 * plan lists:
 *
 *   a A b B c C d D e F g G h H I j m M n p r R S t T u U V w W x X
 *   y Y z Z %%
 *
 * The E and O modifiers select the C locale BASIC form (the locale has
 * no alternative form).  An unknown conversion is an error: strftime
 * answers 0 and writes nothing usable, exactly like a buffer that is
 * too small.
 *
 * The buffer contract is POSIX: the result is NUL terminated, the
 * return value is the number of characters written WITHOUT the NUL,
 * and a result that does not fit answers 0.  A maxsize of 0 answers 0
 * and touches no memory at all.
 */

#include <time.h>
#include <stdint.h>
#include <stddef.h>

#include <time/calendar.h>

/* ── the C locale text ─────────────────────────────────────────────── */

const char *const __os01_wday_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
const char *const __os01_wday_full[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};
const char *const __os01_mon_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
const char *const __os01_mon_full[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* ── the output sink ───────────────────────────────────────────────── */

struct sf_out {
    char  *buf;
    size_t max;    /* the size of the buffer */
    size_t len;    /* the number of characters the result needs */
};

static void sf_putc(struct sf_out *o, char c)
{
    if (o->buf != NULL && o->len + 1 < o->max) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void sf_puts(struct sf_out *o, const char *s)
{
    while (*s != '\0') {
        sf_putc(o, *s++);
    }
}

/* A decimal number, right aligned in `width` characters: zero padded
 * when `zero` is set, space padded otherwise. */
static void sf_putnum(struct sf_out *o, long value, int width, int zero)
{
    char digits[24];
    int n = 0;
    unsigned long mag;
    int sign = 0;

    if (value < 0) {
        sign = 1;
        mag = (unsigned long)(-(value + 1)) + 1UL;
    } else {
        mag = (unsigned long)value;
    }

    if (mag == 0) {
        digits[n++] = '0';
    }
    while (mag > 0) {
        digits[n++] = (char)('0' + (mag % 10));
        mag /= 10;
    }

    for (int i = n + sign; i < width; i++) {
        sf_putc(o, zero ? '0' : ' ');
    }
    if (sign) {
        sf_putc(o, '-');
    }
    while (n > 0) {
        sf_putc(o, digits[--n]);
    }
}

/* A year: at least four digits, more when the year needs them, and a
 * leading '-' for a year before the epoch. */
static void sf_putyear(struct sf_out *o, long year)
{
    char digits[24];
    int n = 0;
    unsigned long mag;

    if (year < 0) {
        sf_putc(o, '-');
        mag = (unsigned long)(-(year + 1)) + 1UL;
    } else {
        mag = (unsigned long)year;
    }

    if (mag == 0) {
        digits[n++] = '0';
    }
    while (mag > 0) {
        digits[n++] = (char)('0' + (mag % 10));
        mag /= 10;
    }

    for (int i = n; i < 4; i++) {
        sf_putc(o, '0');
    }
    while (n > 0) {
        sf_putc(o, digits[--n]);
    }
}

/* ── the ISO week-based year ───────────────────────────────────────── */

/* The number of ISO weeks in a year: 53 when the year starts on a
 * Thursday, or on a Wednesday in a leap year; 52 otherwise. */
static int sf_iso_weeks(int64_t year)
{
    int p = __os01_weekday_from_days(__os01_days_from_civil(year, 1, 1));

    if (p == 4) {
        return 53;
    }
    if (p == 3 && __os01_is_leap_year(year)) {
        return 53;
    }
    return 52;
}

/* The ISO week number of a day and the ISO week-based year it belongs
 * to. */
static int sf_iso_week(int64_t year, int yday, int wday, int64_t *iso_year)
{
    /* The ISO week runs from Monday to Sunday, so the weekday is
     * counted from Monday (0) to Sunday (6). */
    int wday_mon = (wday + 6) % 7;
    int week = (yday - wday_mon + 10) / 7;

    if (week < 1) {
        *iso_year = year - 1;
        week = sf_iso_weeks(year - 1);
    } else if (week > sf_iso_weeks(year)) {
        *iso_year = year + 1;
        week = 1;
    } else {
        *iso_year = year;
    }
    return week;
}

/* ── the formatter ─────────────────────────────────────────────────── */

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *tm)
{
    struct sf_out o;
    const char *f;

    if (format == NULL || tm == NULL) {
        return 0;
    }
    if (maxsize > 0 && s == NULL) {
        return 0;
    }

    o.buf = s;
    o.max = maxsize;
    o.len = 0;

    /* The caller may hand over a broken-down time whose weekday and day
     * of the year do not agree with the rest; the indexes are clamped so
     * that the formatter never reads out of the tables. */
    int wday = tm->tm_wday % 7;
    if (wday < 0) {
        wday += 7;
    }
    int mon = tm->tm_mon % 12;
    if (mon < 0) {
        mon += 12;
    }
    long year = (long)tm->tm_year + 1900;
    int hour12 = tm->tm_hour % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }

    for (f = format; *f != '\0'; f++) {
        if (*f != '%') {
            sf_putc(&o, *f);
            continue;
        }

        f++;
        if (*f == '\0') {
            return 0;               /* a trailing '%' is not a conversion */
        }
        if (*f == 'E' || *f == 'O') {
            /* The C locale has no alternative form: the basic one. */
            f++;
            if (*f == '\0') {
                return 0;
            }
        }

        switch (*f) {
        case 'a':
            sf_puts(&o, __os01_wday_abbr[wday]);
            break;
        case 'A':
            sf_puts(&o, __os01_wday_full[wday]);
            break;
        case 'b':
        case 'h':
            sf_puts(&o, __os01_mon_abbr[mon]);
            break;
        case 'B':
            sf_puts(&o, __os01_mon_full[mon]);
            break;
        case 'c':
            /* The C locale date and time: "%a %b %e %H:%M:%S %Y". */
            sf_puts(&o, __os01_wday_abbr[wday]);
            sf_putc(&o, ' ');
            sf_puts(&o, __os01_mon_abbr[mon]);
            sf_putc(&o, ' ');
            sf_putnum(&o, tm->tm_mday, 2, 0);
            sf_putc(&o, ' ');
            sf_putnum(&o, tm->tm_hour, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_min, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_sec, 2, 1);
            sf_putc(&o, ' ');
            sf_putyear(&o, year);
            break;
        case 'C':
            sf_putnum(&o, year / 100, 2, 1);
            break;
        case 'd':
            sf_putnum(&o, tm->tm_mday, 2, 1);
            break;
        case 'D':
            sf_putnum(&o, tm->tm_mon + 1, 2, 1);
            sf_putc(&o, '/');
            sf_putnum(&o, tm->tm_mday, 2, 1);
            sf_putc(&o, '/');
            sf_putnum(&o, ((year % 100) + 100) % 100, 2, 1);
            break;
        case 'e':
            sf_putnum(&o, tm->tm_mday, 2, 0);
            break;
        case 'F':
            sf_putyear(&o, year);
            sf_putc(&o, '-');
            sf_putnum(&o, tm->tm_mon + 1, 2, 1);
            sf_putc(&o, '-');
            sf_putnum(&o, tm->tm_mday, 2, 1);
            break;
        case 'g':
        case 'G':
        case 'V': {
            int64_t iso_year = 0;
            int iso_week = sf_iso_week((int64_t)year, tm->tm_yday, wday, &iso_year);
            if (*f == 'V') {
                sf_putnum(&o, iso_week, 2, 1);
            } else if (*f == 'G') {
                sf_putyear(&o, (long)iso_year);
            } else {
                sf_putnum(&o, (long)((iso_year % 100 + 100) % 100), 2, 1);
            }
            break;
        }
        case 'H':
            sf_putnum(&o, tm->tm_hour, 2, 1);
            break;
        case 'I':
            sf_putnum(&o, hour12, 2, 1);
            break;
        case 'j':
            sf_putnum(&o, tm->tm_yday + 1, 3, 1);
            break;
        case 'm':
            sf_putnum(&o, tm->tm_mon + 1, 2, 1);
            break;
        case 'M':
            sf_putnum(&o, tm->tm_min, 2, 1);
            break;
        case 'n':
            sf_putc(&o, '\n');
            break;
        case 'p':
            sf_puts(&o, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'r':
            /* The C locale 12-hour clock: "%I:%M:%S %p". */
            sf_putnum(&o, hour12, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_min, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_sec, 2, 1);
            sf_putc(&o, ' ');
            sf_puts(&o, tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'R':
            sf_putnum(&o, tm->tm_hour, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_min, 2, 1);
            break;
        case 'S':
            sf_putnum(&o, tm->tm_sec, 2, 1);
            break;
        case 't':
            sf_putc(&o, '\t');
            break;
        case 'T':
            sf_putnum(&o, tm->tm_hour, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_min, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_sec, 2, 1);
            break;
        case 'u':
            sf_putnum(&o, wday == 0 ? 7 : wday, 1, 0);
            break;
        case 'U':
            sf_putnum(&o, (tm->tm_yday + 7 - wday) / 7, 2, 1);
            break;
        case 'w':
            sf_putnum(&o, wday, 1, 0);
            break;
        case 'W':
            sf_putnum(&o, (tm->tm_yday + 7 - ((wday + 6) % 7)) / 7, 2, 1);
            break;
        case 'x':
            /* The C locale date: "%m/%d/%y". */
            sf_putnum(&o, tm->tm_mon + 1, 2, 1);
            sf_putc(&o, '/');
            sf_putnum(&o, tm->tm_mday, 2, 1);
            sf_putc(&o, '/');
            sf_putnum(&o, ((year % 100) + 100) % 100, 2, 1);
            break;
        case 'X':
            /* The C locale time: "%H:%M:%S". */
            sf_putnum(&o, tm->tm_hour, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_min, 2, 1);
            sf_putc(&o, ':');
            sf_putnum(&o, tm->tm_sec, 2, 1);
            break;
        case 'y':
            sf_putnum(&o, ((year % 100) + 100) % 100, 2, 1);
            break;
        case 'Y':
            sf_putyear(&o, year);
            break;
        case 'z': {
            const struct __os01_tzinfo *tz = __os01_tz();
            int off = (tm->tm_isdst > 0 && tz->has_dst) ? tz->dst_off
                                                         : tz->std_off;
            if (off < 0) {
                sf_putc(&o, '-');
                off = -off;
            } else {
                sf_putc(&o, '+');
            }
            sf_putnum(&o, off / 3600, 2, 1);
            sf_putnum(&o, (off % 3600) / 60, 2, 1);
            break;
        }
        case 'Z': {
            const struct __os01_tzinfo *tz = __os01_tz();
            sf_puts(&o, (tm->tm_isdst > 0 && tz->has_dst) ? tz->dst_name
                                                           : tz->std_name);
            break;
        }
        case '%':
            sf_putc(&o, '%');
            break;
        default:
            /* An unknown conversion: the whole call fails. */
            return 0;
        }
    }

    if (o.len + 1 > o.max) {
        return 0;                   /* the result does not fit */
    }
    if (o.max > 0) {
        o.buf[o.len] = '\0';
    }
    return o.len;
}
