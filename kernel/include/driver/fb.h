#ifndef _KERNEL_FB_H
#define _KERNEL_FB_H

#include <stdint.h>
#include <uapi/fb.h>
#include <driver/fb_state.h>

// Populate `out` from the live framebuffer state (Pos).  Returns
// 0 on success, -EINVAL on a NULL out.  Used by kernel/driver/gfx.c
// to validate view dimensions against the real framebuffer.
int fb_get_info(struct fb_info *out);

// Write `bytes` of pixel data from `pixels` (a kernel buffer)
// into the framebuffer at column `x`, row `y` under an active lease.
int fb_write_row_leased(const fb_lease_t *lease, uint32_t x, uint32_t y,
                        const void *pixels, uint32_t bytes);

// Write `row_bytes` of pixel data from `pixels` (a kernel buffer)
// into the framebuffer at column `x`, row `y`.
// Acquires a single lease and wraps fb_write_row_leased.
int fb_write_row(uint32_t x, uint32_t y, const void *pixels,
                 uint32_t row_bytes);

struct devfs_ops;
extern const struct devfs_ops fb_ops;

#endif