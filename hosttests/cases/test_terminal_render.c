/* hosttests/cases/test_terminal_render.c
 *
 * Host unit test exercising the real user/terminal/terminal_render.c,
 * user/terminal/terminal_core.c, libgfx/sprite.c, libgfx/line.c, and libgfx/gfx.c.
 */

#include "test_framework.h"
#include <terminal_render.h>
#include <terminal_core.h>
#include <gfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif

#define TEST_COLS 10
#define TEST_ROWS 4
#define FONT_W    8
#define FONT_H    16
#define VIEW_W    (TEST_COLS * FONT_W)  /* 80 px */
#define VIEW_H    (TEST_ROWS * FONT_H)  /* 64 px */

#define SENTINEL 0xDEADBEEFu
#define SENTINEL_SLOTS 8

typedef struct {
    uint32_t sentinel_before[SENTINEL_SLOTS];
    uint32_t pixels[VIEW_W * VIEW_H];
    uint32_t sentinel_after[SENTINEL_SLOTS];
} test_buf_t;

static test_buf_t g_buf;
static term_core_t g_core;
static term_render_t g_render;
static uint8_t *g_font_data = NULL;
static size_t g_font_size = 0;
static const psf2_t *g_font = NULL;

/* Load real PSF2 font from repo */
static void load_font_once(void)
{
    if (g_font_data) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/user/terminal/terminal_font.psf", REPO_ROOT);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        /* Try relative */
        fp = fopen("../user/terminal/terminal_font.psf", "rb");
    }
    if (!fp) {
        fp = fopen("user/terminal/terminal_font.psf", "rb");
    }
    if (!fp) {
        fprintf(stderr, "FATAL: cannot find user/terminal/terminal_font.psf\n");
        exit(1);
    }
    size_t cap = 4096, n = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    assert_true(buf != NULL);
    for (;;) {
        if (n + 1024 > cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            assert_true(nb != NULL);
            buf = nb;
        }
        size_t got = fread(buf + n, 1, 1024, fp);
        n += got;
        if (got < 1024) break;
    }
    fclose(fp);
    g_font_data = buf;
    g_font_size = n;

    bool ok = term_font_validate(g_font_data, g_font_size, &g_font);
    assert_true(ok);
    assert_true(g_font != NULL);
}

static gfx_handle_t *create_mock_handle(void)
{
    for (size_t i = 0; i < SENTINEL_SLOTS; ++i) {
        g_buf.sentinel_before[i] = SENTINEL;
        g_buf.sentinel_after[i] = SENTINEL;
    }
    memset(g_buf.pixels, 0, sizeof(g_buf.pixels));

    /* Open a fake handle with in-tree layout (internal.h layout) */
    struct fake_handle {
        int         fd;
        gfx_info_t  info;
        uint32_t   *pixels;
        size_t      pixels_bytes;
        int32_t     clip_x, clip_y;
        uint32_t    clip_w, clip_h;
    };
    struct fake_handle *h = (struct fake_handle *)calloc(1, sizeof(*h));
    h->fd = -1;
    h->info.width = VIEW_W;
    h->info.height = VIEW_H;
    h->info.stride = VIEW_W * 4u;
    h->info.format = GFX_FORMAT_RGB32;
    h->pixels = g_buf.pixels;
    h->pixels_bytes = sizeof(g_buf.pixels);
    h->clip_x = 0;
    h->clip_y = 0;
    h->clip_w = VIEW_W;
    h->clip_h = VIEW_H;

    return (gfx_handle_t *)h;
}

static bool check_sentinels(void)
{
    for (size_t i = 0; i < SENTINEL_SLOTS; ++i) {
        if (g_buf.sentinel_before[i] != SENTINEL) return false;
        if (g_buf.sentinel_after[i] != SENTINEL) return false;
    }
    return true;
}

static inline uint32_t pixel_at(int x, int y)
{
    return g_buf.pixels[y * VIEW_W + x];
}

static void feed_str(term_core_t *c, const char *s)
{
    while (*s) {
        term_core_input(c, (uint8_t)*s++);
    }
}

