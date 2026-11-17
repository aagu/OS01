/* kernel/arch/aarch64/platform/serial.c — aarch64 stub for the
 * arch-neutral driver/serial.h surface (write_serial /
 * write_serial_unlocked / serial_lock).
 *
 * The canonical x86_64 definitions live in kernel/driver/serial.c and
 * use arch_outb(SERIAL_COM1, ...).  That file is x86_64-only and cannot
 * compile on aarch64 (per the Makefile gate in kernel/Makefile).  This
 * TU supplies the aarch64 implementation backed by the PL011 UART
 * driver in kernel/arch/aarch64/platform/pl011.c.
 *
 * Used by libc/printk/printk.c::serial_printk (via libk's link), which
 * is the canonical kernel console on aarch64.
 */

#include <arch/spinlock.h>
#include <driver/serial.h>
#include <stdbool.h>

/* Forward decls: PL011 primitives live in pl011.c — pulled in via their
 * own TU to avoid exposing the PL011 private macros. */
void pl011_putc(char c);
bool pl011_rx_pending(void);
uint32_t pl011_dr_read(void);

spinlock_T serial_lock = {1};

void write_serial_unlocked(char c)
{
    pl011_putc(c);
}

void write_serial(char c)
{
    uint64_t flags = spin_lock_irqsave(&serial_lock);
    write_serial_unlocked(c);
    spin_unlock_irqrestore(&serial_lock, flags);
}

/* Blocking RX read, same contract as the x86 implementation: spin until
 * the RX FIFO holds a byte, then read it (pl011_dr_read consumes the DR
 * and clears the FIFO data+status).  Task-context consumer: devfs
 * /dev/serial chrdev read. */
char read_serial(void)
{
    while (!pl011_rx_pending())
        ;
    return (char)(uint8_t)pl011_dr_read();
}