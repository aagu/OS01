/* libgfx/line.c — clipped 2D primitives for the userland gfx API
 * (Task 4, spec §4).
 *
 * This TU owns:
 *   - the single rectangle-intersection helper every primitive
 *     shares (so clip logic cannot diverge between pixel / line /
 *     rect / sprite).
 *   - gfx__pixel_impl, gfx__hline_impl, gfx__vline_impl,
 *     gfx__rect_impl, gfx__fill_rect_impl — the archive backing
 *     for the `static inline` wrappers in libgfx/gfx.h.  Each is
 *     a one-call deep function that touches internal.h.
 *   - gfx_line, the integer Bresenham line in all 8 octants with
 *     both endpoints.
 *
 * Coordinate model (spec §4):
 *   - Every coordinate pair is view-local.  The view rect is
 *     `(0, 0, info.width, info.height)`.
 *   - The library-local clip rect is `(clip_x, clip_y, clip_w,
 *     clip_h)` (handle state).  A draw MUST intersect the
 *     combined visible rect = view ∩ clip; an empty
 *     intersection is a silent no-op.
 *   - `x + w` is computed in int64_t so INT32_MIN/MAX endpoints
 *     cannot overflow the bounding rect math (gfx_line is the
 *     worst offender: a line whose endpoints are INT32_MIN/MAX
 *     must still terminate, not loop forever).
 *
 * Build:
 *   - Compiled into libgfx.a by libgfx/Makefile with the profile
 *     TARGET_CC (cross-target freestanding flags); the hosttest
 *     in hosttests/Makefile host-compiles the same source with
 *     HOST_CC and reaches the handle layout through the in-tree
 *     internal.h.  Both paths use the same source so the userland
 *     archive and the hosttest cannot drift.
 *   - This TU references NO libc (only <stdint.h>, <stdbool.h>,
 *     <string.h> via internal.h).  Drawing is a pure in-memory
 *     write to the handle's pixels buffer; no fd, no ioctl, no
 *     allocation, no syscall.
 */
#include "internal.h"

#include <stdint.h>
#include <string.h>

/* ── Rectangle intersection (the single shared helper) ────────
 *
 * Inputs: a request rect `(rx, ry, rw, rh)` and a visible rect
 * `(vx, vy, vw, vh)` (the view ∩ clip).  All coordinates are
 * signed (int32_t for x/y, uint32_t for w/h).  `rw == 0` or
 * `rh == 0` means "request is empty, no intersection".  Output
 * rect `(out_x, out_y, out_w, out_h)` is filled with the clipped
 * sub-rect or with `out_w == 0` (and `out_h == 0`) when there is
 * no overlap.
 *
 * The right edge is computed in int64_t because `rx + rw` (or
 * `vx + vw`) can wrap when either is near INT32_MAX.  We clamp
 * the right edge to INT64_MAX so a degenerate request never
 * overflows back into the negative range — a "huge" request then
 * just hits the visible rect's right edge and clips cleanly. */
