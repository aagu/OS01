/*
 * test/cases/test_terminal_core.c — VT100 screen-model unit tests.
 *
 * Compiles the REAL user/terminal_core.c (pure logic).  Exercises:
 * glyph placement, cursor movement, clear ops, scrolling,
 * and the alt-screen (?1049h/?1049l) dual-buffer protocol.
 */
#include "test_framework.h"
#include <terminal_core.h>
#include <string.h>
#include <stdlib.h>

#define R 25
#define C 80

static term_core_t core;

/* Flat cell access into a [rows * cols] buffer, using core.cols as stride. */
static term_cell_t *cell(term_cell_t *buf, int r, int c)
{
    return &buf[r * core.cols + c];
}

static void reset(void)
{
    term_core_init(&core, R, C);
}

TEST_FUNC(test_init_blank) {
    reset();
    assert_eq(0, cell(core.main_buf, 0, 0)->glyph);
    assert_false(core.alt_active);
    assert_true(core.cursor_visible);
}

TEST_FUNC(test_write_glyph) {
    reset();
    term_core_input(&core, 'A');
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);
    assert_true(term_core_is_dirty(&core, 0, 0));
    assert_eq(1, core.col);          /* cursor advanced */
}

TEST_FUNC(test_newline_cursor) {
    reset();
    term_core_input(&core, 'a');
    term_core_input(&core, '\n');
    assert_eq(0, core.col);
    assert_eq(1, core.row);
    term_core_input(&core, 'b');
    assert_eq('b', cell(core.main_buf, 1, 0)->glyph);
}

TEST_FUNC(test_csi_cursor_move) {
    reset();
    term_core_input(&core, 'h');
    /* \e[5C → right 5 */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '5'); term_core_input(&core, 'C');
    assert_eq(6, core.col);
    /* \e[2B → down 2 */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '2'); term_core_input(&core, 'B');
    assert_eq(2, core.row);
    /* \e[D → left 1 */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, 'D');
    assert_eq(5, core.col);
}

TEST_FUNC(test_clear_line) {
    reset();
    term_core_input(&core, 'x'); term_core_input(&core, 'y');
    assert_eq('x', cell(core.main_buf, 0, 0)->glyph);
    /* \e[K clears from cursor to EOL (cursor at col 2) */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, 'K');
    assert_eq(0, cell(core.main_buf, 0, 2)->glyph);
    assert_eq('x', cell(core.main_buf, 0, 0)->glyph);  /* before cursor kept */
}

TEST_FUNC(test_clear_screen) {
    reset();
    term_core_input(&core, 'z');
    assert_eq('z', cell(core.main_buf, 0, 0)->glyph);
    /* \e[2J clears everything + home */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '2'); term_core_input(&core, 'J');
    assert_eq(0, cell(core.main_buf, 0, 0)->glyph);
    assert_eq(0, core.row);
    assert_eq(0, core.col);
}

TEST_FUNC(test_scroll) {
    reset();
    /* write R-1 full lines (cursor on bottom row, no scroll yet) */
    for (int i = 0; i < R - 1; i++) {
        term_core_input(&core, 'L');
        term_core_input(&core, '\n');
    }
    term_core_input(&core, 'X');     /* bottom row (R-1) */
    term_core_input(&core, '\n');    /* wrap past bottom → scroll up */
    assert_eq('X', cell(core.main_buf, R - 2, 0)->glyph);   /* X shifted up one */
    assert_eq(0, cell(core.main_buf, R - 1, 0)->glyph);     /* new bottom blank */
    assert_eq(R - 1, core.row);                              /* cursor on bottom */
}

TEST_FUNC(test_alt_screen_protocol) {
    reset();
    term_core_input(&core, 'A');
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);

    /* enter alt screen: \e[?1049h — blank, main preserved */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '?'); term_core_input(&core, '1');
    term_core_input(&core, '0'); term_core_input(&core, '4');
    term_core_input(&core, '9'); term_core_input(&core, 'h');
    assert_true(core.alt_active);
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);        /* main untouched */

    /* draw in alt */
    term_core_input(&core, 'B');
    assert_eq('B', cell(core.alt_buf, 0, 0)->glyph);
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);

    /* exit alt: \e[?1049l — main restored (marked dirty for redraw) */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '?'); term_core_input(&core, '1');
    term_core_input(&core, '0'); term_core_input(&core, '4');
    term_core_input(&core, '9'); term_core_input(&core, 'l');
    assert_false(core.alt_active);
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);
    assert_true(term_core_is_dirty(&core, 0, 0));   /* full redraw queued */
    assert_eq('B', cell(core.alt_buf, 0, 0)->glyph); /* alt keeps B */
}

TEST_FUNC(test_large_resolution_no_clamp) {
    /* Regression: the cell buffer used to be hardcoded 30x100, so a
     * 1440x900 framebuffer (180x56 cells @ 8x16) was clamped down and the
     * terminal only cleared/rendered the top-left 800x480 corner. */
    term_core_init(&core, 56, 180);   /* 1440x900 @ 8x16 */
    assert_eq(56, core.rows);
    assert_eq(180, core.cols);

    /* Bottom row must be reachable (would be out of bounds / scrolled at
     * the old 30-row clamp). */
    for (int i = 0; i < 55; i++)
        term_core_input(&core, '\n');
    term_core_input(&core, 'X');
    assert_eq('X', cell(core.main_buf, 55, 0)->glyph);
    assert_true(term_core_is_dirty(&core, 55, 0));
}

TEST_FUNC(test_reinit_resizes) {
    /* Re-init on an already-inited core frees the old buffers and allocates
     * fresh ones at the new dimensions (idempotent, no stale clamp). */
    term_core_init(&core, 10, 20);
    term_core_input(&core, 'A');
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);

    term_core_init(&core, 30, 40);
    assert_eq(30, core.rows);
    assert_eq(40, core.cols);
    assert_eq(0, cell(core.main_buf, 0, 0)->glyph);   /* fresh blank grid */
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_init_blank),
    TEST_ENTRY(test_write_glyph),
    TEST_ENTRY(test_newline_cursor),
    TEST_ENTRY(test_csi_cursor_move),
    TEST_ENTRY(test_clear_line),
    TEST_ENTRY(test_clear_screen),
    TEST_ENTRY(test_scroll),
    TEST_ENTRY(test_alt_screen_protocol),
    TEST_ENTRY(test_large_resolution_no_clamp),
    TEST_ENTRY(test_reinit_resizes),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    term_core_free(&core);
    return __test_stats.failed > 0 ? 1 : 0;
}
