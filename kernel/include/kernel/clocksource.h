#ifndef _KERNEL_CLOCKSOURCE_H
#define _KERNEL_CLOCKSOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include <device/timer.h>      // jiffies
#include <kernel/percpu.h>     // this_cpu(), percpu_t->tsc_offset
#include <kernel/arch/cpu.h>   // arch_cycle_counter()

// mult/shift 由 clocksource_init() 计算并导出（static inline read_ns 引用）。
extern bool     clocksource_active;
extern uint32_t clocksource_mult;
extern uint32_t clocksource_shift;

// 依据 arch_cycle_freq() 计算 mult/shift；freq=0 时 active=false（退 jiffies）。
void     clocksource_init(void);

// 已校准的 cycle 频率（Hz），0 = 未校准。
uint64_t clocksource_freq_hz(void);

// 原始 cycle 计数（调试/校准用），不加 tsc_offset。
uint64_t clocksource_cycles(void);

// 单调纳秒。active 时 = (cycle+tsc_offset)*mult>>shift；否则退 jiffies*10ms。
// 仅在 GS base 装之后调用（boot 期校准用 arch_cycle_counter()）。
static inline uint64_t clocksource_read_ns(void)
{
    if (!clocksource_active)
        return jiffies * 10000000ULL;
    uint64_t c = arch_cycle_counter() + (uint64_t)this_cpu()->tsc_offset;
    return (uint64_t)(((__uint128_t)c * clocksource_mult) >> clocksource_shift);
}

#endif