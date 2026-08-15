// user/terminal_core.c — VT100 screen model + CSI parser, pure logic.
//
// Extracted from user/terminal.c so the terminal state machine is
// host-testable.  Behaviour must match the original parser:
//   CSI A/B/C/D (cursor), K (clear line), J (clear screen), H (home),
//   ?25h/?25l (cursor visibility) + \n \r \b \t and printable glyphs.
// NEW: \e[?1049h / \e[?1049l alternate-screen protocol (dual buffer).

#include "terminal_core.h"
#include <string.h>

enum { CS_NORMAL = 0, CS_ESC = 1, CS_CSI = 2 };

static void blank_cell(term_core_t *t, term_cell_t (*buf)[TERM_COLS], int r, int c)
{
    if (buf[r][c].glyph != 0) {
        buf[r][c].glyph = 0;
        t->dirty[r][c] = true;
    }
}

static void set_glyph(term_core_t *t, int col, int row, uint8_t g)
{
    if (col < 0 || col >= t->cols || row < 0 || row >= t->rows)
        return;
    term_cell_t (*buf)[TERM_COLS] = t->alt_active ? t->alt_buf : t->main_buf;
    if (buf[row][col].glyph != g) {
        buf[row][col].glyph = g;
        t->dirty[row][col] = true;
    }
}

static void scroll_active(term_core_t *t)
{
    term_cell_t (*buf)[TERM_COLS] = t->alt_active ? t->alt_buf : t->main_buf;
    memmove(buf[0], buf[1], (t->rows - 1) * sizeof(term_cell_t) * TERM_COLS);
    for (int c = 0; c < TERM_COLS; c++)
        buf[t->rows - 1][c].glyph = 0;
    // whole screen is now different — mark dirty
    for (int r = 0; r < t->rows; r++)
        for (int c = 0; c < TERM_COLS; c++)
            t->dirty[r][c] = true;
    t->row = t->rows - 1;
}

void term_core_init(term_core_t *t, int rows, int cols)
{
    memset(t, 0, sizeof(*t));
    if (rows > TERM_ROWS) rows = TERM_ROWS;
    if (cols > TERM_COLS) cols = TERM_COLS;
    t->rows = rows;
    t->cols = cols;
    t->cursor_visible = true;
}

term_cell_t (*term_core_screen(term_core_t *t))[TERM_COLS]
{
    return t->alt_active ? t->alt_buf : t->main_buf;
}

bool term_core_is_dirty(term_core_t *t, int row, int col)
{
    return t->dirty[row][col];
}

void term_core_clear_dirty(term_core_t *t, int row, int col)
{
    t->dirty[row][col] = false;
}

void term_core_mark_all_dirty(term_core_t *t)
{
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++)
            t->dirty[r][c] = true;
}

static void clear_line(term_core_t *t, int from, int to)
{
    for (int x = from; x < to; x++)
        blank_cell(t, term_core_screen(t), t->row, x);
}

static void clear_screen(term_core_t *t)
{
    term_cell_t (*buf)[TERM_COLS] = term_core_screen(t);
    for (int r = 0; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++)
            blank_cell(t, buf, r, c);
}

bool term_core_input(term_core_t *t, uint8_t c)
{
    bool changed = false;

    if (t->csi_state == CS_NORMAL && c == 0x1b) {
        t->csi_state = CS_ESC;
        return false;
    }
    if (t->csi_state == CS_ESC) {
        if (c == '[') { t->csi_state = CS_CSI; t->csi_param = 0; t->csi_qmark = false; return false; }
        t->csi_state = CS_NORMAL;
        return false;
    }
    if (t->csi_state == CS_CSI) {
        if (c == '?') { t->csi_qmark = true; return false; }
        if (c >= '0' && c <= '9') { t->csi_param = t->csi_param * 10 + (c - '0'); return false; }
        int p = t->csi_param ? t->csi_param : 1;
        switch (c) {
        case 'A': t->row -= p; if (t->row < 0) t->row = 0; break;
        case 'B': t->row += p; if (t->row >= t->rows) t->row = t->rows - 1; break;
        case 'C': t->col += p; if (t->col >= t->cols) t->col = t->cols - 1; break;
        case 'D': t->col -= p; if (t->col < 0) t->col = 0; break;
        case 'K':
            if (t->csi_param == 0)      clear_line(t, t->col, t->cols);
            else if (t->csi_param == 1) clear_line(t, 0, t->col + 1);
            else                        clear_line(t, 0, t->cols);
            changed = true;
            break;
        case 'J':
            if (t->csi_param == 2) {
                clear_screen(t);
                t->row = 0; t->col = 0;
                changed = true;
            }
            break;
        case 'H': t->row = 0; t->col = 0; break;
        case 'h':
            if (t->csi_qmark && t->csi_param == 25) t->cursor_visible = true;
            if (t->csi_qmark && t->csi_param == 1049) {
                t->alt_active = true;
                clear_screen(t);            // alt starts blank
                t->row = 0; t->col = 0;
                changed = true;
            }
            break;
        case 'l':
            if (t->csi_qmark && t->csi_param == 25) t->cursor_visible = false;
            if (t->csi_qmark && t->csi_param == 1049) {
                t->alt_active = false;      // back to main buffer
                term_core_mark_all_dirty(t); // full redraw of main
                t->row = 0; t->col = 0;
                changed = true;
            }
            break;
        }
        t->csi_state = CS_NORMAL;
        return changed;
    }

    // Normal character
    switch (c) {
    case '\n': t->col = 0; t->row++; changed = true; break;
    case '\r': t->col = 0; break;
    case '\b': case 0x7f: if (t->col > 0) t->col--; break;
    case '\t': t->col = (t->col + 8) & ~7; break;
    default:
        if ((unsigned char)c >= ' ') {
            set_glyph(t, t->col, t->row, c);
            t->col++;
            changed = true;
        }
        break;
    }

    if (t->col >= t->cols) { t->col = 0; t->row++; }
    if (t->row >= t->rows) scroll_active(t);
    return changed;
}
