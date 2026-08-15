/* tetris.elf — OS01 Tetris (framebuffer + /dev/keyboard raw scancodes)
 *
 * Enter:   \e[?1049h (terminal switches to alt screen)
 * Input:   /dev/keyboard — PS/2 Set 1 + E0 prefix scancodes
 *          ← →  move, ↓ soft-drop, ↑ rotate, SPACE hard-drop, q quit
 * Exit:    \e[?1049l (terminal restores main screen)
 *
 * Pure game logic lives in tetris_logic.c (host-tested).
 */

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "tetris_logic.h"

// ── fb_info (must match kernel definition) ──────────────────
struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));

#define FBIOSURRENDER  0x00004601

// ── Actions from scancodes ──────────────────────────────────
enum { A_NONE = 0, A_LEFT, A_RIGHT, A_DOWN, A_ROTATE, A_DROP, A_QUIT };

static uint32_t *fb;
static struct fb_info fb_info;
static int cell;             // board cell size in px
static int ox, oy;           // board origin (top-left) in fb px

static const uint32_t colors[8] = {
    0x000000,                // empty
    0x00FFFF, 0xFFFF00, 0xFF00FF,  // I, O, T
    0x00FF00, 0xFF0000, 0x0000FF,  // S, Z, J
    0xFF8000,                // L
};

static uint8_t prev_view[TETRIS_H][TETRIS_W];

static void draw_cell(int col, int row, uint32_t color)
{
    for (int y = 0; y < cell; y++) {
        uint32_t *line = fb + (oy + row * cell + y) * (fb_info.stride / 4)
                         + (ox + col * cell);
        for (int x = 0; x < cell; x++)
            line[x] = color;
    }
}

static void draw_rect(int x0, int y0, int w, int h, uint32_t color)
{
    for (int y = 0; y < h; y++) {
        uint32_t *line = fb + (y0 + y) * (fb_info.stride / 4) + x0;
        for (int x = 0; x < w; x++)
            line[x] = color;
    }
}

// Render board + falling piece, diffing against prev_view.
static void render(const tetris_board_t *b, const tetris_piece_t *p)
{
    uint8_t view[TETRIS_H][TETRIS_W];
    memcpy(view, b->cells, sizeof(view));

    if (p) {
        const uint8_t (*s)[4] = tetris_shape(p->shape, p->rot);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                if (s[r][c]) {
                    int by = p->y + r;
                    int bx = p->x + c;
                    if (by >= 0 && by < TETRIS_H && bx >= 0 && bx < TETRIS_W)
                        view[by][bx] = p->shape + 1;
                }
    }

    for (int r = 0; r < TETRIS_H; r++)
        for (int c = 0; c < TETRIS_W; c++)
            if (view[r][c] != prev_view[r][c]) {
                draw_cell(c, r, colors[view[r][c]]);
                prev_view[r][c] = view[r][c];
            }
}

// PS/2 Set 1 + E0 prefix parser → action (key-up ignored).
static int parse_scancodes(const uint8_t *buf, int n)
{
    static bool e0 = false;
    for (int i = 0; i < n; i++) {
        uint8_t sc = buf[i];
        if (sc == 0xE0) { e0 = true; continue; }
        if (sc & 0x80) { e0 = false; continue; }   // key release — ignore
        int a = A_NONE;
        if (e0) {
            switch (sc) {
            case 0x4B: a = A_LEFT;  break;
            case 0x4D: a = A_RIGHT; break;
            case 0x50: a = A_DOWN;  break;
            case 0x48: a = A_ROTATE; break;
            }
            e0 = false;
        } else {
            switch (sc) {
            case 0x39: a = A_DROP; break;   // SPACE
            case 0x10: a = A_QUIT; break;   // Q
            case 0x1E: a = A_LEFT;  break;  // A
            case 0x20: a = A_RIGHT; break;  // D
            case 0x1F: a = A_DOWN;  break;  // S
            }
        }
        if (a != A_NONE)
            return a;
    }
    return A_NONE;
}

