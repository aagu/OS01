/* libgfx/gfx.h — public 2D graphics API for OS01.
 *
 * Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
 * §3 + §4: a small client library over /dev/gfx0 that draws into
 * a private buffer and presents once per frame.  The handle is
 * opaque; this header does NOT expose the pixels field, the fd,
 * or any library-local clip state.
 *
 * Task 3 (this header): declares the lifecycle wrappers
 * (gfx_open/close/get_info/set_clip/present).  Task 4 adds the
 * point/line/rect/sprite primitives — until then the buffer is
 * always zeroed and the caller cannot draw. */
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

/* Open a view into /dev/gfx0 at full-screen rectangle (x,y,w,h),
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

#endif /* _LIBGFX_GFX_H */
