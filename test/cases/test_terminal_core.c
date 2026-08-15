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

static void reset(void)
{
    term_core_init(&core, R, C);
}

TEST_FUNC(test_init_blank) {
    reset();
    assert_eq(0, core.main_buf[0][0].glyph);
    assert_false(core.alt_active);
    assert_true(core.cursor_visible);
}

TEST_FUNC(test_write_glyph) {
    reset();
    term_core_input(&core, 'A');
    assert_eq('A', core.main_buf[0][0].glyph);
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
    assert_eq('b', core.main_buf[1][0].glyph);
}

TEST_FUNC(test_csi_cursor_move) {
    reset();
    term_core_input(&core, "hel\0"[0] ? 'h' : 'h');  /* keep simple */
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
    assert_eq('x', core.main_buf[0][0].glyph);
    /* \e[K clears from cursor to EOL (cursor at col 2) */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, 'K');
    assert_eq(0, core.main_buf[0][2].glyph);
    assert_eq('x', core.main_buf[0][0].glyph);  /* before cursor kept */
}

TEST_FUNC(test_clear_screen) {
    reset();
    term_core_input(&core, 'z');
    assert_eq('z', core.main_buf[0][0].glyph);
    /* \e[2J clears everything + home */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '2'); term_core_input(&core, 'J');
    assert_eq(0, core.main_buf[0][0].glyph);
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
    assert_eq('X', core.main_buf[R - 2][0].glyph);   /* X shifted up one */
    assert_eq(0, core.main_buf[R - 1][0].glyph);     /* new bottom blank */
    assert_eq(R - 1, core.row);                      /* cursor on bottom */
}

TEST_FUNC(test_alt_screen_protocol) {
    reset();
    term_core_input(&core, 'A');
    assert_eq('A', core.main_buf[0][0].glyph);

    /* enter alt screen: \e[?1049h — blank, main preserved */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '?'); term_core_input(&core, '1');
    term_core_input(&core, '0'); term_core_input(&core, '4');
    term_core_input(&core, '9'); term_core_input(&core, 'h');
    assert_true(core.alt_active);
    assert_eq(0, core.main_buf[0][0].glyph == 0 ? 1 : 0); /* main untouched */
    assert_eq('A', core.main_buf[0][0].glyph);

    /* draw in alt */
    term_core_input(&core, 'B');
    assert_eq('B', core.alt_buf[0][0].glyph);
    assert_eq(0, core.main_buf[0][0].glyph == 'A' ? 0 : 1);

    /* exit alt: \e[?1049l — main restored (marked dirty for redraw) */
    term_core_input(&core, 0x1b); term_core_input(&core, '[');
    term_core_input(&core, '?'); term_core_input(&core, '1');
    term_core_input(&core, '0'); term_core_input(&core, '4');
    term_core_input(&core, '9'); term_core_input(&core, 'l');
    assert_false(core.alt_active);
    assert_eq('A', core.main_buf[0][0].glyph);
    assert_true(term_core_is_dirty(&core, 0, 0));   /* full redraw queued */
    assert_eq(0, core.alt_buf[0][0].glyph == 'B' ? 0 : 1); /* alt keeps B */
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
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
