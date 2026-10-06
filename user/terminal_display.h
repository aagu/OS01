#ifndef _TERMINAL_DISPLAY_H
#define _TERMINAL_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <gfx.h>
#include <uapi/fb.h>
#include "terminal_core.h"
#include "terminal_render.h"

/* Internal helper TU of terminal.elf (NOT a standalone program).  It never
 * owns the PTY/ash or any fd: terminal.c owns fb_fd and passes it in, and
 * owns the term_core_t storage + final cleanup (terminal_display_close()). */

#define TERMINAL_RETRY_MS     250u   /* retry / idle-poll upper bound */

/* Injectable backend (defaults = real libgfx / FBIOGET_STATE ioctl). */
typedef struct terminal_display_ops {
    gfx_handle_t *(*gfx_open)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void (*gfx_close)(gfx_handle_t *h);
    int  (*gfx_present)(gfx_handle_t *h);
    int  (*get_state)(int fb_fd, struct fb_state *st);
} terminal_display_ops_t;

typedef struct terminal_display {
    gfx_handle_t *gfx;
    term_core_t  *core;
    term_render_t render;
    const psf2_t *font;
    struct fb_info info;
    uint64_t generation;
    uint32_t fg, bg;
    uint64_t retry_deadline_ms;   /* 0 = none */
    bool serial_only;             /* permanent device failure: no graphics */
    bool redraw_pending;          /* new view bound, full redraw not yet presented */
    bool pending_winsize;         /* PTY master TIOCSWINSZ not yet delivered */
    terminal_display_ops_t ops;
} terminal_display_t;

/* Bind an already-open view + core.  Does not take ownership of fds. */
void terminal_display_init(terminal_display_t *d, gfx_handle_t *gfx,
                           term_core_t *core, const psf2_t *font,
                           const struct fb_info *info, uint64_t generation,
                           uint32_t fg, uint32_t bg);

/* 0 = nothing to recover / recovered, 1 = retry after 250ms, -1 = permanent
 * device failure (d->serial_only set; caller keeps forwarding to serial). */
int terminal_display_refresh(terminal_display_t *d, int fb_fd, uint64_t now_ms);

/* Flush dirty cells + gfx_present.  0 / -1; errno from the failing present is
 * preserved (this helper makes no syscall after it).  EAGAIN/ESTALE arm the
 * 250ms retry deadline.  In serial_only mode returns 0 without drawing. */
int terminal_display_present(terminal_display_t *d, uint64_t now_ms);

/* poll() timeout: base_timeout_ms (-1 = infinite) clamped to <=250 and to the
 * pending retry deadline. */
int terminal_display_poll_timeout(const terminal_display_t *d,
                                  int base_timeout_ms, uint64_t now_ms);

/* Consume pending_winsize: fills the four winsize fields for the current view
 * and returns true when a TIOCSWINSZ is owed. */
bool terminal_display_winsize(const terminal_display_t *d,
                              unsigned short *rows, unsigned short *cols,
                              unsigned short *xpix, unsigned short *ypix);

/* Close the current view exactly once (idempotent). */
void terminal_display_close(terminal_display_t *d);

#endif
