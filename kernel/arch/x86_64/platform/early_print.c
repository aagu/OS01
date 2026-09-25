// kernel/arch/x86_64/early_print.c — strong override for the
// arch_early_putc / arch_early_puts facade in
// kernel/include/arch/early_print.h.
//
// x86_64 implementation: COM1 (0x3F8) port I/O.  Polls the Line Status
// Register (LSR @ 0x3FD, bit 5 = Transmitter Holding Register Empty)
// and writes one byte at a time.  Deliberately bypasses the
// driver/serial.h write_serial() helper because that helper acquires
// serial_lock via spin_lock_irqsave — which is unsafe from a stack-
// smashing fault path where the lock word itself may live on the
// corrupted stack.
//
// Compiled by kernel/Makefile's $(ARCHDIR)/*.c wildcard (no explicit
// KERNEL_C_SOURCES entry needed).

#include <stdint.h>
#include <arch/cpu.h>       /* arch_cpu_pause()                   */
#include <arch/x86_64/hw.h> /* inb(), outb()                      */

void arch_early_putc(char c)
{
    /* Wait for COM1 transmitter holding register empty (LSR bit 5). */
    while ((inb(0x3FD) & 0x20) == 0)
        arch_cpu_pause();
    outb((uint16_t)0x3F8, (uint8_t)c);
}

void arch_early_puts(const char *s)
{
    while (*s) {
        arch_early_putc(*s);
        s++;
    }
}
