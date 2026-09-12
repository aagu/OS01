#include <driver/pit.h>
#include <time/timer.h>
#include <arch/io.h>
#include <intr/interrupt.h>
#include <stddef.h>
#include <intr/apic.h>
#include <kernel.h>
#include <time/clockevent.h>  // tick_handler()
#include <driver/serial.h>
#include <tty/console.h>
#include <core/debug.h>

void pit_handler(uint64_t nr __attribute__((unused)), uint64_t parameter __attribute__((unused)), pt_regs_t * regs __attribute__((unused)))
{
    tick_handler();      // 统一 tick 语义（jiffies++/poll/need_resched/watchdog/softirq）
    serial_poll();       // x86 串口轮询 fallback，留在本层
}

void pit_init()
{
    register_irq(0, NULL, &pit_handler, 0, IRQF_TRIGGER_EDGE, "pit");
    set_frequency(100); //100 times per sec
}

void set_frequency(uint16_t hz)
{
    uint16_t divisor = CLOCK_FREQUENCY / hz;
    arch_outb(PIT_COMMAND, PIT_ICW);
    arch_outb(PIT_DATA, divisor & 0xff);
    arch_outb(PIT_DATA, (divisor >> 8) & 0xff);
}

#ifdef __x86_64__
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
    pit_init();
    return 0;
}
static int _pit_register(void)
{
    register_subsys("pit", _pit_init_wrapper,
                    SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);
    return 0;
}
SUBSYS_INITCALL(_pit_register);
#endif
