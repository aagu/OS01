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

// NOTE: there is deliberately no lease-less `fb_write_row()` wrapper any
// more.  Every production writer goes through fb_write_row_leased() under
// an explicit lease (gfx.c holds one for the whole frame); the old
// single-lease wrapper had zero callers and was only ever mocked by the
// host tests.  Roadmap parked item "QEMU 分辨率切换器收尾 follow-up" #5.

void fb_set_surrendered_locked(bool surrendered);
bool fb_is_surrendered_locked(void);

struct devfs_ops;
extern const struct devfs_ops fb_ops;

#endif