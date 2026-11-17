// kernel/arch/x86_64/platform/console.c — x86_64 backend of the
// <arch/console.h> facade: framebuffer + serial console output and the
// COM1 / i8042 keyboard input producers for the console TTY.

#include <arch/console.h>
#include <tty/console.h>
#include <driver/serial.h>
#include <driver/keyboard.h>

void arch_console_putchar(char c)
{
    // console_putchar mirrors every byte to serial (unlocked — tty_write
    // already holds serial_lock) and blits it through the VT100 emulator.
    console_putchar(c);
}

void arch_console_input_attach(struct tty_struct *tty)
{
    serial_set_tty(tty);       // serial RX IRQ → tty_push_input
    keyboard_set_tty(tty);     // keyboard IRQ → tty_push_input
}