static void intersect_rect(int32_t rx, int32_t ry, uint32_t rw, uint32_t rh,
                           int32_t vx, int32_t vy, uint32_t vw, uint32_t vh,
                           int32_t *out_x, int32_t *out_y,
                           uint32_t *out_w, uint32_t *out_h)
{
    *out_w = 0;
    *out_h = 0;

    if (rw == 0u || rh == 0u || vw == 0u || vh == 0u) {
        return;
    }

    /* Top-left of the overlap. */
    int32_t ax = (rx > vx) ? rx : vx;
    int32_t ay = (ry > vy) ? ry : vy;

    /* Bottom-right of the overlap, computed in 64-bit to dodge
     * the INT32_MAX overflow trap.  Right edge of (rx, rw) is
     * exclusive (the rect is [rx, rx+rw)) — same convention as
     * GFX_PRESENT.  Saturating add to INT64_MAX keeps degenerate
     * cases finite. */
    int64_t rb = (int64_t)rx + (int64_t)rw;
    int64_t vb = (int64_t)vx + (int64_t)vw;
    if (rb > INT64_C(0x7fffffff)) rb = INT64_C(0x7fffffff);
    if (vb > INT64_C(0x7fffffff)) vb = INT64_C(0x7fffffff);

    int64_t bb = (rb < vb) ? rb : vb;
    int64_t db = (rb < vb) ? vb : rb;  /* not used — kept for clarity */
    (void)db;

    int64_t rbt = (int64_t)ry + (int64_t)rh;
    int64_t vbt = (int64_t)vy + (int64_t)vh;
    if (rbt > INT64_C(0x7fffffff)) rbt = INT64_C(0x7fffffff);
    if (vbt > INT64_C(0x7fffffff)) vbt = INT64_C(0x7fffffff);

    int64_t bbt = (rbt < vbt) ? rbt : vbt;

    int64_t iw = bb - (int64_t)ax;
    int64_t ih = bbt - (int64_t)ay;
    if (iw <= 0 || ih <= 0) {
        return;
    }

    /* Clamp to int32 width range — at this point both are <=
     * INT32_MAX so the cast is safe. */
    if (iw > INT64_C(0x7fffffff)) iw = INT64_C(0x7fffffff);
    if (ih > INT64_C(0x7fffffff)) ih = INT64_C(0x7fffffff);

    *out_x = ax;
    *out_y = ay;
    *out_w = (uint32_t)iw;
    *out_h = (uint32_t)ih;
}

/* ── pixel ───────────────────────────────────────────────────── */
void gfx__pixel_impl(gfx_handle_t *h,
                     int32_t x, int32_t y, uint32_t color)
{
    if (!h || !h->pixels) return;

    /* Visible rect = view ∩ clip.  Computed locally here — there
     * is no shared "visible rect" helper to keep the cost of a
     * single pixel draw to a few comparisons.  The view is the
     * outer bound (the kernel will not display anything outside
     * it on PRESENT), and the clip is the inner bound. */
    int32_t vx = h->clip_x;
    int32_t vy = h->clip_y;
    uint32_t vw = h->clip_w;
    uint32_t vh = h->clip_h;

    /* Empty clip is a silent no-op (intersect would yield 0). */
    if (vw == 0u || vh == 0u) return;

    if (x < vx || y < vy) return;
    /* Right and bottom edges — the view rect is what matters here
     * (the clip is the inner one; the view always wins as the
     * outer bound because the kernel clips PRESENT to it). */
    if (h->info.width == 0u || h->info.height == 0u) return;
    /* Intersect view+clip, then check (x, y) ∈ result.  Faster
     * path: clip ≤ view is the common case (the default clip is
     * the full view), so just check `x < vx + vw` (in 64-bit) and
     * `y < vy + vh`. */
    int64_t xb = (int64_t)vx + (int64_t)vw;
    int64_t yb = (int64_t)vy + (int64_t)vh;
    /* The kernel enforces view; the buffer itself is sized to
     * info.width × info.height.  Cap the right / bottom by the
     * view so a clip wider than the view cannot overflow the
     * buffer. */
    int64_t view_w = (int64_t)h->info.width;
    int64_t view_h = (int64_t)h->info.height;
    if (x > (int32_t)(view_w - 1)) return;
    if (y > (int32_t)(view_h - 1)) return;
    if ((int64_t)x >= xb) return;
    if ((int64_t)y >= yb) return;

    /* The buffer index is `y * info.width + x` — every pixel is
     * a single uint32_t (32 bpp, no padding, stride == width*4
     * per spec §2 "buffer row stride = w*4 bytes"). */
    size_t idx = (size_t)y * (size_t)h->info.width + (size_t)x;
    h->pixels[idx] = color;
}

