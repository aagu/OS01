#ifndef _ARCH_FB_WRITE_H
#define _ARCH_FB_WRITE_H

#include <stdint.h>

#include <driver/fb_state.h>   /* fb_lease_t — TYPE ONLY, no link dependency */
#include <arch/identity.h>     /* ARCH_NAME, ARCH_BOOT_LOGO_LABEL */

/* An exclusive view of the framebuffer.
 *
 * Obtained with arch_fb_write_begin() and released with
 * arch_fb_write_end().  The view OWNS the arch-private storage, so every
 * pointer in it stays valid until the matching end() call:
 *
 *   1. the view is caller-owned and MUST outlive begin -> draw -> end;
 *   2. begin() MUST NOT return a pointer to storage that dies when it
 *      returns (the x86_64 override keeps the lease inside the view);
 *   3. begin() and end() are strictly paired, in the same scope, never
 *      nested, and never end() without begin();
 *   4. the caller serialises the pair (the boot logo holds Pos.lock across
 *      it), so no implementation may assume re-entrancy;
 *   5. the view is INVALID unless begin() returned 0; zero-initialise it first.
 */
struct fb_write_view {
    /* Arch-private storage owned by the view.  The x86_64 override keeps
     * the display-coordinator lease here; the aarch64 weak default leaves
     * it untouched. */
    fb_lease_t lease;

    /* NULL when the arch has no display coordinator (aarch64). */
    const struct fb_snapshot *snap;

    /* Framebuffer base and geometry to draw into. */
    uint32_t *addr;
    uint32_t xres;
    uint32_t yres;
};

/* Acquire exclusive framebuffer write access.  Returns 0 on success,
 * negative on failure (no framebuffer, or no exclusive access). */
int arch_fb_write_begin(struct fb_write_view *view);

/* Release what begin() acquired.  Safe to call after a failed begin(). */
void arch_fb_write_end(struct fb_write_view *view);

#endif /* _ARCH_FB_WRITE_H */
