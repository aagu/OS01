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

/* ── Helpers for damage-set tests (Task 4) ──────────────────────────── */

/* Set up a fresh render, feed the bytes that mark the cursor off and
 * put the renderer in a known initial state.  Caller is responsible for
 * freeing the core + handle. */
static void fresh_render_no_cursor(int rows, int cols)
{
    term_core_init(&g_core, rows, cols);
    gfx_handle_t *gfx = create_mock_handle();
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);
    /* Hide the cursor so cell damage is isolated from cursor underline
     * damage in the assertions below. */
    feed_str(&g_core, "\x1b[?25l");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);
    /* The init+feed+flush path sets pending_damage.full; ack once so the
     * next flush records ONLY the damage we drive. */
    term_damage_t d;
    term_render_get_damage(&g_render, &d);
    term_render_ack_damage(&g_render);
}

/* Direct dirty-bitmap writer: mark one cell at (row, col) dirty without
 * going through the parser.  Used to construct precise damage fixtures
 * independent of the VT100 escape semantics. */
static void mark_one(int row, int col, uint8_t glyph)
{
    term_cell_t *scr = term_core_screen(&g_core);
    scr[row * g_core.cols + col].glyph = glyph;
    g_core.dirty[row * g_core.cols + col] = true;
}

/* Compare one rect (order-insensitive helper).  Returns true if any rect
 * in `d` matches (x, y, w, h). */
static bool contains_rect(const term_damage_t *d, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h)
{
    for (uint32_t i = 0; i < d->count; ++i) {
        if (d->rects[i].x == x && d->rects[i].y == y &&
            d->rects[i].w == w && d->rects[i].h == h) {
            return true;
        }
    }
    return false;
}

/* Sort a damage copy by (y, x) before comparison (matches the renderer's
 * post-flush final order). */
static int cmp_rect_yx(const void *a, const void *b)
{
    const term_damage_rect_t *ra = (const term_damage_rect_t *)a;
    const term_damage_rect_t *rb = (const term_damage_rect_t *)b;
    if (ra->y != rb->y) return (ra->y < rb->y) ? -1 : 1;
    if (ra->x != rb->x) return (ra->x < rb->x) ? -1 : 1;
    return 0;
}
static void sort_damage_copy(term_damage_t *d)
{
    /* qsort is in libc; the host test links libc via $(LIBC_OBJS). */
    extern void qsort(void *base, size_t nmemb, size_t size,
                      int (*compar)(const void *, const void *));
    qsort(d->rects, d->count, sizeof(d->rects[0]), cmp_rect_yx);
}

