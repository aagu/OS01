/* AArch64 implementation of the arch-neutral monotonic-nanosecond hook. */
#include <arch/clocksource.h>
#include <arch/cpu.h>
#include <time/clocksource.h>
#include <time/timer.h>

uint64_t arch_clocksource_read_ns(void)
{
    if (!clocksource_active)
        return jiffies * 10000000ULL;
    uint64_t cycles = arch_cycle_counter();
    return (uint64_t)(((__uint128_t)cycles * clocksource_mult) >>
                      clocksource_shift);
}
