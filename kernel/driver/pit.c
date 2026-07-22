#include <driver/pit.h>
#include <device/timer.h>
#include <kernel/arch/io.h>
#include <kernel/interrupt.h>
#include <stddef.h>
#include <kernel/apic.h>
#include <kernel.h>
#include <kernel/softirq.h>
#include <kernel/task.h>
#include <kernel/wait.h>
#include <kernel/percpu.h>
#include <driver/serial.h>
#include <kernel/console.h>
#include <kernel/debug.h>

// Poll NIC RX (defined in net/net.c)
extern void net_poll_rx(void);

void pit_handler(uint64_t nr __attribute__((unused)), uint64_t parameter __attribute__((unused)), pt_regs_t * regs __attribute__((unused)))
{
    jiffies++;

    // Poll timeout check: if a poll syscall is active and its
    // deadline has passed, wake the polling task.
    extern wait_queue_t *current_poll_wq;
    extern uint64_t poll_deadline_jiffies;
    if (current_poll_wq && jiffies >= poll_deadline_jiffies) {
        wait_queue_wake_all(current_poll_wq);
        current_poll_wq = NULL;
    }

    // Request rescheduling on every timer tick — schedule() manages
    // per-task quantum counters and picks the next task.
    this_cpu()->need_resched = 1;
    this_cpu()->watchdog_counter++;

    // IRQ fallback: poll UART on every tick.  Under normal
    // operation the serial ISR delivers input.  This exists
    // solely to prevent a complete hang if IOAPIC routing
    // or UART IRQ generation ever fails on real hardware.
    serial_poll();

    if ((container_of(list_next(&timer_list_head.list), timer_t, list)->expire_jiffies <= jiffies))
        set_softirq_status(TIMER_SIRQ);
}

void pit_init()
{
    register_irq(0, NULL, &pit_handler, 0, IRQF_TRIGGER_EDGE, "pit");
    set_frequency(100); //100 times per sec
}

void set_frequency(uint16_t hz)
{
    uint16_t divisor = CLOCK_FREQUENCY / hz;
    outb(PIT_COMMAND, PIT_ICW);
    outb(PIT_DATA, divisor & 0xff);
    outb(PIT_DATA, (divisor >> 8) & 0xff);
}