/* ── Test 11: damage basic set and merge (Task 4 brief step 1) ──────── */
TEST_FUNC(test_damage_basic_set_and_merge)
{
    TEST_SUITE("terminal_render: damage set — basic row spans, merge, retain, ack");
    load_font_once();
    /* 4 rows × 10 cols at 8×16 → 80×64 view.  We pick rows 1 and 3 to
     * ensure they are not merged into a single bbox; same-row col 2 +
     * col 4 must merge into one span of width 24; adjacent same-span
     * rows must merge into h=32. */
    const int ROWS = 4, COLS = 10;
    fresh_render_no_cursor(ROWS, COLS);

    term_damage_t d;

    /* 1. Two cells on different rows -> two separate rects in damage. */
    mark_one(1, 2, 'A');
    mark_one(3, 7, 'B');
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    sort_damage_copy(&d);
    assert_false(d.full);                 /* not promoted to full */
    assert_eq(2u, d.count);               /* distinct rows => no merge */
    /* row 1 (y=16, height 16) col 2 (x=16, w=8) */
    assert_true(contains_rect(&d, 16u, 16u, 8u, 16u));
    /* row 3 (y=48) col 7 (x=56) */
    assert_true(contains_rect(&d, 56u, 48u, 8u, 16u));
    /* Bbox (16,16)→(64,64) would be wrong because that's 4 rows tall. */
    assert_false(contains_rect(&d, 16u, 16u, 48u, 48u));

    /* 2. Verify that core dirty bitmap is cleared, but pending damage
     * is NOT cleared by the flush. */
    assert_false(term_core_is_dirty(&g_core, 1, 2));
    assert_false(term_core_is_dirty(&g_core, 3, 7));

    /* 3. Same-row col 2 + col 4 should merge into one span of width 24
     * (col 2 x=16, col 4 x=32, width from col 2 to 5 exclusive = 24). */
    term_render_ack_damage(&g_render);
    mark_one(0, 2, 'C');
    mark_one(0, 4, 'D');
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    sort_damage_copy(&d);
    assert_false(d.full);
    assert_eq(1u, d.count);
    assert_eq(16u, d.rects[0].x);
    assert_eq(0u,  d.rects[0].y);
    assert_eq(24u, d.rects[0].w);
    assert_eq(16u, d.rects[0].h);

    /* 4. Adjacent rows with identical span must merge vertically: rows
     * 1 and 2 each have col 2 dirty → single rect with h=32, x=16,
     * w=8. */
    term_render_ack_damage(&g_render);
    mark_one(1, 2, 'E');
    mark_one(2, 2, 'F');
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    sort_damage_copy(&d);
    assert_eq(1u, d.count);
    assert_eq(16u, d.rects[0].x);
    assert_eq(16u, d.rects[0].y);   /* row 1 starts at y=16 */
    assert_eq(8u,  d.rects[0].w);
    assert_eq(32u, d.rects[0].h);    /* rows 1+2 = 32 px tall */

    /* 5. A second flush with NO new changes must NOT clear pending. */
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    assert_eq(1u, d.count);
    assert_eq(8u, d.rects[0].w);
    assert_eq(32u, d.rects[0].h);
    /* And ack clears. */
    term_render_ack_damage(&g_render);
    term_render_get_damage(&g_render, &d);
    assert_false(d.full);
    assert_eq(0u, d.count);

    /* 6. Initial state after init must be full=true.  fresh_render_*()
     * acked it; a fresh init without acking would show full=true. */
    term_core_free(&g_core);
    free(g_render.gfx);
    gfx_handle_t *gfx2 = create_mock_handle();
    term_core_t core2;
    term_core_init(&core2, ROWS, COLS);
    term_render_t r2;
    term_render_init(&r2, gfx2, g_font, &core2, 0xFFFFFFFFu, 0x00000000u);
    term_render_get_damage(&r2, &d);
    assert_true(d.full);
    assert_eq(0u, d.count);
    term_core_free(&core2);
    free(gfx2);
}

/* ── Test 12: damage retains prior spans when one row updates again ─── */
/* Helper: is the (px, py) pixel covered by any rect in d? */
static bool pixel_is_covered(const term_damage_t *d, uint32_t px, uint32_t py)
{
    for (uint32_t i = 0; i < d->count; ++i) {
        const term_damage_rect_t *r = &d->rects[i];
        if (px >= r->x && px < r->x + r->w &&
            py >= r->y && py < r->y + r->h) {
            return true;
        }
    }
    return false;
}
/* Helper: every pixel of cell (crow, ccol) at 8x16 font, width=8W cols,
 * is in some rect. */
static bool cell_is_covered(const term_damage_t *d, int crow, int ccol,
                            uint32_t font_w, uint32_t font_h)
{
    for (uint32_t y = (uint32_t)crow * font_h;
         y < (uint32_t)(crow + 1) * font_h; ++y) {
        for (uint32_t x = (uint32_t)ccol * font_w;
             x < (uint32_t)(ccol + 1) * font_w; ++x) {
            if (!pixel_is_covered(d, x, y)) return false;
        }
    }
    return true;
}

TEST_FUNC(test_damage_merged_rows_updated_again)
{
    TEST_SUITE("terminal_render: damage merge keeps all old/new pixel spans");
    load_font_once();
    const int ROWS = 4, COLS = 10;
    fresh_render_no_cursor(ROWS, COLS);

    term_damage_t d;

    /* Merge two rows then update one of them; the union of rects must
     * cover ALL old + new pixels (cells (1,2), (2,2), and (2,3)). */
    mark_one(1, 2, 'X');
    mark_one(2, 2, 'Y');
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    /* First flush: rows 1+2 col 2 merged into (16,16,8,32). */
    assert_eq(1u, d.count);
    assert_eq(32u, d.rects[0].h);

    /* Re-dirty a non-mergeable new cell: row 2 col 3. */
    g_core.dirty[2 * g_core.cols + 3] = true;
    term_core_screen(&g_core)[2 * g_core.cols + 3].glyph = 'Z';
    term_render_flush(&g_render);

    term_render_get_damage(&g_render, &d);
    /* Cell coverage invariant: every pixel of cells (1,2), (2,2), (2,3)
     * must still be inside some rect.  The old merged rect spans cols
     * 2 of rows 1+2; the new cell lies at col 3 of row 2 (different x
     * range), so we end up with two rects, but their UNION must cover
     * all changed pixels. */
    assert_false(d.full);
    assert_true(d.count >= 2u);
    assert_true(cell_is_covered(&d, 1, 2, FONT_W, FONT_H));
    assert_true(cell_is_covered(&d, 2, 2, FONT_W, FONT_H));
    assert_true(cell_is_covered(&d, 2, 3, FONT_W, FONT_H));

    term_core_free(&g_core);
    free(g_render.gfx);
}