/* ── Test 1: PSF2 Font Validation ────────────────────────────── */
TEST_FUNC(test_font_validation)
{
    TEST_SUITE("terminal_render: PSF2 font header and size validation");
    load_font_once();

    const psf2_t *out = NULL;
    /* 1. Valid font passes */
    assert_true(term_font_validate(g_font_data, g_font_size, &out));
    assert_eq(0x864ab572u, out->magic);
    assert_eq(32u, out->headersize);
    assert_eq(8u, out->width);
    assert_eq(16u, out->height);

    /* 2. Truncated size rejected */
    assert_false(term_font_validate(g_font_data, g_font_size - 1, &out));
    assert_false(term_font_validate(g_font_data, 31, &out));

    /* 3. Corrupt magic rejected */
    uint8_t bad_magic[64];
    memcpy(bad_magic, g_font_data, 64);
    bad_magic[0] = 0x00;
    assert_false(term_font_validate(bad_magic, sizeof(bad_magic), &out));

    /* 4. Insufficient bytesperglyph rejected */
    uint8_t bad_bpg[64];
    memcpy(bad_bpg, g_font_data, 64);
    psf2_t *h_bpg = (psf2_t *)bad_bpg;
    h_bpg->bytesperglyph = 15; /* ceil(8/8)*16 = 16 needed */
    assert_false(term_font_validate(bad_bpg, g_font_size, &out));

    /* 5. Zero dimensions rejected */
    uint8_t bad_dim[64];
    memcpy(bad_dim, g_font_data, 64);
    ((psf2_t *)bad_dim)->width = 0;
    assert_false(term_font_validate(bad_dim, g_font_size, &out));

    /* 6. NULL buffer rejected */
    assert_false(term_font_validate(NULL, 100, &out));
}

/* ── Test 2: High-Bit Characters Bounds Sanitization ─────────── */
TEST_FUNC(test_render_high_bit_chars)
{
    TEST_SUITE("terminal_render: high-bit characters sanitized without OOB");
    load_font_once();
    gfx_handle_t *gfx = create_mock_handle();

    term_core_init(&g_core, TEST_ROWS, TEST_COLS);
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);

    /* Feed 0x80 and 0xFF */
    term_core_input(&g_core, 0x80);
    term_core_input(&g_core, 0xFF);
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Sentinels must remain intact (no out of bounds read/write) */
    assert_true(check_sentinels());

    /* High bit chars sanitized to space -> all pixels in col 0 are black (bg) */
    for (int y = 0; y < 15; ++y) {
        for (int x = 0; x < FONT_W; ++x) {
            assert_eq(0x00000000u, pixel_at(x, y));
        }
    }

    term_core_free(&g_core);
    free(gfx);
}

/* ── Test 3: Cursor Movement and Underline Erasing ───────────── */
TEST_FUNC(test_render_cursor_underline_movement)
{
    TEST_SUITE("terminal_render: cursor underline placement and movement erasing");
    load_font_once();
    gfx_handle_t *gfx = create_mock_handle();

    term_core_init(&g_core, TEST_ROWS, TEST_COLS);
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);

    /* Type "AB" -> cursor at col 2, row 0 */
    feed_str(&g_core, "AB");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Underline at row 0 bottom (y = 15) for col 2 (x in [16..23]) */
    for (int x = 16; x < 24; ++x) {
        assert_eq(0xFFFFFFFFu, pixel_at(x, 15));
    }
    /* Underline NOT present under col 0 (x in [0..7]) at y = 15 */
    for (int x = 0; x < 8; ++x) {
        assert_eq(0x00000000u, pixel_at(x, 15));
    }

    /* Move cursor left 1: \e[D -> cursor at col 1, row 0 */
    feed_str(&g_core, "\x1b[D");
    assert_eq(1, g_core.col);
    bool changed = term_render_cursor_update(&g_render);
    assert_true(changed);
    term_render_flush(&g_render);

    /* Col 2 underline must now be ERASED (restored to bg 0) */
    for (int x = 16; x < 24; ++x) {
        assert_eq(0x00000000u, pixel_at(x, 15));
    }
    /* Col 1 underline must now be ACTIVE (white) */
    for (int x = 8; x < 16; ++x) {
        assert_eq(0xFFFFFFFFu, pixel_at(x, 15));
    }

    assert_true(check_sentinels());
    term_core_free(&g_core);
    free(gfx);
}

