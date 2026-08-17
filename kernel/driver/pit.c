#include <driver/pit.h>
#include <device/timer.h>
#include <kernel/arch/io.h>
#include <kernel/interrupt.h>
#include <kernel/poll.h>  // poll_timeout_node_t (PIT registry scan)
#include <stddef.h>
#include <kernel/apic.h>
#include <kernel.h>
#include <kernel/softirq.h>
#include <kernel/task.h>
#include <kernel/wait.h>
#include <kernel/percpu.h>
#include <kernel/clockevent.h>  // tick_handler()
#include <driver/serial.h>
#include <kernel/console.h>
#include <kernel/debug.h>

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
