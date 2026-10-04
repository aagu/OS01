/* libgfx/sprite.c — RGB32 sprite blits for the userland gfx API
 * (Task 4, spec §4).
 *
 * This TU owns:
 *   - gfx_sprite_blit: opaque and color-key sprite blits.
 *   - gfx_sprite_blit_mask: 1-bpp MSB-first mask sprite blit.
 *
 * Spec §4 contract:
 *   - Source is RGB32, host-endian.  src_stride is the SOURCE byte
 *     stride (>= src_w * 4); rows can have padding bytes that the
 *     blit just skips.
 *   - Color-key mode (`use_color_key == true`) skips pixels whose
 *     value equals color_key, INCLUDING black (key=0 is valid).
 *   - Mask mode uses a 1-bpp alpha mask; mask_stride is the SOURCE
 *     byte stride (>= ceil(src_w / 8)); the mask is MSB-first —
 *     bit 7 of mask_byte[i] corresponds to src pixel i, 1 = copy.
 *   - Destination is the view-local coordinate system; the blit is
 *     clipped to the view ∩ library-local clip rect (spec §4 "all
 *     primitives intersect the view AND the clip; empty
 *     intersection is silent").  Pixels outside the destination
 *     visible rect are silently skipped (the source buffer must
 *     cover the declared src_w × src_h, but we never read outside
 *     that rect anyway).
 *   - No syscall, no allocation; the only effect is writing to
 *     the handle's pixels buffer.  Zero side effects on the
 *     visible rect outside the destination rectangle.
 */
#include "internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Forward-declare the local rect intersection helper.  We do NOT
 * share the helper with line.c because libgfx/line.c is its own
 * translation unit — line.c's helper is `static` and we cannot
 * see it here without forcing libgfx/ to be a single TU.  Instead,
 * we duplicate the helper.  The duplicate is small (~25 lines)
 * and the alternative (turning it into a non-static helper and
 * exposing it through internal.h) leaks a helper that nobody
 * outside libgfx needs.  Keeping it duplicated guarantees the
 * clip logic can be re-read in this file without flipping back
 * and forth. */
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
    int32_t ax = (rx > vx) ? rx : vx;
    int32_t ay = (ry > vy) ? ry : vy;
    int64_t rb = (int64_t)rx + (int64_t)rw;
    int64_t vb = (int64_t)vx + (int64_t)vw;
    if (rb > INT64_C(0x7fffffff)) rb = INT64_C(0x7fffffff);
    if (vb > INT64_C(0x7fffffff)) vb = INT64_C(0x7fffffff);
    int64_t bb = (rb < vb) ? rb : vb;
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
    if (iw > INT64_C(0x7fffffff)) iw = INT64_C(0x7fffffff);
    if (ih > INT64_C(0x7fffffff)) ih = INT64_C(0x7fffffff);
    *out_x = ax;
    *out_y = ay;
    *out_w = (uint32_t)iw;
    *out_h = (uint32_t)ih;
}