/* ── Test 4: Cursor Hide and Screen Clear ────────────────────── */
TEST_FUNC(test_render_cursor_hide_and_clear)
{
    TEST_SUITE("terminal_render: cursor hide (?25l) and 100% black clear screen");
    load_font_once();
    gfx_handle_t *gfx = create_mock_handle();

    term_core_init(&g_core, TEST_ROWS, TEST_COLS);
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);

    feed_str(&g_core, "HELLO");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Hide cursor: \e[?25l */
    feed_str(&g_core, "\x1b[?25l");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Clear screen: \e[2J\e[H */
    feed_str(&g_core, "\x1b[2J\x1b[H");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Because cursor is hidden, 100% of buffer pixels must be 0x00000000 */
    for (int y = 0; y < VIEW_H; ++y) {
        for (int x = 0; x < VIEW_W; ++x) {
            assert_eq(0x00000000u, pixel_at(x, y));
        }
    }

    /* Restore cursor: \e[?25h at (0, 0) */
    feed_str(&g_core, "\x1b[?25h");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Now only the bottom scanline of col 0 has white pixels */
    for (int x = 0; x < FONT_W; ++x) {
        assert_eq(0xFFFFFFFFu, pixel_at(x, 15));
    }
    for (int x = FONT_W; x < VIEW_W; ++x) {
        assert_eq(0x00000000u, pixel_at(x, 15));
    }

    assert_true(check_sentinels());
    term_core_free(&g_core);
    free(gfx);
}

/* ── Test 5: Alt-Screen Full Wipe and Restoration ────────────── */
TEST_FUNC(test_render_alt_screen_pixel_restoration)
{
    TEST_SUITE("terminal_render: ?1049h full wipe and ?1049l exact pixel restoration");
    load_font_once();
    gfx_handle_t *gfx = create_mock_handle();

    term_core_init(&g_core, TEST_ROWS, TEST_COLS);
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);

    /* Hide cursor to eliminate cursor position differences */
    feed_str(&g_core, "\x1b[?25l");
    feed_str(&g_core, "MAIN_TEXT");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Save snapshot of main screen buffer */
    uint32_t main_snapshot[VIEW_W * VIEW_H];
    memcpy(main_snapshot, g_buf.pixels, sizeof(main_snapshot));

    /* Verify main screen actually has pixels */
    bool has_white = false;
    for (int i = 0; i < VIEW_W * VIEW_H; ++i) {
        if (main_snapshot[i] == 0xFFFFFFFFu) has_white = true;
    }
    assert_true(has_white);

    /* Switch to alt screen: \e[?1049h */
    feed_str(&g_core, "\x1b[?1049h");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* In alt screen (clean blank), all pixels must be zeroed */
    for (int i = 0; i < VIEW_W * VIEW_H; ++i) {
        assert_eq(0x00000000u, g_buf.pixels[i]);
    }

    /* Type on alt screen */
    feed_str(&g_core, "ALT_DATA");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Alt screen is not equal to main snapshot */
    assert_true(memcmp(g_buf.pixels, main_snapshot, sizeof(main_snapshot)) != 0);

    /* Exit alt screen: \e[?1049l */
    feed_str(&g_core, "\x1b[?1049l");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Buffer must match main snapshot 100% */
    assert_eq(0, memcmp(g_buf.pixels, main_snapshot, sizeof(main_snapshot)));

    assert_true(check_sentinels());
    term_core_free(&g_core);
    free(gfx);
}

/* ── Test 6: Fault Injection ─────────────────────────────────── */
TEST_FUNC(test_render_fault_injection)
{
    TEST_SUITE("terminal_render: allocation failure and uninitialized guards");
    load_font_once();

    /* 1. NULL handle / core / font safety in term_render_flush */
    term_render_t r;
    memset(&r, 0, sizeof(r));
    term_render_flush(&r);
    term_render_clear(&r);
    assert_false(term_render_cursor_update(&r));

    /* 2. term_core_init allocation failure simulation */
    term_core_t broken_core;
    memset(&broken_core, 0, sizeof(broken_core));
    /* Leave buffers NULL */
    term_render_init(&r, NULL, g_font, &broken_core, 0xFFFFFFFFu, 0x00000000u);
    term_render_flush(&r);
    assert_false(term_render_cursor_update(&r));
}

/* ── Test 7: Pixel-Level Translation Scrolling ───────────────── */
TEST_FUNC(test_render_pixel_scroll_single_line)
{
    TEST_SUITE("terminal_render: pixel translation scroll single line");
    load_font_once();

    gfx_handle_t *gfx = create_mock_handle();
    term_core_init(&g_core, TEST_ROWS, TEST_COLS);
    g_core.cursor_visible = false; /* Disable cursor underline so full row snapshot matches */
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);

    /* Write 'A' at row 0 and 'B' at row 3 (bottom row of 4-row screen) */
    term_core_input(&g_core, 'A');
    term_core_input(&g_core, '\n'); /* row 1 */
    term_core_input(&g_core, '\n'); /* row 2 */
    term_core_input(&g_core, '\n'); /* row 3 */
    term_core_input(&g_core, 'B');

    term_render_flush(&g_render);

    /* Snapshot row 3 pixels containing 'B' */
    uint32_t row3_snapshot[FONT_H * VIEW_W];
    memcpy(row3_snapshot, &g_buf.pixels[3 * FONT_H * VIEW_W], sizeof(row3_snapshot));

    /* Next newline triggers scroll_active: row 3 shifts to row 2 */
    term_core_input(&g_core, '\n');
    assert_eq(1, g_core.scroll_lines_pending);
    term_core_input(&g_core, 'C');

    term_render_flush(&g_render);
    assert_eq(0, g_core.scroll_lines_pending);

    /* Row 2 pixels must now match the snapshot of 'B' from row 3 */
    assert_eq(0, memcmp(&g_buf.pixels[2 * FONT_H * VIEW_W], row3_snapshot, sizeof(row3_snapshot)));

    assert_true(check_sentinels());
    term_core_free(&g_core);
    free(gfx);
}

