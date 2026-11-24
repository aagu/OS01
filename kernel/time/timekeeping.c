#include <time/timekeeping.h>
#include <arch/clocksource.h>
#include <driver/rtc.h>
#include <log/log.h>
#include <subsys/subsys.h>

static bool     wall_ready;
static uint64_t boot_wall_ns;
static uint64_t boot_mono_ns;

static bool is_leap_year(uint32_t year)
{
    return (year % 4u == 0u && year % 100u != 0u) || (year % 400u == 0u);
}

static uint8_t days_in_month(uint32_t year, uint8_t month)
{
    static const uint8_t days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 2u && is_leap_year(year))
        return 29u;
    return days[month - 1u];
}

static uint32_t resolve_full_year(const datetime_t *dt)
{
    if (dt->century != 0u)
        return (uint32_t)dt->century * 100u + (uint32_t)dt->year;
    return (dt->year >= 70u) ? (1900u + (uint32_t)dt->year)
                             : (2000u + (uint32_t)dt->year);
}

bool timekeeping_datetime_to_epoch(const datetime_t *dt, uint64_t *epoch_sec_out)
{
    if (!dt || !epoch_sec_out)
        return false;

    uint32_t full_year = resolve_full_year(dt);
    if (full_year < 1970u)
        return false;
    if (dt->month < 1u || dt->month > 12u)
        return false;
    if (dt->day < 1u || dt->day > days_in_month(full_year, dt->month))
        return false;
    if (dt->hour > 23u || dt->minute > 59u || dt->second > 59u)
        return false;

    /* Howard Hinnant's civil-to-days algorithm (days since 1970-01-01). */
    int64_t y = (int64_t)full_year;
    int64_t m = (int64_t)dt->month;
    int64_t d = (int64_t)dt->day;
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    if (days < 0)
        return false;

    *epoch_sec_out = (uint64_t)days * 86400ULL
                   + (uint64_t)dt->hour * 3600ULL
                   + (uint64_t)dt->minute * 60ULL
                   + (uint64_t)dt->second;
    return true;
}

void timekeeping_init(void)
{
    datetime_t dt;
    uint64_t epoch_sec = 0;

    if (!rtc_read_datetime(&dt) || !timekeeping_datetime_to_epoch(&dt, &epoch_sec)) {
        __atomic_store_n(&wall_ready, false, __ATOMIC_RELEASE);
        return;
    }

    boot_mono_ns = arch_clocksource_read_ns();
    boot_wall_ns = epoch_sec * 1000000000ULL;
    __atomic_store_n(&wall_ready, true, __ATOMIC_RELEASE);

    uint32_t full_year = resolve_full_year(&dt);
    log_info("[time] RTC wall clock: %04u-%02u-%02u %02u:%02u:%02u UTC\n",
             full_year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

uint64_t timekeeping_real_ns(void)
{
    uint64_t mono = arch_clocksource_read_ns();
    if (!__atomic_load_n(&wall_ready, __ATOMIC_ACQUIRE))
        return mono;

    uint64_t delta = (mono >= boot_mono_ns) ? (mono - boot_mono_ns) : 0ULL;
    return boot_wall_ns + delta;
}

bool timekeeping_has_rtc(void)
{
    return __atomic_load_n(&wall_ready, __ATOMIC_ACQUIRE);
}

static int _timekeeping_init_wrapper(void)
{
    timekeeping_init();
    return 0;
}

static int _timekeeping_register(void)
{
    static const char *const deps[] = { "clocksource" };
    return register_subsys_deps("timekeeping", _timekeeping_init_wrapper,
                                SUBSYS_PHASE_4, 0, deps, 1);
}
SUBSYS_INITCALL(_timekeeping_register);
