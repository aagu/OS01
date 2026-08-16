#ifndef _TETRIS_LOGIC_H
#define _TETRIS_LOGIC_H

#include <stdint.h>

// Tetris game logic — pure C, no OS deps.  Host-testable.
// Board is 10x20.  Pieces are 4x4 bitmaps, 7 shapes x 4 rotations.

#define TETRIS_W 10
#define TETRIS_H 20

typedef struct {
    uint8_t cells[TETRIS_H][TETRIS_W];   // 0 = empty, 1..7 = piece color
} tetris_board_t;

typedef struct {
    int x, y;           // top-left of the 4x4 bounding box in board coords
    uint8_t shape;      // 0..6
    uint8_t rot;        // 0..3
} tetris_piece_t;

// 4x4 bitmap for a shape/rotation (values 0/1).
const uint8_t (*tetris_shape(uint8_t shape, uint8_t rot))[4];

// xorshift32 PRNG — deterministic per seed, host-testable.
// Feed a per-game seed (e.g. time ^ pid); advance via the returned state.
uint32_t tetris_rand(uint32_t *state);

// Which board rows would become FULL if `p` were locked now?
// Writes row indices (0=top) into `rows` (max entries `max`) and
// returns the count.  Used for the clear-line flash animation BEFORE
// the actual lock clears them.
int tetris_preview_full_rows(const tetris_board_t *b, const tetris_piece_t *p,
                             int *rows, int max);

// Does the piece overlap board cells / walls / floor?
int tetris_fits(const tetris_board_t *b, const tetris_piece_t *p);

// Spawn `shape` at top-center.  0 = ok, -1 = game over (spawn blocked).
int tetris_spawn(tetris_board_t *b, tetris_piece_t *p, uint8_t shape);

// Move by (dx,dy).  0 = moved, -1 = blocked (piece unchanged).
int tetris_move(tetris_board_t *b, tetris_piece_t *p, int dx, int dy);

// Rotate clockwise.  0 = rotated, -1 = blocked (piece unchanged).
int tetris_rotate(tetris_board_t *b, tetris_piece_t *p);

// Lock the piece into the board; returns number of full rows cleared.
int tetris_lock(tetris_board_t *b, const tetris_piece_t *p);

// Clear full rows (used by tetris_lock); returns count.
int tetris_clear_rows(tetris_board_t *b);

#endif
