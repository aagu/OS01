/* libgfx/gfx.c — client lifecycle wrappers for /dev/gfx0 (Task 3).
 *
 * Spec (docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md)
 * §3+§4+§5: open /dev/gfx0, configure the view, snapshot the
 * configured local dimensions, allocate a zeroed pixels buffer.
 * Every failure path releases every resource already acquired
 * (handle, fd, buffer) before returning NULL.  Task 4 will add
 * the point/line/rect/sprite primitives.
 *
 * Build: this TU is added to libgfx.a by libgfx/Makefile with the
 * profile TARGET_CC (cross-target freestanding flags) and via the
 * hosttest rule in hosttests/Makefile with HOST_CC (so the Task 3
 * client test can wrap libc's open/ioctl/close/malloc).  Both
 * paths compile cleanly because the only libc entry points
 * referenced are <fcntl.h>'s O_RDWR, <errno.h>'s errno /
 * ENODEV / EINVAL / ENOMEM, and <stdlib.h>'s malloc/calloc/free
 * — all universally available.
 */
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Device path — single source of truth for the client.  Spec §3. */
#define GFX_DEVICE_PATH "/dev/gfx0"

/* ── gfx_open ─────────────────────────────────────────────────── */
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* Spec §5: any open() failure is normalised to ENODEV — the
     * device simply does not exist from the caller's perspective.
     * ENOENT (path missing), EACCES, ENXIO all collapse to the same
     * "no /dev/gfx0 here" contract. */
    int fd = open(GFX_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        errno = ENODEV;
        return NULL;
    }

    /* Allocate the handle up front so every failure path below can
     * funnel through one cleanup block.  The handle carries the fd
     * even before the view is configured, so a CREATE_VIEW failure
     * releases both at once. */
    gfx_handle_t *handle = (gfx_handle_t *)malloc(sizeof(*handle));
    if (!handle) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    memset(handle, 0, sizeof(*handle));
    handle->fd = fd;

    /* Configure the view.  The kernel validates zero/non-zero,
     * bounds, and the subtraction-form overflow check
     * (x <= fb_w && w <= fb_w - x); on failure it returns the raw
     * -EINVAL which the ioctl wrapper propagates to errno. */
    gfx_view_desc_t desc = { x, y, w, h };
    if (ioctl(fd, GFX_CREATE_VIEW, &desc) < 0) {
        int saved = errno;
        close(fd);
        free(handle);
        errno = saved;
        return NULL;
    }

    /* Snapshot the configured local dimensions.  An unconfigured
     * view (impossible here — we just configured it) would return
     * EINVAL; propagated transparently. */
    gfx_info_t info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, GFX_GET_INFO, &info) < 0) {
        int saved = errno;
        close(fd);
        free(handle);
        errno = saved;
        return NULL;
    }
    handle->info = info;

    /* Allocate the private pixels buffer.  Spec §4: width*height*4
     * bytes, zeroed.  calloc guarantees the zeroing. */
    size_t bytes = (size_t)info.width * (size_t)info.height * 4u;
    handle->pixels = (uint32_t *)calloc(1, bytes);
    if (!handle->pixels) {
        int saved = errno;
        close(fd);
        free(handle);
        /* calloc already sets errno=ENOMEM on failure, but make it
         * explicit so we are robust against a future libc that
         * forgets. */
        errno = (saved != 0) ? saved : ENOMEM;
        return NULL;
    }

    /* Initial clip = the full view in local coordinates.  The clip
     * is library-local state; spec §4 says "越出视图的 clip 取交集"
     * — at open time the clip exactly fits the view.  Primitive
     * clipping (Task 4) re-validates this on every draw. */
    handle->clip_x = 0;
    handle->clip_y = 0;
    handle->clip_w = info.width;
    handle->clip_h = info.height;

    return handle;
}

/* ── gfx_close ────────────────────────────────────────────────── */
void gfx_close(gfx_handle_t *h)
{
    if (!h) return;
    free(h->pixels);
    close(h->fd);
    free(h);
}

/* ── gfx_get_info ─────────────────────────────────────────────── */
gfx_info_t gfx_get_info(const gfx_handle_t *h)
{
    if (!h) {
        errno = EINVAL;
        gfx_info_t z = { 0, 0, 0, 0 };
        return z;
    }
    /* The info was snapshotted at gfx_open; this is a plain copy. */
    errno = 0;
    return h->info;
}

/* ── gfx_set_clip ─────────────────────────────────────────────── */
int gfx_set_clip(gfx_handle_t *h, int32_t x, int32_t y,
                 uint32_t w, uint32_t h_)
{
    if (!h) {
        errno = EINVAL;
        return -1;
    }
    /* Spec §4: library-local clip, NO ioctl.  Primitive clipping
     * (Task 4) is responsible for intersecting the clip with the
     * requested draw rectangle.  Out-of-view clips are intersected
     * at use time; gfx_set_clip itself is permissive and does not
     * validate against h->info here — the intersection is what
     * matters for correctness. */
    h->clip_x = x;
    h->clip_y = y;
    h->clip_w = w;
    h->clip_h = h_;
    errno = 0;
    return 0;
}

/* ── gfx_present ──────────────────────────────────────────────── */
int gfx_present(gfx_handle_t *h)
{
    if (!h) {
        errno = EINVAL;
        return -1;
    }
    /* Spec §4: GFX_PRESENT is the only ioctl per frame.  The
     * per-call user pointer contract (kernel never caches it) is
     * satisfied by re-forming the request here — gfx_present always
     * supplies the current handle->pixels.  reserved must be 0. */
    gfx_present_req_t req;
    req.pixels = (uint64_t)(uintptr_t)h->pixels;
    req.stride = h->info.stride;
    req.reserved = 0;
    return ioctl(h->fd, GFX_PRESENT, &req);
}
