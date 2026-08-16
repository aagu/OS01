// user/terminal_core.c — VT100 screen model + CSI parser, pure logic.
//
// Extracted from user/terminal.c so the terminal state machine is
// host-testable.  Behaviour must match the original parser:
//   CSI A/B/C/D (cursor), K (clear line), J (clear screen), H (home),
//   ?25h/?25l (cursor visibility) + \n \r \b \t and printable glyphs.
// NEW: \e[?1049h / \e[?1049l alternate-screen protocol (dual buffer).
//
// Screen buffers are dynamically allocated in term_core_init() to match the
// real framebuffer dimensions; all access is via flat index (row * cols + col).

#include "terminal_core.h"
#include <string.h>
#include <stdlib.h>

enum { CS_NORMAL = 0, CS_ESC = 1, CS_CSI = 2 };

// Flat index into a [rows * cols] buffer.
static inline int cell_idx(const term_core_t *t, int r, int c)
{
    return r * t->cols + c;
}

// True if the core's buffers were allocated and are usable.
static inline bool has_buffers(const term_core_t *t)
{
    return t->main_buf && t->alt_buf && t->dirty;
}

static void blank_cell(term_core_t *t, term_cell_t *buf, int r, int c)
{
    if (!has_buffers(t)) return;
    int i = cell_idx(t, r, c);
    if (buf[i].glyph != 0) {
        buf[i].glyph = 0;
        t->dirty[i] = true;
    }
}

static void set_glyph(term_core_t *t, int col, int row, uint8_t g)
{
    if (col < 0 || col >= t->cols || row < 0 || row >= t->rows)
        return;
    if (!has_buffers(t)) return;
    term_cell_t *buf = t->alt_active ? t->alt_buf : t->main_buf;
    int i = cell_idx(t, row, col);
    if (buf[i].glyph != g) {
        buf[i].glyph = g;
        t->dirty[i] = true;
    }
}

static void scroll_active(term_core_t *t)
{
    term_cell_t *buf = t->alt_active ? t->alt_buf : t->main_buf;
    if (has_buffers(t)) {
        size_t row_elems = (size_t)t->cols;
        // Move rows 1..rows-1 up by one row, then clear the new bottom row.
        memmove(buf, buf + row_elems,
                (size_t)(t->rows - 1) * row_elems * sizeof(term_cell_t));
        for (int c = 0; c < t->cols; c++)
            buf[cell_idx(t, t->rows - 1, c)].glyph = 0;
        // Whole screen is now different — mark dirty.
        for (int i = 0; i < t->rows * t->cols; i++)
            t->dirty[i] = true;
    }
    t->row = t->rows - 1;
}

void term_core_init(term_core_t *t, int rows, int cols)
{
    term_core_free(t);   // idempotent: release any buffers from a prior init
    memset(t, 0, sizeof(*t));
    t->rows = rows;
    t->cols = cols;
    t->cursor_visible = true;

    size_t n = (size_t)rows * (size_t)cols;
    t->main_buf = malloc(n * sizeof(term_cell_t));
    t->alt_buf  = malloc(n * sizeof(term_cell_t));
    t->dirty    = malloc(n * sizeof(bool));
    if (t->main_buf) memset(t->main_buf, 0, n * sizeof(term_cell_t));
    if (t->alt_buf)  memset(t->alt_buf,  0, n * sizeof(term_cell_t));
    if (t->dirty)    memset(t->dirty,    0, n * sizeof(bool));
}

void term_core_free(term_core_t *t)
{
    free(t->main_buf); t->main_buf = NULL;
    free(t->alt_buf);  t->alt_buf  = NULL;
    free(t->dirty);    t->dirty    = NULL;
}

term_cell_t *term_core_screen(term_core_t *t)
{
    return t->alt_active ? t->alt_buf : t->main_buf;
}

bool term_core_is_dirty(term_core_t *t, int row, int col)
{
    if (!has_buffers(t)) return false;
    return t->dirty[cell_idx(t, row, col)];
}

void term_core_clear_dirty(term_core_t *t, int row, int col)
{
    if (!has_buffers(t)) return;
    t->dirty[cell_idx(t, row, col)] = false;
}

void term_core_mark_all_dirty(term_core_t *t)
{
    if (!has_buffers(t)) return;
    for (int i = 0; i < t->rows * t->cols; i++)
        t->dirty[i] = true;
}

static void clear_line(term_core_t *t, int from, int to)
{
    for (int x = from; x < to; x++)
        blank_cell(t, term_core_screen(t), t->row, x);
}

static void clear_screen(term_core_t *t)
{
    term_cell_t *buf = term_core_screen(t);
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