/* ── opaque / color-key sprite blit ──────────────────────────── */
void gfx_sprite_blit(gfx_handle_t *h, int32_t dx, int32_t dy,
                     const uint32_t *src, uint32_t src_stride,
                     uint32_t src_w, uint32_t src_h,
                     bool use_color_key, uint32_t color_key)
{
    if (!h || !h->pixels || !src) return;
    if (src_w == 0u || src_h == 0u) return;
    /* spec §4: src_stride is bytes and must be >= src_w*4. */
    if (src_w > UINT32_MAX / 4u || src_stride < src_w * 4u) return;
    if (h->info.width == 0u || h->info.height == 0u) return;

    /* Compute the destination visible rect = clip ∩ view.  We
     * then intersect the sprite's destination rect (dx, dy,
     * src_w, src_h) with it; pixels outside the visible rect are
     * skipped without ever reading the source. */
    int32_t vx = h->clip_x;
    int32_t vy = h->clip_y;
    uint32_t vw = h->clip_w;
    uint32_t vh = h->clip_h;
    if (vw == 0u || vh == 0u) return;

    int32_t fx, fy;
    uint32_t fw, fh;
    intersect_rect(dx, dy, src_w, src_h,
                   vx, vy, vw, vh,
                   &fx, &fy, &fw, &fh);
    if (fw == 0u || fh == 0u) return;
    /* Cap to view (buffer is sized info.width × info.height). */
    intersect_rect(fx, fy, fw, fh,
                   0, 0, h->info.width, h->info.height,
                   &fx, &fy, &fw, &fh);
    if (fw == 0u || fh == 0u) return;

    /* Translate the visible destination rect back to source
     * coordinates.  The sprite starts at (dx, dy) in the
     * destination, so a visible destination pixel (fx + i, fy + j)
     * corresponds to source pixel (fx - dx + i, fy - dy + j).  The
     * Source rows begin at byte offsets; padding need not be a
     * multiple of a pixel, so read RGB32 values with memcpy. */
    size_t width = (size_t)h->info.width;

    size_t sx0 = (size_t)((int64_t)fx - (int64_t)dx);
    size_t sy0 = (size_t)((int64_t)fy - (int64_t)dy);
    for (uint32_t row = 0; row < fh; ++row) {
        const uint8_t *src_row =
            (const uint8_t *)src + (sy0 + (size_t)row) * (size_t)src_stride;
        uint32_t *dst_row =
            h->pixels + ((size_t)fy + (size_t)row) * width + (size_t)fx;
        for (uint32_t col = 0; col < fw; ++col) {
            uint32_t px;
            memcpy(&px, src_row + (sx0 + (size_t)col) * 4u, sizeof(px));
            if (use_color_key && px == color_key) {
                continue;
            }
            dst_row[col] = px;
        }
    }
}

/* ── mask sprite blit ────────────────────────────────────────── */
void gfx_sprite_blit_mask(gfx_handle_t *h, int32_t dx, int32_t dy,
                          const uint32_t *src, uint32_t src_stride,
                          const uint8_t *mask, uint32_t mask_stride,
                          uint32_t src_w, uint32_t src_h)
{
    if (!h || !h->pixels || !src || !mask) return;
    if (src_w == 0u || src_h == 0u) return;
    if (src_w > UINT32_MAX / 4u || src_stride < src_w * 4u) return;
    /* spec §4: mask_stride >= ceil(src_w / 8). */
    uint32_t mask_row_bytes = src_w / 8u + ((src_w & 7u) != 0u);
    if (mask_stride < mask_row_bytes) return;
    if (h->info.width == 0u || h->info.height == 0u) return;

    int32_t vx = h->clip_x;
    int32_t vy = h->clip_y;
    uint32_t vw = h->clip_w;
    uint32_t vh = h->clip_h;
    if (vw == 0u || vh == 0u) return;

    int32_t fx, fy;
    uint32_t fw, fh;
    intersect_rect(dx, dy, src_w, src_h,
                   vx, vy, vw, vh,
                   &fx, &fy, &fw, &fh);
    if (fw == 0u || fh == 0u) return;
    intersect_rect(fx, fy, fw, fh,
                   0, 0, h->info.width, h->info.height,
                   &fx, &fy, &fw, &fh);
    if (fw == 0u || fh == 0u) return;

    size_t width = (size_t)h->info.width;

    size_t sx0 = (size_t)((int64_t)fx - (int64_t)dx);
    size_t sy0 = (size_t)((int64_t)fy - (int64_t)dy);
    for (uint32_t row = 0; row < fh; ++row) {
        const uint8_t *src_row =
            (const uint8_t *)src + (sy0 + (size_t)row) * (size_t)src_stride;
        const uint8_t *mask_row =
            mask + (sy0 + (size_t)row) * (size_t)mask_stride;
        uint32_t *dst_row =
            h->pixels + ((size_t)fy + (size_t)row) * width + (size_t)fx;
        for (uint32_t col = 0; col < fw; ++col) {
            size_t src_col = sx0 + (size_t)col;
            /* Mask bit: bit 7 of mask_row[src_col / 8] is the
             * leftmost (src_col == 0); bit 0 is the rightmost.
             * 1 = copy, 0 = skip. */
            uint8_t mb = mask_row[src_col >> 3];
            uint8_t bit = (uint8_t)(1u << (7u - (src_col & 7)));
            if ((mb & bit) == 0u) continue;
            memcpy(&dst_row[col], src_row + src_col * 4u,
                   sizeof(dst_row[col]));
        }
    }
}

