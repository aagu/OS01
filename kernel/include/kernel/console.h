#ifndef _KERNEL_CONSOLE_H
#define _KERNEL_CONSOLE_H

// Initialize the software terminal — sets up cursor blink timer.
// Must be called after framebuffer is mapped and PIT is running.
void console_init(void);

// Feed one output character to the terminal.
// Implements a VT100 CSI subset state machine.  Characters that are
// not part of a recognized escape sequence are rendered to the
// framebuffer via putchar_at().
// Use as TTY output_char callback.
void console_putchar(char c);

// Called from PIT handler (100 Hz) to drive cursor blink.
// Safe to call from IRQ context.
void console_blink_tick(void);

#endif
