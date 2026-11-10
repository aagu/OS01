/*
 * time_oracle.c — the STANDALONE glibc oracle for the T13 time table
 * (hosttests/data/time_cases.h).
 *
 * Compiled with the HOST compiler and NO OS01 include paths (the make
 * rule passes none), so every gmtime/localtime/mktime/strftime/asctime
 * below is the HOST's glibc.  The process cannot accidentally test our
 * libc — and the OS01 test runs it as a SEPARATE process and compares
 * the rows byte for byte.
 *
 * One line per case, tab separated:
 *
 *   epoch    id \t OK \t y mo d h mi s wday yday isdst   (or "OVF")
 *   tz       id \t OK \t <local fields> \t <gm fields>
 *   mktime   id \t result \t <normalized fields>
 *   strftime id \t <hex output> \t <length>
 *   asctime  id \t <hex output> \t <length>
 *
 * The strings are hex encoded so that a format with a newline or a tab
 * (%n, %t) cannot break the line protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "../data/time_cases.h"

static void hex_encode(const char *s, char *out, size_t cap)
{
    size_t i = 0;

    for (; s[i] != '\0' && (2 * i + 3) < cap; i++) {
        sprintf(out + 2 * i, "%02x", (unsigned char)s[i]);
    }
    out[2 * i] = '\0';
}

static void print_fields(const struct tm *p)
{
    printf("%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d",
           p->tm_year + 1900, p->tm_mon + 1, p->tm_mday,
           p->tm_hour, p->tm_min, p->tm_sec,
           p->tm_wday, p->tm_yday, p->tm_isdst);
}

int main(void)
{
    char out[2048];

    /* ── 1. the calendar core ── */
    for (int i = 0; i < TIME_EPOCH_CASE_COUNT; i++) {
        const time_epoch_case *c = &time_epoch_cases[i];
        time_t t = (time_t)c->t;
        struct tm tm;

        memset(&tm, 0, sizeof tm);
        errno = 0;
        struct tm *p = gmtime_r(&t, &tm);
        if (p == NULL) {
            printf("%s\tOVF\n", c->id);
        } else {
            printf("%s\tOK\t", c->id);
            print_fields(p);
            printf("\n");
        }
    }

    /* ── 2. the POSIX TZ rules ── */
    for (int i = 0; i < TIME_TZ_CASE_COUNT; i++) {
        const time_tz_case *c = &time_tz_cases[i];
        time_t t = (time_t)c->t;
        struct tm lt;
        struct tm gt;

        setenv("TZ", c->tz, 1);
        tzset();

        memset(&lt, 0, sizeof lt);
        memset(&gt, 0, sizeof gt);
        struct tm *lp = localtime_r(&t, &lt);
        struct tm *gp = gmtime_r(&t, &gt);

        printf("%s\t", c->id);
        if (lp == NULL) {
            printf("OVF\t");
        } else {
            printf("OK\t");
            print_fields(lp);
            printf("\t");
        }
        if (gp == NULL) {
            printf("GMOVF\n");
        } else {
            print_fields(gp);
            printf("\n");
        }
    }

    /* ── 3. mktime ── */
    for (int i = 0; i < TIME_MKTIME_CASE_COUNT; i++) {
        const time_mktime_case *c = &time_mktime_cases[i];
        struct tm tm;

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
        time_t r = mktime(&tm);

        printf("%s\t%lld\t", c->id, (long long)r);
        print_fields(&tm);
        printf("\n");
    }

    /* ── 4. strftime ── */
    for (int i = 0; i < TIME_STRFTIME_CASE_COUNT; i++) {
        const time_strftime_case *c = &time_strftime_cases[i];
        struct tm tm;

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

        memset(out, 0, sizeof out);
        size_t n = strftime(out, sizeof out, c->fmt, &tm);

        char hex[2 * sizeof out + 1];
        hex_encode(out, hex, sizeof hex);
        printf("%s\t%s\t%zu\n", c->id, hex, n);
    }

    /* ── 5. asctime ── */
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
            printf("%s\tNULL\t0\n", c->id);
            continue;
        }

        char hex[2 * sizeof buf + 1];
        hex_encode(buf, hex, sizeof hex);
        printf("%s\t%s\t%zu\n", c->id, hex, strlen(buf));
    }

    return 0;
}
