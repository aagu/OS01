/*
 * libc/time/tz.c — the POSIX TZ parser and cache (T13).
 *
 * The zone comes from the TZ environment variable, in the POSIX
 * spelling:
 *
 *   TZ = std offset [dst [offset] [,start[/time],end[/time]]]
 *
 * The offset is the value ADDED to the local time to arrive at UTC, so
 * "EST5" is UTC-5 and "CST-8" is UTC+8.  The names are alphabetic or
 * <quoted>; the rules are "Jn" (the n-th day of the year, February 29
 * never counted), "n" (the n-th day of the year, February 29 counted)
 * or "Mm.w.d" (the w-th weekday of month m).  A rule may carry a
 * "/time" (default 02:00:00).
 *
 * A DST name WITHOUT rules uses the fixed default rules
 * M3.2.0/2,M11.1.0/2 (the United States rule) — that is the plan's
 * documented choice, so the zone does not depend on a zoneinfo file
 * that OS01 does not ship.
 *
 * Rules:
 *   - an unset or empty TZ is UTC,
 *   - a MALFORMED TZ is UTC as well: the previous state is dropped, no
 *     out-of-bounds read happens, and the parse never leaves a half
 *     filled zone behind,
 *   - the cache follows the ENVIRONMENT: localtime() and mktime() look
 *     at the current TZ value on every call, so a setenv(), an
 *     unsetenv() or a putenv() string edited in place is picked up
 *     instead of being served from a stale cache.
 *
 * The transition instants follow POSIX: the START rule names a local
 * STANDARD time and the END rule a local DAYLIGHT time, so the
 * northern and the southern (year-crossing) spellings both work.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include <time/calendar.h>

#define TZ_NAME_MAX 16
#define TZ_SRC_MAX  256

/* ── the cache ─────────────────────────────────────────────────────── */

static struct __os01_tzinfo tz_cache;
static char tz_cache_src[TZ_SRC_MAX];
static int tz_cache_primed;   /* the cache holds a parsed zone */
static int tz_cache_unset;    /* the cached source was "no TZ at all" */

/* ── parsing helpers ───────────────────────────────────────────────── */

struct tz_scan {
    const char *p;
};

static int tz_is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int tz_is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* A name: alphabetic, or <...> for anything else. */
static int tz_parse_name(struct tz_scan *sc, char *out, size_t cap)
{
    size_t n = 0;

    if (*sc->p == '<') {
        sc->p++;
        while (*sc->p != '\0' && *sc->p != '>') {
            if (n + 1 >= cap) {
                return -1;
            }
            out[n++] = *sc->p++;
        }
        if (*sc->p != '>') {
            return -1;   /* unterminated */
        }
        sc->p++;
    } else {
        while (tz_is_alpha((unsigned char)*sc->p)) {
            if (n + 1 >= cap) {
                return -1;
            }
            out[n++] = *sc->p++;
        }
    }

    if (n == 0) {
        return -1;       /* a name is not optional */
    }
    out[n] = '\0';
    return 0;
}

