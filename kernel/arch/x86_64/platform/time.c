#include <stdint.h>
#include <stdbool.h>
#include <arch/x86_64/cpuid.h>   // cpuid()
#include <intr/apic.h>                 // lapic_timer_start / lapic_timer_set_premeasured
#include <intr/interrupt.h>     // irq_mask / irq_unmask (PIT/LAPIC handoff)
#include <arch/x86_64/rtc.h>     // rtc_pie_calibrate (x86_64 platform glue)
#include <sched/accounting.h>    // sched_accounting_cpu_start (monitor Task 2)

// 三级回落：CPUID 15h → RTC PIE（Task 2 接入）→ 0。
uint64_t arch_cycle_freq(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x15, &eax, &ebx, &ecx, &edx);
    if (ecx != 0 && eax != 0)   // eax==0 防 boot 期 #DE（畸形/虚拟 CPU）
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
// PIT/LAPIC handoff: mask PIT, try LAPIC; if LAPIC fails, restore PIT.
//
// Monitor plan Task 2: this function owns the ONE BSP tick-source
// selection (and the BSP accounting publish).  BOTH success paths
// publish valid=1 — LAPIC success AND the enabled-PIT fallback — so do
// NOT gate the publish on this function's LAPIC-success bool: a LAPIC
// failure here still leaves the PIT ticking at 100 Hz, and the BSP's
// accounting would otherwise stay "unavailable" forever.  (The BSP
// per-cpu subsys callback deliberately skips both the hardware start
// and the accounting publish — see subsys_percpu.c.)
bool arch_tick_start(void)
{
    irq_mask(0);
    if (lapic_timer_start(100)) {
        // LAPIC 接管成功，PIT 保持掩蔽。
        sched_accounting_cpu_start(0, 100, true);
        return true;
    }
    irq_unmask(0);
    // LAPIC 未启动 → PIT 回退已重新使能，BSP tick 源依然有效。
    sched_accounting_cpu_start(0, 100, true);
    return false;
}