/* ── Test 13: 65 non-mergeable spans promote to full ────────────────── */
TEST_FUNC(test_damage_overflow_promotes_full)
{
    TEST_SUITE("terminal_render: 65 non-mergeable spans promote to full=true");
    load_font_once();
    /* We need ≥130 char rows: use a 200-row core.  But TEST_ROWS=4 by
     * default and we want each new row to have a unique non-mergeable
     * span.  Use 132 rows × 2 cols so interleaving x with a gap row
     * gives non-mergeable spans. */
    const int ROWS = 132, COLS = 2;
    fresh_render_no_cursor(ROWS, COLS);

    term_damage_t d;

    /* Mark alternating (col 0 on row r, col 1 on row r+1) — each row
     * dirty so we get 132 single-cell spans which cannot merge across
     * rows (they're different y/h, different x ranges).  Each row has
     * either col 0 or col 1 dirty (NOT both), so even same-y merges
     * are impossible (only one rect per row). */
    for (int r = 0; r < ROWS; ++r) {
        int col = (r & 1);  /* alternate 0,1,0,1,... */
        mark_one(r, col, 'A' + (r % 26));
    }
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);

    /* The first 64 should land in the list.  The 65th arrives when we
     * add one more row past 128.  Since we had 132 dirty cells, the
     * 65th forces promotion to full and discards the list.  Verify
     * after the fact that full=true and list is gone. */
    assert_true(d.full);
    assert_eq(0u, d.count);

    /* Even after additional spans arrive, the full state is sticky. */
    mark_one(0, 0, 'Z');
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    assert_true(d.full);

    term_render_ack_damage(&g_render);
    term_render_get_damage(&g_render, &d);
    assert_false(d.full);
    assert_eq(0u, d.count);

    term_core_free(&g_core);
    free(g_render.gfx);
}

/* ── Test 14: cursor draw and erase contribute damage ──────────────── */
TEST_FUNC(test_damage_cursor_old_and_new_and_erase)
{
    TEST_SUITE("terminal_render: cursor underline adds damage even with no cell change");
    load_font_once();
    const int ROWS = 4, COLS = 10;
    fresh_render_no_cursor(ROWS, COLS);

    term_damage_t d;

    /* 1. Cursor movement with no cell content change.  After init the
     * cursor is at (0, 0) hidden.  We move it right with CURSOR-RIGHT
     * — old cell (0, 0) and new cell (0, 1) both enter damage. */
    feed_str(&g_core, "\x1b[C");
    bool changed = term_render_cursor_update(&g_render);
    assert_true(changed);  /* col moved 0 → 1 */
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    sort_damage_copy(&d);
    /* Old cell x=0 w=8 + new cell x=8 w=8 → same y/h, adjacent x.
     * Merge horizontally → single rect (0, 0, 16, 16). */
    assert_false(d.full);
    assert_eq(1u, d.count);
    assert_eq(0u,  d.rects[0].x);
    assert_eq(0u,  d.rects[0].y);
    assert_eq(16u, d.rects[0].w);
    assert_eq(16u, d.rects[0].h);

    /* 2. Hiding the cursor (visibility toggle without move) erases the
     * old underline — must add damage.  First show the cursor (so we
     * can later hide it) without moving it. */
    term_render_ack_damage(&g_render);
    feed_str(&g_core, "\x1b[?25h");   /* show */
    assert_true(term_render_cursor_update(&g_render));
    term_render_flush(&g_render);
    term_render_ack_damage(&g_render);
    /* Now hide: visibility toggle from true → false WITHOUT cursor move. */
    feed_str(&g_core, "\x1b[?25l");
    bool hid = term_render_cursor_update(&g_render);
    assert_true(hid);  /* visibility flipped */
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    sort_damage_copy(&d);
    /* Last cursor position was (0, 1) — that single cell is the damage
     * target (cursor_update marks it dirty to erase the underline). */
    assert_false(d.full);
    assert_eq(1u, d.count);
    assert_eq(8u,  d.rects[0].x);
    assert_eq(0u,  d.rects[0].y);
    assert_eq(8u,  d.rects[0].w);
    assert_eq(16u, d.rects[0].h);

    term_core_free(&g_core);
    free(g_render.gfx);
}

