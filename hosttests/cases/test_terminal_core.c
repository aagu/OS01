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

TEST_FUNC(test_clear_display_from_cursor) {
    reset();
    /* Write 'a', 'b', 'c' on row 0, then 'd' on row 1 */
    term_core_input(&core, 'a');
    term_core_input(&core, 'b');
    term_core_input(&core, 'c');
    term_core_input(&core, '\n');
    term_core_input(&core, 'd');

    /* Move cursor to row 0, col 1 (over 'b') */
    core.row = 0;
    core.col = 1;

    /* \e[J clears from cursor to end of screen */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, 'J');

    assert_eq('a', cell(core.main_buf, 0, 0)->glyph); /* before cursor kept */
    assert_eq(0, cell(core.main_buf, 0, 1)->glyph);   /* cursor position cleared */
    assert_eq(0, cell(core.main_buf, 0, 2)->glyph);   /* after cursor on row 0 cleared */
    assert_eq(0, cell(core.main_buf, 1, 0)->glyph);   /* subsequent row cleared */
    assert_eq(0, core.row);                            /* cursor position preserved */
    assert_eq(1, core.col);
}

TEST_FUNC(test_backspace_erase) {
    reset();
    /* User types "abc" */
    term_core_input(&core, 'a');
    term_core_input(&core, 'b');
    term_core_input(&core, 'c');
    assert_eq(3, core.col);

    /* Backspace: ash moves cursor left (\b) then clears till end of screen (\e[J) */
    term_core_input(&core, '\b');
    assert_eq(2, core.col);
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, 'J');

    assert_eq('a', cell(core.main_buf, 0, 0)->glyph);
    assert_eq('b', cell(core.main_buf, 0, 1)->glyph);
    assert_eq(0, cell(core.main_buf, 0, 2)->glyph);   /* 'c' must be erased */
    assert_eq(2, core.col);
}

TEST_FUNC(test_clear_display_to_cursor) {
    reset();
    /* Write 'a', 'b', 'c' on row 0, then 'd', 'e' on row 1 */
    term_core_input(&core, 'a');
    term_core_input(&core, 'b');
    term_core_input(&core, 'c');
    term_core_input(&core, '\n');
    term_core_input(&core, 'd');
    term_core_input(&core, 'e');

    /* Move cursor to row 1, col 0 (over 'd') */
    core.row = 1;
    core.col = 0;

    /* \e[1J clears from start of screen to cursor */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '1'); term_core_input(&core, 'J');

    assert_eq(0, cell(core.main_buf, 0, 0)->glyph);   /* earlier row cleared */
    assert_eq(0, cell(core.main_buf, 0, 1)->glyph);
    assert_eq(0, cell(core.main_buf, 0, 2)->glyph);
    assert_eq(0, cell(core.main_buf, 1, 0)->glyph);   /* cursor position cleared */
    assert_eq('e', cell(core.main_buf, 1, 1)->glyph); /* after cursor kept */
    assert_eq(1, core.row);
    assert_eq(0, core.col);
}

TEST_FUNC(test_busybox_clear_command) {
    reset();
    /* Write multi-line text across several rows */
    term_core_input(&core, 'A');
    term_core_input(&core, '\n');
    term_core_input(&core, 'B');
    term_core_input(&core, '\n');
    term_core_input(&core, 'C');

    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);
    assert_eq('B', cell(core.main_buf, 1, 0)->glyph);
    assert_eq('C', cell(core.main_buf, 2, 0)->glyph);

    /* BusyBox clear applet outputs: \e[H\e[J */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, 'H');
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, 'J');

    /* Cursor returned to top-left (0, 0) */
    assert_eq(0, core.row);
    assert_eq(0, core.col);

    /* All rows must be cleared */
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < core.cols; c++) {
            assert_eq(0, cell(core.main_buf, r, c)->glyph);
        }
    }
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
    assert_eq(1, core.scroll_lines_pending);
    assert_true(term_core_is_dirty(&core, R - 1, 0));
}