/* A number with an upper bound; `*digits` is at least one. */
static int tz_parse_number(struct tz_scan *sc, int max, int *out)
{
    int value = 0;
    int digits = 0;

    while (tz_is_digit((unsigned char)*sc->p)) {
        value = value * 10 + (*sc->p - '0');
        if (value > max) {
            return -1;
        }
        sc->p++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = value;
    return 0;
}

/* An offset: [+|-]hh[:mm[:ss]].  The value is the POSIX one — the time
 * to ADD to the local time to arrive at UTC. */
static int tz_parse_offset(struct tz_scan *sc, int *value)
{
    int sign = 1;
    int hours;
    int minutes = 0;
    int seconds = 0;

    if (*sc->p == '+') {
        sc->p++;
    } else if (*sc->p == '-') {
        sign = -1;
        sc->p++;
    }

    if (tz_parse_number(sc, 24, &hours) != 0) {
        return -1;
    }
    if (*sc->p == ':') {
        sc->p++;
        if (tz_parse_number(sc, 59, &minutes) != 0) {
            return -1;
        }
        if (*sc->p == ':') {
            sc->p++;
            if (tz_parse_number(sc, 59, &seconds) != 0) {
                return -1;
            }
        }
    }

    *value = sign * (hours * 3600 + minutes * 60 + seconds);
    return 0;
}

/* A transition rule with its optional "/time". */
static int tz_parse_rule(struct tz_scan *sc, struct __os01_tz_rule *rule)
{
    memset(rule, 0, sizeof *rule);
    rule->time = 2 * 3600;   /* the POSIX default */

    if (*sc->p == 'J') {
        sc->p++;
        if (tz_parse_number(sc, 365, &rule->n) != 0) {
            return -1;
        }
        rule->kind = __OS01_TZ_JULIAN;
    } else if (*sc->p == 'M') {
        sc->p++;
        if (tz_parse_number(sc, 12, &rule->month) != 0) {
            return -1;
        }
        if (*sc->p != '.') {
            return -1;
        }
        sc->p++;
        if (tz_parse_number(sc, 5, &rule->week) != 0) {
            return -1;
        }
        if (*sc->p != '.') {
            return -1;
        }
        sc->p++;
        if (tz_parse_number(sc, 6, &rule->wday) != 0) {
            return -1;
        }
        rule->kind = __OS01_TZ_MWD;
    } else {
        if (tz_parse_number(sc, 365, &rule->n) != 0) {
            return -1;
        }
        rule->kind = __OS01_TZ_DOY;
    }

    if (*sc->p == '/') {
        sc->p++;
        int hours;
        int minutes = 0;
        int seconds = 0;
        if (tz_parse_number(sc, 24, &hours) != 0) {
            return -1;
        }
        if (*sc->p == ':') {
            sc->p++;
            if (tz_parse_number(sc, 59, &minutes) != 0) {
                return -1;
            }
            if (*sc->p == ':') {
                sc->p++;
                if (tz_parse_number(sc, 59, &seconds) != 0) {
                    return -1;
                }
            }
        }
        rule->time = hours * 3600 + minutes * 60 + seconds;
    }

    return 0;
}

/* ── the zone parser ───────────────────────────────────────────────── */

static void tz_set_utc(struct __os01_tzinfo *tz)
{
    memset(tz, 0, sizeof *tz);
    tz->has_dst = 0;
    memcpy(tz->std_name, "UTC", 4);
    memcpy(tz->dst_name, "UTC", 4);
    tz->std_off = 0;
    tz->dst_off = 0;
}

/* The default rules of a DST name without rules: M3.2.0/2 and
 * M11.1.0/2 (the plan's documented choice). */
static void tz_default_rules(struct __os01_tzinfo *tz)
{
    tz->start.kind = __OS01_TZ_MWD;
    tz->start.month = 3;
    tz->start.week = 2;
    tz->start.wday = 0;
    tz->start.time = 2 * 3600;

    tz->end.kind = __OS01_TZ_MWD;
    tz->end.month = 11;
    tz->end.week = 1;
    tz->end.wday = 0;
    tz->end.time = 2 * 3600;
}

/* Parse a TZ string into `tz`.  A malformed string leaves `tz` at UTC —
 * the caller never sees a half parsed zone. */
static void tz_parse(const char *text, struct __os01_tzinfo *tz)
{
    struct tz_scan sc;
    char name[TZ_NAME_MAX];
    int value;

    tz_set_utc(tz);
    sc.p = text;

    if (tz_parse_name(&sc, name, sizeof name) != 0) {
        return;                                  /* invalid → UTC */
    }
    if (tz_parse_offset(&sc, &value) != 0) {
        return;
    }
    /* The POSIX offset is the time to ADD to the local time to arrive
     * at UTC, so the zone's own offset (east of UTC) is its negation. */
    tz->std_off = -value;
    memcpy(tz->std_name, name, strlen(name) + 1);

    if (*sc.p == '\0') {
        return;                                  /* a fixed zone */
    }

    /* A DST name follows. */
    if (tz_parse_name(&sc, name, sizeof name) != 0) {
        tz_set_utc(tz);
        return;
    }
    memcpy(tz->dst_name, name, strlen(name) + 1);
    tz->has_dst = 1;
    /* One hour east of the standard offset unless the TZ says else. */
    tz->dst_off = tz->std_off + 3600;

    if (*sc.p == ',' ) {
        /* the DST offset is missing: keep the default */
    } else if (*sc.p == '+' || *sc.p == '-' || tz_is_digit((unsigned char)*sc.p)) {
        if (tz_parse_offset(&sc, &value) != 0) {
            tz_set_utc(tz);
            return;
        }
        tz->dst_off = -value;
    } else if (*sc.p != '\0') {
        tz_set_utc(tz);
        return;                                  /* junk → UTC */
    }

    if (*sc.p == '\0') {
        /* A DST name without rules: the fixed default rules. */
        tz_default_rules(tz);
        return;
    }
    if (*sc.p != ',') {
        tz_set_utc(tz);
        return;
    }
    sc.p++;

    if (tz_parse_rule(&sc, &tz->start) != 0) {
        tz_set_utc(tz);
        return;
    }
    if (*sc.p != ',') {
        tz_set_utc(tz);
        return;
    }
    sc.p++;

    if (tz_parse_rule(&sc, &tz->end) != 0) {
        tz_set_utc(tz);
        return;
    }
    if (*sc.p != '\0') {
        tz_set_utc(tz);
        return;                                  /* trailing junk → UTC */
    }
}

/* ── the cache ─────────────────────────────────────────────────────── */

/* Read the environment and refresh the cache when the TZ value changed.
 * `force` re-reads the value even when it looks unchanged. */
static void tz_sync(int force)
{
    const char *env = getenv("TZ");
    int empty = (env == NULL || env[0] == '\0');
    size_t len = empty ? 0 : strlen(env);

    if (!force) {
        if (empty && tz_cache_primed && tz_cache_unset) {
            return;
        }
        if (!empty && tz_cache_primed && !tz_cache_unset &&
            len <= TZ_SRC_MAX - 1 && strcmp(env, tz_cache_src) == 0) {
            return;
        }
    }

    tz_cache_primed = 1;
    tz_cache_unset = empty;

    if (empty) {
        tz_set_utc(&tz_cache);
        tz_cache_src[0] = '\0';
        return;
    }

    if (len > TZ_SRC_MAX - 1) {
        /* An absurdly long TZ is not a zone we can parse: UTC. */
        tz_set_utc(&tz_cache);
        tz_cache_src[0] = '\0';
        return;
    }

    memcpy(tz_cache_src, env, len + 1);
    tz_parse(env, &tz_cache);
}

void __os01_tzset(void)
{
    tz_sync(1);
}

/* The POSIX entry point: tzset() refreshes the zone from the
 * environment.  localtime()/mktime() do the same on their own, so an
 * application that never calls tzset() still sees a zone that follows
 * the environment. */
void tzset(void)
{
    __os01_tzset();
}

const struct __os01_tzinfo *__os01_tz(void)
{
    if (!tz_cache_primed) {
        tz_sync(1);
    } else {
        tz_sync(0);
    }
    return &tz_cache;
}

/* ── the offset lookup ─────────────────────────────────────────────── */

/* The day the rule names in `year`. */
static int64_t tz_rule_day(const struct __os01_tz_rule *rule, int64_t year)
{
    switch (rule->kind) {
    case __OS01_TZ_JULIAN: {
        /* The n-th day of a NON-leap year: February 29 never counts, so
         * the rule is mapped through a fixed non-leap reference. */
        int64_t ref = __os01_days_from_civil(2001, 1, 1) + (rule->n - 1);
        int64_t y;
        int m;
        int d;
        __os01_civil_from_days(ref, &y, &m, &d);
        return __os01_days_from_civil(year, m, d);
    }
    case __OS01_TZ_DOY:
        /* The n-th day of the year, February 29 counted. */
        return __os01_days_from_civil(year, 1, 1) + rule->n;
    case __OS01_TZ_MWD:
    default: {
        int64_t first = __os01_days_from_civil(year, rule->month, 1);
        int64_t next = (rule->month == 12)
                     ? __os01_days_from_civil(year + 1, 1, 1)
                     : __os01_days_from_civil(year, rule->month + 1, 1);
        int64_t dim = next - first;
        int first_wday = __os01_weekday_from_days(first);
        int64_t day = 1 + ((rule->wday - first_wday + 7) % 7) +
                      (rule->week - 1) * 7;
        if (day > dim) {
            day -= 7;   /* the fifth weekday does not exist: use the last */
        }
        return first + day - 1;
    }
    }
}

/* The UTC instant of the transition the rule names in `year`.  `off` is
 * the offset (east of UTC) the rule's wall clock is written in: the
 * standard offset for the start, the daylight offset for the end. */
static int64_t tz_transition(const struct __os01_tz_rule *rule, int64_t year,
                              int off)
{
    int64_t day = tz_rule_day(rule, year);
    int64_t local = day * __OS01_SECONDS_PER_DAY + rule->time;
    return local - off;
}

int __os01_tz_offset(int64_t seconds, int *isdst)
{
    const struct __os01_tzinfo *tz = __os01_tz();

    if (!tz->has_dst) {
        if (isdst != NULL) {
            *isdst = 0;
        }
        return tz->std_off;
    }

    /* The year of the instant; the local year may differ from it by one
     * at the edges, so three years are examined. */
    int64_t days = seconds / __OS01_SECONDS_PER_DAY;
    if (seconds < 0 && seconds % __OS01_SECONDS_PER_DAY != 0) {
        days -= 1;
    }
    int64_t year;
    int month;
    int day;
    __os01_civil_from_days(days, &year, &month, &day);

    int dst = 0;
    for (int i = -1; i <= 1; i++) {
        int64_t y = year + i;
        int64_t start = tz_transition(&tz->start, y, tz->std_off);
        int64_t end_same = tz_transition(&tz->end, y, tz->dst_off);
        int64_t end;

        if (start < end_same) {
            /* The daylight period lies inside the year. */
            end = end_same;
        } else {
            /* The daylight period crosses the new year (southern
             * hemisphere): it ends with the next year's end rule. */
            end = tz_transition(&tz->end, y + 1, tz->dst_off);
        }

        if (start < end && seconds >= start && seconds < end) {
            dst = 1;
        }
    }

    if (isdst != NULL) {
        *isdst = dst;
    }
    return dst ? tz->dst_off : tz->std_off;
}
