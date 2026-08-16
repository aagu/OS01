// user/tetris_logic.c — pure tetris game logic (host-testable).

#include "tetris_logic.h"
#include <string.h>

// 7 shapes x 4 rotations, each a 4x4 bitmap (0/1).
static const uint8_t shapes[7][4][4][4] = {
    // I
    { {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
      {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}},
      {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}},
      {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}} },
    // O
    { {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}},
      {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}},
      {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}},
      {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}} },
    // T
    { {{0,0,0,0},{0,1,0,0},{1,1,1,0},{0,0,0,0}},
      {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,1,0,0}},
      {{0,0,0,0},{0,0,0,0},{1,1,1,0},{0,1,0,0}},
      {{0,0,0,0},{0,1,0,0},{1,1,0,0},{0,1,0,0}} },
    // S
    { {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}},
      {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0}},
      {{0,0,0,0},{0,0,0,0},{0,1,1,0},{1,1,0,0}},
      {{0,0,0,0},{1,0,0,0},{1,1,0,0},{0,1,0,0}} },
    // Z
    { {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}},
      {{0,0,0,0},{0,0,1,0},{0,1,1,0},{0,1,0,0}},
      {{0,0,0,0},{0,0,0,0},{0,1,1,0},{1,1,0,0}},
      {{0,0,0,0},{0,1,0,0},{1,1,0,0},{1,0,0,0}} },
    // J
    { {{0,0,0,0},{1,0,0,0},{1,1,1,0},{0,0,0,0}},
      {{0,0,0,0},{0,1,1,0},{0,1,0,0},{0,1,0,0}},
      {{0,0,0,0},{0,0,0,0},{1,1,1,0},{0,0,1,0}},
      {{0,0,0,0},{0,1,0,0},{0,1,0,0},{1,1,0,0}} },
    // L
    { {{0,0,0,0},{0,0,1,0},{1,1,1,0},{0,0,0,0}},
      {{0,0,0,0},{0,1,0,0},{0,1,0,0},{0,1,1,0}},
      {{0,0,0,0},{0,0,0,0},{1,1,1,0},{1,0,0,0}},
      {{0,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,0,0}} },
};

const uint8_t (*tetris_shape(uint8_t shape, uint8_t rot))[4]
{
    if (shape > 6) shape = 0;
    if (rot > 3) rot = 0;
    return shapes[shape][rot];
}

uint32_t tetris_rand(uint32_t *state)
{
    uint32_t x = *state;
    if (x == 0) x = 0x9e3779b9;   // never stuck at 0
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int tetris_preview_full_rows(const tetris_board_t *b, const tetris_piece_t *p,
                             int *rows, int max)
{
    tetris_board_t tmp = *b;
    const uint8_t (*s)[4] = tetris_shape(p->shape, p->rot);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (s[r][c]) {
                int bx = p->x + c;
                int by = p->y + r;
                if (by >= 0 && by < TETRIS_H && bx >= 0 && bx < TETRIS_W)
                    tmp.cells[by][bx] = p->shape + 1;
            }
    int n = 0;
    for (int r = 0; r < TETRIS_H && n < max; r++) {
        int full = 1;
        for (int c = 0; c < TETRIS_W; c++)
            if (!tmp.cells[r][c]) { full = 0; break; }
        if (full)
            rows[n++] = r;
    }
    return n;
}

int tetris_fits(const tetris_board_t *b, const tetris_piece_t *p)
{
    const uint8_t (*s)[4] = tetris_shape(p->shape, p->rot);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!s[r][c]) continue;
            int bx = p->x + c;
            int by = p->y + r;
            if (bx < 0 || bx >= TETRIS_W) return 0;   // wall
            if (by >= TETRIS_H) return 0;             // floor
            if (by < 0) continue;                     // above top: allow (spawn)
            if (b->cells[by][bx]) return 0;           // occupied
        }
    }
    return 1;
}

int tetris_spawn(tetris_board_t *b, tetris_piece_t *p, uint8_t shape)
{
    tetris_piece_t np = { .x = TETRIS_W / 2 - 2, .y = 0,
                          .shape = shape, .rot = 0 };
    if (!tetris_fits(b, &np))
        return -1;                       // game over
    *p = np;
    return 0;
}

int tetris_move(tetris_board_t *b, tetris_piece_t *p, int dx, int dy)
{
    tetris_piece_t np = *p;
    np.x += dx;
    np.y += dy;
    if (!tetris_fits(b, &np))
        return -1;
    *p = np;
    return 0;
}

int tetris_rotate(tetris_board_t *b, tetris_piece_t *p)
{
    tetris_piece_t np = *p;
    np.rot = (np.rot + 1) & 3;
    // simple wall kicks: try shifts of -1,0,+1,+2 horizontally
    const int kicks[] = { 0, -1, 1, -2, 2 };
    for (int i = 0; i < 5; i++) {
        np.x = p->x + kicks[i];
        if (tetris_fits(b, &np)) {
            *p = np;
            return 0;
        }
    }
    return -1;
}

int tetris_clear_rows(tetris_board_t *b)
{
    int cleared = 0;
    for (int r = TETRIS_H - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < TETRIS_W; c++)
            if (!b->cells[r][c]) { full = 0; break; }
        if (full) {
            // shift everything above down one
            for (int rr = r; rr > 0; rr--)
                memcpy(b->cells[rr], b->cells[rr - 1], TETRIS_W);
            memset(b->cells[0], 0, TETRIS_W);
            cleared++;
            r++;   // re-check same row (now shifted down content)
        }
    }
    return cleared;
}

int tetris_lock(tetris_board_t *b, const tetris_piece_t *p)
{
    const uint8_t (*s)[4] = tetris_shape(p->shape, p->rot);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (s[r][c]) {
                int bx = p->x + c;
                int by = p->y + r;
                if (by >= 0 && by < TETRIS_H && bx >= 0 && bx < TETRIS_W)
                    b->cells[by][bx] = p->shape + 1;   // 1..7
            }
    return tetris_clear_rows(b);
}
