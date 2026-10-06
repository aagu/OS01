#include <tty/console.h>
#include <core/printk.h>
#include <driver/font.h>
#include <driver/fb_state.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <driver/serial.h>

// font is a global in kernel/core/printk.c
extern psf2_t *font;

// Terminal cursor state
static int term_cursor_row = 0;
static int term_cursor_col = 0;
static unsigned int term_fg = WHITE;
static unsigned int term_bg = BLACK;
static bool term_initialized = false;
static bool console_fb_active = true;

// Scroll the entire framebuffer up by one character row.
static void console_scroll(void)
{
    int rows = (int)(Pos.YResolution / font->height);
    uint32_t pitch = Pos.XResolution * sizeof(uint32_t);
    int row_bytes = (int)(pitch * font->height);
    uint8_t *fb = (uint8_t *)Pos.FB_addr;
    memmove(fb, fb + row_bytes, (uintptr_t)row_bytes * (rows - 1));
    memset(fb + (uintptr_t)row_bytes * (rows - 1), 0, (uintptr_t)row_bytes);
    term_cursor_row = rows - 1;
}

// Degraded console_putchar: only \n \r \b \t + printable chars + scroll.
// No VT100 CSI parsing, no cursor blink.
void console_putchar(char c)
{
    if (!term_initialized) return;

    // write_serial_unlocked: console_putchar is installed as the
    // console TTY's output_char (see kernel/core/main.c), so it
    // runs inside tty_write() which already holds serial_lock.
    // Calling write_serial() here would re-acquire the non-
    // recursive spinlock and deadlock.  Other call sites (early
    // init, debug prints from IRQ context) use this same entry
    // point but tty_write is not on their path.
    write_serial_unlocked(c);

    uint64_t flags = spin_lock_irqsave(&Pos.lock);
    if (!console_fb_active) {
        spin_unlock_irqrestore(&Pos.lock, flags);
        return;
    }

    fb_lease_t lease;
    int lrc = fb_writer_begin(&lease, 0);
    if (lrc < 0) {
        // Fallback to serial only; do NOT alter screen cursor
        spin_unlock_irqrestore(&Pos.lock, flags);
        return;
    }

    switch (c) {
    case '\n':
        term_cursor_col = 0;
        term_cursor_row++;
        break;
    case '\r':
        term_cursor_col = 0;
        break;
    case '\b': case 0x7F:
        if (term_cursor_col > 0) term_cursor_col--;
        break;
    case '\t':
        term_cursor_col = (term_cursor_col + 8) & ~7;
        break;
    default:
        if ((unsigned char)c >= ' ') {
            putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, c);
            term_cursor_col++;
        }
        break;
    }

    int max_cols = (int)(Pos.XResolution / font->width);
    if (term_cursor_col >= max_cols) {
        term_cursor_col = 0;
        term_cursor_row++;
    }
    int max_rows = (int)(Pos.YResolution / font->height);
    if (term_cursor_row >= max_rows)
        console_scroll();

    fb_writer_end(&lease);
    spin_unlock_irqrestore(&Pos.lock, flags);
}

void console_init(void)
{
    uint64_t flags = spin_lock_irqsave(&Pos.lock);
    term_cursor_row = Pos.YPosition + 1;
    int max_rows = (int)(Pos.YResolution / font->height);
    if (term_cursor_row >= max_rows) term_cursor_row = max_rows - 1;
    term_cursor_col = 0;
    term_fg = WHITE;
    term_bg = BLACK;
    term_initialized = true;
    console_fb_active = true;
    spin_unlock_irqrestore(&Pos.lock, flags);
}

void console_surrender_fb(void)
{
    uint64_t flags = spin_lock_irqsave(&Pos.lock);
    console_fb_active = false;
    spin_unlock_irqrestore(&Pos.lock, flags);
}

void console_force_enable(void)
{
    uint64_t flags = spin_lock_irqsave(&Pos.lock);
    console_fb_active = true;
    spin_unlock_irqrestore(&Pos.lock, flags);
}

void console_notify_resize_locked(void)
{
    term_cursor_row = 0;
    term_cursor_col = 0;
    Pos.XPosition = 0;
    Pos.YPosition = 0;
}

#ifdef OS01_HOST_TEST
void console__test_get_cursors(int *row, int *col, int32_t *pos_x, int32_t *pos_y)
{
    if (row) *row = term_cursor_row;
    if (col) *col = term_cursor_col;
    if (pos_x) *pos_x = Pos.XPosition;
    if (pos_y) *pos_y = Pos.YPosition;
}

void console__test_set_cursors(int row, int col, int32_t pos_x, int32_t pos_y)
{
    term_cursor_row = row;
    term_cursor_col = col;
    Pos.XPosition = pos_x;
    Pos.YPosition = pos_y;
}
#endif

