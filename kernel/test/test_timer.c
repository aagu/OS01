// kernel/test/test_timer.c
// ── timer subsystem selftests ─────────────────────────────
// 本任务只放最小 arch_cycle_freq 验证（RTC PIE 回落是否解出非零 TSC 频率）；
// Task 5 扩展 jiffies/clockevent 相关测试。
// 经 main.c 的 OS01_SELFTEST 注册 → 启动早期 selftest_run_all() 执行。

#if defined(OS01_SELFTEST)

#include <kernel/clocksource.h>
#include <kernel/printk.h>

// 验证 arch_cycle_freq 在 QEMU（RTC PIE 回落）下返回非零（~2.99GHz）。
int test_timer_tsc_freq(void)
{
    uint64_t f = clocksource_freq_hz();
    if (f == 0) {
        serial_printk("[selftest] timer_tsc_freq: freq=0 (calibration failed)\n");
        return -1;
    }
    serial_printk("[selftest] timer_tsc_freq: %lu Hz\n", (unsigned long)f);
    return 0;
}

#endif // OS01_SELFTEST