/*
 * libc/time/calendar.c — the integer days/seconds calendar core (T13).
 *
 * Everything here is INTEGER arithmetic: a broken-down time is a day
 * count plus a second-of-day, never a floating point number of
 * seconds.  Two consequences the tests pin down:
 *
 *   - the answer does not depend on the machine's FPU, so 1970 and 2038
 *     and 2100 are all the same code path;
 *   - the epoch is day 0, and the weekday of day 0 is Thursday.
 *
 * The civil-date <-> day-count conversion is the standard "days from
 * civil" algorithm (Howard Hinnant's, as used by the C++20
 * chrono calendar): it shifts the year into a 400-year era, computes
 * the day of the era with a closed-form expression, and shifts back.
 * It is exact for the whole proleptic Gregorian range and uses no
 * lookup table.
 *
 * The normalization follows POSIX: the broken-down fields are free-form
 * (the month may be out of range, the day may be 0 or 32, the time of
 * day may overflow) and mktime() puts them back into their canonical
 * ranges.
 */

#include <time.h>
#include <stdint.h>

#include <time/calendar.h>

/* ── civil date <-> day count ──────────────────────────────────────── */

int64_t __os01_days_from_civil(int64_t year, int month, int day)
{
    /* March-based year: the leap day lands at the end of the year. */
    int64_t y = year - (month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    /* [0, 399] */
    int64_t yoe = y - era * 400;
    /* [0, 365], March = 0 */
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    /* [0, 146096] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

void __os01_civil_from_days(int64_t days, int64_t *year, int *month, int *day)
{
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    /* [0, 146096] */
    int64_t doe = z - era * 146097;
    /* [0, 399] */
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    /* [0, 365] */
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    /* [0, 11], March = 0 */
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;   /* [1, 31] */
    int64_t m = mp + (mp < 10 ? 3 : -9);         /* [1, 12] */

    *year = y + (m <= 2 ? 1 : 0);
    *month = (int)m;
    *day = (int)d;
}

int __os01_is_leap_year(int64_t year)
{
    if (year % 4 != 0) {
        return 0;
    }
    if (year % 100 != 0) {
        return 1;
    }
    return year % 400 == 0;
}

int __os01_weekday_from_days(int64_t days)
{
    /* Day 0 (1970-01-01) was a Thursday. */
    int64_t w = (days + 4) % 7;
    if (w < 0) {
        w += 7;
    }
    return (int)w;
}

int __os01_year_day(int64_t year, int month, int day)
{
    return (int)(__os01_days_from_civil(year, month, day) -
                 __os01_days_from_civil(year, 1, 1));
}

/* ── broken-down time <-> instant ──────────────────────────────────── */

/* Floor division that also works for a negative numerator. */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q -= 1;
    }
    return q;
}

int __os01_tm_normalize(struct tm *tm, int64_t *seconds)
{
    int64_t year = (int64_t)tm->tm_year + 1900;
    int64_t month = (int64_t)tm->tm_mon + 1;    /* 1-12 */
    int64_t day = (int64_t)tm->tm_mday;
    int64_t secs = (int64_t)tm->tm_sec
                 + (int64_t)tm->tm_min * __OS01_SECONDS_PER_MINUTE
                 + (int64_t)tm->tm_hour * __OS01_SECONDS_PER_HOUR;

    /* The month may be out of range: normalize it against the year. */
    int64_t mon0 = month - 1;
    year += floor_div(mon0, 12);
    mon0 = mon0 - floor_div(mon0, 12) * 12;
    month = mon0 + 1;

    /* The time of day may overflow into the neighbouring days. */
    int64_t day_carry = floor_div(secs, __OS01_SECONDS_PER_DAY);
    secs -= day_carry * __OS01_SECONDS_PER_DAY;

    /* The day may be out of range (0, 32, negative). */
    int64_t days = __os01_days_from_civil(year, (int)month, 1) + (day - 1) +
                   day_carry;

    int64_t civil_year;
    int civil_month;
    int civil_day;
    __os01_civil_from_days(days, &civil_year, &civil_month, &civil_day);

    if (civil_year < __OS01_MIN_YEAR || civil_year > __OS01_MAX_YEAR) {
        return -1;
    }

    int64_t total = days * __OS01_SECONDS_PER_DAY + secs;
    /* The day count is bounded by the year range above, so the product
     * cannot overflow a 64-bit second count; the check is kept as the
     * contract, not as decoration. */
    if (floor_div(total, __OS01_SECONDS_PER_DAY) != days) {
        return -1;
    }

    tm->tm_year = (int)(civil_year - 1900);
    tm->tm_mon = civil_month - 1;
    tm->tm_mday = civil_day;
    tm->tm_hour = (int)(secs / __OS01_SECONDS_PER_HOUR);
    tm->tm_min = (int)((secs / __OS01_SECONDS_PER_MINUTE) %
                       (__OS01_SECONDS_PER_MINUTE));
    tm->tm_sec = (int)(secs % __OS01_SECONDS_PER_MINUTE);
    tm->tm_wday = __os01_weekday_from_days(days);
    tm->tm_yday = (int)(days - __os01_days_from_civil(civil_year, 1, 1));

    *seconds = total;
    return 0;
}

int __os01_tm_from_seconds(int64_t seconds, struct tm *tm)
{
    int64_t days = floor_div(seconds, __OS01_SECONDS_PER_DAY);
    int64_t secs = seconds - days * __OS01_SECONDS_PER_DAY;

    int64_t year;
    int month;
    int day;
    __os01_civil_from_days(days, &year, &month, &day);

    if (year < __OS01_MIN_YEAR || year > __OS01_MAX_YEAR) {
        return -1;
    }

    tm->tm_year = (int)(year - 1900);
    tm->tm_mon = month - 1;
    tm->tm_mday = day;
    tm->tm_hour = (int)(secs / __OS01_SECONDS_PER_HOUR);
    tm->tm_min = (int)((secs / __OS01_SECONDS_PER_MINUTE) %
                       (__OS01_SECONDS_PER_MINUTE));
    tm->tm_sec = (int)(secs % __OS01_SECONDS_PER_MINUTE);
    tm->tm_wday = __os01_weekday_from_days(days);
    tm->tm_yday = (int)(days - __os01_days_from_civil(year, 1, 1));

    return 0;
}
