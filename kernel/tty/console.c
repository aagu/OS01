#include <kernel/console.h>
#include <kernel/printk.h>
#include <font.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// font is a global in kernel/kernel/printk.c
extern psf2_t *font;

// ═══════════════════════════════════════════════════════
//  Terminal cursor state
// ═══════════════════════════════════════════════════════

static int term_cursor_row = 0;
static int term_cursor_col = 0;
static bool term_cursor_visible = true;   // ?25h/?25l
static bool term_blink_on = false;        // current blink phase
static int  term_blink_counter = 0;       // ticks since last toggle
static unsigned int term_fg = WHITE;
static unsigned int term_bg = BLACK;
static bool term_initialized = false;

// ═══════════════════════════════════════════════════════
//  VT100 CSI state machine
// ═══════════════════════════════════════════════════════

enum csi_state {
    CSI_NORMAL,
    CSI_ESC,
    CSI_BRACKET,    // after ESC [
    CSI_PARAM,      // accumulating digit(s)
    CSI_QMARK,      // after ESC [ ?
};

static enum csi_state cs = CSI_NORMAL;
static int cs_param = 0;
static bool cs_qmark = false;

// ═══════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════

// Erase from current cursor column to end of line (inclusive).
static void console_clear_to_eol(void)
{
    int max_cols = (int)(Pos.XResolution / font->width);
    for (int x = term_cursor_col; x < max_cols; x++) {
        putchar_at(x, term_cursor_row, term_fg, term_bg, ' ');
    }
}

// Scroll the entire framebuffer up by one character row.
// Moves rows 1..N-1 up, clears the bottom row.
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

// Draw or erase the blink cursor block at current position.
// blink_on=true -> reverse video; blink_on=false -> draw space.
static void console_draw_blink(bool on)
{
    if (!term_cursor_visible || !term_initialized)
        return;

    int max_cols = (int)(Pos.XResolution / font->width);
    int max_rows = (int)(Pos.YResolution / font->height);

    if (term_cursor_row < 0 || term_cursor_row >= max_rows) return;
    if (term_cursor_col < 0 || term_cursor_col >= max_cols) return;

    if (on) {
        // Reverse video: swap fg/bg, write a space block
        putchar_at(term_cursor_col, term_cursor_row, term_bg, term_fg, ' ');
    } else {
        // Restore: draw a space with normal colors
        putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, ' ');
    }
}

// Advance cursor one column; wrap to next line.
static void console_advance(void)
{
    int max_cols = (int)(Pos.XResolution / font->width);
    int max_rows = (int)(Pos.YResolution / font->height);

    term_cursor_col++;
    if (term_cursor_col >= max_cols) {
        term_cursor_col = 0;
        term_cursor_row++;
        if (term_cursor_row >= max_rows)
            console_scroll();
    }
}

// Move cursor left by n (clamped).
static void console_cursor_left(int n)
{
    if (n <= 0) n = 1;
    term_cursor_col -= n;
    if (term_cursor_col < 0) term_cursor_col = 0;
}

// Move cursor right by n (clamped).
static void console_cursor_right(int n)
{
    if (n <= 0) n = 1;
    int max_cols = (int)(Pos.XResolution / font->width);
    term_cursor_col += n;
    if (term_cursor_col >= max_cols) term_cursor_col = max_cols - 1;
}

// Commit a normal (non-escape) character to the framebuffer.
static void console_put_normal(char c)
{
    if (c == '\n') {
        if (term_blink_on) console_draw_blink(false);
        term_cursor_col = 0;
        term_cursor_row++;
        int max_rows = (int)(Pos.YResolution / font->height);
        if (term_cursor_row >= max_rows)
            console_scroll();
        return;
    }

    if (c == '\r') {
        if (term_blink_on) console_draw_blink(false);
        term_cursor_col = 0;
        return;
    }

    if (c == '\b' || c == 0x7F) {
        if (term_blink_on) console_draw_blink(false);
        if (term_cursor_col > 0) term_cursor_col--;
        // Erase the character at the new position
        putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, ' ');
        return;
    }

    if (c == '\t') {
        if (term_blink_on) console_draw_blink(false);
        int max_cols = (int)(Pos.XResolution / font->width);
        term_cursor_col = (term_cursor_col + 8) & ~7;
        if (term_cursor_col >= max_cols) term_cursor_col = max_cols - 1;
        return;
    }

    // Printable characters (space and above) — skip ASCII controls
    if ((unsigned char)c >= ' ') {
        if (term_blink_on) console_draw_blink(false);
        putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, c);
        console_advance();
    }
}

