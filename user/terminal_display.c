// user/terminal_display.c — display-mode transaction helper for terminal.elf.
//
// NOT a standalone program (excluded from user/Makefile C_SOURCES, linked
// explicitly into terminal.elf).  Owns neither fb_fd nor the PTY/ash: the
// caller passes fb_fd in and cleans up with terminal_display_close().
//
// Recovery order (old view stays alive until the new one is fully bound):
//   GET_STATE -> gfx_open(new) -> GET_STATE again (must be unchanged)
//   -> term_core_resize -> term_render_init(new) -> swap -> close(old)
//   -> redraw + present.
// Any failure before the swap closes only the *new* view and arms a 250ms
// retry; the old view and the core are left untouched.

#include "terminal_display.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

static gfx_handle_t *real_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return gfx_open(x, y, w, h);
}
static void real_close(gfx_handle_t *h) { gfx_close(h); }
static int real_present(gfx_handle_t *h) { return gfx_present(h); }
static int real_state(int fb_fd, struct fb_state *st)
{
    return ioctl(fb_fd, FBIOGET_STATE, st) < 0 ? -1 : 0;
}

void terminal_display_init(terminal_display_t *d, gfx_handle_t *gfx,
                           term_core_t *core, const psf2_t *font,
                           const struct fb_info *info, uint64_t generation,
                           uint32_t fg, uint32_t bg)
{
    memset(d, 0, sizeof(*d));
    d->ops.gfx_open = real_open;
    d->ops.gfx_close = real_close;
    d->ops.gfx_present = real_present;
    d->ops.get_state = real_state;
    d->gfx = gfx;
    d->core = core;
    d->font = font;
    d->info = *info;
    d->generation = generation;
    d->fg = fg;
    d->bg = bg;
    term_render_init(&d->render, gfx, font, core, fg, bg);
}

// Device errors that no amount of retrying will fix.
static bool errno_is_permanent(int e)
{
    return e == EIO || e == ENODEV || e == ENXIO || e == EBADF || e == ENOTTY;
}

// Arm the retry window, or go serial_only on a permanent error.
static int fail(terminal_display_t *d, int e, uint64_t now_ms)
{
    if (errno_is_permanent(e)) {
        d->serial_only = true;
        d->retry_deadline_ms = 0;
        errno = e;
        return -1;
    }
    d->retry_deadline_ms = now_ms + TERMINAL_RETRY_MS;
    errno = e;
    return 1;
}

int terminal_display_present(terminal_display_t *d, uint64_t now_ms)
{
    if (d->serial_only || !d->gfx) return 0;
    term_render_flush(&d->render);
    if (d->ops.gfx_present(d->gfx) == 0) return 0;
    // errno belongs to the failing present; only arm the retry (no syscalls).
    if (errno == EAGAIN || errno == ESTALE)
        d->retry_deadline_ms = now_ms + TERMINAL_RETRY_MS;
    return -1;
}

// Redraw the whole screen into the current view and present it.
static int redraw(terminal_display_t *d, uint64_t now_ms)
{
    term_render_clear(&d->render);
    term_core_mark_all_dirty(d->core);
    d->redraw_pending = true;
    if (terminal_display_present(d, now_ms) == 0) {
        d->redraw_pending = false;
        d->retry_deadline_ms = 0;
        return 0;
    }
    return fail(d, errno, now_ms);
}

int terminal_display_refresh(terminal_display_t *d, int fb_fd, uint64_t now_ms)
{
    if (d->serial_only) return -1;
    if (d->retry_deadline_ms && now_ms < d->retry_deadline_ms) return 1;

    struct fb_state st;
    if (d->ops.get_state(fb_fd, &st) != 0) {
        int e = errno;
        return fail(d, e, now_ms);
    }
    if (st.generation == d->generation) {
        if (d->redraw_pending) return redraw(d, now_ms);
        d->retry_deadline_ms = 0;
        return 0;
    }

    uint32_t w = st.info.width, h = st.info.height;
    if (w == 0 || h == 0 || d->font->width == 0 || d->font->height == 0)
        return fail(d, EINVAL, now_ms);
    int cols = (int)(w / d->font->width);
    int rows = (int)(h / d->font->height);
    if (cols <= 0 || rows <= 0) return fail(d, EINVAL, now_ms);

    gfx_handle_t *nh = d->ops.gfx_open(0, 0, w, h);
    if (!nh) {
        int e = errno;
        return fail(d, e, now_ms);
    }

    // The mode may have changed again while the view was being created.
    struct fb_state st2;
    if (d->ops.get_state(fb_fd, &st2) != 0) {
        int e = errno;
        d->ops.gfx_close(nh);
        return fail(d, e, now_ms);
    }
    if (st2.generation != st.generation ||
        st2.info.width != w || st2.info.height != h) {
        d->ops.gfx_close(nh);
        d->retry_deadline_ms = now_ms + TERMINAL_RETRY_MS;
        errno = ESTALE;
        return 1;
    }

    if (term_core_resize(d->core, rows, cols) != 0) {
        d->ops.gfx_close(nh);
        return fail(d, ENOMEM, now_ms);
    }

    // Commit point: bind the renderer to the new view, then drop the old one.
    gfx_handle_t *old = d->gfx;
    term_render_init(&d->render, nh, d->font, d->core, d->fg, d->bg);
    d->gfx = nh;
    d->info = st2.info;
    d->generation = st2.generation;
    d->pending_winsize = true;
    if (old) d->ops.gfx_close(old);

    return redraw(d, now_ms);
}

int terminal_display_poll_timeout(const terminal_display_t *d,
                                  int base_timeout_ms, uint64_t now_ms)
{
    int t = base_timeout_ms;
    if (t < 0 || t > (int)TERMINAL_RETRY_MS) t = (int)TERMINAL_RETRY_MS;
    if (!d->serial_only && d->retry_deadline_ms) {
        int w = d->retry_deadline_ms > now_ms
                    ? (int)(d->retry_deadline_ms - now_ms) : 0;
        if (w < t) t = w;
    }
    return t;
}

bool terminal_display_winsize(const terminal_display_t *d,
                              unsigned short *rows, unsigned short *cols,
                              unsigned short *xpix, unsigned short *ypix)
{
    if (!d->pending_winsize) return false;
    *rows = (unsigned short)d->core->rows;
    *cols = (unsigned short)d->core->cols;
    *xpix = (unsigned short)d->info.width;
    *ypix = (unsigned short)d->info.height;
    return true;
}

void terminal_display_close(terminal_display_t *d)
{
    if (d->gfx) {
        d->ops.gfx_close(d->gfx);
        d->gfx = NULL;
        d->render.gfx = NULL;
    }
}
