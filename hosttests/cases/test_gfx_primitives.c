/*
 * test_gfx_primitives.c — libgfx 2D primitives (Task 4)
 *
 * Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
 * §4 ("ABI and userspace API"): point / horizontal line / vertical
 * line / integer Bresenham line (8 octants, both endpoints) / rect
 * / filled rect / opaque sprite blit / color-key sprite blit (key=0
 * is transparent) / mask-bit sprite blit (MSB = leftmost).  All
 * primitives intersect the configured view AND the library-local
 * clip rectangle, silently no-op on empty intersection.  Every
 * coordinate pair is in VIEW-LOCAL space; INT32_MIN/MAX endpoints
 * must not overflow and must not loop forever.  Drawing happens
 * into the handle's private pixels buffer; the test reaches the
 * buffer through the in-tree internal header so the test exercises
 * the SAME code that /bin/test_gfx (Task 5) uses.
 *
 * Test buffer layout (matches spec wording: 7×5 buffer + sentinel
 * word before/after):
 *
 *   struct {
 *       uint32_t sentinel_before[8];   // one row worth of sentinels
 *       uint32_t pixels[BUF_W * BUF_H]; // 7×5 = 35 buffer pixels
 *       uint32_t sentinel_after[8];    // one row worth of sentinels
 *   };
 *
 * The C standard guarantees struct member layout, so sentinel_before
 * lives immediately BEFORE pixels in memory and sentinel_after lives
 * immediately AFTER.  Any out-of-bounds pixel write by the lib (e.g.,
 * y < 0 → writes into sentinel_before; y >= BUF_H → writes into
 * sentinel_after) clobbers a sentinel and the test detects it.
 *
 * The view the lib sees is BUF_W × BUF_H = 7 × 5 (set via
 * info.width and info.height when the test constructs the handle).
 * libgfx indexes pixels as `pixels[y * info.width + x]`; with
 * info.width = 7, the buffer matches that layout exactly.
 */

#include "test_framework.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gfx.h>
#include <uapi/gfx.h>

/* internal.h is NOT installed to the sysroot; the test reads the
 * handle layout directly through the in-tree include path. */
#include "internal.h"

/* ── Buffer geometry ────────────────────────────────────────── */
#define SENTINEL 0xDEADBEEFu

#define BUF_W 7    /* view width  (= info.width) */
#define BUF_H 5    /* view height (= info.height) */

/* The sentinel slots need to cover one full row of width BUF_W
 * (7 pixels) for the y-axis overflow checks to land somewhere. */
#define SENTINEL_SLOTS 8

typedef struct {
    uint32_t sentinel_before[SENTINEL_SLOTS];
    uint32_t pixels[BUF_W * BUF_H];
    uint32_t sentinel_after[SENTINEL_SLOTS];
} test_buffer_t;

static test_buffer_t test_mem;

static inline uint32_t *view_at(int32_t y, int32_t x)
{
    return &test_mem.pixels[(size_t)y * BUF_W + (size_t)x];
}

static void reset_state(void)
{
    /* Fill sentinels first; clear the buffer to zero.  Drawing a
     * color of 0 will not accidentally match a sentinel. */
    for (size_t i = 0; i < SENTINEL_SLOTS; ++i) {
        test_mem.sentinel_before[i] = SENTINEL;
        test_mem.sentinel_after[i] = SENTINEL;
    }
    for (size_t i = 0; i < BUF_W * BUF_H; ++i) {
        test_mem.pixels[i] = 0u;
    }
}

static int sentinels_intact(void)
{
    for (size_t i = 0; i < SENTINEL_SLOTS; ++i) {
        if (test_mem.sentinel_before[i] != SENTINEL) return 0;
        if (test_mem.sentinel_after[i]  != SENTINEL) return 0;
    }
    return 1;
}

/* Build a freshly opened handle backed by the test buffer.  The
 * handle's `pixels` pointer is forced to point at test_mem.pixels;
 * info.width = BUF_W, info.height = BUF_H match the buffer's
 * contiguous 7×5 layout. */
static gfx_handle_t *open_test_handle(void)
{
    gfx_handle_t *h = (gfx_handle_t *)calloc(1, sizeof(*h));
    /* No fd — the test only exercises the in-memory drawing path;
     * gfx_present would need the fd, but no test in this file
     * calls it. */
    h->fd = -1;
    h->info.width = BUF_W;
    h->info.height = BUF_H;
    h->info.stride = BUF_W * 4u;
    h->info.format = GFX_FORMAT_RGB32;
    h->pixels = test_mem.pixels;
    /* Default clip = full view. */
    h->clip_x = 0;
    h->clip_y = 0;
    h->clip_w = BUF_W;
    h->clip_h = BUF_H;
    return h;
}