/* ── 1-bpp glyph / mask drawing ──────────────────────────────── */
void gfx_draw_glyph(gfx_handle_t *h, int32_t dx, int32_t dy,
                    const uint8_t *mask, uint32_t mask_stride,
                    uint32_t w, uint32_t h_,
                    uint32_t fgc, uint32_t bgc, bool bg_opaque)
{
    if (!h || !h->pixels || !mask) return;
    if (w == 0u || h_ == 0u) return;
    uint32_t mask_row_bytes = w / 8u + ((w & 7u) != 0u);
    if (mask_stride < mask_row_bytes) return;
    if (h->info.width == 0u || h->info.height == 0u) return;

    int32_t vx = h->clip_x;
    int32_t vy = h->clip_y;
    uint32_t vw = h->clip_w;
    uint32_t vh = h->clip_h;
    if (vw == 0u || vh == 0u) return;

    int32_t fx, fy;
    uint32_t fw, fh;
    intersect_rect(dx, dy, w, h_,
                   vx, vy, vw, vh,
                   &fx, &fy, &fw, &fh);
    if (fw == 0u || fh == 0u) return;
    intersect_rect(fx, fy, fw, fh,
                   0, 0, h->info.width, h->info.height,
                   &fx, &fy, &fw, &fh);
    if (fw == 0u || fh == 0u) return;

    size_t width = (size_t)h->info.width;
    size_t sx0 = (size_t)((int64_t)fx - (int64_t)dx);
    size_t sy0 = (size_t)((int64_t)fy - (int64_t)dy);

    for (uint32_t row = 0; row < fh; ++row) {
        const uint8_t *mask_row =
            mask + (sy0 + (size_t)row) * (size_t)mask_stride;
        uint32_t *dst_row =
            h->pixels + ((size_t)fy + (size_t)row) * width + (size_t)fx;
        for (uint32_t col = 0; col < fw; ++col) {
            size_t src_col = sx0 + (size_t)col;
            uint8_t mb = mask_row[src_col >> 3];
            uint8_t bit = (uint8_t)(1u << (7u - (src_col & 7)));
            if (mb & bit) {
                dst_row[col] = fgc;
            } else if (bg_opaque) {
                dst_row[col] = bgc;
            }
        }
    }
}

