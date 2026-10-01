#ifndef _UAPI_GFX_H
#define _UAPI_GFX_H

#include <stdint.h>

/* /dev/gfx0 — 2D graphics view UAPI.
 *
 * All structs are fixed-width and must NOT change field order or
 * size (libgfx and the kernel build against this header).  reserved
 * fields must be transmitted as 0; the kernel returns EINVAL when
 * they are not.  Every ioctl argument is either copied in/out via
 * copy_{from,to}_user_ft or staged line-by-line through kernel
 * bounce (never dereferenced directly). */

#define GFX_FORMAT_RGB32 0u

/* ioctl numbers — see kernel/fs/devfs.c devfs_ioctl_node dispatch. */
#define GFX_CREATE_VIEW  0x4701
#define GFX_GET_INFO     0x4702
#define GFX_PRESENT      0x4703

/* GFX_CREATE_VIEW — input; describes the full-screen rectangle the
 * caller wants the kernel to render into.  x, y are full-screen
 * coordinates; w, h must fit within the framebuffer and not overflow
 * when multiplied (kernel checks `x <= fb_w && w <= fb_w - x`). */
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} gfx_view_desc_t;

_Static_assert(sizeof(gfx_view_desc_t) == 16,
               "gfx_view_desc_t must be 16 bytes (ABI)");

/* GFX_GET_INFO — output; describes the configured view's local
 * coordinate frame.  stride is `width * 4` in bytes; format is
 * GFX_FORMAT_RGB32 today. */
typedef struct {
    uint32_t width;       /* configured view's local width  */
    uint32_t height;      /* configured view's local height */
    uint32_t stride;      /* bytes per scanline = width * 4 */
    uint32_t format;      /* GFX_FORMAT_RGB32 */
} gfx_info_t;

_Static_assert(sizeof(gfx_info_t) == 16,
               "gfx_info_t must be 16 bytes (ABI)");

/* GFX_PRESENT — input; user supplies the framebuffer pointer and
 * stride for this present.  The kernel never caches the pointer;
 * the next call must re-supply it.  reserved must be 0. */
typedef struct {
    uint64_t pixels;      /* this-process user pointer; per-call */
    uint32_t stride;      /* must equal info.stride */
    uint32_t reserved;    /* must be 0 */
} gfx_present_req_t;

_Static_assert(sizeof(gfx_present_req_t) == 16,
               "gfx_present_req_t must be 16 bytes (ABI)");

#endif /* _UAPI_GFX_H */
