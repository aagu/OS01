#include <arch/clocksource.h>
#include <arch/x86_64/clocksource.h>

uint64_t arch_clocksource_read_ns(void)
{
    return clocksource_read_ns();
}
