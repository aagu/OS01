/* tetris.elf — OS01 Tetris (libgfx + stdin ANSI/VT100 escape sequences)
 *
 * Enter:   \e[?1049h (terminal switches to alt screen)
 * Input:   stdin (fd 0) — ANSI / VT100 navigation keys + ASCII
 *          ← →  move, ↓ soft-drop, ↑ rotate, SPACE hard-drop, q quit
 * Exit:    \e[?1049l (terminal restores main screen)
 *
 * Pure game logic lives in tetris_logic.c (host-tested).
 *
 * Rendering uses the libgfx 2D API (Task 6 of the 2D graphics API
 * plan — spec docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md
 * §6 + plan Task 6): open /dev/fb once for the width/height metadata,
 * close it, then call gfx_open(0,0,fb_info.width,fb_info.height).
 * All drawing goes to libgfx's private buffer via gfx_fill_rect;
 * gfx_present is invoked exactly once per visual event (after each
 * render, and after each clear-line flash) so the kernel does at
 * most one GFX_PRESENT ioctl per visual update.  The kernel-side
 * FBIOSURRENDER ioctl is still issued on /dev/fb so the kernel
 * console yields the framebuffer before gfx writes go out.
 */

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <sys/time.h>
#include <gfx.h>
#include <uapi/fb.h>
#include "tetris_logic.h"
#include "gfx_client_policy.h"

static gfx_handle_t *gfx;

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
    gfx_fill_rect(gfx,
                  (int32_t)(ox + col * cell),
                  (int32_t)(oy + row * cell),
                  (uint32_t)cell, (uint32_t)cell,
                  color);
}

static void draw_rect(int x0, int y0, int w, int h, uint32_t color)
{
    gfx_fill_rect(gfx, (int32_t)x0, (int32_t)y0,
                  (uint32_t)w, (uint32_t)h, color);
}

// One GFX_PRESENT ioctl per visual event (spec §6: at most one
// present per frame).  Used after each render() and after each
// clear-line flash so the user sees the updated framebuffer in
// step with the game state.
//
// Task 8: apply the shared stale-view policy.  A transient EAGAIN
// keeps the view and arms a >=250ms retry deadline (the caller must
// not present again before it); a stale/permanent failure releases the
// view and makes the caller exit non-zero.  Returns false on the
// permanent class.  The failing errno is preserved.
static uint64_t gfx_retry_deadline_ms = 0;

static uint64_t client_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool present(void)
{
    uint64_t now = client_now_ms();
    if (gfx_retry_deadline_ms != 0 && now < gfx_retry_deadline_ms)
        return true;                 // throttled: skip until the deadline
    errno = 0;
    if (gfx_present(gfx) == 0) {
        gfx_retry_deadline_ms = 0;
        return true;
    }
    int saved = errno;
    if (gfx_client_present_policy(saved) == 1) {     // EAGAIN: keep view
        gfx_retry_deadline_ms = now + GFX_CLIENT_RETRY_MS;
        return true;
    }
    const char *why = (saved == ESTALE)
        ? "display mode changed, restart the app"
        : "display device failure";
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "tetris: %s (errno=%d); exiting\n",
                     why, saved);
    if (n > 0) {
        ssize_t w = write(2, msg, (size_t)n);
        (void)w;
    }
    errno = saved;                   // cleanup below must not clobber it
    return false;
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

static void clear_screen(void)