// ═══════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════

// Feed one character through the VT100 state machine + render.
void console_putchar(char c)
{
    if (!term_initialized) return;

    switch (cs) {
    case CSI_NORMAL:
        if (c == '\x1b') {
            cs = CSI_ESC;
            return;
        }
        console_put_normal(c);
        break;

    case CSI_ESC:
        if (c == '[') {
            cs = CSI_BRACKET;
            cs_param = 0;
            cs_qmark = false;
        } else {
            // Unrecognized ESC sequence - discard, return to normal
            cs = CSI_NORMAL;
        }
        break;

    case CSI_BRACKET:
        if (c == '?') {
            cs = CSI_QMARK;
            cs_qmark = true;
        } else if (c == ';') {
            // Semicolon after empty param (e.g. ESC [ ; n D)
            // Stay in CSI state, reset param accumulator.
            cs_param = 0;
        } else if (c >= '0' && c <= '9') {
            cs = CSI_PARAM;
            cs_param = c - '0';
        } else {
            // Terminal character with no parameter
            switch (c) {
            case 'D':  // Cursor Left (default 1)
                if (term_blink_on) console_draw_blink(false);
                console_cursor_left(1);
                break;
            case 'C':  // Cursor Right (default 1)
                if (term_blink_on) console_draw_blink(false);
                console_cursor_right(1);
                break;
            case 'K':  // Erase to end of line
                if (term_blink_on) console_draw_blink(false);
                console_clear_to_eol();
                break;
            default:
                break;
            }
            cs = CSI_NORMAL;
        }
        break;

    case CSI_PARAM:
        if (c >= '0' && c <= '9') {
            cs_param = cs_param * 10 + (c - '0');
        } else if (c == ';') {
            // Semicolon separates params (e.g. ESC [ 1 ; 3 2 m)
            // Reset accumulator, stay in CSI_PARAM for next param.
            cs_param = 0;
        } else {
            // Terminal character with parameter
            switch (c) {
            case 'D':  // Cursor Left n
                if (term_blink_on) console_draw_blink(false);
                console_cursor_left(cs_param);
                break;
            case 'C':  // Cursor Right n
                if (term_blink_on) console_draw_blink(false);
                console_cursor_right(cs_param);
                break;
            case 'K':  // Erase to end of line (param ignored)
                if (term_blink_on) console_draw_blink(false);
                console_clear_to_eol();
                break;
            default:
                break;
            }
            cs = CSI_NORMAL;
        }
        break;

    case CSI_QMARK:
        if (c >= '0' && c <= '9') {
            cs_param = cs_param * 10 + (c - '0');
            // Stay in CSI_QMARK — do NOT transition to CSI_PARAM
        } else {
            // Terminal character with ? prefix (private mode)
            switch (c) {
            case 'h':  // DECSET - enable mode
                if (cs_param == 25) {
                    // Show cursor
                    term_cursor_visible = true;
                    term_blink_on = false;
                    term_blink_counter = 0;
                }
                break;
            case 'l':  // DECRST - disable mode
                if (cs_param == 25) {
                    // Hide cursor
                    if (term_blink_on) console_draw_blink(false);
                    term_cursor_visible = false;
                    term_blink_on = false;
                }
                break;
            default:
                break;
            }
            cs = CSI_NORMAL;
        }
        break;
    }
}

// ═══════════════════════════════════════════════════════
//  Cursor blink - called from PIT handler (100 Hz)
// ═══════════════════════════════════════════════════════

void console_blink_tick(void)
{
    if (!term_initialized) return;
    if (!term_cursor_visible) return;

    term_blink_counter++;
    if (term_blink_counter >= 80) {  // 800ms @ 100Hz
        term_blink_counter = 0;
        term_blink_on = !term_blink_on;
        console_draw_blink(term_blink_on);
    }
}

// ═══════════════════════════════════════════════════════
//  Init
// ═══════════════════════════════════════════════════════

void console_init(void)
{
    // Start terminal cursor one row below the last kernel printk output.
    // Called at the very end of kernel_main(), so Pos.YPosition is stable.
    term_cursor_row = Pos.YPosition + 1;
    int max_rows = (int)(Pos.YResolution / font->height);
    if (term_cursor_row >= max_rows) term_cursor_row = max_rows - 1;
    term_cursor_col = 0;
    term_cursor_visible = true;
    term_blink_on = false;
    term_blink_counter = 0;
    term_fg = WHITE;
    term_bg = BLACK;
    term_initialized = true;
    cs = CSI_NORMAL;
    cs_param = 0;
    cs_qmark = false;
}
