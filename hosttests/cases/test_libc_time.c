/*
 * test_libc_time.c — libc-completion Task T13: the calendar core, the
 * POSIX TZ rules and the time formatting.
 *
 * The test drives the PRODUCTION libc objects (libc/time/calendar.c,
 * tz.c, strftime.c, time.c and time_r.c) — they are linked into this
 * binary, so their definitions shadow the host's, and the binary is
 * checked with nm for a stray GLIBC binding of anything under test.
 *
 * Layer 1 — THE ORACLE TABLE: the shared table
 * (hosttests/data/time_cases.h) is evaluated by the standalone glibc
 * oracle (hosttests/oracle/time_oracle.c, a SEPARATE process compiled
 * with no OS01 include paths) and by THIS binary against the OS01
 * implementation.  Both sides print one line per case and the lines
 * are compared byte for byte.
 *
 * Layer 2 — THE DIRECT ASSERTIONS: the properties the oracle cannot
 * pin, because OS01 answers them DIFFERENTLY from glibc ON PURPOSE:
 *
 *   - a malformed or missing TZ is UTC (glibc falls back to the SYSTEM
 *     zone, /etc/localtime, which is not a reproducible answer),
 *   - the zone follows the ENVIRONMENT: a setenv(), an unsetenv() or a
 *     putenv() string edited in place is picked up without a tzset(),
 *   - an ambiguous wall clock is resolved to the STANDARD time when
 *     tm_isdst is negative (the plan's rule; glibc picks the daylight
 *     time),
 *   - a mktime() that SUCCEEDS never sets EOVERFLOW, even when the
 *     answer is (time_t)-1.
 *
 * The test time is always INJECTED (the table's fixed instants and the
 * fixed constants below): nothing here reads the machine's wall clock.
 *
 * NOTE: assert_eq() stringizes its expression into a printf format, so
 * every call whose format string contains a '%' is hoisted into a
 * variable first.
 */

#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "../data/time_cases.h"

/* baked in by the make rule; stays inside the profile build dir */
#ifndef TIME_ORACLE_PATH
#define TIME_ORACLE_PATH "time_oracle-missing"
#endif

/* The libc objects under test set errno; the host's errno is a TLS
 * symbol, so the test provides the plain one the libc declares. */
int errno = 0;

/* ── the injected test time ────────────────────────────────────────── */

/* The instants the direct assertions use.  They are constants, not
 * readings of the machine's clock. */
#define T_EPOCH        0LL
#define T_2000_LEAP    951782400LL     /* 2000-02-29T00:00:00Z */
#define T_2100_MAR     4107542400LL    /* 2100-03-01T00:00:00Z */
#define T_2038         2147483648LL    /* 2038-01-19T03:14:08Z */
#define T_2024_MAR_10  1710050400LL    /* 2024-03-10T06:00:00Z */
#define T_2024_NOV_03  1730611800LL    /* 2024-11-03T05:30:00Z */
#define T_2024_NOV_03_STD 1730615400LL /* 2024-11-03T06:30:00Z */
#define T_2024_JUL_01  1719849600LL    /* 2024-07-01T16:00:00Z */
#define T_2024_JAN_15  1705276800LL    /* 2024-01-15T00:00:00Z */

/* ── helpers ───────────────────────────────────────────────────────── */

/* A broken-down time that came back NULL must not be dereferenced:
 * the RED run (the shell stubs) has to fail with an ASSERTION, not
 * with a crash. */
#define CHECK_TM(p) do { assert_not_null(p); if ((p) == NULL) { return; } } while (0)

static void tm_fields(char *out, size_t cap, const struct tm *p)
{
    snprintf(out, cap, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d",
             p->tm_year + 1900, p->tm_mon + 1, p->tm_mday,
             p->tm_hour, p->tm_min, p->tm_sec,
             p->tm_wday, p->tm_yday, p->tm_isdst);
}

static void hex_encode(const char *s, char *out, size_t cap)
{
    size_t i = 0;

    for (; s[i] != '\0' && (2 * i + 3) < cap; i++) {
        snprintf(out + 2 * i, 3, "%02x", (unsigned char)s[i]);
    }
    out[2 * i] = '\0';
}

/* ── 1. the oracle comparison ──────────────────────────────────────── */