{
    draw_rect(0, 0, (int)fb_info.width, (int)fb_info.height, 0x000000);
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
    /* Task 8 of the user-heap/ELF-isolation plan: a non-interactive
     * "smoke" path that runs ONE real gfx allocation, render and
     * present, then prints a fixed marker line and exits.  The
     * gfx QEMU runner executes ``/bin/tetris smoke`` after
     * ``/bin/test_gfx`` and requires the marker — it proves the
     * full-screen 1440×900 RGB32 pixels buffer (5.18 MiB) fits
     * in the program's heap AND the kernel's gfx0 device can
     * accept a real present, end-to-end, without entering the
     * interactive game loop. */
    bool smoke = (argc > 1 && argv[1] &&
                  strcmp(argv[1], "smoke") == 0);

    // ── Framebuffer metadata ──────────────────────────────
    // /dev/fb is opened only to read its struct fb_info (width,
    // height).  The kernel-side fb mmap path is no longer used
    // here; libgfx owns the pixels buffer that gfx_present ships
    // to /dev/gfx0 each frame.
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) return 1;
    ssize_t r = read(fb_fd, &fb_info, sizeof(fb_info));
    /* Ask the kernel console to step aside before we present.
     * Mirrors the pre-migration behaviour so the kernel console
     * doesn't trample our pixels between presents. */
    ioctl(fb_fd, FBIOSURRENDER, NULL);
    close(fb_fd);
    if (r != (ssize_t)sizeof(fb_info) ||
        fb_info.width == 0 || fb_info.height == 0)
        return 1;

    /* Open the full-screen gfx view.  gfx_open allocates a
     * zeroed pixels buffer of width*height*4 bytes and configures
     * a single bounded rectangle in /dev/gfx0's view table. */
    gfx = gfx_open(0, 0, fb_info.width, fb_info.height);
    if (!gfx) return 1;

    cell = fb_info.height / (TETRIS_H + 4);
    if (cell > 48) cell = 48;
    ox = (fb_info.width - TETRIS_W * cell) / 2;
    oy = (fb_info.height - TETRIS_H * cell) / 2;

    /* ── Smoke path: one real render+present + marker + exit ───
     * Bypass the alt-screen, keyboard polling and game loop —
     * the runner only needs to prove the gfx stack still works
     * after a full-screen allocation.  The board outline + a
     * single T-piece render is enough to make the present
     * non-trivial (covers >1 KiB of pixel writes).  We do NOT
     * enter the alt screen on this path — the marker line must
     * reach stdout / the serial line unmodified. */
    if (smoke) {
        gfx_fill_rect(gfx, 0, 0, fb_info.width, fb_info.height,
                      0x00202020u);            /* dark grey bg */
        /* Border around the board (matches the in-game look). */
        gfx_fill_rect(gfx, ox - 4, oy - 4,
                      TETRIS_W * cell + 8, 4, 0x00444444u);
        gfx_fill_rect(gfx, ox - 4, oy + TETRIS_H * cell,
                      TETRIS_W * cell + 8, 4, 0x00444444u);
        gfx_fill_rect(gfx, ox - 4, oy - 4,
                      4, TETRIS_H * cell + 8, 0x00444444u);
        gfx_fill_rect(gfx, ox + TETRIS_W * cell, oy - 4,
                      4, TETRIS_H * cell + 8, 0x00444444u);
        /* A single T-piece (shape index 2 = T in tetris_logic) at
         * the top-left of the board — 4 cells × cell^2 px = a
         * non-trivial write that also exercises gfx_line / rect
         * path coverage at the small scale. */
        static const int t_cells[4][2] = {
            { 0, 1 }, { 1, 1 }, { 2, 1 }, { 1, 0 }
        };
        for (int i = 0; i < 4; i++) {
            gfx_fill_rect(gfx,
                          (int32_t)(ox + t_cells[i][0] * cell),
                          (int32_t)(oy + t_cells[i][1] * cell),
                          (uint32_t)cell, (uint32_t)cell,
                          0x00FF00FFu);          /* T = magenta */
        }
        /* One real gfx_present ships the buffer to the kernel. */
        if (gfx_present(gfx) != 0) {
            gfx_close(gfx);
            return 1;
        }
        gfx_close(gfx);
        /* The marker line — single line, no extra banners.  Goes
         * to stdout (fd 1), which under -serial stdio is the QEMU
         * serial port the runner reads. */
        static const char msg[] = "[TETRIS] SMOKE PASS\n";
        if (write(1, msg, sizeof(msg) - 1) !=
            (ssize_t)(sizeof(msg) - 1))
            return 1;
        return 0;
    }

    // ── Save termios and enter raw mode on stdin ─────────
    struct termios orig_term;
    bool has_term = (tcgetattr(STDIN_FILENO, &orig_term) == 0);
    if (has_term) {
        struct termios raw = orig_term;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    // ── Enter alt screen (terminal restores main on exit) ─
    write(1, "\x1b[?1049h", 8);
    write(1, "\x1b[2J", 4);

    // ── Game loop ────────────────────────────────────────
    tetris_board_t board;
    memset(&board, 0, sizeof(board));
    tetris_piece_t piece;
    memset(&piece, 0, sizeof(piece));
    memset(prev_view, 0, sizeof(prev_view));

    int lines = 0;
    int tick_ms = fast ? 50 : 800;   // classic-start gravity (0.8s/row)
    bool game_over = false;
    int exit_code = 0;               // non-zero once the gfx view is stale
    // RNG seed: time ^ pid — different every launch
    uint32_t rng = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    if (rng == 0) rng = 0x9e3779b9;

    clear_screen();
    memset(prev_view, 0, sizeof(prev_view));
    if (tetris_spawn(&board, &piece, 0) != 0)
        game_over = true;

    tetris_input_t input;
    tetris_input_init(&input);
    uint8_t buf[64];

    while (!game_over) {
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, tick_ms);

        if (pr > 0 && (pfd.revents & POLLIN)) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                int off = 0;
                while (off < n) {
                    int a = tetris_input_parse(&input, buf, n, &off);
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
        }

        // gravity
        if (tetris_move(&board, &piece, 0, 1) != 0) {
lock_piece:
            // Which rows complete with this lock? (flash them before clearing)
            int full_rows[TETRIS_H], nfull;
            nfull = tetris_preview_full_rows(&board, &piece, full_rows, TETRIS_H);
            int cleared = tetris_lock(&board, &piece);
            if (cleared > 0) {
                lines += cleared;
                if (!fast) {
                    tick_ms = 800 - (lines / 10) * 40;
                    if (tick_ms < 150) tick_ms = 150;
                }
                // Flash completed rows white, present once, pause briefly
                // (poll timeout as delay — nanosleep is broken in this kernel).
                for (int i = 0; i < nfull; i++)
                    for (int c = 0; c < TETRIS_W; c++)
                        draw_cell(c, full_rows[i], 0xFFFFFF);
                /* One present per visual event: flash now visible. */
                if (!present()) { exit_code = 1; goto done; }
                struct pollfd pf = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
                poll(&pf, 1, fast ? 50 : 200);
                // Force redraw of the cleared rows: prev_view must DIFFER
                // from the cleared board cells (0), or render()'s diff
                // skips them and the white flash stays on screen forever.
                for (int i = 0; i < nfull; i++)
                    for (int c = 0; c < TETRIS_W; c++)
                        prev_view[full_rows[i]][c] = 0xFF;
            }
            if (tetris_spawn(&board, &piece, tetris_rand(&rng) % 7) != 0)
                game_over = true;
        }

        render(&board, &piece);
        /* One present per visual event: the render diff. */
        if (!present()) { exit_code = 1; goto done; }
    }

    // Leave the alt screen immediately (nanosleep is known-broken in
    // this kernel — see plan doc; tetris doesn't need it).
done:
    write(1, "\x1b[?1049l", 8);
    if (has_term) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    }
    gfx_close(gfx);
    return exit_code;
}

