#ifndef _KERNEL_CLOCKSOURCE_H
#define _KERNEL_CLOCKSOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include <time/timer.h>      // jiffies
#include <arch/cpu.h>   // arch_cycle_counter()

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

#endif