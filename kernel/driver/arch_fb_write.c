/* kernel/driver/arch_fb_write.c — weak default arch_fb_write_begin/end.
 *
 * The default describes an arch that has a framebuffer but NO display
 * coordinator: there is no writer lease, no owner and no mode concept, so
 * "exclusive access" is just the caller's Pos.lock.  aarch64 links this
 * default; x86_64 overrides it with the coordinator-backed strong symbol
 * in kernel/arch/x86_64/platform/fb_write.c.
 *
 * Nothing here pulls in the coordinator: the aarch64 build stays minimal.
 */

#include <arch/fb_write.h>
#include <core/printk.h>
#include <errno.h>

__attribute__((weak))
int arch_fb_write_begin(struct fb_write_view *view)
{
    if (!view)
        return -EINVAL;

    view->snap = NULL;
    view->addr = Pos.FB_addr;
    view->xres = (uint32_t)Pos.XResolution;
    view->yres = (uint32_t)Pos.YResolution;

    if (!view->addr || view->xres == 0 || view->yres == 0)
        return -ENODEV;

    return 0;
}

__attribute__((weak))
void arch_fb_write_end(struct fb_write_view *view)
{
    (void)view;
}
