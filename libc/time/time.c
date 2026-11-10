/*
 * libc/time/time.c — the public time entry points (T13).
 *
 * The conversion core lives in libc/time/calendar.c (integer days and
 * seconds, no floating point), the zone lives in libc/time/tz.c and the
 * formatter in libc/time/strftime.c.  This file owns the public
 * contract:
 *
 *   - gmtime()/localtime() use static storage (POSIX), the _r variants
 *     write into the caller's object and touch nothing else,
 *   - mktime() normalizes the broken-down fields and honours tm_isdst,
 *   - a conversion that cannot be represented fails with EOVERFLOW,
 *   - a SUCCESSFUL mktime() never sets errno, even when the answer is
 *     (time_t)-1 (which is a legitimate instant: 1969-12-31 23:59:59).
 */

#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

#include <time/calendar.h>

/* ── the broken-down time <-> local wall clock ─────────────────────── */

/* The instant a local wall clock names, honouring `isdst`:
 *
 *   isdst > 0  the caller insists on daylight time: the daylight
 *              offset is used, even when the wall clock does not exist
 *              (the spring gap),
 *   isdst == 0 the caller insists on standard time,
 *   isdst < 0  the libc decides: the offset whose round trip is exact
 *              wins, and an AMBIGUOUS wall clock (the autumn repeat)
 *              is resolved to the STANDARD time — the later of the two
 *              instants.  A wall clock that does not exist at all (the
 *              spring gap) is normalized FORWARD, using the standard
 *              offset. */
static int64_t tz_local_seconds(int64_t wall, int isdst)
{
    const struct __os01_tzinfo *tz = __os01_tz();

    if (!tz->has_dst) {
        return wall - tz->std_off;
    }

    int64_t cand_std = wall - tz->std_off;
    int64_t cand_dst = wall - tz->dst_off;
    int flag_std = 0;
    int flag_dst = 0;
    int64_t off_std = __os01_tz_offset(cand_std, &flag_std);
    int64_t off_dst = __os01_tz_offset(cand_dst, &flag_dst);

    if (isdst > 0) {
        return cand_dst;
    }
    if (isdst == 0) {
        return cand_std;
    }

    int exact_std = (cand_std + off_std == wall);
    int exact_dst = (cand_dst + off_dst == wall);

    if (exact_std && exact_dst) {
        /* Ambiguous: the plan fixes the choice on the standard time. */
        return cand_std;
    }
    if (exact_std) {
        return cand_std;
    }
    if (exact_dst) {
        return cand_dst;
    }
    /* The spring gap: the wall clock does not exist, normalize it
     * forward with the standard offset. */
    return cand_std;
}

/* Does the instant fit in a time_t? */
static int tz_seconds_fit(int64_t seconds)
{
    return (int64_t)(time_t)seconds == seconds;
}

/* ── gmtime / localtime ────────────────────────────────────────────── */

struct tm *gmtime(const time_t *t)
{
    static struct tm result;

    if (t == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (gmtime_r(t, &result) == NULL) {
        return NULL;
    }
    return &result;
}

struct tm *localtime(const time_t *t)
{
    static struct tm result;

    if (t == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (localtime_r(t, &result) == NULL) {
        return NULL;
    }
    return &result;
}

/* ── mktime / timegm ───────────────────────────────────────────────── */

time_t mktime(struct tm *tm)
{
    int64_t wall;
    int64_t seconds;
    struct tm normalized;

    if (tm == NULL) {
        errno = EINVAL;
        return (time_t)-1;
    }

    if (__os01_tm_normalize(tm, &wall) != 0) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }

    seconds = tz_local_seconds(wall, tm->tm_isdst);

    /* Write back the local time of the answer. */
    int isdst = 0;
    int64_t offset = __os01_tz_offset(seconds, &isdst);
    if (__os01_tm_from_seconds(seconds + offset, &normalized) != 0) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    normalized.tm_isdst = isdst;
    *tm = normalized;

    if (!tz_seconds_fit(seconds)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    return (time_t)seconds;
}

time_t timegm(struct tm *tm)
{
    int64_t seconds;
    struct tm normalized;

    if (tm == NULL) {
        errno = EINVAL;
        return (time_t)-1;
    }

    if (__os01_tm_normalize(tm, &seconds) != 0) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    if (__os01_tm_from_seconds(seconds, &normalized) != 0) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    normalized.tm_isdst = 0;
    *tm = normalized;

    if (!tz_seconds_fit(seconds)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    return (time_t)seconds;
}

/* ── difftime ──────────────────────────────────────────────────────── */

double difftime(time_t time1, time_t time0)
{
    /* The subtraction is done in floating point so that a pair of
     * extreme time_t values does not overflow on the way. */
    return (double)time1 - (double)time0;
}

/* ── clock ─────────────────────────────────────────────────────────── */

clock_t clock(void)
{
    /*
     * OS01 has no process CPU time accounting: the kernel's SYS_times
     * is a shell that reports zeroes.  clock() therefore answers
     * "(clock_t)-1 with errno = EINVAL" — the POSIX spelling of "this
     * clock is not available" — instead of passing the wall clock off
     * as CPU time.
     */
    errno = EINVAL;
    return (clock_t)-1;
}

/* ── settimeofday ──────────────────────────────────────────────────── */

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv; (void)tz;
    errno = ENOSYS;
    return -1;
}