/* ── pixel ────────────────────────────────────────────────── */
TEST_FUNC(test_pixel_inside_view)
{
    TEST_SUITE("pixel: inside view writes the color");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_pixel(h, 3, 2, 0xAABBCCDDu);
    assert_eq((uint32_t)0xAABBCCDDu, *view_at(2, 3));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_pixel_outside_view_no_op)
{
    TEST_SUITE("pixel: outside view is a no-op");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_pixel(h, -5, 2, 0x11111111u);
    gfx_pixel(h, 100, 2, 0x22222222u);
    gfx_pixel(h, 3, -5, 0x33333333u);
    gfx_pixel(h, 3, 100, 0x44444444u);
    /* The buffer is untouched. */
    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_pixel_respects_clip)
{
    TEST_SUITE("pixel: respects clip rectangle");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_set_clip(h, 2, 1, 3, 3);  /* x ∈ [2, 5), y ∈ [1, 4) */
    gfx_pixel(h, 1, 1, 0xAAAAAAAAu);  /* inside view, outside clip */
    gfx_pixel(h, 2, 1, 0xBBBBBBBBu);  /* inside clip */
    gfx_pixel(h, 5, 1, 0xCCCCCCCCu);  /* inside view, outside clip */
    gfx_pixel(h, 2, 0, 0xDDDDDDDDu);  /* outside clip */
    assert_eq(0u,                   *view_at(1, 1));
    assert_eq((uint32_t)0xBBBBBBBBu, *view_at(1, 2));
    assert_eq(0u,                   *view_at(1, 5));
    assert_eq(0u,                   *view_at(0, 2));
    assert_true(sentinels_intact());
    free(h);
}

