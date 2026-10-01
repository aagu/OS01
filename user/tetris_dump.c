/* tetris_dump.elf — sample pixels from /dev/fb to verify Tetris output.
 *
 * Mmaps /dev/fb, reads struct fb_info, then prints sampled pixel
 * colors at coordinates that match Tetris's expected layout:
 *   - (0, 0) should be BLACK (full-screen bg)
 *   - (ox, oy) center of the board's top-left cell
 *   - The border around the board should be 0x444444 (gray)
 *
 * Run AFTER /bin/tetris so the pixels reflect the migrated renderer's
 * output.  Used by .superpowers/sdd/2026-09-30-2d-graphics-api/
 * manual_tetris_test.py for the Task 6 visual-equivalence gate.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>

struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));

#define TETRIS_W 10
#define TETRIS_H 20

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) {
        printf("FAIL: cannot open /dev/fb\n");
        return 1;
    }

    struct fb_info fb_info;
    ssize_t r = read(fb_fd, &fb_info, sizeof(fb_info));
    if (r != (ssize_t)sizeof(fb_info)) {
        printf("FAIL: read fb_info incomplete (got %zd bytes)\n", r);
        close(fb_fd);
        return 1;
    }

    uint32_t *fb = (uint32_t *)mmap(NULL,
                                     fb_info.height * fb_info.stride,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fb_fd, 0);
    if ((intptr_t)fb < 0) {
        printf("FAIL: mmap returned %p\n", (void *)fb);
        close(fb_fd);
        return 1;
    }

    /* Match tetris's layout: cell = fb_info.height / (TETRIS_H + 4),
     * capped at 48, with the board centered. */
    int cell = fb_info.height / (TETRIS_H + 4);
    if (cell > 48) cell = 48;
    int ox = (fb_info.width - TETRIS_W * cell) / 2;
    int oy = (fb_info.height - TETRIS_H * cell) / 2;

    /* Sample background: (5, 5) should be BLACK (0x000000) — clear_screen
     * paints the full screen black before the board border. */
    uint32_t bg = fb[5 * (fb_info.stride / 4) + 5];
    printf("bg(5,5)      = 0x%08X\n", bg);

    /* Sample top-left cell center of board: should be a piece color
     * (the spawned I piece at y=0 covers the top center cells). */
    int cx = ox + TETRIS_W * cell / 2;       /* center column */
    int cy = oy + cell / 2;                  /* first row center */
    uint32_t piece = fb[cy * (fb_info.stride / 4) + cx];
    printf("piece(cx,cy) = 0x%08X (ox=%d oy=%d cell=%d)\n",
           piece, ox, oy, cell);

    /* Sample the top border (just above the board): should be
     * GRAY (0x00444444) — the 2-px border drawn around the board. */
    int bx = ox + TETRIS_W * cell / 2;
    int by = oy - 1;
    uint32_t border = fb[by * (fb_info.stride / 4) + bx];
    printf("border(top)  = 0x%08X\n", border);

    /* Sample the LEFT border: should be GRAY. */
    int lx = ox - 1;
    int ly = oy + TETRIS_H * cell / 2;
    uint32_t lborder = fb[ly * (fb_info.stride / 4) + lx];
    printf("border(left) = 0x%08X\n", lborder);

    munmap(fb, fb_info.height * fb_info.stride);
    close(fb_fd);

    /* Summary verdict: bg must be black, borders gray. */
    int ok = 1;
    if (bg != 0x000000) {
        printf("VERDICT: bg(5,5) = 0x%08X (expected 0x000000)\n", bg);
        ok = 0;
    }
    if (border != 0x00444444) {
        printf("VERDICT: top border = 0x%08X (expected 0x00444444)\n", border);
        ok = 0;
    }
    if (lborder != 0x00444444) {
        printf("VERDICT: left border = 0x%08X (expected 0x00444444)\n", lborder);
        ok = 0;
    }
    if (!ok) {
        printf("FAIL\n");
        return 1;
    }
    /* The piece color can be any of the 7 tetromino colors — only check
     * it's non-zero (background) and non-gray (border). */
    if (piece == 0x000000) {
        printf("VERDICT: piece pixel is black (expected a tetromino color)\n");
        printf("FAIL\n");
        return 1;
    }
    if (piece == 0x00444444) {
        printf("VERDICT: piece pixel is gray (expected a tetromino color)\n");
        printf("FAIL\n");
        return 1;
    }
    printf("PASS: bg black, borders gray, piece a tetromino color (0x%08X)\n",
           piece);
    return 0;
}
