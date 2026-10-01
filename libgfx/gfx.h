/* libgfx/gfx.h — public 2D graphics API for OS01.
 *
 * Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
 * §3 + §4: a small client library over /dev/gfx0 that draws into
 * a private buffer and presents once per frame.  The handle is
 * opaque; this header does NOT expose the pixels field, the fd,
 * or any library-local clip state.
 *
 * Tasks:
 *   - Task 3: open / close / get_info / set_clip / present.
 *   - Task 4: pixel / hline / vline / line (Bresenham) / rect /
 *     fill_rect / sprite blit (opaque and color-key) / mask blit.
 *
 * Drawing API design (spec §4):
 *   - pixel, hline, vline, rect, fill_rect are `static inline`
 *     thin wrappers around `gfx__*_impl` archive functions in
 *     libgfx/line.c.  The inlines live here so callers see the
 *     surface directly, but they DO NOT touch the handle layout:
 *     the inlines are one-call deep into the archive, which can
 *     see `internal.h`.  This is how gfx.h stays a zero-pixel-
 *     pointer leak surface while keeping every primitive a real
 *     function call (no header inlining of internal layout).
 *   - line, sprite_blit, sprite_blit_mask are full archive
 *     functions declared here because their implementations are
 *     too large for `static inline` (Bresenham iteration,
 *     clipping, mask bit walking).
 *   - All primitives are view-local and intersect the
 *     library-local clip rectangle; out-of-view draws are silent
 *     no-ops.  No primitive allocates, opens a file, or issues
 *     any syscall — every draw writes only to the handle's
 *     pixels buffer in memory. */
#ifndef _LIBGFX_GFX_H
#define _LIBGFX_GFX_H

#include <stdint.h>
#include <stdbool.h>

/* The kernel UAPI ships through the sysroot (usr/include/uapi/gfx.h)
 * and defines gfx_view_desc_t / gfx_info_t / gfx_present_req_t plus
 * the ioctl numbers.  Including it here keeps the public ABI mirror
 * of the kernel — no copy/paste divergence possible. */
#include <uapi/gfx.h>

/* Opaque handle.  Internal layout lives in libgfx/internal.h, which
 * is NEVER installed to the sysroot.  Callers must not assume the
 * size or layout. */
typedef struct gfx_handle gfx_handle_t;

/* ── Lifecycle (Task 3) ────────────────────────────────────────
 * Open a view into /dev/gfx0 at full-screen rectangle (x,y,w,h),
 * where w and h are the configured view's LOCAL dimensions (the
 * kernel fills `info.width`/`info.height` from these).  Returns a
 * heap-allocated handle whose pixels buffer is zeroed.
 *
 * Errors:
 *   - /dev/gfx0 missing or open() failure → NULL, errno=ENODEV
 *     (the library normalises the raw errno from open).
 *   - zero / out-of-range / overflowed view → NULL, errno=EINVAL
 *     (propagated from the kernel's GFX_CREATE_VIEW).
 *   - 16 view slots already in use → NULL, errno=EMFILE.
 *   - malloc/calloc failure → NULL, errno=ENOMEM.
 *
 * On any failure, every resource acquired so far (fd, handle,
 * pixels buffer) is released before returning. */
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Release the handle, free the pixels buffer, and close the fd.
 * gfx_close(NULL) is a safe no-op. */
void gfx_close(gfx_handle_t *h);

/* Return the configured view's local dimensions.  gfx_get_info(NULL)
 * returns a zeroed gfx_info_t and sets errno=EINVAL; the call does
 * NOT issue any ioctl (the info was snapshotted at gfx_open). */
gfx_info_t gfx_get_info(const gfx_handle_t *h);

/* Set the library-local clip rectangle in LOCAL coordinates.  The
 * clip is purely a library convenience (spec §4); the kernel is
 * NEVER told, and gfx_present still uploads the full buffer.
 * gfx_set_clip(NULL,...) returns -1 with errno=EINVAL and issues
 * no ioctl. */
int gfx_set_clip(gfx_handle_t *h, int32_t x, int32_t y,
                 uint32_t w, uint32_t h_);

/* Issue exactly one GFX_PRESENT ioctl carrying the handle's pixels
 * buffer (a per-call pointer; the kernel never caches it) and the
 * configured stride.  Returns 0 on success; on kernel rejection
 * returns -1 and the ioctl wrapper sets errno (EFAULT for an
 * invalid buffer, ENOTTY for an unknown cmd, EINVAL for an
 * unconfigured view, etc.).  gfx_present(NULL,...) returns -1
 * with errno=EINVAL without issuing an ioctl. */
int gfx_present(gfx_handle_t *h);

