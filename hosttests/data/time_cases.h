/*
 * time_cases.h — the shared table of the T13 time group.
 *
 * The table is compiled into BOTH the standalone glibc oracle
 * (hosttests/oracle/time_oracle.c, a SEPARATE process built with no
 * OS01 include paths) and the OS01 test
 * (hosttests/cases/test_libc_time.c).  Both sides walk the table in
 * the same order and print one line per case; the test compares the
 * lines byte for byte.
 *
 * The cases that OS01 answers DIFFERENTLY from glibc on purpose are
 * NOT here: they are asserted directly in the test file, with the
 * reason next to them (the plan's mktime rule for an ambiguous wall
 * clock, and the fixed default DST rules of a DST name without
 * rules).
 */

#ifndef TIME_CASES_H
#define TIME_CASES_H 1

/* The zone a case runs under.  A NULL zone means the plain UTC zone,
 * so that a case never depends on the zone a previous case left in
 * the environment. */
#define TIME_CASE_TZ(tz) ((tz) != NULL ? (tz) : "UTC0")

/* ── 1. the calendar core: gmtime of a fixed set of instants ──────── */

typedef struct {
    const char *id;
    long long   t;        /* seconds since 1970-01-01T00:00:00Z */
    int         overflow; /* 1 when the instant is not representable */
} time_epoch_case;

static const time_epoch_case time_epoch_cases[] = {
    { "epoch",                     0LL, 0 },
    { "epoch_minus_one",          -1LL, 0 },
    { "one_second_after_epoch",     1LL, 0 },
    { "leap_day_2000",      951782400LL, 0 },  /* 2000-02-29T00:00:00Z */
    { "march_2000",         953433600LL, 0 },  /* 2000-03-01T00:00:00Z */
    { "march_2100",        4107542400LL, 0 },  /* 2100-03-01T00:00:00Z */
    { "dec_2100",          4122144000LL, 0 },  /* 2100-12-31T00:00:00Z */
    { "after_2038",        2147483648LL, 0 },  /* 2038-01-19T03:14:08Z */
    { "year_9999",       253402300799LL, 0 },  /* 9999-12-31T23:59:59Z */
    { "year_1",          -62135596800LL, 0 },  /* 0001-01-01T00:00:00Z */
    { "before_epoch",      -86400LL, 0 },      /* 1969-12-31T00:00:00Z */
    { "time_t_max",   9223372036854775807LL, 1 },
    { "time_t_min",  -9223372036854775807LL - 1, 1 },
};

#define TIME_EPOCH_CASE_COUNT \
    ((int)(sizeof(time_epoch_cases) / sizeof(time_epoch_cases[0])))

/* ── 2. the POSIX TZ rules: localtime under a zone ────────────────── */

typedef struct {
    const char *id;
    const char *tz;   /* NULL: TZ is unset */
    long long   t;
} time_tz_case;