/* ── hline / vline ─────────────────────────────────────────── */
TEST_FUNC(test_hline_horizontal_axis)
{
    TEST_SUITE("hline: writes row y across [x, x+w)");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_hline(h, 1, 2, 4, 0x12345678u);  /* y=2, x=1..4 inclusive */
    for (int32_t x = 1; x <= 4; ++x) {
        assert_eq((uint32_t)0x12345678u, *view_at(2, x));
    }
    /* Unset columns stay zero */
    assert_eq(0u, *view_at(2, 0));
    assert_eq(0u, *view_at(2, 5));
    /* Other rows untouched */
    for (int32_t y = 0; y < BUF_H; ++y) {
        if (y == 2) continue;
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_hline_clipped_left_and_right)
{
    TEST_SUITE("hline: clipped to view");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* Request spans (-3, +5) at y=3 → visible is [0, 5) on row 3. */
    gfx_hline(h, -3, 3, 8, 0xCAFE0001u);
    for (int32_t x = 0; x <= 4; ++x) {
        assert_eq((uint32_t)0xCAFE0001u, *view_at(3, x));
    }
    /* Columns 5..6 untouched */
    assert_eq(0u, *view_at(3, 5));
    assert_eq(0u, *view_at(3, 6));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_vline_clipped_top_and_bottom)
{
    TEST_SUITE("vline: clipped to view");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_vline(h, 4, -2, 12, 0xBEEF0002u);  /* spans [-2, +10), view y ∈ [0, 5) */
    /* Visible y=0..4 at x=4 */
    for (int32_t y = 0; y < BUF_H; ++y) {
        assert_eq((uint32_t)0xBEEF0002u, *view_at(y, 4));
    }
    /* Other columns untouched */
    for (int32_t x = 0; x < BUF_W; ++x) {
        if (x == 4) continue;
        for (int32_t y = 0; y < BUF_H; ++y) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_hline_completely_outside)
{
    TEST_SUITE("hline: completely outside is a no-op");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_hline(h, 0, -10, 7, 0xDEAD0001u);  /* y above view */
    gfx_hline(h, 0, 100, 7, 0xDEAD0002u);  /* y below view */
    gfx_hline(h, -100, 2, 7, 0xDEAD0003u);  /* x entirely negative */
    gfx_hline(h, 100, 2, 7, 0xDEAD0004u);  /* x entirely > view */
    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

/* ── rect / fill_rect ──────────────────────────────────────── */
TEST_FUNC(test_rect_outline_only)
{
    TEST_SUITE("rect: outline only, interior zeroed");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_rect(h, 1, 0, 5, 3, 0xFEEDFACEu);  /* (x=1..5, y=0..2) */
    /* Top edge */
    for (int32_t x = 1; x <= 5; ++x) {
        assert_eq((uint32_t)0xFEEDFACEu, *view_at(0, x));
    }
    /* Bottom edge */
    for (int32_t x = 1; x <= 5; ++x) {
        assert_eq((uint32_t)0xFEEDFACEu, *view_at(2, x));
    }
    /* Left edge */
    for (int32_t y = 0; y <= 2; ++y) {
        assert_eq((uint32_t)0xFEEDFACEu, *view_at(y, 1));
    }
    /* Right edge */
    for (int32_t y = 0; y <= 2; ++y) {
        assert_eq((uint32_t)0xFEEDFACEu, *view_at(y, 5));
    }
    /* Single-row rect reduces to a single hline; the interior
     * cells of the larger rect must NOT be touched. */
    for (int32_t y = 1; y <= 1; ++y) {
        for (int32_t x = 2; x <= 4; ++x) {
            assert_eq(0u, *view_at(y, x));  /* interior stays zero */
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_fill_rect_full)
{
    TEST_SUITE("fill_rect: fills the entire buffer");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_fill_rect(h, 0, 0, BUF_W, BUF_H, 0xA5A5A5A5u);
    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq((uint32_t)0xA5A5A5A5u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_fill_rect_clipped)
{
    TEST_SUITE("fill_rect: clipped to view (negative and oversize coords)");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* Request spans (-3, -2) → (+5, +8).  Visible is the full
     * intersection with view (0, 0, 7, 5): from (0, 0) to (5, 5). */
    gfx_fill_rect(h, -3, -2, 8, 10, 0xCAFEBABEu);
    for (int32_t y = 0; y <= 4; ++y) {
        for (int32_t x = 0; x <= 4; ++x) {
            assert_eq((uint32_t)0xCAFEBABEu, *view_at(y, x));
        }
    }
    /* Last two columns and last row untouched */
    for (int32_t y = 0; y < BUF_H; ++y) {
        assert_eq(0u, *view_at(y, 5));
        assert_eq(0u, *view_at(y, 6));
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_fill_rect_zero_size_no_op)
{
    TEST_SUITE("fill_rect: zero w or h is a no-op");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_fill_rect(h, 0, 0, 0, 5, 0xBADD0001u);
    gfx_fill_rect(h, 0, 0, 5, 0, 0xBADD0002u);
    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

/* ── Bresenham line: 8 octants + extreme endpoints ─────────── */
TEST_FUNC(test_line_horizontal_octant)
{
    TEST_SUITE("line: horizontal octant (y constant)");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_line(h, 1, 2, 5, 2, 0x01020304u);
    for (int32_t x = 1; x <= 5; ++x) {
        assert_eq((uint32_t)0x01020304u, *view_at(2, x));
    }
    /* Both endpoints included */
    assert_eq((uint32_t)0x01020304u, *view_at(2, 1));
    assert_eq((uint32_t)0x01020304u, *view_at(2, 5));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_line_vertical_octant)
{
    TEST_SUITE("line: vertical octant (x constant)");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_line(h, 3, 0, 3, 4, 0x0A0B0C0Du);
    for (int32_t y = 0; y <= 4; ++y) {
        assert_eq((uint32_t)0x0A0B0C0Du, *view_at(y, 3));
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_line_diagonal_octants)
{
    /* Four diagonal lines crossing the same pixels — tested in
     * isolation per direction so each color is verifiable.  Each
     * line is drawn on a fresh buffer to avoid colour
     * overwrites. */
    {
        reset_state();
        gfx_handle_t *h = open_test_handle();
        gfx_line(h, 0, 0, 4, 4, 0x10101010u);
        for (int32_t k = 0; k <= 4; ++k) {
            assert_eq((uint32_t)0x10101010u, *view_at(k, k));
        }
        assert_eq((uint32_t)0x10101010u, *view_at(0, 0));
        assert_eq((uint32_t)0x10101010u, *view_at(4, 4));
        assert_true(sentinels_intact());
        free(h);
    }
    {
        reset_state();
        gfx_handle_t *h = open_test_handle();
        gfx_line(h, 4, 0, 0, 4, 0x20202020u);
        for (int32_t k = 0; k <= 4; ++k) {
            assert_eq((uint32_t)0x20202020u, *view_at(k, 4 - k));
        }
        assert_eq((uint32_t)0x20202020u, *view_at(0, 4));
        assert_eq((uint32_t)0x20202020u, *view_at(4, 0));
        assert_true(sentinels_intact());
        free(h);
    }
    {
        reset_state();
        gfx_handle_t *h = open_test_handle();
        gfx_line(h, 0, 4, 4, 0, 0x30303030u);
        for (int32_t k = 0; k <= 4; ++k) {
            assert_eq((uint32_t)0x30303030u, *view_at(4 - k, k));
        }
        assert_eq((uint32_t)0x30303030u, *view_at(4, 0));
        assert_eq((uint32_t)0x30303030u, *view_at(0, 4));
        assert_true(sentinels_intact());
        free(h);
    }
    {
        reset_state();
        gfx_handle_t *h = open_test_handle();
        gfx_line(h, 4, 4, 0, 0, 0x40404040u);
        for (int32_t k = 0; k <= 4; ++k) {
            assert_eq((uint32_t)0x40404040u, *view_at(4 - k, 4 - k));
        }
        assert_eq((uint32_t)0x40404040u, *view_at(4, 4));
        assert_eq((uint32_t)0x40404040u, *view_at(0, 0));
        assert_true(sentinels_intact());
        free(h);
    }
    TEST_SUITE("line: 4 diagonal octants, both endpoints");
}

TEST_FUNC(test_line_shallow_octants)
{
    TEST_SUITE("line: shallow octants (|dx| > |dy|, 4 directions)");
    reset_state();
    gfx_handle_t *h = open_test_handle();

    /* slope 1/2: from (0,0) to (6,3).  We only assert endpoints
     * — the inner distribution depends on the Bresenham convention. */
    gfx_line(h, 0, 0, 6, 3, 0xA1A1A1A1u);
    assert_eq((uint32_t)0xA1A1A1A1u, *view_at(0, 0));
    assert_eq((uint32_t)0xA1A1A1A1u, *view_at(3, 6));

    /* slope -1/2: from (6,0) to (0,3) */
    gfx_line(h, 6, 0, 0, 3, 0xB2B2B2B2u);
    assert_eq((uint32_t)0xB2B2B2B2u, *view_at(0, 6));
    assert_eq((uint32_t)0xB2B2B2B2u, *view_at(3, 0));

    /* slope 1/2 reversed */
    gfx_line(h, 6, 3, 0, 0, 0xC3C3C3C3u);
    assert_eq((uint32_t)0xC3C3C3C3u, *view_at(3, 6));
    assert_eq((uint32_t)0xC3C3C3C3u, *view_at(0, 0));

    /* slope -1/2 reversed */
    gfx_line(h, 0, 3, 6, 0, 0xD4D4D4D4u);
    assert_eq((uint32_t)0xD4D4D4D4u, *view_at(3, 0));
    assert_eq((uint32_t)0xD4D4D4D4u, *view_at(0, 6));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_line_steep_octants)
{
    TEST_SUITE("line: steep octants (|dy| > |dx|, 4 directions)");
    reset_state();
    gfx_handle_t *h = open_test_handle();

    /* slope 2: from (0,0) to (3,4). Endpoints are in the buffer. */
    gfx_line(h, 0, 0, 3, 4, 0x1A2B3C4Du);
    assert_eq((uint32_t)0x1A2B3C4Du, *view_at(0, 0));
    assert_eq((uint32_t)0x1A2B3C4Du, *view_at(4, 3));

    /* slope -2: from (3,0) to (0,4).  Endpoints. */
    gfx_line(h, 3, 0, 0, 4, 0x2A2A2A2Au);
    assert_eq((uint32_t)0x2A2A2A2Au, *view_at(0, 3));
    assert_eq((uint32_t)0x2A2A2A2Au, *view_at(4, 0));

    /* slope 2 reversed */
    gfx_line(h, 3, 4, 0, 0, 0x3A3A3A3Au);
    assert_eq((uint32_t)0x3A3A3A3Au, *view_at(4, 3));
    assert_eq((uint32_t)0x3A3A3A3Au, *view_at(0, 0));

    /* slope -2 reversed */
    gfx_line(h, 0, 4, 3, 0, 0x4A4A4A4Au);
    assert_eq((uint32_t)0x4A4A4A4Au, *view_at(4, 0));
    assert_eq((uint32_t)0x4A4A4A4Au, *view_at(0, 3));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_line_clipped_to_view)
{
    TEST_SUITE("line: long line clipped to view bounds");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* From far outside on both sides; only the visible segment
     * crosses the view.  At least one pixel inside must be drawn. */
    gfx_line(h, -3, 2, 8, 3, 0xABCDEF01u);
    int saw_color = 0;
    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            if (*view_at(y, x) == 0xABCDEF01u) saw_color = 1;
        }
    }
    assert_true(saw_color);
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_line_completely_outside)
{
    TEST_SUITE("line: completely outside is a no-op");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_line(h, -100, -100, -50, -50, 0x0BAD0001u);
    gfx_line(h, 100, 100, 200, 200, 0x0BAD0002u);
    gfx_line(h, -100, 100, 50, -100, 0x0BAD0003u);
    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_line_extreme_endpoints_no_overflow)
{
    TEST_SUITE("line: INT32_MIN/MAX endpoints do not overflow or loop");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* The canonical overflow test: the algorithm must (a) not
     * loop forever, (b) not write outside the view.  The line
     * from (INT32_MIN, INT32_MIN) to (INT32_MAX, INT32_MAX) DOES
     * cross the (0, 0) → (4, 4) diagonal of the view (the line
     * y = x crosses x = 0..4); the other three lines miss.  We
     * only assert the contract "no buffer overflow", which is
     * what sentinels_intact() checks. */
    gfx_line(h, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, 0xFFFF0001u);
    gfx_line(h, INT32_MIN, INT32_MAX, INT32_MAX, INT32_MIN, 0xFFFF0002u);
    gfx_line(h, INT32_MIN, 0,       INT32_MAX, 0,       0xFFFF0003u);
    gfx_line(h, 0,       INT32_MIN, 0,       INT32_MAX, 0xFFFF0004u);
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_line_extreme_clip_partial)
{
    TEST_SUITE("line: extreme endpoints with a clip that excludes the view");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* Tight clip inside the view; the line crosses the view but
     * never enters the clip rectangle.  The Bresenham clipping
     * must still terminate without overflow. */
    gfx_set_clip(h, 2, 2, 2, 2);
    gfx_line(h, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, 0xFEED0001u);
    assert_true(sentinels_intact());
    free(h);
}

/* ── sprite: opaque, color-key, mask, stride ───────────────── */
TEST_FUNC(test_sprite_opaque_with_stride_padding)
{
    TEST_SUITE("sprite: opaque blit, source stride > src_w*4");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* 3×2 sprite with 16-byte (4-pixel) stride padding per row:
     * src layout (one pixel = 4 bytes):
     *   row 0: p0 p1 p2 PAD
     *   row 1: p3 p4 p5 PAD
     */
    uint32_t src[2 * 4] = {
        0x11111111u, 0x22222222u, 0x33333333u, 0xDEADDEADu,
        0x44444444u, 0x55555555u, 0x66666666u, 0xDEADDEADu,
    };
    /* Draw at (1, 1) of the view → fills a 3x2 block at rows 1..2,
     * cols 1..3. */
    gfx_sprite_blit(h, 1, 1, src, 4 * 4, 3, 2, false, 0u);
    assert_eq((uint32_t)0x11111111u, *view_at(1, 1));
    assert_eq((uint32_t)0x22222222u, *view_at(1, 2));
    assert_eq((uint32_t)0x33333333u, *view_at(1, 3));
    assert_eq((uint32_t)0x44444444u, *view_at(2, 1));
    assert_eq((uint32_t)0x55555555u, *view_at(2, 2));
    assert_eq((uint32_t)0x66666666u, *view_at(2, 3));
    /* Sprite stride padding MUST NOT contaminate the destination. */
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_sprite_opaque_with_byte_stride)
{
    TEST_SUITE("sprite: byte stride need not be pixel aligned");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    _Alignas(4) uint8_t bytes[26] = {0};
    const uint32_t values[6] = {
        0x11111111u, 0x22222222u, 0x33333333u,
        0x44444444u, 0x55555555u, 0x66666666u,
    };
    for (size_t col = 0; col < 3; ++col) {
        memcpy(bytes + col * 4, &values[col], 4);
        memcpy(bytes + 13 + col * 4, &values[3 + col], 4);
    }
    gfx_sprite_blit(h, 1, 1, (const uint32_t *)bytes, 13, 3, 2,
                    false, 0u);
    for (int32_t row = 0; row < 2; ++row) {
        for (int32_t col = 0; col < 3; ++col) {
            assert_eq(values[row * 3 + col], *view_at(1 + row, 1 + col));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_sprite_mask_with_byte_stride)
{
    TEST_SUITE("sprite: mask blit honors byte stride");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    _Alignas(4) uint8_t bytes[26] = {0};
    const uint32_t values[6] = {
        0x11111111u, 0x22222222u, 0x33333333u,
        0x44444444u, 0x55555555u, 0x66666666u,
    };
    for (size_t col = 0; col < 3; ++col) {
        memcpy(bytes + col * 4, &values[col], 4);
        memcpy(bytes + 13 + col * 4, &values[3 + col], 4);
    }
    const uint8_t mask[2] = {0xA0u, 0x60u};
    gfx_sprite_blit_mask(h, 1, 1, (const uint32_t *)bytes, 13,
                         mask, 1, 3, 2);
    assert_eq(values[0], *view_at(1, 1));
    assert_eq(0u, *view_at(1, 2));
    assert_eq(values[2], *view_at(1, 3));
    assert_eq(0u, *view_at(2, 1));
    assert_eq(values[4], *view_at(2, 2));
    assert_eq(values[5], *view_at(2, 3));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_sprite_color_key_black_transparent)
{
    TEST_SUITE("sprite: color_key=0 is transparent (key includes black)");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* Seed the destination with a known color so we can confirm
     * the transparent pixel kept it. */
    gfx_fill_rect(h, 0, 0, BUF_W, BUF_H, 0x99999999u);
    /* 3×1 sprite: middle pixel is key color (black). */
    uint32_t src[3] = {
        0x11223344u,
        0x00000000u,  /* transparent */
        0x55667788u,
    };
    gfx_sprite_blit(h, 0, 0, src, 3 * 4, 3, 1, true, 0u);
    /* Pixel at (0,0) and (0,2) are overwritten; pixel at (0,1)
     * keeps the background because the source matches the
     * color key (key=0). */
    assert_eq((uint32_t)0x11223344u, *view_at(0, 0));
    assert_eq((uint32_t)0x99999999u, *view_at(0, 1));
    assert_eq((uint32_t)0x55667788u, *view_at(0, 2));
    free(h);
}

TEST_FUNC(test_sprite_color_key_nonzero)
{
    TEST_SUITE("sprite: non-zero color key is also transparent");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_fill_rect(h, 0, 0, BUF_W, BUF_H, 0x77777777u);
    uint32_t src[4] = {
        0xAAAA0001u, 0xBEEF0002u, 0xAAAA0003u, 0xAAAA0004u,
    };
    gfx_sprite_blit(h, 0, 0, src, 4 * 4, 4, 1, true, 0xBEEF0002u);
    /* Only the second pixel was key=0xBEEF0002 → kept background. */
    assert_eq((uint32_t)0xAAAA0001u, *view_at(0, 0));
    assert_eq((uint32_t)0x77777777u, *view_at(0, 1));
    assert_eq((uint32_t)0xAAAA0003u, *view_at(0, 2));
    assert_eq((uint32_t)0xAAAA0004u, *view_at(0, 3));
    free(h);
}

TEST_FUNC(test_sprite_color_key_disabled)
{
    TEST_SUITE("sprite: use_color_key=false disables transparency");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_fill_rect(h, 0, 0, BUF_W, BUF_H, 0x88888888u);
    uint32_t src[3] = {
        0x11111111u, 0x00000000u, 0x22222222u,
    };
    gfx_sprite_blit(h, 0, 0, src, 3 * 4, 3, 1, false, 0u);
    /* All three pixels copied even though one is black. */
    assert_eq((uint32_t)0x11111111u, *view_at(0, 0));
    assert_eq((uint32_t)0x00000000u, *view_at(0, 1));
    assert_eq((uint32_t)0x22222222u, *view_at(0, 2));
    free(h);
}

TEST_FUNC(test_sprite_clip_and_partial)
{
    TEST_SUITE("sprite: clipped to view + clip rectangle");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* Tight clip: x ∈ [2, 4), y ∈ [1, 3) — a 2x2 visible rect. */
    gfx_set_clip(h, 2, 1, 2, 2);
    uint32_t src[3 * 3] = {
        0xAA000001u, 0xAA000002u, 0xAA000003u,
        0xAA000004u, 0xAA000005u, 0xAA000006u,
        0xAA000007u, 0xAA000008u, 0xAA000009u,
    };
    /* Sprite drawn at (2, 1): sprite pixel (sx, sy) lands at
     * destination (2 + sx, 1 + sy).  The clip allows destination
     * (2..3, 1..2), which corresponds to sprite (0..1, 0..1). */
    gfx_sprite_blit(h, 2, 1, src, 3 * 4, 3, 3, false, 0u);
    assert_eq((uint32_t)0xAA000001u, *view_at(1, 2));
    assert_eq((uint32_t)0xAA000002u, *view_at(1, 3));
    assert_eq((uint32_t)0xAA000004u, *view_at(2, 2));
    assert_eq((uint32_t)0xAA000005u, *view_at(2, 3));
    /* Outside the clip rectangle stays zero */
    assert_eq(0u, *view_at(1, 1));
    assert_eq(0u, *view_at(1, 4));
    assert_eq(0u, *view_at(2, 1));
    assert_eq(0u, *view_at(2, 4));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_sprite_mask_bit_order)
{
    TEST_SUITE("sprite: mask is MSB-first, 1 means copy");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* 8-pixel-wide sprite row.  Mask byte 0xA5 = 10100101:
     *   bit 7 (leftmost) = 1 → copy pixel 0
     *   bit 6           = 0 → skip pixel 1
     *   bit 5           = 1 → copy pixel 2
     *   bit 4           = 0 → skip pixel 3
     *   bit 3           = 0 → skip pixel 4
     *   bit 2           = 1 → copy pixel 5
     *   bit 1           = 0 → skip pixel 6
     *   bit 0 (rightmost) = 1 → copy pixel 7
     */
    uint32_t src[8] = {
        0xC0000001u, 0xC0000002u, 0xC0000003u, 0xC0000004u,
        0xC0000005u, 0xC0000006u, 0xC0000007u, 0xC0000008u,
    };
    uint8_t  mask[1] = { 0xA5u };
    gfx_fill_rect(h, 0, 0, BUF_W, BUF_H, 0x77777777u);
    /* Draw a 7-wide sprite at (0, 0); only 7 mask bits apply
     * because the buffer is only 7 wide.  Bit 0 (rightmost) is
     * outside the destination — it must not write anywhere. */
    gfx_sprite_blit_mask(h, 0, 0, src, 8 * 4, mask, 1, 8, 1);
    assert_eq((uint32_t)0xC0000001u, *view_at(0, 0));
    assert_eq((uint32_t)0x77777777u, *view_at(0, 1));
    assert_eq((uint32_t)0xC0000003u, *view_at(0, 2));
    assert_eq((uint32_t)0x77777777u, *view_at(0, 3));
    assert_eq((uint32_t)0x77777777u, *view_at(0, 4));
    assert_eq((uint32_t)0xC0000006u, *view_at(0, 5));
    assert_eq((uint32_t)0x77777777u, *view_at(0, 6));
    /* Mask bit 0 would have copied src[7] at destination col 7
     * — but the destination is only 7 wide, so col 7 is past the
     * view.  The bit must not write anywhere.  The sentinel after
     * the buffer catches a buffer-overrun. */
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_sprite_mask_padded_stride)
{
    TEST_SUITE("sprite: mask stride padding is ignored");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* 8-pixel-wide sprite, 2 rows.  Mask row stride is 4 bytes
     * but the row only uses 1 byte; the extra 3 bytes per row are
     * ignored. */
    uint32_t src[8 * 2] = {
        0xD0000001u, 0xD0000002u, 0xD0000003u, 0xD0000004u,
        0xD0000005u, 0xD0000006u, 0xD0000007u, 0xD0000008u,
        0xD0000011u, 0xD0000012u, 0xD0000013u, 0xD0000014u,
        0xD0000015u, 0xD0000016u, 0xD0000017u, 0xD0000018u,
    };
    uint8_t mask[4 * 2] = {
        /* row 0: copy pixel 0, skip 1..7 */
        0x80u, 0x00u, 0x00u, 0x00u,
        /* row 1: copy pixel 7, skip 0..6 */
        0x01u, 0x00u, 0x00u, 0x00u,
    };
    /* Draw a 7-wide sprite at (0, 0); mask covers bits for src
     * pixels 0..7, but the destination is only 7 columns wide.
     * For row 1 mask 0x01: bit 0 means copy pixel 7 → but pixel
     * 7 is past the destination.  The lib must not write there.
     * The sentinel after the buffer catches a buffer-overrun. */
    gfx_sprite_blit_mask(h, 0, 0, src, 8 * 4, mask, 4, 8, 2);
    assert_eq((uint32_t)0xD0000001u, *view_at(0, 0));
    assert_eq((uint32_t)0u,           *view_at(0, 1));
    assert_eq((uint32_t)0u,           *view_at(1, 0));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_sprite_completely_outside)
{
    TEST_SUITE("sprite: completely outside view is a no-op");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    uint32_t src[4] = { 0x1u, 0x2u, 0x3u, 0x4u };
    gfx_sprite_blit(h, -100, -100, src, 4 * 4, 2, 2, false, 0u);
    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

/* ── glyph: opaque, transparent bg, clipping, padding ───────── */
TEST_FUNC(test_glyph_opaque_fg_bg)
{
    TEST_SUITE("glyph: opaque writes fgc for 1 and bgc for 0");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    uint8_t mask[2] = { 0x90u, 0x60u }; /* row 0: 1001, row 1: 0110 */
    uint32_t fgc = 0xFFFFFFFFu;
    uint32_t bgc = 0x11223344u;

    gfx_draw_glyph(h, 1, 1, mask, 1, 4, 2, fgc, bgc, true);

    /* Row 1 */
    assert_eq(fgc, *view_at(1, 1));
    assert_eq(bgc, *view_at(1, 2));
    assert_eq(bgc, *view_at(1, 3));
    assert_eq(fgc, *view_at(1, 4));
    /* Row 2 */
    assert_eq(bgc, *view_at(2, 1));
    assert_eq(fgc, *view_at(2, 2));
    assert_eq(fgc, *view_at(2, 3));
    assert_eq(bgc, *view_at(2, 4));

    /* Untouched pixels */
    assert_eq(0u, *view_at(0, 0));
    assert_eq(0u, *view_at(3, 1));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_glyph_transparent_bg)
{
    TEST_SUITE("glyph: transparent background preserves destination");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    gfx_fill_rect(h, 0, 0, BUF_W, BUF_H, 0x77777777u);
    uint8_t mask[2] = { 0x90u, 0x60u };
    uint32_t fgc = 0xAABBCCDDu;

    gfx_draw_glyph(h, 1, 1, mask, 1, 4, 2, fgc, 0x00000000u, false);

    /* Row 1: 1001 -> fgc, bg, bg, fgc */
    assert_eq(fgc, *view_at(1, 1));
    assert_eq(0x77777777u, *view_at(1, 2));
    assert_eq(0x77777777u, *view_at(1, 3));
    assert_eq(fgc, *view_at(1, 4));
    /* Row 2: 0110 -> bg, fgc, fgc, bg */
    assert_eq(0x77777777u, *view_at(2, 1));
    assert_eq(fgc, *view_at(2, 2));
    assert_eq(fgc, *view_at(2, 3));
    assert_eq(0x77777777u, *view_at(2, 4));

    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_glyph_clipped_partial)
{
    TEST_SUITE("glyph: clipped across boundaries");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* 4x2 glyph placed at (-2, -1):
     * visible part is only rows y in [0..0], cols x in [0..1]
     * which corresponds to src row 1, src cols 2..3 (mask row 1: 0x60 = 0110 -> 1, 0) */
    uint8_t mask[2] = { 0x90u, 0x60u };
    uint32_t fgc = 0xFF00FF00u;
    uint32_t bgc = 0x00FF00FFu;

    gfx_draw_glyph(h, -2, -1, mask, 1, 4, 2, fgc, bgc, true);

    assert_eq(fgc, *view_at(0, 0)); /* src_col 2 = 1 */
    assert_eq(bgc, *view_at(0, 1)); /* src_col 3 = 0 */
    assert_eq(0u, *view_at(0, 2));
    assert_eq(0u, *view_at(1, 0));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_glyph_stride_padding)
{
    TEST_SUITE("glyph: stride padding bytes ignored");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    /* 4x1 glyph, mask_stride = 2 (1 extra padding byte) */
    uint8_t mask[2] = { 0x80u, 0xFFu }; /* 1000 ..., padding 0xFF */
    uint32_t fgc = 0x11111111u;
    uint32_t bgc = 0x22222222u;

    gfx_draw_glyph(h, 0, 0, mask, 2, 4, 1, fgc, bgc, true);

    assert_eq(fgc, *view_at(0, 0));
    assert_eq(bgc, *view_at(0, 1));
    assert_eq(bgc, *view_at(0, 2));
    assert_eq(bgc, *view_at(0, 3));
    assert_eq(0u, *view_at(0, 4));
    assert_true(sentinels_intact());
    free(h);
}

TEST_FUNC(test_glyph_null_and_zero_safety)
{
    TEST_SUITE("glyph: NULL and zero size safe no-ops");
    reset_state();
    gfx_handle_t *h = open_test_handle();
    uint8_t mask[1] = { 0x80u };

    gfx_draw_glyph(NULL, 0, 0, mask, 1, 4, 1, 0xFFu, 0x00u, true);
    gfx_draw_glyph(h, 0, 0, NULL, 1, 4, 1, 0xFFu, 0x00u, true);
    gfx_draw_glyph(h, 0, 0, mask, 1, 0, 1, 0xFFu, 0x00u, true);
    gfx_draw_glyph(h, 0, 0, mask, 1, 4, 0, 0xFFu, 0x00u, true);
    gfx_draw_glyph(h, 0, 0, mask, 0, 4, 1, 0xFFu, 0x00u, true); /* stride < 1 */

    for (int32_t y = 0; y < BUF_H; ++y) {
        for (int32_t x = 0; x < BUF_W; ++x) {
            assert_eq(0u, *view_at(y, x));
        }
    }
    assert_true(sentinels_intact());
    free(h);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_pixel_inside_view),
    TEST_ENTRY(test_pixel_outside_view_no_op),
    TEST_ENTRY(test_pixel_respects_clip),
    TEST_ENTRY(test_hline_horizontal_axis),
    TEST_ENTRY(test_hline_clipped_left_and_right),
    TEST_ENTRY(test_hline_completely_outside),
    TEST_ENTRY(test_vline_clipped_top_and_bottom),
    TEST_ENTRY(test_rect_outline_only),
    TEST_ENTRY(test_fill_rect_full),
    TEST_ENTRY(test_fill_rect_clipped),
    TEST_ENTRY(test_fill_rect_zero_size_no_op),
    TEST_ENTRY(test_line_horizontal_octant),
    TEST_ENTRY(test_line_vertical_octant),
    TEST_ENTRY(test_line_diagonal_octants),
    TEST_ENTRY(test_line_shallow_octants),
    TEST_ENTRY(test_line_steep_octants),
    TEST_ENTRY(test_line_clipped_to_view),
    TEST_ENTRY(test_line_completely_outside),
    TEST_ENTRY(test_line_extreme_endpoints_no_overflow),
    TEST_ENTRY(test_line_extreme_clip_partial),
    TEST_ENTRY(test_sprite_opaque_with_stride_padding),
    TEST_ENTRY(test_sprite_opaque_with_byte_stride),
    TEST_ENTRY(test_sprite_mask_with_byte_stride),
    TEST_ENTRY(test_sprite_color_key_black_transparent),
    TEST_ENTRY(test_sprite_color_key_nonzero),
    TEST_ENTRY(test_sprite_color_key_disabled),
    TEST_ENTRY(test_sprite_clip_and_partial),
    TEST_ENTRY(test_sprite_mask_bit_order),
    TEST_ENTRY(test_sprite_mask_padded_stride),
    TEST_ENTRY(test_sprite_completely_outside),
    TEST_ENTRY(test_glyph_opaque_fg_bg),
    TEST_ENTRY(test_glyph_transparent_bg),
    TEST_ENTRY(test_glyph_clipped_partial),
    TEST_ENTRY(test_glyph_stride_padding),
    TEST_ENTRY(test_glyph_null_and_zero_safety),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
