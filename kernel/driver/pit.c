#include <driver/pit.h>
#include <time/timer.h>
#include <arch/io.h>
#include <intr/interrupt.h>
#include <arch/irq.h>
#include <stddef.h>
#include <intr/apic.h>
#include <kernel.h>
#include <time/clockevent.h>  // tick_handler()
#include <driver/serial.h>
#include <tty/console.h>
#include <core/debug.h>
#ifdef X86_BOOT_IRQ_TIME_TEST
#include <selftest/boot_irq_time_fixture.h>
#endif

void pit_handler(uint64_t nr __attribute__((unused)), uint64_t parameter __attribute__((unused)), pt_regs_t * regs __attribute__((unused)))
{
#ifdef X86_BOOT_IRQ_TIME_TEST
    boot2_test_timer_irq(0, regs);   /* BOOT-2 fixture (variants only) */
#endif
    tick_handler();      // 统一 tick 语义（jiffies++/poll/need_resched/watchdog/softirq）
    serial_poll();       // x86 串口轮询 fallback，留在本层
}

/* BOOT-2 prepare/start split: the prepare registers masked
 * (IRQF_NO_AUTOEN) and programs 100 Hz; the route opens only in
 * pit_start, after the runtime-open precondition check. */
static bool pit_prepared_flag;
static bool pit_started_flag;

bool pit_prepared(void) { return pit_prepared_flag; }

int pit_init(void)
{
    if (pit_prepared_flag)
        return -1;              /* repeat prepare: boot protocol error */
    uint32_t pit_gsi = arch_irq_isa_to_gsi(0);
    if (!register_irq(pit_gsi, NULL, &pit_handler, 0,
                      IRQF_TRIGGER_EDGE | IRQF_NO_AUTOEN, "pit"))
        return -1;
    set_frequency(100);         /* 100 times per sec */
    pit_prepared_flag = true;
    return 0;
}

int pit_start(void)
{
    if (!pit_prepared_flag || pit_started_flag)
        return -1;
    uint32_t pit_gsi = arch_irq_isa_to_gsi(0);
    irq_unmask(pit_gsi);
    pit_started_flag = true;
    return 0;
}

void set_frequency(uint16_t hz)
{
    uint16_t divisor = CLOCK_FREQUENCY / hz;
    arch_outb(PIT_COMMAND, PIT_ICW);
    arch_outb(PIT_DATA, divisor & 0xff);
    arch_outb(PIT_DATA, (divisor >> 8) & 0xff);
}

#include <subsys/subsys.h>
// Register this driver into the platform's subsystem table. The
// _register function is collected by arch_register_subsys() at boot via
// the .subsys_init linker section; it calls register_subsys() to queue
// the init wrapper for subsys_init_phase() to run later. The split
// (register vs init) ensures each driver's init runs ONCE — at the
// right phase — and never during the .subsys_init pass (which would
// race init order with sibling drivers).
static int _pit_init_wrapper(void)
{
    return pit_init();
}
static int _pit_register(void)
{
    register_subsys("pit", _pit_init_wrapper,
                    SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);
    return 0;
}
SUBSYS_INITCALL(_pit_register);
