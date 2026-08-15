/*
 * test/cases/test_tetris_logic.c — tetris game logic unit tests.
 *
 * Compiles the REAL user/tetris_logic.c (pure C).  Exercises: spawn,
 * wall/floor/occupied collision, movement, rotation + wall kicks,
 * locking, row clearing, game over.
 *
 * NOTE on shape geometry: pieces live in rows 1-2 / cols 1-2 of the
 * 4x4 box (not row 0), so a T piece occupies board rows y+1..y+2 and
 * its lowest cell is y+2.  Tests below account for that.
 */
#include "test_framework.h"
#include <tetris_logic.h>
#include <string.h>
#include <stdlib.h>

static tetris_board_t board;
static tetris_piece_t piece;

static void reset_board(void)
{
    memset(&board, 0, sizeof(board));
}

TEST_FUNC(test_spawn_ok) {
    reset_board();
    assert_eq(0, tetris_spawn(&board, &piece, 0));   /* I */
    assert_eq(3, piece.x);                            /* 10/2-2 */
    assert_eq(0, piece.y);
}

TEST_FUNC(test_wall_left_blocked) {
    reset_board();
    tetris_spawn(&board, &piece, 2);                  /* T */
    /* T leftmost cell is x+0 → can move until x=0 */
    int moves = 0;
    while (tetris_move(&board, &piece, -1, 0) == 0) moves++;
    assert_eq(3, moves);                              /* 3→2→1→0 */
    assert_eq(0, piece.x);
    assert_eq(-1, tetris_move(&board, &piece, -1, 0)); /* wall blocks */
}

TEST_FUNC(test_wall_right_blocked) {
    reset_board();
    tetris_spawn(&board, &piece, 2);                  /* T at x=3 */
    /* T rightmost cell is x+2 → max x = 10-1-2 = 7 */
    int moves = 0;
    while (tetris_move(&board, &piece, 1, 0) == 0) moves++;
    assert_eq(4, moves);                              /* 3→...→7 */
    assert_eq(7, piece.x);
    assert_eq(-1, tetris_move(&board, &piece, 1, 0));
}

TEST_FUNC(test_floor_blocked) {
    reset_board();
    tetris_spawn(&board, &piece, 2);                  /* T */
    /* T lowest cell is y+2 → max y = 20-1-2 = 17 */
    int moves = 0;
    while (tetris_move(&board, &piece, 0, 1) == 0) moves++;
    assert_eq(17, moves);                             /* y 0→17 */
    assert_eq(17, piece.y);
    assert_eq(-1, tetris_move(&board, &piece, 0, 1)); /* floor */
}

TEST_FUNC(test_rotate_t) {
    reset_board();
    tetris_spawn(&board, &piece, 2);                  /* T rot0 */
    assert_eq(0, tetris_rotate(&board, &piece));
    assert_eq(1, piece.rot);
    assert_eq(0, tetris_rotate(&board, &piece));
    assert_eq(2, piece.rot);
}

TEST_FUNC(test_rotate_blocked_by_floor) {
    reset_board();
    tetris_spawn(&board, &piece, 0);                  /* I horizontal (row y+1) */
    /* drop to bottom: lowest cell y+1=19 → y=18 */
    while (tetris_move(&board, &piece, 0, 1) == 0) {}
    assert_eq(18, piece.y);
    /* rotate to vertical: needs rows y..y+3 = 18..21 → floor blocks */
    assert_eq(-1, tetris_rotate(&board, &piece));
    assert_eq(0, piece.rot);                          /* unchanged */
    /* move up one, rotate ok: rows 17..20? no — 17..20 needs 20 → blocked.
       move up two: rows 16..19 → fits */
    tetris_move(&board, &piece, 0, -1);
    tetris_move(&board, &piece, 0, -1);
    assert_eq(16, piece.y);
    assert_eq(0, tetris_rotate(&board, &piece));
    assert_eq(1, piece.rot);
}

TEST_FUNC(test_lock_and_clear_row) {
    reset_board();
    /* fill row 19 except col 1; O piece (2x2 at rows y+1..y+2,
       cols x+1..x+2) at y=17 covers (19,1) and (19,2) → completes row */
    for (int c = 0; c < TETRIS_W; c++)
        board.cells[19][c] = 1;
    board.cells[19][1] = 0;                           /* hole at col 1 */

    tetris_piece_t o = { .x = 0, .y = 17, .shape = 1, .rot = 0 };
    assert_eq(1, tetris_lock(&board, &o));            /* completes row 19 */
    /* row 19 was the full line → it is replaced by row 18 (O's top cells) */
    assert_eq(2, board.cells[19][1]);                 /* O top shifted down */
    assert_eq(0, board.cells[18][1]);                 /* row 18 now blank */
    assert_eq(0, board.cells[19][0]);                 /* col 0 of old row blank */
}

TEST_FUNC(test_clear_four_rows) {
    reset_board();
    for (int r = 16; r <= 19; r++)
        for (int c = 0; c < TETRIS_W; c++)
            board.cells[r][c] = 1;
    assert_eq(4, tetris_clear_rows(&board));
    for (int r = 0; r < TETRIS_H; r++)
        for (int c = 0; c < TETRIS_W; c++)
            assert_eq(0, board.cells[r][c]);          /* all shifted out */
}

TEST_FUNC(test_occupied_blocked) {
    reset_board();
    board.cells[2][4] = 1;                            /* T spawn occupies
                                                         (1,4),(2,3),(2,4),(2,5) */
    tetris_spawn(&board, &piece, 2);
    assert_eq(-1, tetris_move(&board, &piece, 0, 1)); /* collides at y=1 */
}

TEST_FUNC(test_game_over_on_spawn) {
    reset_board();
    /* stack blocks where a T spawn lands: (1,4) is T's center-top cell */
    board.cells[1][4] = 1;
    assert_eq(-1, tetris_spawn(&board, &piece, 2));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_spawn_ok),
    TEST_ENTRY(test_wall_left_blocked),
    TEST_ENTRY(test_wall_right_blocked),
    TEST_ENTRY(test_floor_blocked),
    TEST_ENTRY(test_rotate_t),
    TEST_ENTRY(test_rotate_blocked_by_floor),
    TEST_ENTRY(test_lock_and_clear_row),
    TEST_ENTRY(test_clear_four_rows),
    TEST_ENTRY(test_occupied_blocked),
    TEST_ENTRY(test_game_over_on_spawn),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
