// kernel/arch/x86_64/driver/fb_copy.c — the x86_64 strong override of
// the arch-neutral framebuffer copy facade.
//
// The default (kernel/driver/arch_fb_write.c) is a plain byte copy.
// x86_64 replaces it with the wide 32-bit sequential store, which is
// ~58x faster on real hardware when a full frame is written.
//
// The override lives in the x86 tree so the arch-neutral sources —
// kernel/driver/fb.c and kernel/core/printk.c — never have to name an
// x86 header.

#include <arch/fb_copy.h>
#include <arch/x86_64/fb_copy.h>

void fb_copy_rgb32(void *dst, const void *src, size_t bytes)
{
    x86_fb_copy_rgb32(dst, src, bytes);
}
