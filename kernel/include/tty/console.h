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

// Force re-enable framebuffer output while Pos.lock is already held.
void console_force_enable_locked(void);

// Notify console of mode resize while holding Pos.lock.
// Resets term_cursor_row/col and Pos.XPosition/YPosition to 0.
void console_notify_resize_locked(void);

#ifdef OS01_HOST_TEST
bool console__test_is_active(void);
void console__test_get_cursors(int *row, int *col, int32_t *pos_x, int32_t *pos_y);
void console__test_set_cursors(int row, int col, int32_t pos_x, int32_t pos_y);
#endif

#endif