TEST_FUNC(test_alt_screen_protocol) {
    reset();
    term_core_input(&core, 'A');
    term_core_input(&core, '\n');
    term_core_input(&core, 'X');
    term_core_input(&core, 'Y');
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);
    assert_eq('X', cell(core.main_buf, 1, 0)->glyph);
    assert_eq('Y', cell(core.main_buf, 1, 1)->glyph);
    assert_eq(1, core.row);
    assert_eq(2, core.col);

    /* enter alt screen: \e[?1049h — blank, main preserved, cursor saved */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '?'); term_core_input(&core, '1');
    term_core_input(&core, '0'); term_core_input(&core, '4');
    term_core_input(&core, '9'); term_core_input(&core, 'h');
    assert_true(core.alt_active);
    assert_eq(0, core.row);
    assert_eq(0, core.col);
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);        /* main untouched */

    /* draw in alt */
    term_core_input(&core, 'B');
    term_core_input(&core, '\n');
    assert_eq('B', cell(core.alt_buf, 0, 0)->glyph);
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);
    assert_eq(1, core.row);
    assert_eq(0, core.col);

    /* exit alt: \e[?1049l — main restored and cursor restored to (1, 2) */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '?'); term_core_input(&core, '1');
    term_core_input(&core, '0'); term_core_input(&core, '4');
    term_core_input(&core, '9'); term_core_input(&core, 'l');
    assert_false(core.alt_active);
    assert_eq('A', cell(core.main_buf, 0, 0)->glyph);
    assert_true(term_core_is_dirty(&core, 0, 0));   /* full redraw queued */
    assert_eq('B', cell(core.alt_buf, 0, 0)->glyph); /* alt keeps B */
    assert_eq(1, core.row);                          /* cursor preserved! */
    assert_eq(2, core.col);                          /* cursor preserved! */
}

TEST_FUNC(test_cursor_save_restore_dec) {
    reset();
    core.row = 5;
    core.col = 12;
    /* \e7 (DECSC: Save Cursor) */
    term_core_input(&core, 0x1b);
    term_core_input(&core, '7');

    /* Move cursor away */
    core.row = 1;
    core.col = 0;

    /* \e8 (DECRC: Restore Cursor) */
    term_core_input(&core, 0x1b);
    term_core_input(&core, '8');
    assert_eq(5, core.row);
    assert_eq(12, core.col);

    /* \e[s (ANSI.SYS Save Cursor) */
    core.row = 8;
    core.col = 20;
    term_core_input(&core, 0x1b);
    term_core_input(&core, '[');
    term_core_input(&core, 's');

    core.row = 0;
    core.col = 0;

    /* \e[u (ANSI.SYS Restore Cursor) */
    term_core_input(&core, 0x1b);
    term_core_input(&core, '[');
    term_core_input(&core, 'u');
    assert_eq(8, core.row);
    assert_eq(20, core.col);
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
    TEST_ENTRY(test_clear_display_from_cursor),
    TEST_ENTRY(test_backspace_erase),
    TEST_ENTRY(test_clear_display_to_cursor),
    TEST_ENTRY(test_busybox_clear_command),
    TEST_ENTRY(test_scroll),
    TEST_ENTRY(test_alt_screen_protocol),
    TEST_ENTRY(test_cursor_save_restore_dec),
    TEST_ENTRY(test_large_resolution_no_clamp),
    TEST_ENTRY(test_reinit_resizes),
TEST_LIST_END

int main() {
    printf("=== Test Runner ===\n");
    int __table_size = sizeof(__test_table) / sizeof(__test_table[0]);
    for (int __i = 0; __i < __table_size; __i++) {
        printf("\n--- %s ---\n", __test_table[__i].name);
        __test_table[__i].fn();
    }
    int failed = __test_stats.failed;
    TEST_RESULTS();
    term_core_free(&core);
    return failed > 0 ? 1 : 0;
}
