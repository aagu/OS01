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

    /* The view is INVALID unless begin() returns 0: clear it first, so a
     * failure can never leave a stale framebuffer base behind. */
    view->snap = NULL;
    view->addr = NULL;
    view->xres = 0;
    view->yres = 0;

    /* Pos.XResolution/YResolution are signed: a negative value must not be
     * cast into a huge unsigned resolution. */
    if (!Pos.FB_addr || Pos.XResolution <= 0 || Pos.YResolution <= 0)
        return -ENODEV;

    view->addr = Pos.FB_addr;
    view->xres = (uint32_t)Pos.XResolution;
    view->yres = (uint32_t)Pos.YResolution;

    return 0;
}

__attribute__((weak))
void arch_fb_write_end(struct fb_write_view *view)
{
    (void)view;
}