static const time_tz_case time_tz_cases[] = {
    { "utc0_epoch",        "UTC0",                        0LL },
    { "utc0_no_dst",       "UTC0",                 1710050400LL },
    { "cst_minus_8",       "CST-8",                1710050400LL },
    { "cst_minus_8_neg",   "CST-8",                   -3600LL },
    { "gmt_plus_5",        "GMT+5",                1710050400LL },
    { "quoted_name",       "<+07>-7",              1710050400LL },
    /* The unset, the empty and the malformed TZ are NOT here: glibc
     * falls back to the SYSTEM zone (/etc/localtime) for them, while
     * the plan fixes the answer at UTC.  The test asserts that
     * directly. */
    /* the northern DST rules, around both transitions */
    { "est_spring_before", "EST5EDT,M3.2.0/2,M11.1.0/2", 1710050400LL },
    { "est_spring_at",     "EST5EDT,M3.2.0/2,M11.1.0/2", 1710054000LL },
    { "est_spring_after",  "EST5EDT,M3.2.0/2,M11.1.0/2", 1710057600LL },
    { "est_summer",        "EST5EDT,M3.2.0/2,M11.1.0/2", 1719849600LL },
    { "est_fall_first",    "EST5EDT,M3.2.0/2,M11.1.0/2", 1730611800LL },
    { "est_fall_second",   "EST5EDT,M3.2.0/2,M11.1.0/2", 1730615400LL },
    { "est_winter",        "EST5EDT,M3.2.0/2,M11.1.0/2", 1730601600LL },
    /* the rule spellings: Jn, n and Mm.w.d */
    { "rule_julian",       "EST5EDT,J60/2,J300/2", 1710050400LL },
    { "rule_julian_2",     "EST5EDT,J60/2,J300/2", 1730611800LL },
    { "rule_doy",          "EST5EDT,59/2,299/2",   1710050400LL },
    { "rule_doy_2",        "EST5EDT,59/2,299/2",   1730611800LL },
    { "rule_mwd",          "EST5EDT,M3.2.0/2,M11.1.0/2", 1710050400LL },
    /* the southern hemisphere: the daylight period crosses the year */
    { "aest_summer",       "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 1719849600LL },
    { "aest_winter",       "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 1730601600LL },
    { "aest_january",      "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 1705276800LL },
    { "aest_october",      "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 1728345600LL },
    /* a DST name without rules uses the fixed default rules */
    { "default_rules_spring", "EST5EDT",           1710050400LL },
    { "default_rules_summer", "EST5EDT",           1719849600LL },
    { "default_rules_fall",   "EST5EDT",           1730611800LL },
};

#define TIME_TZ_CASE_COUNT \
    ((int)(sizeof(time_tz_cases) / sizeof(time_tz_cases[0])))

/* ── 3. mktime: a broken-down local time -> an instant ────────────── */

typedef struct {
    const char *id;
    const char *tz;
    int year;
    int mon;    /* 1-12 */
    int mday;
    int hour;
    int min;
    int sec;
    int isdst;  /* -1: the libc decides */
} time_mktime_case;

static const time_mktime_case time_mktime_cases[] = {
    { "mk_epoch",        "UTC0", 1970,  1,  1,  0,  0,  0,  0 },
    { "mk_minus_one",    "UTC0", 1969, 12, 31, 23, 59, 59,  0 },
    { "mk_month_13",     "UTC0", 1970, 13,  1,  0,  0,  0,  0 },
    { "mk_month_zero",   "UTC0", 1970,  0,  1,  0,  0,  0,  0 },
    { "mk_day_32",       "UTC0", 1970,  1, 32,  0,  0,  0,  0 },
    { "mk_day_zero",     "UTC0", 1970,  1,  0,  0,  0,  0,  0 },
    { "mk_hour_25",      "UTC0", 1970,  1,  1, 25,  0,  0,  0 },
    { "mk_min_61",       "UTC0", 1970,  1,  1,  0, 61,  0,  0 },
    { "mk_sec_61",       "UTC0", 1970,  1,  1,  0,  0, 61,  0 },
    { "mk_leap_day",     "UTC0", 2000,  2, 29, 12,  0,  0,  0 },
    /* February 2024 has 29 days, so its 32nd is the 3rd of March */
    { "mk_feb_32",       "UTC0", 2024,  2, 32, 12,  0,  0,  0 },
    { "mk_feb_30",       "UTC0", 2023,  2, 30, 12,  0,  0,  0 },
    { "mk_non_leap",     "UTC0", 2100,  2, 29, 12,  0,  0,  0 },
    { "mk_2038",         "UTC0", 2038,  1, 19,  3, 14,  8,  0 },
    { "mk_after_2038",   "UTC0", 2038,  1, 19,  3, 14,  9,  0 },
    { "mk_dst_summer",   "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 7, 1, 12, 0, 0, -1 },
    { "mk_dst_summer_0", "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 7, 1, 12, 0, 0,  0 },
    { "mk_dst_winter",   "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 1, 15, 12, 0, 0, -1 },
    { "mk_gap",          "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 3, 10,  2, 30, 0, -1 },
    { "mk_gap_0",        "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 3, 10,  2, 30, 0,  0 },
    { "mk_gap_1",        "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 3, 10,  2, 30, 0,  1 },
    { "mk_dup_1",        "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 11, 3,  1, 30, 0,  1 },
    { "mk_dup_0",        "EST5EDT,M3.2.0/2,M11.1.0/2", 2024, 11, 3,  1, 30, 0,  0 },
    { "mk_south_dst",    "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 2024, 1, 15, 12, 0, 0, -1 },
    { "mk_south_std",    "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 2024, 7, 15, 12, 0, 0, -1 },
};

#define TIME_MKTIME_CASE_COUNT \
    ((int)(sizeof(time_mktime_cases) / sizeof(time_mktime_cases[0])))

/* ── 4. strftime: a broken-down time and a format ─────────────────── */

typedef struct {
    const char *id;
    const char *tz;   /* NULL: the TZ is left alone */
    int year;
    int mon;    /* 1-12 */
    int mday;
    int hour;
    int min;
    int sec;
    int wday;   /* 0 = Sunday */
    int yday;   /* 0-based */
    int isdst;
    const char *fmt;
} time_strftime_case;

static const time_strftime_case time_strftime_cases[] = {
    { "sf_all", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0,
      "%a|%A|%b|%B|%c|%C|%d|%D|%e|%F|%g|%G|%h|%H|%I|%j|%m|%M|%n|"
      "%p|%r|%R|%S|%t|%T|%u|%U|%V|%w|%W|%x|%X|%y|%Y|%%" },
    { "sf_year", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%Y" },
    { "sf_century", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%C" },
    { "sf_year2", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%y" },
    { "sf_yday", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%j" },
    { "sf_week_sun", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%U" },
    { "sf_week_mon", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%W" },
    { "sf_iso_week", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%V" },
    { "sf_iso_year", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%G" },
    { "sf_iso_year2", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%g" },
    { "sf_hour12", "UTC0", 2021, 1, 3, 0, 5, 6, 0, 2, 0, "%I %p" },
    { "sf_hour12b", "UTC0", 2021, 1, 3, 12, 5, 6, 0, 2, 0, "%I %p" },
    { "sf_hour12c", "UTC0", 2021, 1, 3, 13, 5, 6, 0, 2, 0, "%I %p" },
    { "sf_date_c", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%c" },
    { "sf_date_x", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%x" },
    { "sf_time_X", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%X" },
    { "sf_time_r", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%r" },
    { "sf_iso_2019_12_30", "UTC0", 2019, 12, 30, 12, 0, 0, 1, 363, 0, "%G-W%V-%m-%d" },
    { "sf_iso_2020_12_31", "UTC0", 2020, 12, 31, 12, 0, 0, 4, 365, 0, "%G-W%V-%m-%d" },
    { "sf_iso_2021_01_03", "UTC0", 2021, 1, 3, 12, 0, 0, 0, 2, 0, "%G-W%V-%m-%d" },
    { "sf_iso_2016_01_01", "UTC0", 2016, 1, 1, 12, 0, 0, 5, 0, 0, "%G-W%V-%m-%d" },
    { "sf_iso_2015_12_28", "UTC0", 2015, 12, 28, 12, 0, 0, 1, 361, 0, "%G-W%V-%m-%d" },
    /* The zone NAME is compared with the oracle; the zone OFFSET (%z)
     * is NOT: glibc's strftime computes %z from the TZ rules and the
     * broken-down time, and for a hand-built time it answers +0000 for
     * every POSIX zone (the offset is only right when the time comes
     * from localtime).  OS01 answers the zone's real offset, and the
     * test asserts that directly. */
    { "sf_zone_utc", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%Z" },
    { "sf_zone_cst", "CST-8", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%Z" },
    { "sf_zone_est", "EST5EDT,M3.2.0/2,M11.1.0/2", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%Z" },
    { "sf_zone_edt", "EST5EDT,M3.2.0/2,M11.1.0/2", 2021, 7, 3, 4, 5, 6, 6, 183, 1, "%Z" },
    { "sf_modifier", "UTC0", 2021, 1, 3, 4, 5, 6, 0, 2, 0, "%EY %Oy %EC" },
    { "sf_leap", "UTC0", 2000, 2, 29, 23, 59, 60, 2, 59, 0, "%Y-%m-%d %H:%M:%S %j" },
};

#define TIME_STRFTIME_CASE_COUNT \
    ((int)(sizeof(time_strftime_cases) / sizeof(time_strftime_cases[0])))

/* ── 5. asctime: the fixed 26-byte layout ─────────────────────────── */

typedef struct {
    const char *id;
    int year;
    int mon;    /* 1-12 */
    int mday;
    int hour;
    int min;
    int sec;
    int wday;   /* 0 = Sunday */
} time_asctime_case;

static const time_asctime_case time_asctime_cases[] = {
    { "as_epoch",      1970,  1,  1,  0,  0,  0, 4 },
    { "as_sunday",     2021,  1,  3,  4,  5,  6, 0 },
    { "as_two_digit",  2021, 12, 31, 23, 59, 59, 5 },
    { "as_nine",       1999,  9,  9,  9,  9,  9, 4 },
};

#define TIME_ASCTIME_CASE_COUNT \
    ((int)(sizeof(time_asctime_cases) / sizeof(time_asctime_cases[0])))

#endif /* TIME_CASES_H */
