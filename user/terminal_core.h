#ifndef _TERMINAL_CORE_H
#define _TERMINAL_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Terminal core — VT100 screen model + CSI parser, pure logic.
// No fb/OS/IO dependencies — host-testable (test/cases/test_terminal_core.c).
//
// terminal.elf drives: feed bytes → term_core_input() → render dirty cells
// via term_core_is_dirty()/term_core_clear_dirty() + term_core_screen().
//
// Screen buffers are allocated dynamically in term_core_init() to match the
// actual framebuffer size (rows × cols), so any resolution works — there is
// no hardcoded cell cap.  Buffers are laid out flat (row * cols + col).

typedef struct {
    uint8_t glyph;     // 0 = blank
} term_cell_t;

typedef struct {
    term_cell_t *main_buf;   // [rows * cols]
    term_cell_t *alt_buf;    // [rows * cols]
    bool *dirty;             // [rows * cols] — needs redraw
    int  col, row;
    bool alt_active;                    // \e[?1049h/l
    bool cursor_visible;                // \e[?25h/l
    int  rows, cols;                    // usable dimensions

    // CSI parser state
    int  csi_state;                     // 0=normal 1=esc 2=csi
    int  csi_param;
    bool csi_qmark;
} term_core_t;

// Initialize the core for a rows × cols screen.  Allocates the internal
// buffers; on re-init of an already-initialized core it releases the prior
// buffers first (so it is idempotent).  Call term_core_free() to release.
void term_core_init(term_core_t *t, int rows, int cols);

// Release the buffers allocated by term_core_init().  Safe to call more than
// once, and on a zero-initialized (never-inited) core.
void term_core_free(term_core_t *t);

// Feed one output byte through the VT100 parser.  Returns true if any
// cell changed or the cursor moved (caller may want to flush).
bool term_core_input(term_core_t *t, uint8_t c);

// Pointer to the ACTIVE buffer (main or alt), flat [rows * cols].
term_cell_t *term_core_screen(term_core_t *t);

// Dirty-cell queries for the renderer.
bool term_core_is_dirty(term_core_t *t, int row, int col);
void term_core_clear_dirty(term_core_t *t, int row, int col);
void term_core_mark_all_dirty(term_core_t *t);

#endif