/* ── hline / vline ───────────────────────────────────────────── */
void gfx__hline_impl(gfx_handle_t *h,
                     int32_t x, int32_t y, uint32_t w, uint32_t color)
{
    if (!h || !h->pixels || w == 0u) return;

    int32_t cx, cy;
    uint32_t cw, ch;
    intersect_rect(x, y, w, 1u,
                   h->clip_x, h->clip_y, h->clip_w, h->clip_h,
                   &cx, &cy, &cw, &ch);
    if (cw == 0u || ch == 0u) return;
    /* Now also clip to the view (the kernel clips PRESENT to the
     * view; the buffer is sized to info.width × info.height so
     * any write beyond info.width is a buffer overrun). */
    intersect_rect(cx, cy, cw, ch,
                   0, 0, h->info.width, h->info.height,
                   &cx, &cy, &cw, &ch);
    if (cw == 0u) return;
    (void)ch;

    /* Single-row fill — pixels are contiguous. */
    size_t row = (size_t)cy * (size_t)h->info.width;
    for (uint32_t i = 0; i < cw; ++i) {
        h->pixels[row + (size_t)cx + (size_t)i] = color;
    }
}

void gfx__vline_impl(gfx_handle_t *h,
                     int32_t x, int32_t y, uint32_t h_, uint32_t color)
{
    if (!h || !h->pixels || h_ == 0u) return;

    int32_t cx, cy;
    uint32_t cw, ch;
    intersect_rect(x, y, 1u, h_,
                   h->clip_x, h->clip_y, h->clip_w, h->clip_h,
                   &cx, &cy, &cw, &ch);
    if (cw == 0u || ch == 0u) return;
    intersect_rect(cx, cy, cw, ch,
                   0, 0, h->info.width, h->info.height,
                   &cx, &cy, &cw, &ch);
    if (ch == 0u) return;
    (void)cw;

    /* Single-column fill — index = y*W + x. */
    size_t col_x = (size_t)cx;
    size_t width = (size_t)h->info.width;
    for (uint32_t i = 0; i < ch; ++i) {
        size_t idx = ((size_t)cy + (size_t)i) * width + col_x;
        h->pixels[idx] = color;
    }
}

/* ── rect (outline) / fill_rect ──────────────────────────────── */
void gfx__rect_impl(gfx_handle_t *h,
                    int32_t x, int32_t y,
                    uint32_t w, uint32_t h_, uint32_t color)
{
    /* Outline = top edge + bottom edge + left edge + right edge.
     * Single-pixel w or h_ reduces to a 1-px hline / vline; the
     * spec includes the bottom and right edges (the outline is
     * the closed rectangle).  We use uint32_t h_-1 to detect the
     * degenerate "single-row rect" and "single-col rect" cases. */
    if (!h || !h->pixels || w == 0u || h_ == 0u) return;

    /* The top and bottom rows collapse to a single row when
     * h_ == 1; the left and right columns collapse when w == 1.
     * We always emit at least the top hline, then the bottom
     * hline if distinct, then the left and right vlines if
     * distinct.  The shared intersect_rect inside each helper
     * clips for us, so the whole rectangle can safely live
     * outside the view — the four calls will each become no-ops. */
    gfx__hline_impl(h, x, y, w, color);
    if (h_ > 1u) {
        gfx__hline_impl(h, x, y + (int32_t)(h_ - 1u), w, color);
    }
    if (h_ > 2u) {
        gfx__vline_impl(h, x,             y + 1, h_ - 2u, color);
        if (w > 1u) {
            gfx__vline_impl(h, x + (int32_t)(w - 1u), y + 1, h_ - 2u, color);
        }
    }
}

