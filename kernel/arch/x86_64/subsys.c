// kernel/arch/x86_64/subsys.c

#include <kernel/subsys.h>
#include <kernel/apic.h>
#include <device/pic.h>
#include <device/timer.h>
#include <driver/pit.h>
#include <driver/keyboard.h>
#include <driver/serial.h>
#include <driver/ahci.h>
#include <net/net.h>
#include <kernel/clocksource.h>

// ── RSDP 地址（由 kernel_main 在调用 arch_register_subsys 前设置） ──
uint64_t arch_boot_rsdp = 0;

// ── 包装函数（void → int (*)(void)） ──────────────────────

static int _clocksource_init_wrapper(void)
{
    clocksource_init();
    return 0;
}

static int _apic_init_wrapper(void)
{
    apic_init(arch_boot_rsdp);
    return 0;
}

static int _pic_init_wrapper(void)
{
    pic_init();
    return 0;
}

static int _timer_init_wrapper(void)
{
    timer_init();
    return 0;
}

static int _pit_init_wrapper(void)
{
    pit_init();
    return 0;
}

static int _lapic_timer_init_wrapper(void)
{
    lapic_timer_init();
    return 0;
}

static int _keyboard_init_wrapper(void)
{
    keyboard_init();
    return 0;
}

static int _init_serial_irq_wrapper(void)
{
    init_serial_irq();
    return 0;
}

static int _ahci_init_wrapper(void)
{
    ahci_init();
    return 0;
}

static int _net_hw_init_wrapper(void)
{
    return net_hw_init();
}

// ── Arch 注册入口 ─────────────────────────────────────────

void arch_register_subsys(void)
{
    // Phase 3: 中断控制器
    register_subsys("apic", _apic_init_wrapper,           SUBSYS_PHASE_3, 0);
    register_subsys("pic",  _pic_init_wrapper,            SUBSYS_PHASE_3, SUBSYS_FLAG_OPTIONAL);

    // Phase 4: 定时器
    register_subsys("timer",      _timer_init_wrapper,      SUBSYS_PHASE_4, 0);
    register_subsys("clocksource", _clocksource_init_wrapper, SUBSYS_PHASE_4, 0);
    register_subsys("pit",        _pit_init_wrapper,        SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);
    register_subsys("lapic-timer", _lapic_timer_init_wrapper, SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);

    // Phase 5: 设备 IRQ
    register_subsys("keyboard", _keyboard_init_wrapper,    SUBSYS_PHASE_5, SUBSYS_FLAG_OPTIONAL);
    register_subsys("serial",   _init_serial_irq_wrapper,  SUBSYS_PHASE_5, 0);

    // Phase 6: 存储 + 网络
    register_subsys("ahci", _ahci_init_wrapper,            SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
    register_subsys("net-hw", _net_hw_init_wrapper,        SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
}
