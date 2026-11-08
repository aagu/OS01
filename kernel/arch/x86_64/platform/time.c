#include <stdint.h>
#include <stdbool.h>
#include <arch/x86_64/cpuid.h>   // cpuid()
#include <intr/apic.h>                 // lapic_timer_start / lapic_timer_set_premeasured
#include <intr/interrupt.h>     // irq_mask / irq_unmask (PIT/LAPIC handoff)
#include <arch/irq.h>
#include <arch/x86_64/rtc.h>     // rtc_pie_calibrate (x86_64 platform glue)
#include <sched/accounting.h>    // sched_accounting_cpu_start (monitor Task 2)
#include <driver/pit.h>         // pit_prepared (BOOT-2 handoff gate)
#include <core/panic.h>         // kpanic (BOOT-2 fatal)
#ifdef X86_BOOT_IRQ_TIME_TEST
#include <selftest/boot_irq_time_fixture.h>
#endif

// 三级回落：CPUID 15h → RTC PIE（BOOT-2 受控窗口）→ 0。
// BOOT-2: the FIRST boot-time decision (including a failed 0) is cached;
// later calls are pure queries and never touch the RTC/LAPIC again.
// Re-calibration on a running system (SMP/timer up) is forbidden — the
// cache is also what makes that impossible by construction.
static uint64_t cycle_freq_cache;
static bool cycle_freq_cached;

uint64_t arch_cycle_freq(void)
{
    if (cycle_freq_cached)
        return cycle_freq_cache;

    uint32_t eax, ebx, ecx, edx;
#ifndef X86_BOOT_IRQ_TIME_FORCE_RTC
    cpuid(0x15, &eax, &ebx, &ecx, &edx);
#else
    eax = ebx = ecx = 0;   /* force-rtc: bypass the CPUID nominal answer */
#endif
    if (ecx != 0 && eax != 0) {   // eax==0 防 boot 期 #DE（畸形/虚拟 CPU）
        /* CPUID answered: no RTC side effects at all. */
        cycle_freq_cache = (uint64_t)ecx * ebx / eax;
        cycle_freq_cached = true;
        return cycle_freq_cache;
    }

    // RTC PIE joint calibration inside the controlled window.
    uint64_t tsc_hz = 0, lapic_hz = 0;
    if (rtc_pie_calibrate(&tsc_hz, &lapic_hz) == 0) {
        lapic_timer_set_premeasured(lapic_hz);
        cycle_freq_cache = tsc_hz;
    } else {
        cycle_freq_cache = 0;     /* failure cached as 0 */
    }
    cycle_freq_cached = true;
    return cycle_freq_cache;
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
    /* BOOT-2 BSP handoff (spec §5.2): one IRQ-off region; at most one
     * periodic source unmasked at any moment.  A PIT that was never
     * prepared is a boot-protocol error (no fallback to publish). */
    if (!pit_prepared())
        kpanic("BOOT-2: tick handoff without a prepared bootstrap PIT");

    uint32_t pit_gsi = arch_irq_isa_to_gsi(0);
    arch_irq_state_t flags = arch_local_irq_save();

    irq_mask(pit_gsi);
    bool ok = lapic_timer_start(100);
    if (ok) {
        // LAPIC 接管成功，PIT 保持掩蔽。
        sched_accounting_cpu_start(0, 100, true);
    } else {
        // 失败：LAPIC 已保证 masked/stopped（start 失败路径），PIT 重新
        // 放行 —— false 表示 PIT 有效回退，不是无 tick。
        irq_unmask(pit_gsi);
        sched_accounting_cpu_start(0, 100, true);
    }
    arch_local_irq_restore(flags);
#ifdef X86_BOOT_IRQ_TIME_TEST
    boot2_test_after_handoff(ok);
#endif
    return ok;
}
