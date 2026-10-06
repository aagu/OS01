#ifndef _KERNEL_FB_H
#define _KERNEL_FB_H

#include <stdint.h>
#include <uapi/fb.h>

// Populate `out` from the live framebuffer state (Pos).  Returns
// 0 on success, -EINVAL on a NULL out.  Used by kernel/driver/gfx.c
// to validate view dimensions against the real framebuffer.
int fb_get_info(struct fb_info *out);

// Write `row_bytes` of pixel data from `pixels` (a kernel buffer)
// into the framebuffer at column `x`, row `y`.  Validates the
// rectangle against Pos.XResolution, Pos.YResolution, and
// Pos.FB_length; returns -EINVAL on out-of-range.  The pixels
// pointer MUST point to kernel memory (caller-supplied); no
// fault-tolerant copy is performed here.  Used by kernel/driver/gfx.c
// to render rows of a configured /dev/gfx0 view.
int fb_write_row(uint32_t x, uint32_t y, const void *pixels,
                 uint32_t row_bytes);

#endif