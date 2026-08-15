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
#include <driver/serial.h>
#include <kernel/console.h>
#include <kernel/debug.h>

void pit_handler(uint64_t nr __attribute__((unused)), uint64_t parameter __attribute__((unused)), pt_regs_t * regs __attribute__((unused)))
{
    jiffies++;

    // Poll timeout check: wake every registered poll whose
    // deadline has passed.  Registry nodes live per-poll (kernel/fs/
    // poll.c) — concurrent pollers don't clobber each other, and a
    // wake that lands before the poller sleeps is retried next tick.
    extern poll_timeout_node_t *poll_timeout_head;
    extern spinlock_T poll_timeout_lock;
    if (poll_timeout_head) {
        spin_lock(&poll_timeout_lock);
        for (poll_timeout_node_t *n = poll_timeout_head; n; n = n->next)
            if (jiffies >= n->deadline)
                wait_queue_wake_all(n->wq);
        spin_unlock(&poll_timeout_lock);
    }

    // RX is now interrupt-driven: the e1000 MSI-X handler buffers
    // packets and wakes tcpip_thread (see e1000_handler).  No PIT-tick
    // polling needed — the mbox idle timer remains as a wake-up
    // fallback inside sys_arch_mbox_fetch.

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
    arch_outb(PIT_COMMAND, PIT_ICW);
    arch_outb(PIT_DATA, divisor & 0xff);
    arch_outb(PIT_DATA, (divisor >> 8) & 0xff);
}