/* ── Archive-function declarations (Task 4) ────────────────────
 *
 * Each primitive routes through one `gfx__*_impl` function in
 * libgfx/line.c or libgfx/sprite.c.  The library uses the
 * `gfx__` prefix to mark "libgfx-internal implementation entry",
 * which prevents any naming clash with the public `gfx_pixel`
 * etc. wrappers below.  Callers should NEVER call these
 * directly — they take a `gfx_handle_t *` but assume the handle
 * is fully populated (caller-facing entry points have the NULL
 * guard + clip intersection baked in).
 *
 * The simple primitives (pixel / hline / vline / rect /
 * fill_rect) are exposed as `static inline` wrappers that
 * forward to the archive: callers see the short names, but the
 * archive is the only place that touches internal.h.  This is
 * how `gfx.h` stays a zero-pixel-pointer leak surface. */
void gfx__pixel_impl(gfx_handle_t *h,
                     int32_t x, int32_t y, uint32_t color);
void gfx__hline_impl(gfx_handle_t *h,
                     int32_t x, int32_t y, uint32_t w, uint32_t color);
void gfx__vline_impl(gfx_handle_t *h,
                     int32_t x, int32_t y, uint32_t h_, uint32_t color);
void gfx__rect_impl(gfx_handle_t *h,
                    int32_t x, int32_t y,
                    uint32_t w, uint32_t h_, uint32_t color);
void gfx__fill_rect_impl(gfx_handle_t *h,
                         int32_t x, int32_t y,
                         uint32_t w, uint32_t h_, uint32_t color);

/* gfx_line: integer Bresenham, both endpoints, all 8 octants.
 * Extreme endpoints (INT32_MIN / INT32_MAX) must not overflow or
 * loop forever; the implementation clips the line to the view /
 * library-local clip first, then iterates only the visible
 * segment. */
void gfx_line(gfx_handle_t *h,
              int32_t x0, int32_t y0,
              int32_t x1, int32_t y1, uint32_t color);

/* gfx_sprite_blit: opaque or color-keyed RGB32 sprite blit.
 * src_stride is the SOURCE byte stride (>= src_w*4); the source
 * row can have padding bytes that are simply not read.
 * use_color_key=false is fully opaque (every pixel copied);
 * use_color_key=true skips pixels that match color_key, INCLUDING
 * black (key=0).  Coordinates are view-local; the destination is
 * intersected with the view AND the library-local clip. */
void gfx_sprite_blit(gfx_handle_t *h, int32_t dx, int32_t dy,
                     const uint32_t *src, uint32_t src_stride,
                     uint32_t src_w, uint32_t src_h,
                     bool use_color_key, uint32_t color_key);

/* gfx_sprite_blit_mask: 1-bpp mask sprite blit.  mask_stride is
 * in BYTES (>= ceil(src_w/8)); mask is MSB-first (bit 7 = leftmost
 * src pixel).  A mask bit of 1 means "copy this pixel", 0 means
 * "keep destination".  Like gfx_sprite_blit, the destination is
 * intersected with the view and the library-local clip; mask
 * bits for pixels outside the destination rectangle are simply
 * not consulted. */
void gfx_sprite_blit_mask(gfx_handle_t *h, int32_t dx, int32_t dy,
                          const uint32_t *src, uint32_t src_stride,
                          const uint8_t *mask, uint32_t mask_stride,
                          uint32_t src_w, uint32_t src_h);

/* ── Public `static inline` primitive wrappers (Task 4) ───────
 *
 * Each wrapper handles the caller's NULL-handle guard + the
 * argument-shape contract (zero-size → silent no-op) and then
 * forwards to the matching archive function.  The archive is
 * the only place that touches `internal.h`, so `gfx.h` does NOT
 * reveal `pixels` or any other handle field. */
static inline void gfx_pixel(gfx_handle_t *h,
                             int32_t x, int32_t y, uint32_t color)
{
    if (!h) return;
    gfx__pixel_impl(h, x, y, color);
}

static inline void gfx_hline(gfx_handle_t *h,
                             int32_t x, int32_t y,
                             uint32_t w, uint32_t color)
{
    if (!h) return;
    if (w == 0u) return;
    gfx__hline_impl(h, x, y, w, color);
}

static inline void gfx_vline(gfx_handle_t *h,
                             int32_t x, int32_t y,
                             uint32_t h_, uint32_t color)
{
    if (!h) return;
    if (h_ == 0u) return;
    gfx__vline_impl(h, x, y, h_, color);
}

static inline void gfx_rect(gfx_handle_t *h,
                            int32_t x, int32_t y,
                            uint32_t w, uint32_t h_, uint32_t color)
{
    if (!h) return;
    if (w == 0u || h_ == 0u) return;
    gfx__rect_impl(h, x, y, w, h_, color);
}

static inline void gfx_fill_rect(gfx_handle_t *h,
                                 int32_t x, int32_t y,
                                 uint32_t w, uint32_t h_, uint32_t color)
{
    if (!h) return;
    if (w == 0u || h_ == 0u) return;
    gfx__fill_rect_impl(h, x, y, w, h_, color);
}

#endif /* _LIBGFX_GFX_H */