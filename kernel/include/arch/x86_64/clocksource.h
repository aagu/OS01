#ifndef _KERNEL_ARCH_X86_64_CLOCKSOURCE_H
#define _KERNEL_ARCH_X86_64_CLOCKSOURCE_H

#include <time/clocksource.h>   // clocksource_active/mult/shift, jiffies fallback
#include <time/timer.h>
#include <arch/cpu.h>           // arch_cycle_counter()
#include <percpu/percpu.h>      // this_cpu()->tsc_offset

/* x86_64-only clocksource_read_ns() inline.
 *
 * 单调纳秒。active 时 = (cycle+tsc_offset)*mult>>shift；否则退 jiffies*10ms。
 * 仅在 GS base 装之后调用（boot 期校准用 arch_cycle_counter()）。
 *
 * Moved from kernel/include/time/clocksource.h in the AAGU-4 residual
 * cleanup to remove the #ifdef __x86_64__ guard from the arch-neutral
 * header (spec §2.3). aarch64 phase 1 does not need this inline; callers
 * on aarch64 use clocksource_cycles() / clocksource_init() directly. */
static inline uint64_t clocksource_read_ns(void)
{
    if (!clocksource_active)
        return jiffies * 10000000ULL;
    uint64_t c = arch_cycle_counter() + (uint64_t)this_cpu()->tsc_offset;
    return (uint64_t)(((__uint128_t)c * clocksource_mult) >> clocksource_shift);
}

#endif