TEST_FUNC(test_render_pixel_scroll_cursor_cleanup)
{
    TEST_SUITE("terminal_render: pixel scroll cleans up shifted cursor underline");
    load_font_once();

    gfx_handle_t *gfx = create_mock_handle();
    term_core_init(&g_core, TEST_ROWS, TEST_COLS);
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);

    /* Move cursor to bottom row (row 3, col 2) and render cursor underline */
    term_core_input(&g_core, '\n');
    term_core_input(&g_core, '\n');
    term_core_input(&g_core, '\n');
    term_core_input(&g_core, 'X');
    term_core_input(&g_core, 'Y');
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Cursor underline is drawn at (core.row+1)*font_h - 1, which is y=63 */
    uint32_t baseline_y = 4 * FONT_H - 1; /* y=63 */
    assert_eq(0xFFFFFFFFu, g_buf.pixels[baseline_y * VIEW_W + 2 * FONT_W]);

    /* Now scroll up by 1 row without cursor at col 2: cursor moves to row 3, col 0 */
    term_core_input(&g_core, '\n');
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* The old underline was shifted up to y = 63 - 16 = 47.
     * Because term_render_flush dirties the shifted cursor cell,
     * cell (row 2, col 2) was redrawn with its glyph, clearing the shifted underline at y=47! */
    uint32_t shifted_baseline_y = 3 * FONT_H - 1; /* y=47 */
    assert_eq(0x00000000u, g_buf.pixels[shifted_baseline_y * VIEW_W + 2 * FONT_W]);

    /* And new cursor underline is at bottom row col 0 (y=63) */
    assert_eq(0xFFFFFFFFu, g_buf.pixels[baseline_y * VIEW_W + 0]);

    assert_true(check_sentinels());
    term_core_free(&g_core);
    free(gfx);
}

TEST_FUNC(test_glyph_8wide_fastpath)
{
    TEST_SUITE("libgfx: 8-wide glyph fast-path MSB-first bit unpacking");
    gfx_handle_t *gfx = create_mock_handle();

    /* 2 rows, mask_stride = 1 byte:
     * row 0: 0xA5 = 10100101b
     * row 1: 0x3C = 00111100b */
    const uint8_t mask[2] = { 0xA5u, 0x3Cu };
    uint32_t fg = 0xFFFFFFFFu;
    uint32_t bg = 0x11223344u;

    gfx_draw_glyph(gfx, 8, 4, mask, 1, 8, 2, fg, bg, true);

    /* Row 0 at y=4, x=8..15 */
    uint32_t expected_r0[8] = { fg, bg, fg, bg, bg, fg, bg, fg };
    for (int i = 0; i < 8; i++) {
        assert_eq(expected_r0[i], g_buf.pixels[4 * VIEW_W + 8 + i]);
    }

    /* Row 1 at y=5, x=8..15 */
    uint32_t expected_r1[8] = { bg, bg, fg, fg, fg, fg, bg, bg };
    for (int i = 0; i < 8; i++) {
        assert_eq(expected_r1[i], g_buf.pixels[5 * VIEW_W + 8 + i]);
    }

    /* Test transparent background (bg_opaque = false) */
    const uint8_t mask_trans[1] = { 0x81u }; /* 10000001b */
    gfx_draw_glyph(gfx, 8, 4, mask_trans, 1, 8, 1, 0x77777777u, bg, false);

    assert_eq(0x77777777u, g_buf.pixels[4 * VIEW_W + 8]); /* bit 7 set to new fg */
    assert_eq(bg,          g_buf.pixels[4 * VIEW_W + 9]); /* bit 6 transparent -> keeps old bg */
    assert_eq(0x77777777u, g_buf.pixels[4 * VIEW_W + 15]); /* bit 0 set to new fg */

    assert_true(check_sentinels());
    free(gfx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_font_validation),
    TEST_ENTRY(test_render_high_bit_chars),
    TEST_ENTRY(test_render_cursor_underline_movement),
    TEST_ENTRY(test_render_cursor_hide_and_clear),
    TEST_ENTRY(test_render_alt_screen_pixel_restoration),
    TEST_ENTRY(test_render_fault_injection),
    TEST_ENTRY(test_render_pixel_scroll_single_line),
    TEST_ENTRY(test_render_pixel_scroll_cursor_cleanup),
    TEST_ENTRY(test_glyph_8wide_fastpath),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