/* ── Test 15: term_render_clear promotes to full ────────────────────── */
TEST_FUNC(test_damage_clear_promotes_full)
{
    TEST_SUITE("terminal_render: term_render_clear promotes pending damage to full");
    load_font_once();
    const int ROWS = 4, COLS = 10;
    fresh_render_no_cursor(ROWS, COLS);

    term_damage_t d;

    /* First add some real damage. */
    mark_one(1, 0, 'A');
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    assert_false(d.full);
    assert_eq(1u, d.count);

    /* Now call clear; damage must collapse to full. */
    term_render_clear(&g_render);
    term_render_get_damage(&g_render, &d);
    assert_true(d.full);
    assert_eq(0u, d.count);

    /* Even after ack, a subsequent cell render does NOT add a partial;
     * a fresh full view has nothing dirty yet so flush sees nothing to
     * do — pending stays empty (NOT full). */
    term_render_ack_damage(&g_render);
    term_render_flush(&g_render);
    term_render_get_damage(&g_render, &d);
    assert_false(d.full);
    assert_eq(0u, d.count);

    term_core_free(&g_core);
    free(g_render.gfx);
}

/* ── Test 16: scroll promotes pending to full ──────────────────────── */
TEST_FUNC(test_damage_scroll_promotes_full)
{
    TEST_SUITE("terminal_render: pixel scroll promotes pending damage to full");
    load_font_once();
    const int ROWS = 4, COLS = 10;

    /* Drive the renderer through a real scroll so we keep cursor hidden
     * and the render pre-flushed. */
    term_core_init(&g_core, ROWS, COLS);
    gfx_handle_t *gfx = create_mock_handle();
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);
    feed_str(&g_core, "\x1b[?25l");   /* hide cursor */
    feed_str(&g_core, "AB");           /* 2 cells on row 0 */
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);

    /* Ack first; the next scroll-induced flush must promote to full.
     * To force a pixel scroll we must write past the last row: type
     * enough text with newlines to push content 5 rows deep (one past
     * the 4-row screen), so the parser invokes scroll_active and sets
     * scroll_lines_pending. */
    term_render_ack_damage(&g_render);
    /* Fill rows 0..3 with content; cursor will be at (col-last, 3). */
    feed_str(&g_core, "XXXX\nYYYY\nZZZZ\nWWWW\n");
    /* The trailing \n pushes the parser to scroll once (rows=4 → row
     * becomes 4 → scroll_active: rows shift up, scroll_lines_pending=1). */
    term_render_flush(&g_render);

    term_damage_t d;
    term_render_get_damage(&g_render, &d);
    assert_true(d.full);
    assert_eq(0u, d.count);

    term_core_free(&g_core);
    free(gfx);
}

/* ── Test 17: alt-screen toggle promotes to full ───────────────────── */
TEST_FUNC(test_damage_alt_screen_promotes_full)
{
    TEST_SUITE("terminal_render: alt-screen toggle promotes pending damage to full");
    load_font_once();
    const int ROWS = 4, COLS = 10;

    term_core_init(&g_core, ROWS, COLS);
    gfx_handle_t *gfx = create_mock_handle();
    term_render_init(&g_render, gfx, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);
    feed_str(&g_core, "\x1b[?25l");    /* hide cursor */
    feed_str(&g_core, "MAIN");
    term_render_cursor_update(&g_render);
    term_render_flush(&g_render);
    term_render_ack_damage(&g_render);

    /* Enter alt screen — flush must promote. */
    feed_str(&g_core, "\x1b[?1049h");
    term_render_flush(&g_render);
    term_damage_t d;
    term_render_get_damage(&g_render, &d);
    assert_true(d.full);
    assert_eq(0u, d.count);

    term_core_free(&g_core);
    free(gfx);
}

