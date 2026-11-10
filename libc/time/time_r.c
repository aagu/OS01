/*
 * libc/time/time_r.c — the reentrant variants and the static-storage
 * wrappers (T13).
 *
 * The _r variants write ONLY into the caller's object: they touch no
 * static storage, so two threads (or two nested calls) never see each
 * other's answer.  The plain variants keep the POSIX promise of a
 * static result that the next call overwrites.
 *
 * asctime()/ctime() use the fixed 26-byte layout
 *
 *     "Www Mmm dd hh:mm:ss yyyy\n"
 *
 * and FAIL when the broken-down time cannot be laid out in it (a year
 * that needs more than four characters, a field that is out of range):
 * the layout is a promise about the buffer size, so a value that does
 * not fit is an error, not a truncated string.
 */

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>

#include <time/calendar.h>

/* ── gmtime_r / localtime_r ────────────────────────────────────────── */

struct tm *gmtime_r(const time_t *t, struct tm *r)
{
    struct tm tm;

    if (t == NULL || r == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (__os01_tm_from_seconds((int64_t)*t, &tm) != 0) {
        errno = EOVERFLOW;
        return NULL;
    }
    /* gmtime does not depend on the zone: there is no daylight saving
     * time in UTC. */
    tm.tm_isdst = 0;
    *r = tm;
    return r;
}

struct tm *localtime_r(const time_t *t, struct tm *r)
{
    struct tm tm;
    int isdst = 0;
    int64_t offset;

    if (t == NULL || r == NULL) {
        errno = EINVAL;
        return NULL;
    }

    offset = __os01_tz_offset((int64_t)*t, &isdst);
    if (__os01_tm_from_seconds((int64_t)*t + offset, &tm) != 0) {
        errno = EOVERFLOW;
        return NULL;
    }
    tm.tm_isdst = isdst;
    *r = tm;
    return r;
}

/* ── asctime / ctime ───────────────────────────────────────────────── */

/* The fixed layout, built in a scratch buffer so that its length can
 * be checked before anything is written to the caller's memory. */
static char *asctime_format(const struct tm *t, char *buf)
{
    char digits[24];
    int ndigits = 0;
    int year_len;
    long year_value = (long)t->tm_year + 1900;
    int i;

    if (t->tm_mday > 999 || t->tm_mday < 0 ||
        t->tm_hour > 99 || t->tm_hour < 0 ||
        t->tm_min > 99 || t->tm_min < 0 ||
        t->tm_sec > 99 || t->tm_sec < 0) {
        errno = EOVERFLOW;
        return NULL;
    }

    /* The year must fit the four characters the layout gives it. */
    {
        long value = year_value;
        unsigned long mag = (value < 0) ? (unsigned long)(-(value + 1)) + 1UL
                                        : (unsigned long)value;
        if (mag == 0) {
            digits[ndigits++] = '0';
        }
        while (mag > 0 && ndigits < (int)sizeof(digits)) {
            digits[ndigits++] = (char)('0' + (mag % 10));
            mag /= 10;
        }
        /* The digits were collected backwards: reverse them. */
        for (i = 0; i < ndigits / 2; i++) {
            char tmp = digits[i];
            digits[i] = digits[ndigits - 1 - i];
            digits[ndigits - 1 - i] = tmp;
        }
        year_len = ndigits + (value < 0 ? 1 : 0);
    }
    if (year_len > 4) {
        /* The layout has room for four year characters only. */
        errno = EOVERFLOW;
        return NULL;
    }

    int wday = t->tm_wday % 7;
    if (wday < 0) {
        wday += 7;
    }
    int mon = t->tm_mon % 12;
    if (mon < 0) {
        mon += 12;
    }

    /* "Www Mmm dd hh:mm:ss yyyy\n" */
    const char *wd = __os01_wday_abbr[wday];
    const char *mo = __os01_mon_abbr[mon];
    int n = 0;

    buf[n++] = wd[0];
    buf[n++] = wd[1];
    buf[n++] = wd[2];
    buf[n++] = ' ';
    buf[n++] = mo[0];
    buf[n++] = mo[1];
    buf[n++] = mo[2];
    /* the day of the month, right aligned in three characters */
    if (t->tm_mday >= 100) {
        buf[n++] = (char)('0' + (t->tm_mday / 100) % 10);
    } else {
        buf[n++] = ' ';
    }
    if (t->tm_mday >= 10) {
        buf[n++] = (char)('0' + (t->tm_mday / 10) % 10);
    } else {
        buf[n++] = ' ';
    }
    buf[n++] = (char)('0' + t->tm_mday % 10);
    buf[n++] = ' ';
    buf[n++] = (char)('0' + (t->tm_hour / 10) % 10);
    buf[n++] = (char)('0' + t->tm_hour % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + (t->tm_min / 10) % 10);
    buf[n++] = (char)('0' + t->tm_min % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + (t->tm_sec / 10) % 10);
    buf[n++] = (char)('0' + t->tm_sec % 10);
    buf[n++] = ' ';
    if (year_value < 0) {
        buf[n++] = '-';
    }
    for (i = 0; i < ndigits; i++) {
        buf[n++] = digits[i];
    }
    buf[n++] = '\n';
    buf[n] = '\0';

    return buf;
}

char *asctime_r(const struct tm *t, char *b)
{
    char scratch[64];

    if (t == NULL || b == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (asctime_format(t, scratch) == NULL) {
        return NULL;
    }
    /* The layout is 26 bytes: 25 characters and the NUL. */
    {
        size_t i = 0;
        while (scratch[i] != '\0') {
            b[i] = scratch[i];
            i++;
        }
        b[i] = '\0';
    }
    return b;
}

char *ctime_r(const time_t *t, char *b)
{
    struct tm tm;

    if (t == NULL || b == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (localtime_r(t, &tm) == NULL) {
        return NULL;
    }
    return asctime_r(&tm, b);
}

char *asctime(const struct tm *t)
{
    static char buf[26];

    return asctime_r(t, buf);
}

char *ctime(const time_t *t)
{
    static char buf[26];

    return ctime_r(t, buf);
}
