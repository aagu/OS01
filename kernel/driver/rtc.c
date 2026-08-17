#include <driver/rtc.h>
#include <kernel/arch/io.h>
#include <kernel/interrupt.h>      // register_irq / unregister_irq / IRQF_TRIGGER_LEVEL
#include <kernel/apic.h>           // lapic_read / lapic_write / LAPIC_* / LVT_MASK
#include <kernel/arch/cpu.h>       // arch_cycle_counter / arch_cpu_pause
#include <stddef.h>                // NULL

#if defined(__x86_64__)

#define RTC_PIE_TICKS    256      // 采样 tick 数（~250ms）
#define RTC_PIE_IRQ_GSI  8        // register_irq 用 gsi
#define RTC_PIE_IRQ_VEC  0x28     // unregister_irq 用 vector（0x20 + 8）

static volatile uint32_t rtc_pie_count;
static volatile uint64_t rtc_pie_tsc0;
static volatile uint64_t rtc_pie_tsc1;

static void rtc_pie_handler(uint64_t nr, uint64_t parameter, pt_regs_t *regs)
{
    (void)nr; (void)parameter; (void)regs;
    // 读 RTC reg 0x0C 清 PIE 中断标志（否则真实硬件第一个中断后 PIE 停摆）。
    arch_outb(CMOS_ADDR, 0x80 | 0x0C);
    arch_inb(CMOS_DATA);

    if (rtc_pie_count == 0)
        rtc_pie_tsc0 = arch_cycle_counter();
    rtc_pie_count++;
    if (rtc_pie_count >= RTC_PIE_TICKS)
        rtc_pie_tsc1 = arch_cycle_counter();
}

int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out)
{
    // 1. 掩 LAPIC timer，避免 countdown 到零触发未注册的 vector 0x38（GP# 三重故障）。
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK);
    //    divisor=0（÷2，SDM 000b；lapic_timer.c 原文 "divide by 1" 注释是错的）。
    //    必须显式写。
    lapic_write(LAPIC_TIMER_DIV, 0);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // 2. 临时注册 IRQ8（gsi=8），level 触发，检查返回值。
    //    ⚠️ 这里传 gsi（8）；unregister_irq 才传 vector（0x28）。传反 →
    //    register_irq(0x28) 因 gsi=40≥MAX_GSI(24) 静默不注册；unregister_irq(8)
    //    → irq_table[-24] 野内存写。
    if (!register_irq(RTC_PIE_IRQ_GSI, NULL, rtc_pie_handler, 0,
                      IRQF_TRIGGER_LEVEL, "rtc-pie")) {
        return -1;
    }

    // 3. 使能 PIE：reg 0x0B bit6 = PIE；reg 0x0A 低 4 位 = 1024Hz (0b0110=6)。
    uint8_t b = get_rtc_register(0x0B);
    set_rtc_register(0x0B, b | 0x40);
    uint8_t a = get_rtc_register(0x0A);
    set_rtc_register(0x0A, (a & 0xF0) | 0x06);

    // 4. 主循环：双条件（tick 数未达 && TSC 流逝 < 宽松上限）。
    //    RTC PIE 校准时 freq 未知（cpuid 15h=0 才会进这里），无法把「500ms」精确
    //    换算成 cycle 数。用 2^32 cycle 作宽松兜底：@8.6GHz ≈ 500ms、@3GHz ≈
    //    1.43s、@1GHz ≈ 4.3s。正常路径 250ms 内 tick 达标，不依赖此值精度；
    //    它只防 IRQ8 完全失效时的无限自旋（挂 boot）。
    rtc_pie_count = 0;
    uint64_t tsc_start = arch_cycle_counter();
    while (rtc_pie_count < RTC_PIE_TICKS) {
        if (arch_cycle_counter() - tsc_start > 0x100000000ULL) {  // 2^32 cycle 兜底
            break;
        }
        arch_cpu_pause();
    }

    // 5. 禁 PIE，注销 IRQ8。
    b = get_rtc_register(0x0B);
    set_rtc_register(0x0B, b & ~0x40);
    unregister_irq(RTC_PIE_IRQ_VEC);     // ⚠️ 传 vector 0x28（非 gsi 8）

    // 6. 读 LAPIC 剩余计数。
    uint32_t cur = lapic_read(LAPIC_TIMER_CUR);
    uint64_t elapsed_lapic = 0xFFFFFFFFULL - cur;

    // 7. 检查是否采到足够 tick。
    if (rtc_pie_count < RTC_PIE_TICKS)
        return -1;                        // 超时/中断不到

    uint64_t tsc_elapsed = rtc_pie_tsc1 - rtc_pie_tsc0;
    // off-by-one 修正：tsc0 在沿#1、tsc1 在沿#N，实际跨 N-1 个周期。
    uint64_t n = RTC_PIE_TICKS - 1;
    *tsc_hz_out   = tsc_elapsed * 1024 / n;
    // ⚠️ lapic_hz_out = 递减率（divisor ÷2 已折算），**不 ×2**：elapsed_lapic 是
    // ÷2 后递减量，除以窗口秒数 n/1024 即递减率；lapic_timer_start 的
    // init_count=hz/freq 直接基于递减率。×2 会得真实频率 → init_count 被 divisor
    // 再 ÷2 → 50Hz。与 Task 3 Step 1 的 `elapsed * 100`（不 ×2）语义一致。
    *lapic_hz_out = elapsed_lapic * 1024 / n;
    return 0;
}

#endif // __x86_64__

bool is_updating_rtc()
{
    arch_outb(CMOS_ADDR, 0x0a);
    uint32_t status = arch_inb(CMOS_DATA);
    return (status & 0x80);
}

uint8_t get_rtc_register(uint8_t nr)
{
    arch_outb(CMOS_ADDR, 0x80 | nr);
    return arch_inb(CMOS_DATA);
}

void set_rtc_register(uint8_t nr, uint8_t val)
{
    arch_outb(CMOS_ADDR, 0x80 | nr);
    arch_outb(CMOS_DATA,val);
}

void rtc_read_datetime(datetime_t * dt)
{
    while (is_updating_rtc());
    
    dt->year = get_rtc_register(0x09);
    dt->month = get_rtc_register(0x08);
    dt->day = get_rtc_register(0x07);
    dt->hour = get_rtc_register(0x04);
    dt->minute = get_rtc_register(0x02);
    dt->second = get_rtc_register(0x00);
    
    uint8_t Use_BCD = get_rtc_register(0x0b);
    if (!(Use_BCD & 0x04))
    {
        dt->year = BCD2BIN(dt->year);
        dt->month = BCD2BIN(dt->month);
        dt->day = BCD2BIN(dt->day);
        dt->hour = BCD2BIN(dt->hour);
        dt->minute = BCD2BIN(dt->minute);
        dt->second = BCD2BIN(dt->second);
    }
    
}

void rtc_write_datetime(datetime_t * dt)
{
    while (is_updating_rtc());

    set_rtc_register(0x00, dt->second);
    set_rtc_register(0x02, dt->minute);
    set_rtc_register(0x04, dt->hour);
    set_rtc_register(0x07, dt->day);
    set_rtc_register(0x08, dt->month);
    set_rtc_register(0x09, dt->year);
}
