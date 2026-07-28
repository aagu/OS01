#include <kernel/console.h>
#include <kernel/printk.h>
#include <font.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <driver/serial.h>

// font is a global in kernel/kernel/printk.c
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

    if (!console_fb_active) {
        write_serial(c);
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
}

void console_init(void)
{
    term_cursor_row = Pos.YPosition + 1;
    int max_rows = (int)(Pos.YResolution / font->height);
    if (term_cursor_row >= max_rows) term_cursor_row = max_rows - 1;
    term_cursor_col = 0;
    term_fg = WHITE;
    term_bg = BLACK;
    term_initialized = true;
    console_fb_active = true;
}

void console_surrender_fb(void)
{
    console_fb_active = false;
}

void console_force_enable(void)
{
    console_fb_active = true;
}