TEST_FUNC(test_oracle_agreement) {
    static char out[64 * 1024];
    static char want[2048];
    static char got[2048];

    int fds[2];
    assert_eq(0, pipe(fds));
    int64_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char *argv[2] = { (char *)TIME_ORACLE_PATH, NULL };
        execv(TIME_ORACLE_PATH, argv);
        _exit(127);
    }
    close(fds[1]);
    size_t total = 0;
    for (;;) {
        if (total + 1 >= sizeof(out)) break;
        ssize_t n = read(fds[0], out + total, sizeof(out) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fds[0]);
    out[total] = '\0';
    int st = -1;
    int64_t w = waitpid(pid, &st, 0);
    if (w != pid || st != 0) {
        printf("  [FAIL] the oracle process: pid=%lld wait=%lld status=%d\n",
               (long long)pid, (long long)w, st);
    }
    assert_true(w == pid && st == 0);
    if (total == 0) {
        printf("  [FAIL] the oracle produced no output\n");
    }
    assert_true(total > 0);

    const char *cursor = out;
    int rows = 0;
    int mismatches = 0;

    /* (a) the calendar core */
    for (int i = 0; i < TIME_EPOCH_CASE_COUNT; i++) {
        const time_epoch_case *c = &time_epoch_cases[i];
        time_t t = (time_t)c->t;
        struct tm tm;
        struct tm *p;

        memset(&tm, 0, sizeof tm);
        errno = 0;
        p = gmtime_r(&t, &tm);
        if (p == NULL) {
            snprintf(got, sizeof(got), "%s\tOVF", c->id);
        } else {
            char fields[256];
            tm_fields(fields, sizeof(fields), p);
            snprintf(got, sizeof(got), "%s\tOK\t%s", c->id, fields);
        }

        const char *nl = strchr(cursor, '\n');
        if (nl == NULL) {
            printf("  [FAIL] the oracle produced only %d of %d epoch rows\n",
                   i, TIME_EPOCH_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor);
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;
        rows++;

        if (strcmp(want, got) != 0) {
            printf("  [FAIL] epoch case %d (id=%s)\n"
                   "         oracle: %s\n"
                   "         os01:   %s\n", i, c->id, want, got);
            mismatches++;
        }
    }

    /* (b) the POSIX TZ rules */
    for (int i = 0; i < TIME_TZ_CASE_COUNT; i++) {
        const time_tz_case *c = &time_tz_cases[i];
        time_t t = (time_t)c->t;
        struct tm lt;
        struct tm gt;
        struct tm *lp;
        struct tm *gp;

        setenv("TZ", c->tz, 1);
        tzset();

        memset(&lt, 0, sizeof lt);
        memset(&gt, 0, sizeof gt);
        errno = 0;
        lp = localtime_r(&t, &lt);
        gp = gmtime_r(&t, &gt);

        if (lp == NULL) {
            snprintf(got, sizeof(got), "%s\tOVF\t", c->id);
        } else {
            char fields[256];
            tm_fields(fields, sizeof(fields), lp);
            snprintf(got, sizeof(got), "%s\tOK\t%s\t", c->id, fields);
        }
        if (gp == NULL) {
            strncat(got, "GMOVF", sizeof(got) - strlen(got) - 1);
        } else {
            char fields[256];
            tm_fields(fields, sizeof(fields), gp);
            strncat(got, fields, sizeof(got) - strlen(got) - 1);
        }

        const char *nl = strchr(cursor, '\n');
        if (nl == NULL) {
            printf("  [FAIL] the oracle produced only %d of %d tz rows\n",
                   i, TIME_TZ_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor);
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;
        rows++;

        if (strcmp(want, got) != 0) {
            printf("  [FAIL] tz case %d (id=%s, tz=%s)\n"
                   "         oracle: %s\n"
                   "         os01:   %s\n", i, c->id, c->tz, want, got);
            mismatches++;
        }
    }

    /* (c) mktime */
    for (int i = 0; i < TIME_MKTIME_CASE_COUNT; i++) {
        const time_mktime_case *c = &time_mktime_cases[i];
        struct tm tm;
        time_t r;

        setenv("TZ", c->tz, 1);
        tzset();

        memset(&tm, 0, sizeof tm);
        tm.tm_year = c->year - 1900;
        tm.tm_mon = c->mon - 1;
        tm.tm_mday = c->mday;
        tm.tm_hour = c->hour;
        tm.tm_min = c->min;
        tm.tm_sec = c->sec;
        tm.tm_isdst = c->isdst;

        errno = 0;
        r = mktime(&tm);

        char fields[256];
        tm_fields(fields, sizeof(fields), &tm);
        snprintf(got, sizeof(got), "%s\t%lld\t%s", c->id, (long long)r, fields);

        const char *nl = strchr(cursor, '\n');
        if (nl == NULL) {
            printf("  [FAIL] the oracle produced only %d of %d mktime rows\n",
                   i, TIME_MKTIME_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor);
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;
        rows++;

        if (strcmp(want, got) != 0) {
            printf("  [FAIL] mktime case %d (id=%s, tz=%s)\n"
                   "         oracle: %s\n"
                   "         os01:   %s\n", i, c->id, c->tz, want, got);
            mismatches++;
        }
    }

    /* (d) strftime */
    for (int i = 0; i < TIME_STRFTIME_CASE_COUNT; i++) {
        const time_strftime_case *c = &time_strftime_cases[i];
        struct tm tm;
        char buf[1024];

        setenv("TZ", TIME_CASE_TZ(c->tz), 1);
        tzset();

        memset(&tm, 0, sizeof tm);
        tm.tm_year = c->year - 1900;
        tm.tm_mon = c->mon - 1;
        tm.tm_mday = c->mday;
        tm.tm_hour = c->hour;
        tm.tm_min = c->min;
        tm.tm_sec = c->sec;
        tm.tm_wday = c->wday;
        tm.tm_yday = c->yday;
        tm.tm_isdst = c->isdst;

        memset(buf, 0, sizeof buf);
        size_t n = strftime(buf, sizeof(buf), c->fmt, &tm);
        char hex[2 * sizeof(buf) + 1];
        hex_encode(buf, hex, sizeof hex);
        snprintf(got, sizeof(got), "%s\t%s\t%zu", c->id, hex, n);

        const char *nl = strchr(cursor, '\n');
        if (nl == NULL) {
            printf("  [FAIL] the oracle produced only %d of %d strftime rows\n",
                   i, TIME_STRFTIME_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor);
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;
        rows++;

        if (strcmp(want, got) != 0) {
            printf("  [FAIL] strftime case %d (id=%s)\n"
                   "         oracle: %s\n"
                   "         os01:   %s\n", i, c->id, want, got);
            mismatches++;
        }
    }

    /* (e) asctime */
    for (int i = 0; i < TIME_ASCTIME_CASE_COUNT; i++) {
        const time_asctime_case *c = &time_asctime_cases[i];
        struct tm tm;
        char buf[64];

        memset(&tm, 0, sizeof tm);
        tm.tm_year = c->year - 1900;
        tm.tm_mon = c->mon - 1;
        tm.tm_mday = c->mday;
        tm.tm_hour = c->hour;
        tm.tm_min = c->min;
        tm.tm_sec = c->sec;
        tm.tm_wday = c->wday;

        memset(buf, 0, sizeof buf);
        char *r = asctime_r(&tm, buf);
        if (r == NULL) {
            snprintf(got, sizeof(got), "%s\tNULL\t0", c->id);
        } else {
            char hex[2 * sizeof(buf) + 1];
            hex_encode(buf, hex, sizeof hex);
            snprintf(got, sizeof(got), "%s\t%s\t%zu", c->id, hex, strlen(buf));
        }

        const char *nl = strchr(cursor, '\n');
        if (nl == NULL) {
            printf("  [FAIL] the oracle produced only %d of %d asctime rows\n",
                   i, TIME_ASCTIME_CASE_COUNT);
            mismatches++;
            break;
        }
        size_t len = (size_t)(nl - cursor);
        if (len >= sizeof(want)) len = sizeof(want) - 1;
        memcpy(want, cursor, len);
        want[len] = '\0';
        cursor = nl + 1;
        rows++;

        if (strcmp(want, got) != 0) {
            printf("  [FAIL] asctime case %d (id=%s)\n"
                   "         oracle: %s\n"
                   "         os01:   %s\n", i, c->id, want, got);
            mismatches++;
        }
    }

    int expected_rows = TIME_EPOCH_CASE_COUNT + TIME_TZ_CASE_COUNT +
                        TIME_MKTIME_CASE_COUNT + TIME_STRFTIME_CASE_COUNT +
                        TIME_ASCTIME_CASE_COUNT;
    printf("  (the oracle produced %d rows, the table has %d)\n",
           rows, expected_rows);
    assert_eq(expected_rows, rows);
    assert_eq(0, mismatches);
}

/* ── 2. the calendar core, directly ────────────────────────────────── */

TEST_FUNC(test_calendar_core) {
    struct tm tm;
    struct tm *p;
    time_t t;

    /* the epoch is a Thursday */
    t = (time_t)T_EPOCH;
    p = gmtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(1970, p->tm_year + 1900);
    assert_eq(1, p->tm_mon + 1);
    assert_eq(1, p->tm_mday);
    assert_eq(0, p->tm_hour);
    assert_eq(0, p->tm_min);
    assert_eq(0, p->tm_sec);
    assert_eq(4, p->tm_wday);       /* Thursday */
    assert_eq(0, p->tm_yday);
    assert_eq(0, p->tm_isdst);

    /* one second before the epoch */
    t = (time_t)(T_EPOCH - 1);
    p = gmtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(1969, p->tm_year + 1900);
    assert_eq(12, p->tm_mon + 1);
    assert_eq(31, p->tm_mday);
    assert_eq(23, p->tm_hour);
    assert_eq(59, p->tm_min);
    assert_eq(59, p->tm_sec);
    assert_eq(3, p->tm_wday);       /* Wednesday */
    assert_eq(364, p->tm_yday);

    /* 2000 is a leap year, 2100 is not */
    t = (time_t)T_2000_LEAP;
    p = gmtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(2000, p->tm_year + 1900);
    assert_eq(2, p->tm_mon + 1);
    assert_eq(29, p->tm_mday);
    assert_eq(59, p->tm_yday);

    t = (time_t)T_2100_MAR;
    p = gmtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(2100, p->tm_year + 1900);
    assert_eq(3, p->tm_mon + 1);
    assert_eq(1, p->tm_mday);
    assert_eq(59, p->tm_yday);

    /* the same day of the year in a leap and in a non-leap year */
    t = (time_t)(T_2000_LEAP + 86400);
    p = gmtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(3, p->tm_mon + 1);
    assert_eq(1, p->tm_mday);
    assert_eq(60, p->tm_yday);

    /* after the 32-bit boundary */
    t = (time_t)T_2038;
    p = gmtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(2038, p->tm_year + 1900);
    assert_eq(1, p->tm_mon + 1);
    assert_eq(19, p->tm_mday);
    assert_eq(3, p->tm_hour);
    assert_eq(14, p->tm_min);
    assert_eq(8, p->tm_sec);

    /* an extreme time_t does not fit: EOVERFLOW, and no answer */
    t = (time_t)9223372036854775807LL;
    errno = 0;
    p = gmtime_r(&t, &tm);
    assert_null(p);
    assert_eq(EOVERFLOW, errno);

    t = (time_t)(-9223372036854775807LL - 1);
    errno = 0;
    p = gmtime_r(&t, &tm);
    assert_null(p);
    assert_eq(EOVERFLOW, errno);

    /* the same for localtime */
    setenv("TZ", "UTC0", 1);
    tzset();
    t = (time_t)9223372036854775807LL;
    errno = 0;
    p = localtime_r(&t, &tm);
    assert_null(p);
    assert_eq(EOVERFLOW, errno);

    /* a NULL argument is EINVAL, not a crash */
    errno = 0;
    assert_null(gmtime_r(NULL, &tm));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_null(localtime_r(NULL, &tm));
    assert_eq(EINVAL, errno);
}

/* ── 3. mktime: the normalization and the errno discipline ─────────── */

TEST_FUNC(test_mktime_normalization) {
    struct tm tm;
    time_t r;

    setenv("TZ", "UTC0", 1);
    tzset();

    /* the fields are normalized: the month 13 is January of the next
     * year */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 1970 - 1900;
    tm.tm_mon = 12;          /* January 1971 */
    tm.tm_mday = 1;
    r = mktime(&tm);
    assert_eq(31536000L, (long)r);
    assert_eq(1971, tm.tm_year + 1900);
    assert_eq(0, tm.tm_mon);
    assert_eq(1, tm.tm_mday);

    /* the day 32 of January is the 1st of February */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 1970 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 32;
    r = mktime(&tm);
    assert_eq(2678400L, (long)r);
    assert_eq(1, tm.tm_mon);
    assert_eq(1, tm.tm_mday);

    /* the hour 25 is 01:00 of the next day */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 1970 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 1;
    tm.tm_hour = 25;
    r = mktime(&tm);
    assert_eq(90000L, (long)r);
    assert_eq(2, tm.tm_mday);
    assert_eq(1, tm.tm_hour);

    /* the weekday and the day of the year are filled in */
    assert_eq(5, tm.tm_wday);       /* 1970-01-02 was a Friday */
    assert_eq(1, tm.tm_yday);

    /* a SUCCESSFUL mktime that answers (time_t)-1 does NOT set
     * EOVERFLOW: 1969-12-31 23:59:59 UTC is a legitimate instant. */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 1969 - 1900;
    tm.tm_mon = 11;
    tm.tm_mday = 31;
    tm.tm_hour = 23;
    tm.tm_min = 59;
    tm.tm_sec = 59;
    errno = 0;
    r = mktime(&tm);
    assert_eq(-1L, (long)r);
    assert_true(errno != EOVERFLOW);

    /* timegm is the UTC twin of mktime */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 2;
    tm.tm_mday = 10;
    tm.tm_hour = 7;
    errno = 0;
    r = timegm(&tm);
    assert_eq(1710054000L, (long)r);
    assert_eq(0, tm.tm_isdst);

    /* timegm ignores the zone */
    setenv("TZ", "CST-8", 1);
    tzset();
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 2;
    tm.tm_mday = 10;
    tm.tm_hour = 7;
    r = timegm(&tm);
    assert_eq(1710054000L, (long)r);

    /* difftime is the difference of two instants */
    assert_true(difftime((time_t)100, (time_t)40) == 60.0);
    assert_true(difftime((time_t)40, (time_t)100) == -60.0);
    assert_true(difftime((time_t)7, (time_t)7) == 0.0);
}

/* ── 4. the TZ rules ───────────────────────────────────────────────── */

TEST_FUNC(test_tz_rules) {
    struct tm tm;
    struct tm *p;
    time_t t;

    /* the spring gap: the wall clock 02:30 does not exist on
     * 2024-03-10 and is normalized FORWARD */
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();

    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 2;
    tm.tm_mday = 10;
    tm.tm_hour = 2;
    tm.tm_min = 30;
    tm.tm_isdst = -1;
    time_t r = mktime(&tm);
    assert_eq(1710055800L, (long)r);      /* 2024-03-10T07:30:00Z */
    assert_eq(3, tm.tm_hour);             /* normalized to 03:30 */
    assert_eq(1, tm.tm_isdst);

    /* the autumn repeat: the wall clock 01:30 happens TWICE on
     * 2024-11-03.  With tm_isdst negative the plan fixes the choice on
     * the STANDARD time — the later of the two instants.  (glibc picks
     * the daylight time; the divergence is deliberate.) */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = 3;
    tm.tm_hour = 1;
    tm.tm_min = 30;
    tm.tm_isdst = -1;
    r = mktime(&tm);
    assert_eq(1730615400L, (long)r);      /* 2024-11-03T06:30:00Z */
    assert_eq(0, tm.tm_isdst);

    /* with tm_isdst positive the daylight time is used */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = 3;
    tm.tm_hour = 1;
    tm.tm_min = 30;
    tm.tm_isdst = 1;
    r = mktime(&tm);
    assert_eq(1730611800L, (long)r);      /* 2024-11-03T05:30:00Z */
    assert_eq(1, tm.tm_isdst);

    /* and with tm_isdst zero the standard time */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = 3;
    tm.tm_hour = 1;
    tm.tm_min = 30;
    tm.tm_isdst = 0;
    r = mktime(&tm);
    assert_eq(1730615400L, (long)r);
    assert_eq(0, tm.tm_isdst);

    /* localtime reports the flag of the INSTANT, so the repeated hour
     * is daylight time once and standard time once */
    t = (time_t)T_2024_NOV_03;
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(1, p->tm_hour);
    assert_eq(30, p->tm_min);
    assert_eq(1, p->tm_isdst);            /* the first 01:30 is EDT */

    t = (time_t)T_2024_NOV_03_STD;
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(1, p->tm_hour);
    assert_eq(30, p->tm_min);
    assert_eq(0, p->tm_isdst);            /* the second 01:30 is EST */

    /* the southern hemisphere: the daylight period crosses the year */
    setenv("TZ", "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 1);
    tzset();

    t = (time_t)T_2024_JAN_15;
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(11, p->tm_hour);            /* 00:00Z is 11:00 AEDT */
    assert_eq(1, p->tm_isdst);

    t = (time_t)T_2024_JUL_01;
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(2, p->tm_hour);             /* 16:00Z is 02:00 AEST */
    assert_eq(0, p->tm_isdst);

    /* the rule spellings: Jn, n and Mm.w.d name the same transition */
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    t = (time_t)T_2024_MAR_10;
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(1, p->tm_hour);
    assert_eq(0, p->tm_isdst);            /* 06:00Z is still 01:00 EST */

    t = (time_t)1710054000LL;             /* 07:00Z */
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(3, p->tm_hour);             /* 03:00 EDT */
    assert_eq(1, p->tm_isdst);

    setenv("TZ", "EST5EDT,J60/2,J300/2", 1);
    tzset();
    t = (time_t)T_2024_MAR_10;
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(2, p->tm_hour);             /* the J60 transition is earlier */
    assert_eq(1, p->tm_isdst);

    setenv("TZ", "EST5EDT,59/2,299/2", 1);
    tzset();
    t = (time_t)T_2024_MAR_10;
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(2, p->tm_hour);
    assert_eq(1, p->tm_isdst);
}

/* ── 5. the TZ environment ─────────────────────────────────────────── */

TEST_FUNC(test_tz_environment) {
    struct tm tm;
    struct tm *p;
    time_t t = (time_t)T_2024_MAR_10;

    /* an unset TZ is UTC */
    unsetenv("TZ");
    tzset();
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(6, p->tm_hour);
    assert_eq(0, p->tm_isdst);

    /* an empty TZ is UTC */
    setenv("TZ", "", 1);
    tzset();
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(6, p->tm_hour);

    /* a MALFORMED TZ is UTC as well, and the previous zone is dropped */
    setenv("TZ", "CST-8", 1);
    tzset();
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(14, p->tm_hour);

    setenv("TZ", "not-a-zone", 1);
    tzset();
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(6, p->tm_hour);
    assert_eq(0, p->tm_isdst);

    setenv("TZ", "EST5EDT,M3.2.0", 1);
    tzset();
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(6, p->tm_hour);

    /* the zone follows the environment WITHOUT a tzset(): a setenv in
     * the same process is picked up */
    setenv("TZ", "CST-8", 1);
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(14, p->tm_hour);

    /* a putenv string edited IN PLACE is picked up too */
    {
        static char borrowed[] = "TZ=CST-8";
        putenv(borrowed);
        p = localtime_r(&t, &tm);
        CHECK_TM(p);
        assert_eq(14, p->tm_hour);

        borrowed[3] = 'U';
        borrowed[4] = 'T';
        borrowed[5] = 'C';
        borrowed[6] = '0';
        borrowed[7] = '\0';
        p = localtime_r(&t, &tm);
        CHECK_TM(p);
        assert_eq(6, p->tm_hour);
    }

    /* and an unsetenv goes back to UTC */
    unsetenv("TZ");
    p = localtime_r(&t, &tm);
    CHECK_TM(p);
    assert_eq(6, p->tm_hour);

    /* mktime follows the environment as well */
    setenv("TZ", "CST-8", 1);
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 2;
    tm.tm_mday = 10;
    tm.tm_hour = 14;
    tm.tm_isdst = 0;
    time_t r = mktime(&tm);
    assert_eq(T_2024_MAR_10, (long)r);

    unsetenv("TZ");
    tzset();
}

/* ── 6. the static storage and the _r variants ─────────────────────── */

TEST_FUNC(test_static_and_reentrant) {
    time_t t = (time_t)T_2024_MAR_10;
    struct tm mine;
    struct tm snapshot;
    struct tm *a;
    struct tm *b;
    char buf[64];

    setenv("TZ", "UTC0", 1);
    tzset();

    /* gmtime uses static storage: the same object every time */
    a = gmtime(&t);
    CHECK_TM(a);
    b = gmtime(&t);
    assert_true(a == b);

    /* the _r variant writes into the caller's object ... */
    memset(&mine, 0xA5, sizeof mine);
    assert_true(gmtime_r(&t, &mine) == &mine);
    assert_eq(2024, mine.tm_year + 1900);

    /* ... and touches nothing else: the static object is unchanged */
    snapshot = *a;
    memset(&mine, 0xA5, sizeof mine);
    assert_true(gmtime_r(&t, &mine) == &mine);
    assert_mem_eq(&snapshot, a, sizeof(struct tm));

    /* the same for localtime */
    a = localtime(&t);
    CHECK_TM(a);
    snapshot = *a;
    memset(&mine, 0xA5, sizeof mine);
    assert_true(localtime_r(&t, &mine) == &mine);
    assert_mem_eq(&snapshot, a, sizeof(struct tm));

    /* ctime and asctime use static storage as well */
    {
        char *c1 = ctime(&t);
        char *c2 = ctime(&t);
        assert_not_null(c1);
        if (c1 == NULL) { return; }
        assert_true(c1 == c2);
    }

    /* ctime_r writes into the caller's buffer only */
    memset(buf, 0xA5, sizeof buf);
    if (ctime_r(&t, buf) == NULL) { return; }
    assert_eq(25, (long)strlen(buf));

    /* a NULL argument is EINVAL */
    errno = 0;
    assert_null(asctime_r(NULL, buf));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_null(ctime_r(NULL, buf));
    assert_eq(EINVAL, errno);
}

/* ── 7. strftime ───────────────────────────────────────────────────── */

TEST_FUNC(test_strftime) {
    struct tm tm;
    char buf[256];
    size_t n;
    const char *text;

    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2021 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 3;
    tm.tm_hour = 4;
    tm.tm_min = 5;
    tm.tm_sec = 6;
    tm.tm_wday = 0;
    tm.tm_yday = 2;
    tm.tm_isdst = 0;

    setenv("TZ", "UTC0", 1);
    tzset();

    /* every conversion of the C locale, in one row */
    memset(buf, 0, sizeof buf);
    n = strftime(buf, sizeof buf,
                 "%a|%A|%b|%B|%c|%C|%d|%D|%e|%F|%g|%G|%h|%H|%I|%j|%m|%M|%n|"
                 "%p|%r|%R|%S|%t|%T|%u|%U|%V|%w|%W|%x|%X|%y|%Y|%%", &tm);
    assert_true(n > 0);
    text = buf;
    assert_str_eq("Sun|Sunday|Jan|January|Sun Jan  3 04:05:06 2021|20|03|"
                  "01/03/21| 3|2021-01-03|20|2020|Jan|04|04|003|01|05|\n"
                  "|AM|04:05:06 AM|04:05|06|\t|04:05:06|7|01|53|0|00|"
                  "01/03/21|04:05:06|21|2021|%", text);

    /* the E and O modifiers select the C locale basic form */
    memset(buf, 0, sizeof buf);
    n = strftime(buf, sizeof buf, "%EY %Oy %EC", &tm);
    assert_true(n > 0);
    assert_str_eq("2021 21 20", buf);

    /* the zone name and the offset */
    memset(buf, 0, sizeof buf);
    n = strftime(buf, sizeof buf, "%z %Z", &tm);
    assert_true(n > 0);
    assert_str_eq("+0000 UTC", buf);

    setenv("TZ", "CST-8", 1);
    tzset();
    memset(buf, 0, sizeof buf);
    n = strftime(buf, sizeof buf, "%z %Z", &tm);
    assert_true(n > 0);
    assert_str_eq("+0800 CST", buf);

    /* the daylight offset and name */
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    memset(buf, 0, sizeof buf);
    n = strftime(buf, sizeof buf, "%z %Z", &tm);
    assert_true(n > 0);
    assert_str_eq("-0500 EST", buf);

    tm.tm_isdst = 1;
    memset(buf, 0, sizeof buf);
    n = strftime(buf, sizeof buf, "%z %Z", &tm);
    assert_true(n > 0);
    assert_str_eq("-0400 EDT", buf);
    tm.tm_isdst = 0;

    /* the offset of a time that came from localtime() */
    {
        time_t lt = (time_t)T_2024_MAR_10;
        struct tm local;
        CHECK_TM(localtime_r(&lt, &local));
        memset(buf, 0, sizeof buf);
        n = strftime(buf, sizeof buf, "%z %Z", &local);
        assert_true(n > 0);
        assert_str_eq("-0500 EST", buf);
    }

    /* the buffer contract */
    memset(buf, 0xA5, sizeof buf);
    n = strftime(buf, 0, "%Y", &tm);
    assert_eq(0, (long)n);

    /* exactly too small: the result does not fit */
    memset(buf, 0xA5, sizeof buf);
    n = strftime(buf, 4, "%Y-%m-%d", &tm);
    assert_eq(0, (long)n);

    /* exactly enough: the result and its NUL fit */
    memset(buf, 0xA5, sizeof buf);
    n = strftime(buf, 11, "%Y-%m-%d", &tm);
    assert_eq(10, (long)n);
    assert_str_eq("2021-01-03", buf);
    assert_eq(0, (long)buf[10]);

    /* an unknown conversion fails */
    memset(buf, 0xA5, sizeof buf);
    n = strftime(buf, sizeof buf, "%Q", &tm);
    assert_eq(0, (long)n);

    /* a trailing '%' fails */
    memset(buf, 0xA5, sizeof buf);
    n = strftime(buf, sizeof buf, "abc%", &tm);
    assert_eq(0, (long)n);

    /* the ISO week-based year */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2021 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 3;
    tm.tm_wday = 0;
    tm.tm_yday = 2;
    memset(buf, 0, sizeof buf);
    n = strftime(buf, sizeof buf, "%G-W%V", &tm);
    assert_true(n > 0);
    assert_str_eq("2020-W53", buf);
}

/* ── 8. asctime and ctime: the 26-byte layout ──────────────────────── */

TEST_FUNC(test_asctime_guard) {
    struct tm tm;
    char buf[64];
    char *r;

    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2021 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 3;
    tm.tm_hour = 4;
    tm.tm_min = 5;
    tm.tm_sec = 6;
    tm.tm_wday = 0;

    memset(buf, 0xA5, sizeof buf);
    r = asctime_r(&tm, buf);
    assert_not_null(r);
    if (r == NULL) { return; }
    assert_true(r == buf);
    /* "Sun Jan  3 04:05:06 2021\n" is 25 characters and the NUL makes
     * the 26 bytes of the layout */
    assert_eq(25, (long)strlen(buf));
    assert_str_eq("Sun Jan  3 04:05:06 2021\n", buf);
    assert_eq(0, (long)buf[25]);

    /* a year that does not fit the layout cannot be represented */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 10000 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 1;
    tm.tm_wday = 4;
    memset(buf, 0xA5, sizeof buf);
    errno = 0;
    r = asctime_r(&tm, buf);
    assert_null(r);
    assert_eq(EOVERFLOW, errno);

    /* the same for ctime_r, through localtime */
    setenv("TZ", "UTC0", 1);
    tzset();
    {
        time_t t = (time_t)253402300799LL;    /* 9999-12-31T23:59:59Z */
        memset(buf, 0xA5, sizeof buf);
        errno = 0;
        r = ctime_r(&t, buf);
        assert_not_null(r);
        if (r == NULL) { return; }
        assert_eq(25, (long)strlen(buf));
    }
}

/* ── 9. strptime round-trips with strftime ─────────────────────────── */

TEST_FUNC(test_strptime_roundtrip) {
    struct tm tm;
    struct tm parsed;
    char text[64];
    const char *fmt = "%Y-%m-%d %H:%M:%S";
    size_t n;

    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2021 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 3;
    tm.tm_hour = 4;
    tm.tm_min = 5;
    tm.tm_sec = 6;
    tm.tm_wday = 0;
    tm.tm_yday = 2;

    setenv("TZ", "UTC0", 1);
    tzset();

    memset(text, 0, sizeof text);
    n = strftime(text, sizeof(text), fmt, &tm);
    assert_true(n > 0);
    assert_str_eq("2021-01-03 04:05:06", text);

    memset(&parsed, 0, sizeof parsed);
    assert_true(strptime(text, fmt, &parsed) != NULL);
    assert_eq(tm.tm_year, parsed.tm_year);
    assert_eq(tm.tm_mon, parsed.tm_mon);
    assert_eq(tm.tm_mday, parsed.tm_mday);
    assert_eq(tm.tm_hour, parsed.tm_hour);
    assert_eq(tm.tm_min, parsed.tm_min);
    assert_eq(tm.tm_sec, parsed.tm_sec);

    /* the month and the day names round-trip too */
    memset(text, 0, sizeof text);
    n = strftime(text, sizeof(text), "%b %d %Y", &tm);
    assert_true(n > 0);
    assert_str_eq("Jan 03 2021", text);
    memset(&parsed, 0, sizeof parsed);
    assert_true(strptime(text, "%b %d %Y", &parsed) != NULL);
    assert_eq(0, parsed.tm_mon);
    assert_eq(3, parsed.tm_mday);
    assert_eq(2021 - 1900, parsed.tm_year);

    /* the range errors: an out-of-range field is a mismatch, not a
     * silent truncation */
    memset(&parsed, 0, sizeof parsed);
    assert_null(strptime("2021-13-01", "%Y-%m-%d", &parsed));
    memset(&parsed, 0, sizeof parsed);
    assert_null(strptime("2021-01-32", "%Y-%m-%d", &parsed));
    memset(&parsed, 0, sizeof parsed);
    assert_null(strptime("25:00:00", "%H:%M:%S", &parsed));
    memset(&parsed, 0, sizeof parsed);
    assert_null(strptime("2021-01-03", "%Y-%m-%d %H:%M:%S", &parsed));
}

/* ── 10. clock(): explicitly unavailable ────────────────────────────── */

TEST_FUNC(test_clock_unavailable) {
    clock_t c;

    errno = 0;
    c = clock();
    /* OS01 has no process CPU time accounting: the answer must say so
     * instead of passing the wall clock off as CPU time. */
    assert_eq(-1L, (long)c);
    assert_eq(EINVAL, errno);
}

/* ── the runner ────────────────────────────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_oracle_agreement),
    TEST_ENTRY(test_calendar_core),
    TEST_ENTRY(test_mktime_normalization),
    TEST_ENTRY(test_tz_rules),
    TEST_ENTRY(test_tz_environment),
    TEST_ENTRY(test_static_and_reentrant),
    TEST_ENTRY(test_strftime),
    TEST_ENTRY(test_asctime_guard),
    TEST_ENTRY(test_strptime_roundtrip),
    TEST_ENTRY(test_clock_unavailable),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