void gfx__fill_rect_impl(gfx_handle_t *h,
                         int32_t x, int32_t y,
                         uint32_t w, uint32_t h_, uint32_t color)
{
    if (!h || !h->pixels || w == 0u || h_ == 0u) return;

    int32_t cx, cy;
    uint32_t cw, ch;
    intersect_rect(x, y, w, h_,
                   h->clip_x, h->clip_y, h->clip_w, h->clip_h,
                   &cx, &cy, &cw, &ch);
    if (cw == 0u || ch == 0u) return;
    /* Clip to the view (the buffer is sized to info.width ×
     * info.height).  intersect_rect is exact — the visible rect
     * (cx, cy, cw, ch) is fully inside the view. */
    intersect_rect(cx, cy, cw, ch,
                   0, 0, h->info.width, h->info.height,
                   &cx, &cy, &cw, &ch);
    if (cw == 0u || ch == 0u) return;

    size_t width = (size_t)h->info.width;
    for (uint32_t row = 0; row < ch; ++row) {
        size_t base = ((size_t)cy + (size_t)row) * width + (size_t)cx;
        for (uint32_t col = 0; col < cw; ++col) {
            h->pixels[base + col] = color;
        }
    }
}

/* ── Bresenham line ────────────────────────────────────────────
 *
 * Algorithm choice: clip the line against the view ∩ clip first,
 * then run Bresenham on the clipped segment only.  This is what
 * guarantees INT32_MIN/MAX endpoints terminate — the clip either
 * eliminates the line entirely or produces a finite segment whose
 * integer steps are bounded by max(|dx|, |dy|) of the clipped
 * segment, which fits comfortably in int32.
 *
 * Cohen-Sutherland iterative clipping using double for the
 * intersection math.  double is supported by both clang's host
 * and target compilers (it does not require a math library; only
 * libm functions like sin / cos would).  The visible rect's
 * coordinates are bounded, so the double arithmetic is safe even
 * for INT32_MIN/MAX endpoints — the subtraction `x1 - x0` in
 * double is exact for int32 inputs (53-bit mantissa vs 32-bit
 * range). */
#define CS_INSIDE 0
#define CS_LEFT   0x01
#define CS_RIGHT  0x02
#define CS_BOTTOM 0x04
#define CS_TOP    0x08

static uint8_t cs_outcode(double x, double y,
                          int32_t fx, int32_t fy,
                          int32_t right, int32_t top)
{
    uint8_t code = CS_INSIDE;
    if (x < (double)fx)        code |= CS_LEFT;
    if (x >= (double)right)    code |= CS_RIGHT;
    if (y < (double)fy)        code |= CS_BOTTOM;
    if (y >= (double)top)      code |= CS_TOP;
    return code;
}

