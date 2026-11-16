#ifndef _ARCH_FB_COPY_H
#define _ARCH_FB_COPY_H

// kernel/include/arch/fb_copy.h — the ARCH-NEUTRAL framebuffer copy
// facade and the VISIBLE-row geometry helpers.
//
// The facade follows the same weak-default / strong-override shape as
// kernel/include/arch/fb_write.h:
//
//   kernel/driver/arch_fb_write.c        weak default  (every arch)
//   kernel/arch/x86_64/driver/fb_copy.c   strong override (x86_64)
//
// The x86 override is the wide 32-bit sequential store; every other
// architecture keeps the conservative byte copy.
//
// The geometry helpers are pure address arithmetic: they compute WHERE
// a visible pixel row lives in a framebuffer whose real byte pitch may
// be larger than width*4 (a 1024x768 panel with a 4096-byte pitch is
// the common real-hardware case), and they refuse to touch a pixel they
// cannot prove is inside the mapped range.
//
// Everything here is header-only so the host regression can exercise the
// very same code against a plain RAM buffer — no PMM, no device, no
// framebuffer involved.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <core/printk.h>   /* printk_fb_view_t */

/* ── fb_copy_rgb32 — the framebuffer copy primitive ────────────
 *
 * Copy `bytes` of RGB32 pixel data from `src` to `dst`.
 *
 * Preconditions the CALLER must have established:
 *   * `dst` points into a framebuffer pixel region whose format and
 *     bounds have already been validated;
 *   * `src` and `dst` MUST NOT overlap.
 *
 * The production use is a RAM -> pixel copy (the kernel row buffer
 * into the framebuffer).  The same primitive serves the framebuffer's
 * own row moves, where the source and destination rows are disjoint.
 */
void fb_copy_rgb32(void *dst, const void *src, size_t bytes);

/* ── Geometry helpers ─────────────────────────────────────────── */

/* The geometry is usable when there is a framebuffer, a non-empty
 * visible area and a pitch that can hold one visible row. */
static inline bool fb_view_ok(const printk_fb_view_t *v)
{
    return printk_fb_view_ok(v);
}

/* Byte offset of the FIRST VISIBLE pixel of pixel row `y`.
 *
 * Fails when the row does not fit inside the writable range.  The
 * check covers the WHOLE pitch extent of the row, not just its
 * visible pixels: a framebuffer whose last row is truncated is a
 * broken geometry, and a writer must refuse it instead of painting a
 * partial frame.
 *
 * A `fb_bytes` of 0 means "unbounded" (the legacy no-snapshot path,
 * where the framebuffer size is not published).
 */
static inline bool fb_row_offset(const printk_fb_view_t *v, uint32_t y,
                                 uint64_t *off)
{
    return printk_fb_row_offset(v, y, off);
}

/* Copy `rows` VISIBLE pixel rows (`row_bytes` each) out of a PACKED
 * RAM source into the framebuffer, honouring the real byte pitch.
 *
 * The whole destination range is validated BEFORE the first store, so
 * a short mapping means NOTHING is written — a partially written frame
 * is worse than an unwritten one.
 */
static inline bool fb_copy_rows(const printk_fb_view_t *v, const uint8_t *src,
                                uint32_t row_bytes, uint32_t rows)
{
    if (!fb_view_ok(v) || !src)
        return false;
    if (row_bytes < v->width * 4u)
        return false;
    if (rows > v->height)
        return false;

    for (uint32_t y = 0; y < rows; y++) {
        uint64_t off;
        if (!fb_row_offset(v, y, &off))
            return false;
    }

    for (uint32_t y = 0; y < rows; y++) {
        uint64_t off;
        (void)fb_row_offset(v, y, &off);
        fb_copy_rgb32(v->fb + off,
                      src + (uint64_t)y * (uint64_t)row_bytes,
                      row_bytes);
    }
    return true;
}

/* Move `rows` rows of `row_bytes` each INSIDE a PACKED RAM buffer: the
 * rows at `src_row` move to `dst_row`.
 *
 * The source and destination row ranges MUST NOT overlap. Moving one
 * row at a time lets the scroll use the wide copy.
 */
static inline bool fb_move_rows_packed(uint8_t *buf, uint32_t row_bytes,
                                       uint32_t dst_row, uint32_t src_row,
                                       uint32_t rows)
{
    if (!buf || row_bytes == 0 || rows == 0) return false;
    if (dst_row == src_row) return true;
    /* Overlap test: the two ranges must be disjoint. */
    if (dst_row + rows > src_row && src_row + rows > dst_row) return false;

    fb_copy_rgb32(buf + (size_t)dst_row * (size_t)row_bytes,
                  buf + (size_t)src_row * (size_t)row_bytes,
                  (size_t)row_bytes * (size_t)rows);
    return true;
}

/* Scroll a packed RAM shadow up, then clear the exposed rows.
 * The caller has validated the buffer, geometry and 0 < shift < height.
 */
static inline void fb_scroll_packed(uint8_t *buf, uint32_t row_bytes,
                                    uint32_t height, uint32_t shift)
{
    /* Copy top to bottom so no write overwrites an unread source row. */
    for (uint32_t y = 0; y < height - shift; y++)
        (void)fb_move_rows_packed(buf, row_bytes, y, y + shift, 1);
    memset(buf + (size_t)(height - shift) * row_bytes, 0,
           (size_t)shift * row_bytes);
}

/* Scroll the VISIBLE pixel rows up by `shift` pixel rows, in place.
 *
 * Every visible row moves up by `shift` rows and the bottom `shift`
 * visible rows are cleared.  The row PADDING (the bytes between the
 * end of the visible area and the end of the pitch) is never touched.
 */
static inline bool fb_scroll_rows(const printk_fb_view_t *v, uint32_t shift)
{
    if (!fb_view_ok(v) || shift == 0 || shift >= v->height)
        return false;

    uint32_t row_bytes = v->width * 4u;

    for (uint32_t y = 0; y < v->height; y++) {
        uint64_t off;
        if (!fb_row_offset(v, y, &off))
            return false;
    }

    /* The source and destination rows are disjoint: the stride is at
     * least one visible row wide and `shift` is at least one row. */
    for (uint32_t y = 0; y + shift < v->height; y++) {
        uint64_t dst_off, src_off;
        (void)fb_row_offset(v, y, &dst_off);
        (void)fb_row_offset(v, y + shift, &src_off);
        fb_copy_rgb32(v->fb + dst_off, v->fb + src_off, row_bytes);
    }

    for (uint32_t y = v->height - shift; y < v->height; y++) {
        uint64_t off;
        (void)fb_row_offset(v, y, &off);
        memset(v->fb + off, 0, row_bytes);
    }
    return true;
}

#endif /* _ARCH_FB_COPY_H */
