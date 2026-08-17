#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/x86_64/cpuid.h>   // cpuid()
#include <kernel/apic.h>                 // lapic_timer_start / lapic_timer_set_premeasured
#include <driver/rtc.h>                  // rtc_pie_calibrate

// 三级回落：CPUID 15h → RTC PIE（Task 2 接入）→ 0。
uint64_t arch_cycle_freq(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x15, &eax, &ebx, &ecx, &edx);
    if (ecx != 0)
        return (uint64_t)ecx * ebx / eax;   // core crystal Hz * ratio

    // RTC PIE 联合校准：同时测 TSC + LAPIC，LAPIC 结果暂存复用。
    uint64_t tsc_hz = 0, lapic_hz = 0;
    if (rtc_pie_calibrate(&tsc_hz, &lapic_hz) == 0) {
        lapic_timer_set_premeasured(lapic_hz);
        return tsc_hz;
    }
    return 0;
}

// 启动 x86 tick 源：LAPIC 周期模式。返回是否启动成功。
bool arch_tick_start(void)
{
    return lapic_timer_start(100);
}