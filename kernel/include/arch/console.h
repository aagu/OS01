#ifndef ARCH_CONSOLE_H
#define ARCH_CONSOLE_H

/*
 * Console TTY per-arch wiring (facade).
 *
 * The console TTY (kernel/tty/boot.c tty_boot_init) is arch-neutral, but
 * its output sink and input producers are platform devices:
 *
 *   arch_console_putchar   — output_char for the console TTY.  Called from
 *                            tty_write() with serial_lock held, so the
 *                            implementation must NOT re-acquire serial_lock
 *                            (use the *_unlocked serial primitive).
 *   arch_console_input_attach — bind the per-arch console input producers
 *                            to the console TTY (x86: COM1 RX IRQ + i8042
 *                            keyboard IRQ; aarch64: PL011 RX IRQ).  Each
 *                            producer feeds the TTY via tty_push_input().
 *                            Must not fail the boot — a console without
 *                            input degrades, it does not abort.
 *
 * Implementations: kernel/arch/x86_64/platform/console.c,
 *                  kernel/arch/aarch64/platform/console.c.
 */

struct tty_struct;

void arch_console_putchar(char c);
void arch_console_input_attach(struct tty_struct *tty);

#endif /* ARCH_CONSOLE_H */
