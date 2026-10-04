/* user/test_terminal_screen.c — Ring-3 QEMU terminal screen visual E2E test.
 *
 * Verifies that the migrated terminal emulator correctly renders glyphs,
 * cursor underline, hides/shows cursor, transitions to/from alt-screen
 * without stale pixels, and maintains responsive ~30 FPS presentation
 * under continuous PTY streaming.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>

struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed));

#define FAIL(...) do {                                       \
    printf("\r\n[TERM SCREEN TEST] FAIL: ");                 \
    printf(__VA_ARGS__);                                     \
    printf("\r\n");                                          \
    exit(1);                                                 \
} while (0)

#define PASS() do {                                          \
    printf("\r\n[TERM SCREEN TEST] PASS\r\n");               \
    exit(0);                                                 \
} while (0)

static void sleep_ms(int ms)
{
    poll(NULL, 0, ms);
}

static int wait_for_pixel(uint32_t *fb, uint32_t stride_pixels,
                          int x, int y, uint32_t expected, int timeout_ms)
{
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        if (fb[(size_t)y * stride_pixels + x] == expected) {
            return 0;
        }
        sleep_ms(10);
    }
    return -1;
}

int main(void)
{
    int fb_fd = open("/dev/fb", O_RDONLY);
    if (fb_fd < 0) {
        FAIL("cannot open /dev/fb (errno=%d)", errno);
    }

    struct fb_info info;
    if (read(fb_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) {
        FAIL("short read on /dev/fb info");
    }

    if (info.width == 0 || info.height == 0 || info.stride == 0) {
        FAIL("invalid fb geometry: %ux%u stride=%u", info.width, info.height, info.stride);
    }

    size_t fb_bytes = (size_t)info.height * (size_t)info.stride;
    uint32_t *fb = mmap(NULL, fb_bytes, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) {
        FAIL("mmap /dev/fb failed (errno=%d)", errno);
    }

    uint32_t stride_pixels = info.stride / 4;

    /* ── Test 1: Visual pattern test & high-bit character sanitization ── */
    /* Clear screen and print test string with a high-bit byte (0x80) */
    printf("\x1b[2J\x1b[HTERM_VISUAL_OK\x80");
    fflush(stdout);

    /* Wait for 'T' at (3, 2) to become white (0xFFFFFFFFu) */
    if (wait_for_pixel(fb, stride_pixels, 3, 2, 0xFFFFFFFFu, 1500) != 0) {
        FAIL("character 'T' top-bar foreground not rendered at (3, 2), got 0x%08x",
             fb[2 * stride_pixels + 3]);
    }
    if (wait_for_pixel(fb, stride_pixels, 3, 6, 0xFFFFFFFFu, 500) != 0) {
        FAIL("character 'T' stem foreground not rendered at (3, 6)");
    }
    if (fb[0 * stride_pixels + 0] != 0x00000000u) {
        FAIL("character 'T' background corrupted at (0, 0)");
    }
    if (fb[6 * stride_pixels + 0] != 0x00000000u) {
        FAIL("character 'T' background corrupted at (0, 6)");
    }

    /* Col 14 is the 0x80 character: must be sanitized to space (all 0x00000000u) */
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 8; cx++) {
            if (fb[(size_t)cy * stride_pixels + 14 * 8 + cx] != 0x00000000u) {
                FAIL("high-bit character 0x80 not sanitized to space at (%d, %d)", 14 * 8 + cx, cy);
            }
        }
    }

    /* Cursor is currently at column 15 (x = 15 * 8 = 120).
     * Underline at row y = 15 must be white (0xFFFFFFFFu). */
    if (wait_for_pixel(fb, stride_pixels, 120 + 3, 15, 0xFFFFFFFFu, 500) != 0) {
        FAIL("cursor underline not visible at (123, 15)");
    }

    /* ── Test 2: Cursor hide/show test ── */
    printf("\x1b[?25l");
    fflush(stdout);
    if (wait_for_pixel(fb, stride_pixels, 120 + 3, 15, 0x00000000u, 1000) != 0) {
        FAIL("cursor underline still visible after \\e[?25l");
    }

    printf("\x1b[?25h");
    fflush(stdout);
    if (wait_for_pixel(fb, stride_pixels, 120 + 3, 15, 0xFFFFFFFFu, 1000) != 0) {
        FAIL("cursor underline not restored after \\e[?25h");
    }

    /* ── Test 3: Dedicated alt-screen (?1049h/l) screen restoration test ── */
    /* Hide cursor so it does not interfere with pixel comparisons */
    printf("\x1b[?25l");
    fflush(stdout);

    /* Write MAIN_OK on main screen */
    printf("\x1b[2J\x1b[HMAIN_OK");
    fflush(stdout);

    /* Verify 'M' at (0, 2) is white */
    if (wait_for_pixel(fb, stride_pixels, 0, 2, 0xFFFFFFFFu, 1000) != 0) {
        FAIL("main screen 'M' not rendered at (0, 2)");
    }
    /* Verify 'A' at (12, 4) is white */
    if (wait_for_pixel(fb, stride_pixels, 12, 4, 0xFFFFFFFFu, 500) != 0) {
        FAIL("main screen 'A' not rendered at (12, 4)");
    }

    /* Capture snapshot of row 0..15 for columns 0..7 (64 pixels wide) */
    uint32_t main_snapshot[16 * 64];
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 64; cx++) {
            main_snapshot[cy * 64 + cx] = fb[(size_t)cy * stride_pixels + cx];
        }
    }

    /* Enter alt-screen and write ALT_OK */
    printf("\x1b[?1049h\x1b[2J\x1b[HALT_OK");
    fflush(stdout);

    /* Verify 'A' in ALT_OK at (3, 2) is white */
    if (wait_for_pixel(fb, stride_pixels, 3, 2, 0xFFFFFFFFu, 1000) != 0) {
        FAIL("alt screen 'A' not rendered at (3, 2)");
    }
    /* Verify 'M' from main screen is completely gone at (0, 2) */
    if (fb[2 * stride_pixels + 0] != 0x00000000u) {
        FAIL("main screen 'M' leaked into alt screen at (0, 2)");
    }
    /* Verify 'A' from main screen is completely gone at (12, 4) */
    if (fb[4 * stride_pixels + 12] != 0x00000000u) {
        FAIL("main screen 'A' leaked into alt screen at (12, 4)");
    }

    /* Exit alt-screen */
    printf("\x1b[?1049l");
    fflush(stdout);

    /* Wait for 'M' at (0, 2) to return to white */
    if (wait_for_pixel(fb, stride_pixels, 0, 2, 0xFFFFFFFFu, 1000) != 0) {
        FAIL("main screen 'M' not restored at (0, 2)");
    }

    /* Verify entire 64x16 snapshot matches restored main screen */
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 64; cx++) {
            if (fb[(size_t)cy * stride_pixels + cx] != main_snapshot[cy * 64 + cx]) {
                FAIL("main screen snapshot mismatch at (%d, %d): expected 0x%08x got 0x%08x",
                     cx, cy, main_snapshot[cy * 64 + cx], fb[(size_t)cy * stride_pixels + cx]);
            }
        }
    }

    /* Restore cursor */
    printf("\x1b[?25h");
    fflush(stdout);

    /* ── Test 4: Continuous PTY streaming presentation test ── */
    printf("\x1b[2J\x1b[HSTREAM_AAA\n");
    fflush(stdout);

    /* Col 7 has 'A': abs x = 7 * 8 + 3 = 59, y = 2 is white */
    if (wait_for_pixel(fb, stride_pixels, 59, 2, 0xFFFFFFFFu, 1000) != 0) {
        FAIL("initial STREAM_AAA not rendered at (59, 2)");
    }

    /* Continuously stream STREAM_BBB updates to row 0.
     * In 'B', abs x = 7 * 8 + 1 = 57, y = 2 is white (in 'A' it is black).
     * Verify that an intermediate frame is presented during the stream,
     * demonstrating that continuous PTY input does not starve the 30 FPS throttle. */
    int intermediate_observed = 0;
    for (int i = 0; i < 150; i++) {
        char s[32];
        int len = snprintf(s, sizeof(s), "\x1b[HSTREAM_BBB\n");
        write(STDOUT_FILENO, s, len);
        if (!intermediate_observed && i >= 10 && i <= 140) {
            if (fb[2 * stride_pixels + 57] == 0xFFFFFFFFu) {
                intermediate_observed = 1;
            }
        }
        sleep_ms(1);
    }

    if (wait_for_pixel(fb, stride_pixels, 57, 2, 0xFFFFFFFFu, 1000) != 0) {
        FAIL("final STREAM_BBB not rendered at (57, 2)");
    }
    if (!intermediate_observed) {
        FAIL("continuous PTY streaming starved graphics presentation (no intermediate frame observed)");
    }

    PASS();
}
