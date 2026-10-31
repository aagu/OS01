/* kernel/arch/x86_64/platform/fb_write.c — strong override of the
 * arch_fb_write_begin/end facade.
 *
 * x86_64 owns the display coordinator (kernel/driver/fb_state.c), so
 * exclusive framebuffer access is a WRITER LEASE, not just Pos.lock.  The
 * lease lives inside the caller's view — never in this function's frame —
 * so the snapshot pointer stays valid until arch_fb_write_end().
 *
 * The coordinator's errno is propagated verbatim: -EAGAIN (not ready /
 * transitioning), -EIO (backend failed) and -EBUSY (an owner holds the
 * display) are different failures and must stay distinguishable.
 */

#include <arch/fb_write.h>
#include <core/printk.h>
#include <errno.h>

int arch_fb_write_begin(struct fb_write_view *view)
{
    int rc;

    if (!view)
        return -EINVAL;

    /* The view is INVALID unless begin() returns 0: clear it first, so a
     * failure can never leave a stale framebuffer base behind. */
    view->snap = NULL;
    view->addr = NULL;
    view->xres = 0;
    view->yres = 0;

    rc = fb_writer_begin(&view->lease, 0);
    if (rc < 0)
        return rc;

    view->snap = &view->lease.snapshot;
    view->addr = view->lease.snapshot.addr;
    view->xres = view->lease.snapshot.state.info.width;
    view->yres = view->lease.snapshot.state.info.height;

    /* Fall back to the live Pos geometry when the coordinator has no
     * published mode info yet.  Pos.XResolution/YResolution are signed:
     * a negative value must not be cast into a huge unsigned resolution. */
    if (!view->xres && Pos.XResolution > 0)
        view->xres = (uint32_t)Pos.XResolution;
    if (!view->yres && Pos.YResolution > 0)
        view->yres = (uint32_t)Pos.YResolution;

    if (!view->addr || view->xres == 0 || view->yres == 0) {
        fb_writer_end(&view->lease);
        view->snap = NULL;
        view->addr = NULL;
        view->xres = 0;
        view->yres = 0;
        return -ENODEV;
    }

    return 0;
}

void arch_fb_write_end(struct fb_write_view *view)
{
    if (!view)
        return;
    fb_writer_end(&view->lease);
    view->snap = NULL;
}