void gfx_line(gfx_handle_t *h,
              int32_t x0, int32_t y0,
              int32_t x1, int32_t y1, uint32_t color)
{
    if (!h || !h->pixels) return;
    if (h->info.width == 0u || h->info.height == 0u) return;

    /* Visible rect = clip ∩ view (cap to the buffer-bounded view). */
    int32_t fx, fy;
    uint32_t fw, fh;
    intersect_rect(h->clip_x, h->clip_y, h->clip_w, h->clip_h,
                   0, 0, h->info.width, h->info.height,
                   &fx, &fy, &fw, &fh);
    if (fw == 0u || fh == 0u) return;

    int32_t right = (int32_t)(fx + fw);
    int32_t top   = (int32_t)(fy + fh);

    /* Cohen-Sutherland iterative clipping.  At most 4
     * iterations regardless of endpoint range; each iteration
     * clips one endpoint against one edge using double math. */
    double x0d = (double)x0;
    double y0d = (double)y0;
    double x1d = (double)x1;
    double y1d = (double)y1;
    double dxd = x1d - x0d;
    double dyd = y1d - y0d;

    for (int iter = 0; iter < 8; ++iter) {  /* 8 = 4 edges x 2 endpoints */
        uint8_t code0 = cs_outcode(x0d, y0d, fx, fy, right, top);
        uint8_t code1 = cs_outcode(x1d, y1d, fx, fy, right, top);
        if ((code0 | code1) == 0) break;            /* both inside */
        if ((code0 & code1) != 0) return;          /* both outside same region */
        uint8_t code = code0 ? code0 : code1;
        double nx = x0d, ny = y0d;
        if (code & CS_LEFT) {
            /* x = fx; parameterise the line as x = x0 + dx*t = fx
             * so t = (fx - x0) / dx.  Use only when dx != 0. */
            nx = (double)fx;
            ny = y0d + dyd * ((double)fx - x0d) / dxd;
        } else if (code & CS_RIGHT) {
            /* x = right - 1 (inclusive) — last visible column. */
            nx = (double)(right - 1);
            ny = y0d + dyd * ((double)(right - 1) - x0d) / dxd;
        } else if (code & CS_BOTTOM) {
            ny = (double)fy;
            nx = x0d + dxd * ((double)fy - y0d) / dyd;
        } else { /* CS_TOP */
            ny = (double)(top - 1);
            nx = x0d + dxd * ((double)(top - 1) - y0d) / dyd;
        }
        if (code == code0) { x0d = nx; y0d = ny; }
        else               { x1d = nx; y1d = ny; }
    }

    /* Defensive: if either endpoint is still outside, something
     * went wrong (parallel line + outside, etc.).  The view is
     * bounded; an outside point cannot be valid. */
    uint8_t code0 = cs_outcode(x0d, y0d, fx, fy, right, top);
    uint8_t code1 = cs_outcode(x1d, y1d, fx, fy, right, top);
    if ((code0 | code1) != 0) return;

    /* Round to integer pixel coordinates (round-half-away-from-
     * zero so both endpoints are reliably included). */
    int32_t ix0 = (int32_t)(x0d + (x0d < 0 ? -0.5 : 0.5));
    int32_t iy0 = (int32_t)(y0d + (y0d < 0 ? -0.5 : 0.5));
    int32_t ix1 = (int32_t)(x1d + (x1d < 0 ? -0.5 : 0.5));
    int32_t iy1 = (int32_t)(y1d + (y1d < 0 ? -0.5 : 0.5));

    /* Snap to the visible rect — defensive against double rounding. */
    if (ix0 < fx)            ix0 = fx;
    if (iy0 < fy)            iy0 = fy;
    if (ix0 > right - 1)     ix0 = right - 1;
    if (iy0 > top   - 1)     iy0 = top   - 1;
    if (ix1 < fx)            ix1 = fx;
    if (iy1 < fy)            iy1 = fy;
    if (ix1 > right - 1)     ix1 = right - 1;
    if (iy1 > top   - 1)     iy1 = top   - 1;

    /* ── Bresenham iteration over the clipped segment ────────
     * Standard 8-octant form: walk the major axis, decide per
     * step whether to step the minor axis.  Both endpoints are
     * included (the iteration count is `steps + 1`). */
    int32_t adx = (ix0 > ix1) ? (ix0 - ix1) : (ix1 - ix0);
    int32_t ady = (iy0 > iy1) ? (iy0 - iy1) : (iy1 - iy0);
    int32_t sign_x = (ix0 < ix1) ? 1 : -1;
    int32_t sign_y = (iy0 < iy1) ? 1 : -1;
    bool x_major = (adx >= ady);
    int32_t steps = x_major ? adx : ady;

    size_t width = (size_t)h->info.width;
    int32_t x = ix0, y = iy0;
    int32_t err = 0;

    for (int32_t s = 0; s <= steps; ++s) {
        /* The Cohen-Sutherland clip already placed both endpoints
         * inside the visible rect; the Bresenham iteration walks
         * the segment between them and never leaves that range.
         * We still defensively clip each pixel to [0, view) — this
         * catches rounding edge cases without affecting the test
         * contract. */
        if (x >= 0 && y >= 0 &&
            (size_t)x < width && (size_t)y < (size_t)h->info.height) {
            h->pixels[(size_t)y * width + (size_t)x] = color;
        }
        if (s == steps) break;
        if (x_major) {
            err += ady;
            if (err * 2 >= adx) { y += sign_y; err -= adx; }
            x += sign_x;
        } else {
            err += adx;
            if (err * 2 >= ady) { x += sign_x; err -= ady; }
            y += sign_y;
        }
    }
}