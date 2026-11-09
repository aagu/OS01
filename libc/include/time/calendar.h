#ifndef _TIME_CALENDAR_H
#define _TIME_CALENDAR_H 1

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libc/include/time/calendar.h — the PRIVATE time core (libc-completion
 * plan T13).
 *
 * Two things live here, and nothing else in the libc may re-implement
 * them:
 *
 *   1. The CALENDAR CORE: an integer days/seconds conversion between a
 *      time_t and a broken-down UTC time.  There is NO floating point
 *      anywhere in the conversion path — a second is a second and a day
 *      is a day, so the same answer comes out on every machine and the
 *      2038 boundary is not a special case.
 *
 *   2. The POSIX TZ SURFACE: the parsed zone (name, offsets, DST
 *      rules) and the offset lookup.  The parser lives in libc/time/tz.c
 *      and the cache lives with it; this header only carries the shape
 *      of the zone and the two calls the rest of the libc makes.
 *
 * The public <time.h> keeps its own declarations; this header is not
 * installed as a public interface and the audit excludes it.
 */

/* ── constants ─────────────────────────────────────────────────────── */

#define __OS01_SECONDS_PER_MINUTE 60L
#define __OS01_SECONDS_PER_HOUR   3600L
#define __OS01_SECONDS_PER_DAY    86400L

/* The broken-down year must survive the trip through `int tm_year`
 * (the public <time.h> spells it that way), so the representable range
 * is the one the field can carry.  The seconds of that range still fit
 * in a 64-bit time_t with room to spare. */
#define __OS01_MIN_YEAR (-2147483647LL - 1900LL)
#define __OS01_MAX_YEAR ( 2147483647LL + 1900LL)

/* ── the calendar core ─────────────────────────────────────────────── */

/* Days since 1970-01-01 (proleptic Gregorian) for a civil date.
 * `month` is 1-12 and `day` is 1-31; the caller normalizes first. */
int64_t __os01_days_from_civil(int64_t year, int month, int day);

/* The inverse: the civil date of a day count. */
void __os01_civil_from_days(int64_t days, int64_t *year, int *month, int *day);

/* 1 when `year` is a leap year in the proleptic Gregorian calendar. */
int __os01_is_leap_year(int64_t year);

/* 0-6 (0 = Sunday) for a day count since the epoch. */
int __os01_weekday_from_days(int64_t days);

/* 0-based day of the year (0 = January 1st). */
int __os01_year_day(int64_t year, int month, int day);

/* Normalize the broken-down fields of `tm` and report the instant they
 * name, treating the fields as a UTC wall clock:
 *
 *   - the month and the year are normalized against each other (a
 *     tm_mon of 12 is January of the next year),
 *   - the day may be out of range (32 January is 1 February),
 *   - the time of day may overflow into the next day,
 *   - tm_wday and tm_yday are filled in.
 *
 * The fields are written back in their canonical ranges.  Returns 0 and
 * stores the seconds in `*seconds` on success, and -1 when the result
 * is not representable (the caller turns that into EOVERFLOW). */
int __os01_tm_normalize(struct tm *tm, int64_t *seconds);

/* The inverse: break an instant into a broken-down UTC time.  Returns 0
 * on success and -1 when the instant is out of the representable
 * range. */
int __os01_tm_from_seconds(int64_t seconds, struct tm *tm);

/* ── the C locale text ─────────────────────────────────────────────── */

/* The day and month names of the C locale.  They live in
 * libc/time/strftime.c (the formatter owns the text) and are shared
 * with the asctime() layout in libc/time/time_r.c. */
extern const char *const __os01_wday_abbr[7];
extern const char *const __os01_wday_full[7];
extern const char *const __os01_mon_abbr[12];
extern const char *const __os01_mon_full[12];

/* ── the POSIX TZ surface ─────────────────────────────────────────── */

/* A DST transition rule.  Three spellings, per POSIX:
 *   __OS01_TZ_JULIAN  "Jn"   the n-th day of the year, February 29
 *                           never counted (n is 1-365),
 *   __OS01_TZ_DOY     "n"    the n-th day of the year, February 29
 *                           counted (n is 0-365),
 *   __OS01_TZ_MWD     "Mm.w.d" the w-th weekday d of month m. */
#define __OS01_TZ_JULIAN 0
#define __OS01_TZ_DOY    1
#define __OS01_TZ_MWD    2

struct __os01_tz_rule {
    int kind;   /* __OS01_TZ_* */
    int n;      /* Jn: 1-365, n: 0-365 */
    int month;  /* M: 1-12 */
    int week;   /* M: 1-5 */
    int wday;   /* M: 0-6 (0 = Sunday) */
    int time;   /* seconds after local midnight */
};

struct __os01_tzinfo {
    int  has_dst;
    char std_name[16];
    char dst_name[16];
    int  std_off;   /* seconds EAST of UTC */
    int  dst_off;   /* seconds EAST of UTC */
    struct __os01_tz_rule start;   /* when DST begins */
    struct __os01_tz_rule end;     /* when DST ends */
};

/* Re-read the TZ environment variable and refresh the cache.  A missing,
 * empty or malformed TZ leaves the zone at UTC. */
void __os01_tzset(void);

/* The cached zone.  Never NULL: an unset or malformed TZ reads back as
 * UTC. */
const struct __os01_tzinfo *__os01_tz(void);

/* The offset (seconds EAST of UTC) in effect at the given instant, and
 * the DST flag that goes with it (`*isdst` may be NULL). */
int __os01_tz_offset(int64_t seconds, int *isdst);

#ifdef __cplusplus
}
#endif

#endif /* _TIME_CALENDAR_H */
