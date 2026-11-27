/* @os01-test
 * id: test_timekeeping
 * layer: host
 * subsystem: sched
 * timing: strict
 * tier: pr
 * link: timekeeping
 * scenarios: [sched.timekeeping.wall_clock]
 */
/*
 * test_timekeeping.c — host unit tests for kernel/time/timekeeping.c.
 *
 * Verifies:
 *   1. timekeeping_datetime_to_epoch() against known UTC timestamps
 *      (epoch 0, leap years, century=0 19xx/20xx inference, invalid inputs).
 *   2. timekeeping_init() + timekeeping_real_ns() advancing with the
 *      monotonic clocksource when an RTC is present.
 *   3. Fallback to monotonic uptime when rtc_read_datetime() returns false.
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time/timekeeping.h>
#include <subsys/subsys.h>

/* ── Host mocks for kernel/time/timekeeping.c dependencies ── */
static bool       mock_rtc_ok;
static datetime_t mock_rtc_dt;
static uint64_t   mock_mono_ns;

bool rtc_read_datetime(datetime_t *dt)
{
    if (!mock_rtc_ok || !dt)
        return false;
    *dt = mock_rtc_dt;
    return true;
}

uint64_t arch_clocksource_read_ns(void)
{
    return mock_mono_ns;
}

int register_subsys_deps(const char *name, int (*init)(void),
                         int phase, uint32_t flags,
                         const char *const *requires, unsigned require_count)
{
    (void)name; (void)init; (void)phase; (void)flags;
    (void)requires; (void)require_count;
    return 0;
}

int g_log_level = 0;

void _log_info_impl(const char *fmt, ...)
{
    (void)fmt;
}

/* ── Suites ── */
static void suite_datetime_to_epoch(void)
{
    TEST_SUITE("timekeeping_datetime_to_epoch");
    uint64_t sec = 0;

    /* 1970-01-01 00:00:00 -> 0 */
    datetime_t dt_epoch = { .century = 19, .year = 70, .month = 1, .day = 1,
                            .hour = 0, .minute = 0, .second = 0 };
    assert_true(timekeeping_datetime_to_epoch(&dt_epoch, &sec));
    assert_eq(0ULL, sec);

    /* 2023-11-14 22:13:20 -> 1700000000 */
    datetime_t dt_2023 = { .century = 20, .year = 23, .month = 11, .day = 14,
                           .hour = 22, .minute = 13, .second = 20 };
    assert_true(timekeeping_datetime_to_epoch(&dt_2023, &sec));
    assert_eq(1700000000ULL, sec);

    /* century=0 inference: year=26 -> 2026-11-24 14:45:58 -> 1795531558 */
    datetime_t dt_2026 = { .century = 0, .year = 26, .month = 11, .day = 24,
                           .hour = 14, .minute = 45, .second = 58 };
    assert_true(timekeeping_datetime_to_epoch(&dt_2026, &sec));
    assert_eq(1795531558ULL, sec);

    /* 2000-02-29 12:00:00 (leap year) -> 951825600 */
    datetime_t dt_leap = { .century = 20, .year = 0, .month = 2, .day = 29,
                           .hour = 12, .minute = 0, .second = 0 };
    assert_true(timekeeping_datetime_to_epoch(&dt_leap, &sec));
    assert_eq(951825600ULL, sec);

    /* 2100-02-29 (not a leap year) -> rejected */
    datetime_t dt_nonleap = { .century = 21, .year = 0, .month = 2, .day = 29,
                              .hour = 0, .minute = 0, .second = 0 };
    assert_true(!timekeeping_datetime_to_epoch(&dt_nonleap, &sec));

    /* Out-of-range fields -> rejected */
    datetime_t dt_bad_mon = { .century = 20, .year = 26, .month = 13, .day = 1 };
    assert_true(!timekeeping_datetime_to_epoch(&dt_bad_mon, &sec));

    datetime_t dt_bad_hr = { .century = 20, .year = 26, .month = 1, .day = 1,
                             .hour = 24 };
    assert_true(!timekeeping_datetime_to_epoch(&dt_bad_hr, &sec));

    datetime_t dt_pre_epoch = { .century = 19, .year = 69, .month = 12, .day = 31,
                                .hour = 23, .minute = 59, .second = 59 };
    assert_true(!timekeeping_datetime_to_epoch(&dt_pre_epoch, &sec));
}

static void suite_wall_clock_tracking(void)
{
    TEST_SUITE("timekeeping_init and timekeeping_real_ns");

    /* 1. No RTC -> falls back to monotonic clocksource */
    mock_rtc_ok = false;
    mock_mono_ns = 123456789ULL;
    timekeeping_init();
    assert_true(!timekeeping_has_rtc());
    assert_eq(123456789ULL, timekeeping_real_ns());

    /* 2. RTC present -> anchored at boot_mono_ns and advances with mono */
    mock_rtc_ok = true;
    mock_rtc_dt = (datetime_t){
        .century = 20, .year = 23, .month = 11, .day = 14,
        .hour = 22, .minute = 13, .second = 20
    };
    mock_mono_ns = 500000000ULL;
    timekeeping_init();
    assert_true(timekeeping_has_rtc());
    assert_eq(1700000000ULL * 1000000000ULL, timekeeping_real_ns());

    /* Advance monotonic clock by 2.5 seconds */
    mock_mono_ns = 3000000000ULL;
    assert_eq(1700000002ULL * 1000000000ULL + 500000000ULL,
              timekeeping_real_ns());
}

int main(void)
{
    suite_datetime_to_epoch();
    suite_wall_clock_tracking();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_timekeeping",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed > 0 ? 1 : 0;
}
