#include <driver/pit.h>
#include <device/timer.h>
#include <kernel/arch/x86_64/hw.h>
#include <kernel/interrupt.h>
#include <stddef.h>
#include <device/pic.h>
#include <kernel.h>
#include <kernel/softirq.h>

hw_int_controller_t pit_controller =
{
    .enable = pic_enable,
    .disable = pic_disable,
    .install = pic_install,
    .uninstall = pic_uninstall,
    .ack = pic_ack,
};

void pit_handler(uint64_t nr __attribute__((unused)), uint64_t parameter __attribute__((unused)), pt_regs_t * regs __attribute__((unused)))
{
    jiffies++;

    if ((container_of(list_next(&timer_list_head.list), timer_t, list)->expire_jiffies <= jiffies))
        set_softirq_status(TIMER_SIRQ);
}

void pit_init()
{
    register_irq(32, NULL, &pit_handler, 0, &pit_controller, "pit");
    set_frequency(100); //100 times per sec
}

void set_frequency(uint16_t hz)
{
    uint16_t divisor = CLOCK_FREQUENCY / hz;
    outb(PIT_COMMAND, PIT_ICW);
    outb(PIT_DATA, divisor & 0xff);
    outb(PIT_DATA, (divisor >> 8) & 0xff);
}