/* ── Test 18: reinit (renderer swap on rebind) resets to full ───────── */
TEST_FUNC(test_damage_reinit_resets_to_full)
{
    TEST_SUITE("terminal_render: term_render_init re-arms pending as full");
    load_font_once();
    const int ROWS = 4, COLS = 10;
    fresh_render_no_cursor(ROWS, COLS);

    /* Add a non-full state then re-init the same struct: full must
     * come back. */
    mark_one(0, 0, 'A');
    term_render_flush(&g_render);
    term_damage_t d;
    term_render_get_damage(&g_render, &d);
    assert_false(d.full);

    gfx_handle_t *gfx2 = create_mock_handle();
    term_render_init(&g_render, gfx2, g_font, &g_core, 0xFFFFFFFFu, 0x00000000u);
    term_render_get_damage(&g_render, &d);
    assert_true(d.full);
    assert_eq(0u, d.count);

    term_core_free(&g_core);
    free(g_render.gfx);
}

/* ── Test 19: sort order is (y asc, x asc) ──────────────────────────── */
TEST_FUNC(test_damage_sort_order_yx)
{
    TEST_SUITE("terminal_render: damage list sorted by (y asc, x asc)");
    load_font_once();
    const int ROWS = 4, COLS = 10;
    fresh_render_no_cursor(ROWS, COLS);

    /* Mark three cells in deliberately inverted order. */
    mark_one(2, 1, 'B');
    mark_one(0, 2, 'A');
    mark_one(2, 0, 'C');
    term_render_flush(&g_render);

    term_damage_t d;
    term_render_get_damage(&g_render, &d);
    /* The renderer must already be sorted by (y, x).  d.rects[0]
     * should be row 0 (y=0); d.rects[1] should be row 2 (y=2*FONT_H).
     * Within row 2, the column 0 cell comes before column 1 (x asc). */
    assert_eq(2u, d.count);
    assert_eq(0u,                       d.rects[0].y);
    assert_eq((uint32_t)(2 * FONT_H),   d.rects[1].y);
    assert_eq((uint32_t)(2 * FONT_W),   d.rects[0].x);   /* col 2 of row 0 */
    /* Within row 2 (rects[1]) col 0 < col 1 → x_asc. */
    assert_true(d.rects[1].x < (uint32_t)(1 * FONT_W + d.rects[1].w));

    term_core_free(&g_core);
    free(g_render.gfx);
}

/* ── Test 20: blank-overwrite on a single row stays partial ────────── */
TEST_FUNC(test_damage_blank_overwrite_stays_partial)
{
    TEST_SUITE("terminal_render: blank-overwrite on a single row stays partial");
    load_font_once();
    const int ROWS = 4, COLS = 10;
    fresh_render_no_cursor(ROWS, COLS);

    /* Fill row 0 with 'X' on every cell, then flush+ack.
     * mark_one() bypasses the parser, so the cursor stays at (0, 0)
     * (it was set there by fresh_render_no_cursor and not moved). */
    for (int c = 0; c < COLS; ++c) mark_one(0, c, 'X');
    term_render_flush(&g_render);
    term_render_ack_damage(&g_render);

    /* \e[K (EIL param=0) erases from cursor to EOL.  Cursor at (0, 0)
     * erases ALL 10 cells of row 0; each cell changes from 'X' to ' '
     * so each is marked dirty by the core. */
    feed_str(&g_core, "\x1b[K");
    term_render_flush(&g_render);
    term_damage_t d;
    term_render_get_damage(&g_render, &d);
    sort_damage_copy(&d);
    /* Whole row 0 affected, single contiguous span. */
    assert_false(d.full);
    assert_eq(1u, d.count);
    assert_eq(0u, d.rects[0].y);
    assert_eq((uint32_t)FONT_H, d.rects[0].h);
    assert_eq((uint32_t)(COLS * FONT_W), d.rects[0].w);

    term_core_free(&g_core);
    free(g_render.gfx);
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
    TEST_ENTRY(test_damage_basic_set_and_merge),
    TEST_ENTRY(test_damage_merged_rows_updated_again),
    TEST_ENTRY(test_damage_overflow_promotes_full),
    TEST_ENTRY(test_damage_cursor_old_and_new_and_erase),
    TEST_ENTRY(test_damage_clear_promotes_full),
    TEST_ENTRY(test_damage_scroll_promotes_full),
    TEST_ENTRY(test_damage_alt_screen_promotes_full),
    TEST_ENTRY(test_damage_reinit_resets_to_full),
    TEST_ENTRY(test_damage_sort_order_yx),
    TEST_ENTRY(test_damage_blank_overwrite_stays_partial),
    TEST_ENTRY(test_glyph_8wide_fastpath),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