/* ── Pixel-level translation scrolling ───────────────────────── */
void gfx_scroll(gfx_handle_t *h, int32_t dx, int32_t dy, uint32_t fill_color)
{
    if (!h || !h->pixels) return;
    if (h->info.width == 0u || h->info.height == 0u) return;
    if (dx == 0 && dy == 0) return;

    int32_t vx, vy;
    uint32_t vw, vh;
    intersect_rect(h->clip_x, h->clip_y, h->clip_w, h->clip_h,
                   0, 0, h->info.width, h->info.height,
                   &vx, &vy, &vw, &vh);
    if (vw == 0u || vh == 0u) return;

    // If shift magnitude covers the entire visible area, clear everything
    if (dx >= (int32_t)vw || dx <= -(int32_t)vw ||
        dy >= (int32_t)vh || dy <= -(int32_t)vh) {
        gfx_fill_rect(h, vx, vy, vw, vh, fill_color);
        return;
    }

    size_t stride = (size_t)h->info.width;

    // Fast-path: Full-width vertical scroll (the dominant terminal workload)
    if (dx == 0 && vx == 0 && vw == h->info.width) {
        if (dy < 0) {
            uint32_t abs_dy = (uint32_t)(-dy);
            size_t copy_lines = (size_t)vh - abs_dy;
            uint32_t *dst = h->pixels + (size_t)vy * stride;
            const uint32_t *src = h->pixels + ((size_t)vy + abs_dy) * stride;
            memmove(dst, src, copy_lines * stride * sizeof(uint32_t));
            gfx_fill_rect(h, 0, (int32_t)(vy + copy_lines), h->info.width, abs_dy, fill_color);
        } else {
            uint32_t abs_dy = (uint32_t)dy;
            size_t copy_lines = (size_t)vh - abs_dy;
            uint32_t *dst = h->pixels + ((size_t)vy + abs_dy) * stride;
            const uint32_t *src = h->pixels + (size_t)vy * stride;
            memmove(dst, src, copy_lines * stride * sizeof(uint32_t));
            gfx_fill_rect(h, 0, vy, h->info.width, abs_dy, fill_color);
        }
        return;
    }

    // General path: Arbitrary sub-rect or horizontal/diagonal shifts
    if (dy <= 0) {
        uint32_t abs_dy = (uint32_t)(-dy);
        uint32_t copy_lines = vh - abs_dy;
        for (uint32_t r = 0; r < copy_lines; ++r) {
            uint32_t y_dst = (uint32_t)vy + r;
            uint32_t y_src = (uint32_t)vy + r + abs_dy;
            uint32_t *dst_row = h->pixels + (size_t)y_dst * stride + (size_t)vx;
            const uint32_t *src_row = h->pixels + (size_t)y_src * stride + (size_t)vx;
            if (dx < 0) {
                uint32_t abs_dx = (uint32_t)(-dx);
                uint32_t copy_cols = vw - abs_dx;
                memmove(dst_row, src_row + abs_dx, (size_t)copy_cols * sizeof(uint32_t));
                for (uint32_t c = copy_cols; c < vw; ++c) dst_row[c] = fill_color;
            } else if (dx > 0) {
                uint32_t abs_dx = (uint32_t)dx;
                uint32_t copy_cols = vw - abs_dx;
                memmove(dst_row + abs_dx, src_row, (size_t)copy_cols * sizeof(uint32_t));
                for (uint32_t c = 0; c < abs_dx; ++c) dst_row[c] = fill_color;
            } else {
                memmove(dst_row, src_row, (size_t)vw * sizeof(uint32_t));
            }
        }
        if (abs_dy > 0u) {
            gfx_fill_rect(h, vx, (int32_t)(vy + copy_lines), vw, abs_dy, fill_color);
        }
    } else {
        uint32_t abs_dy = (uint32_t)dy;
        uint32_t copy_lines = vh - abs_dy;
        for (uint32_t i = copy_lines; i > 0; --i) {
            uint32_t r = i - 1;
            uint32_t y_dst = (uint32_t)vy + r + abs_dy;
            uint32_t y_src = (uint32_t)vy + r;
            uint32_t *dst_row = h->pixels + (size_t)y_dst * stride + (size_t)vx;
            const uint32_t *src_row = h->pixels + (size_t)y_src * stride + (size_t)vx;
            if (dx < 0) {
                uint32_t abs_dx = (uint32_t)(-dx);
                uint32_t copy_cols = vw - abs_dx;
                memmove(dst_row, src_row + abs_dx, (size_t)copy_cols * sizeof(uint32_t));
                for (uint32_t c = copy_cols; c < vw; ++c) dst_row[c] = fill_color;
            } else if (dx > 0) {
                uint32_t abs_dx = (uint32_t)dx;
                uint32_t copy_cols = vw - abs_dx;
                memmove(dst_row + abs_dx, src_row, (size_t)copy_cols * sizeof(uint32_t));
                for (uint32_t c = 0; c < abs_dx; ++c) dst_row[c] = fill_color;
            } else {
                memmove(dst_row, src_row, (size_t)vw * sizeof(uint32_t));
            }
        }
        if (abs_dy > 0u) {
            gfx_fill_rect(h, vx, vy, vw, abs_dy, fill_color);
        }
    }
}
