#ifndef _KERNEL_CLOCKEVENT_H
#define _KERNEL_CLOCKEVENT_H

#include <stdint.h>

// 静态注册阶段：不启动任何 tick 源（PIT 在 phase 4 由 pit_init 照常启动）。
void tick_init(void);

// 显式启动（percpu+GS 就绪后调用）：先掩 PIT，arch_tick_start() 成功则 LAPIC
// 接管，失败则回退 PIT。
void tick_start(void);

// 统一 tick 语义（arch IRQ handler 调用，无参）：jiffies++ → poll 扫描
// （deadline 纳秒比较）→ need_resched → watchdog → TIMER_SIRQ。EOI 由各
// arch handler 负责。
void tick_handler(void);

#endif