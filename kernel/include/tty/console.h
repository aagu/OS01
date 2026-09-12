#ifndef _KERNEL_CONSOLE_H
#define _KERNEL_CONSOLE_H

// Initialize the software terminal.
// Must be called after framebuffer is mapped and PIT is running.
void console_init(void);

// Feed one output character to the emergency console.
// Only handles \n \r \b \t and printable characters.
// No VT100 CSI parsing, no cursor blink.
// After surrender, output goes to serial only.
void console_putchar(char c);

// Surrender the framebuffer to userspace (terminal.elf).
// Called by /dev/fb FBIOSURRENDER ioctl.
void console_surrender_fb(void);

// Force re-enable framebuffer output for kernel panic.
void console_force_enable(void);

#endif