static void clear_screen(void)
{
    draw_rect(0, 0, fb_info.width, fb_info.height, 0x000000);
    // border around board
    draw_rect(ox - 2, oy - 2, TETRIS_W * cell + 4, 2, 0x444444);
    draw_rect(ox - 2, oy + TETRIS_H * cell, TETRIS_W * cell + 4, 2, 0x444444);
    draw_rect(ox - 2, oy - 2, 2, TETRIS_H * cell + 4, 0x444444);
    draw_rect(ox + TETRIS_W * cell, oy - 2, 2, TETRIS_H * cell + 4, 0x444444);
}

int main(int argc, char **argv)
{
    (void)argc;
    bool fast = (argc > 1 && argv[1] && strcmp(argv[1], "fast") == 0);

    // ── Framebuffer ──────────────────────────────────────
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) return 1;
    read(fb_fd, &fb_info, sizeof(fb_info));
    fb = mmap(NULL, fb_info.height * fb_info.stride,
              PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_info.width == 0 || (int64_t)(intptr_t)fb < 0)
        return 1;
    ioctl(fb_fd, FBIOSURRENDER, NULL);

    cell = fb_info.height / (TETRIS_H + 4);
    if (cell > 48) cell = 48;
    ox = (fb_info.width - TETRIS_W * cell) / 2;
    oy = (fb_info.height - TETRIS_H * cell) / 2;

    // ── Enter alt screen (terminal restores main on exit) ─
    write(1, "\x1b[?1049h", 8);
    write(1, "\x1b[2J", 4);

    // ── Keyboard ─────────────────────────────────────────
    int kbd = open("/dev/keyboard", O_RDONLY);
    if (kbd < 0) { write(1, "\x1b[?1049l", 8); return 1; }

    // ── Game loop ────────────────────────────────────────
    tetris_board_t board;
    memset(&board, 0, sizeof(board));
    tetris_piece_t piece;
    memset(&piece, 0, sizeof(piece));
    memset(prev_view, 0, sizeof(prev_view));

    int lines = 0, score = 0;
    int tick_ms = fast ? 50 : 500;
    bool game_over = false;

    clear_screen();
    memset(prev_view, 0, sizeof(prev_view));
    if (tetris_spawn(&board, &piece, 0) != 0)
        game_over = true;

    uint8_t buf[16];

    while (!game_over) {
        struct pollfd pfd = { .fd = kbd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, tick_ms);

        if (pr > 0 && (pfd.revents & POLLIN)) {
            int n = read(kbd, buf, sizeof(buf));
            if (n > 0) {
                int a = parse_scancodes(buf, n);
                switch (a) {
                case A_LEFT:  tetris_move(&board, &piece, -1, 0); break;
                case A_RIGHT: tetris_move(&board, &piece,  1, 0); break;
                case A_DOWN:  tetris_move(&board, &piece,  0, 1); break;
                case A_ROTATE: tetris_rotate(&board, &piece); break;
                case A_DROP:
                    while (tetris_move(&board, &piece, 0, 1) == 0) {}
                    goto lock_piece;
                case A_QUIT: goto done;
                default: break;
                }
            }
        }

        // gravity
        if (tetris_move(&board, &piece, 0, 1) != 0) {
lock_piece:
            int cleared = tetris_lock(&board, &piece);
            if (cleared > 0) {
                lines += cleared;
                score += cleared * 100;
                tick_ms = 500 - (lines / 10) * 50;
                if (tick_ms < 100) tick_ms = 100;
                memset(prev_view, 0, sizeof(prev_view)); // full redraw
            }
            if (tetris_spawn(&board, &piece, lines % 7) != 0)
                game_over = true;
        }

        render(&board, &piece);
    }

    // Leave the alt screen immediately (nanosleep is known-broken in
    // this kernel — see plan doc; tetris doesn't need it).
done:
    write(1, "\x1b[?1049l", 8);
    close(kbd);
    munmap(fb, fb_info.height * fb_info.stride);
    close(fb_fd);
    return 0;
}
