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

// 测 jiffies ~100Hz：TSC 采样 ~500ms 窗口，断言 Δjiffies ≈ 50（±10%）。
// 注意：selftest.h 建议测试 sub-ms，本测试是 spec §11 明确要求的 500ms 窗口，
// 例外处理。
int test_timer_jiffies_hz(void)
{
    uint64_t freq = clocksource_freq_hz();
    if (!freq) {
        serial_printk("[selftest] timer_jiffies_hz: no freq, skip\n");
        return 0;  // 无法精确计时，跳过（不算失败）
    }
    uint64_t tsc0 = arch_cycle_counter();
    uint64_t jif0 = jiffies;
    uint64_t budget = freq / 2;   // 0.5s
    while (arch_cycle_counter() - tsc0 < budget)
        arch_cpu_pause();
    uint64_t dj = jiffies - jif0;
    if (dj >= 45 && dj <= 55)     // 期望 ~50（±10%）
        return 0;
    serial_printk("[selftest] timer_jiffies_hz: dj=%lu (expected ~50)\n", (unsigned long)dj);
    return -1;
}

#endif // OS01_SELFTEST