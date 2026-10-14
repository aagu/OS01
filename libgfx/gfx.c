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
#include <sys/mman.h>
#include <unistd.h>

/* UAPI mirror — the kernel UAPI ships through the sysroot.  This
 * explicit include binds libgfx/gfx.c to the UAPI struct definitions
 * (GFX_PRESENT_RECT 0x4709, gfx_present_rect_req_t) so re-declaring
 * the struct here is impossible. */
#include <uapi/gfx.h>

/* Device path — single source of truth for the client.  Spec §3. */
#define GFX_DEVICE_PATH "/dev/gfx0"

/* ── gfx_open ─────────────────────────────────────────────────── */
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* Spec §5: only a missing device path maps to ENODEV.
     * Preserve resource and permission errors from open(). */
    int fd = open(GFX_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        if (errno == ENOENT) errno = ENODEV;
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

    /* Allocate the private pixels buffer via anonymous mmap.
     * Spec §4: width*height*4 bytes, zeroed.  Anonymous mmap
     * guarantees zeroed memory without consuming brk heap budget. */
    size_t bytes = (size_t)info.width * (size_t)info.height * 4u;
    void *pixels = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pixels == MAP_FAILED) {
        int saved = errno;
        close(fd);
        free(handle);
        errno = (saved != 0) ? saved : ENOMEM;
        return NULL;
    }
    handle->pixels = (uint32_t *)pixels;
    handle->pixels_bytes = bytes;
    memset(pixels, 0, bytes);

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
    if (h->pixels && h->pixels != (void *)-1) {
        munmap(h->pixels, h->pixels_bytes);
    }
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
/* errno contract (resolution-switcher spec §3.3): the wrapper must
 * propagate the kernel errno of a FAILED present unchanged (EAGAIN =
 * transient drain timeout, ESTALE = stale view after a mode switch,
 * EIO = permanent device failure) and must not touch errno on success.
 * Callers classify that errno with gfx_client_present_policy(); the
 * client policy must never alter this propagation.  The request is
 * re-formed on every call, so nothing is cached across present calls. */
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

/* ── gfx_present_rect ──────────────────────────────────────────── */
/* Spec (docs/superpowers/specs/2026-10-12-terminal-partial-present-design.md)
 * §4: view-local rectangle present over a leased ioctl.  Re-uses the
 * same handle-layout invariants as gfx_present (per-call user pointer,
 * info.stride, reserved=0) and adds geometry validation up-front.
 *
 * Dispatch rule (test-pinned, no fallback):
 *   - Rectangle == configured view → call gfx_present (one GFX_PRESENT
 *     ioctl).  Spec §4 disallows an ENOTTY → full-frame fallback.
 *   - Any other valid rect → exactly one GFX_PRESENT_RECT ioctl.
 *
 * Geometry rule: subtraction form.  check `x > view_w || w > view_w - x`
 * BEFORE subtracting so x=view_w, w=0 is rejected (x > view_w) without
 * underflowing view_w - x.  Same for y / h_.  UINT32_MAX inputs also
 * fall out for free: view_w - 0 fits in uint32_t, and UINT32_MAX > any
 * non-negative value, so the predicate rejects them.
 *
 * Errors: h == NULL OR geometry invalid → return -1 with errno=EINVAL
 * NO ioctl.  Successful return (rc == 0) does NOT modify errno (the
 * ioctl wrapper leaves errno untouched on success).  Failed ioctl
 * returns its errno verbatim — no remapping, no swallowing.
 *
 * Allocation: zero.  Handle layout unchanged.  Caller's clip is
 * library-local drawing state and is NOT consulted here. */
int gfx_present_rect(gfx_handle_t *h,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h_)
{
    if (!h) {
        errno = EINVAL;
        return -1;
    }
    /* Subtraction-form geometry check.  The `x > view_w` half is what
     * stops `view_w - x` from wrapping to a huge unsigned value when
     * x exceeds the view; the `w > view_w - x` half bounds the
     * rectangle's right edge without ever computing x+w. */
    uint32_t view_w = h->info.width;
    uint32_t view_h = h->info.height;
    if (w == 0u || h_ == 0u ||
        x > view_w || w > view_w - x ||
        y > view_h || h_ > view_h - y) {
        errno = EINVAL;
        return -1;
    }
    /* Full-view dispatch: route to old gfx_present so one GFX_PRESENT
     * ioctl carries the request.  Spec §4 forbids ENOTTY fallback, so
     * this branching is the only place the two ioctls meet. */
    if (x == 0u && y == 0u && w == view_w && h_ == view_h) {
        return gfx_present(h);
    }
    /* Partial-view: one GFX_PRESENT_RECT ioctl.  Re-form every call —
     * the kernel never caches the user pointer, and the test verifies
     * the source base is the handle's CURRENT pixels (snapshot at
     * open time is exactly what handle->pixels is).  reserved = 0 is
     * required by the UAPI (the kernel returns EINVAL otherwise). */
    gfx_present_rect_req_t req;
    req.pixels  = (uint64_t)(uintptr_t)h->pixels;
    req.stride  = h->info.stride;
    req.reserved = 0;
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h_;
    return ioctl(h->fd, GFX_PRESENT_RECT, &req);
}

/* ── Ownership & Fallback (Display Ownership Plan) ───────────── */

/* Issue GFX_ACQUIRE ioctl with explicit third argument (uint64_t)0.
 * Error returns -1 with errno set from ioctl; success returns 0 without
 * altering errno. h == NULL returns -1 with errno=EINVAL without ioctl. */
int gfx_acquire(gfx_handle_t *h)
{
    if (!h) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(h->fd, GFX_ACQUIRE, (uint64_t)0);
}

/* Issue GFX_RELEASE ioctl with explicit third argument (uint64_t)0.
 * Error returns -1 with errno set from ioctl; success returns 0 without
 * altering errno. h == NULL returns -1 with errno=EINVAL without ioctl. */
int gfx_release(gfx_handle_t *h)
{
    if (!h) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(h->fd, GFX_RELEASE, (uint64_t)0);
}

/* Issue GFX_REGISTER_FALLBACK ioctl with explicit third argument (uint64_t)0.
 * Error returns -1 with errno set from ioctl; success returns 0 without
 * altering errno. h == NULL returns -1 with errno=EINVAL without ioctl. */
int gfx_register_fallback(gfx_handle_t *h)
{
    if (!h) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(h->fd, GFX_REGISTER_FALLBACK, (uint64_t)0);
}

/* Issue GFX_GET_DISPLAY_STATE ioctl to populate *out.
 * Error returns -1 with errno set from ioctl; success returns 0 without
 * altering errno. h == NULL or out == NULL returns -1 with errno=EINVAL
 * without ioctl. */
int gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out)
{
    if (!h || !out) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(h->fd, GFX_GET_DISPLAY_STATE, out);
}

/* Issue GFX_REPLACE_FALLBACK ioctl on new_h->fd with req.old_fd = old_h->fd
 * and req.reserved = 0.
 * Error returns -1 with errno set from ioctl; success returns 0 without
 * altering errno. old_h == NULL or new_h == NULL returns -1 with errno=EINVAL
 * without ioctl. */
int gfx_replace_fallback(gfx_handle_t *old_h, gfx_handle_t *new_h)
{
    if (!old_h || !new_h) {
        errno = EINVAL;
        return -1;
    }
    gfx_replace_fallback_t req;
    req.old_fd = old_h->fd;
    req.reserved = 0;
    return ioctl(new_h->fd, GFX_REPLACE_FALLBACK, &req);
}

