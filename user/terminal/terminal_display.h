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
#define TERMINAL_PRESENT_SKIPPED 1   /* normal pause in background: 0=committed, -1=error, 1=skipped */

/* Per-call metrics (Task 3 spec §8).  All counters are uint64_t.
 *
 * success_* fields accumulate the SUCCESSFUL logical frames:
 *   - full_frames    : successful full-view presents (Task 3)
 *   - partial_frames : successful partial-rect presents (Task 5+)
 *   - success_ns     : total wall-clock time of those frames (flush+ioctl)
 *   - success_bytes  : total bytes the kernel actually wrote (sum of rect
 *                      sizes; full-frame uses view_w*view_h*4)
 *   - success_max_ns : longest single successful frame
 *
 * failed_* fields accumulate the FAILED logical-frame attempts (anything
 *   that issued an ioctl that returned -1):
 *   - failed_attempts     : count of failed logical frames
 *   - failed_ns           : total wall-clock time of those frames
 *   - failed_max_ns       : longest single failed frame
 *   - failed_completed_rect_bytes : bytes that succeeded BEFORE the failing
 *                                      ioctl (lower bound; the failing
 *                                      ioctl's prefix is unknown — always
 *                                      reported as `failed_prefix=unknown`
 *                                      in the serial record)
 *
 * neutral rect (zeroed/never emitted):
 *   - noop_calls    : call returned without any damage to submit
 *   - skipped_calls : call was skipped because !foreground or serial_only
 *   - rect_calls    : total number of ioctl attempts issued across all
 *                     frames (success + failed); not in the per-call
 *                     record — sum across the interval */
typedef struct {
    uint64_t full_frames;
    uint64_t partial_frames;
    uint64_t success_ns;
    uint64_t success_bytes;
    uint64_t success_max_ns;
    uint64_t noop_calls;
    uint64_t skipped_calls;
    uint64_t failed_attempts;
    uint64_t failed_ns;
    uint64_t failed_max_ns;
    uint64_t failed_completed_rect_bytes;
    uint64_t rect_calls;
} terminal_present_metrics_t;

/* stats_fd lifecycle values (terminal_display_t::stats_fd): */
#define TERMINAL_STATS_FD_UNOPENED  (-2)  /* stats_open() has not been attempted */
#define TERMINAL_STATS_FD_DISABLED  (-1)  /* open() failed once; never retry */

/* Injectable backend (defaults = real libgfx / FBIOGET_STATE ioctl /
 * CLOCK_MONOTONIC / /dev/serial). */
typedef struct terminal_display_ops {
    gfx_handle_t *(*gfx_open)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void (*gfx_close)(gfx_handle_t *h);
    int  (*gfx_present)(gfx_handle_t *h);
    int  (*gfx_present_rect)(gfx_handle_t *h, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h_);
    int  (*gfx_register_fallback)(gfx_handle_t *h);
    int  (*gfx_get_display_state)(gfx_handle_t *h, gfx_display_state_t *st);
    int  (*gfx_replace_fallback)(gfx_handle_t *old_h, gfx_handle_t *new_h);
    int  (*get_state)(int fb_fd, struct fb_state *st);
    /* CLOCK_MONOTONIC reader.  Returns nanoseconds since an arbitrary
     * but stable epoch; injectable for host tests so the harness can
 *     pin time without touching libc's clock_gettime. */
    uint64_t (*now_ns)(void);
    /* Lazy stats-fd lifecycle: open the dedicated /dev/serial for the
     * per-second metrics dump; default impl returns -1 if the device is
     * missing.  Hooks are idempotent and must NOT clobber caller's
     * errno on success. */
    int  (*stats_open)(void);
    int  (*stats_write)(int fd, const char *text, size_t len); /* 0/-1 */
    int  (*stats_close)(int fd);
    /* Test-only (FB_RESOLUTION_TEST) fault hook: returns 1 once to force
     * the next resource prepare to fail with ENOMEM.  NULL — and never
     * consulted — in production builds. */
    int  (*consume_terminal_enomem)(void);
} terminal_display_ops_t;

typedef struct terminal_display {
    gfx_handle_t *gfx;
    term_core_t  *core;
    term_render_t render;
    const psf2_t *font;
    struct fb_info info;
    uint64_t generation;
    uint64_t owner_generation;   /* display ownership generation from GFX_GET_DISPLAY_STATE */
    bool foreground;             /* true if this view currently holds screen foreground */
    uint32_t fg, bg;
    uint64_t retry_deadline_ms;   /* 0 = none */
    bool serial_only;             /* permanent device failure: no graphics */
    bool redraw_pending;          /* new view bound or foreground restored, full redraw not yet presented */
    bool pending_winsize;         /* PTY master TIOCSWINSZ not yet delivered */
    bool refresh_presented;       /* true if the last terminal_display_refresh executed a redraw+present */
    /* Task 3: presenter-side metrics.  Only display_submit() touches the
     * counters; callers read metrics for assertions but must not mutate. */
    bool present_retry_pending;   /* spec §7; Task 3 reserves the bit (always
                                 * false in this task — Task 5 arms it). */
    int stats_fd;                 /* TERMINAL_STATS_FD_UNOPENED until first
                                 * stats_open() attempt; TERMINAL_STATS_FD_
                                 * DISABLED after a failed open (no retry). */
    terminal_present_metrics_t metrics;
    uint64_t metrics_begin_ns;    /* ns timestamp when the current metrics
                                 * interval started; reset on each successful
                                 * interval emit. */
    terminal_display_ops_t ops;
} terminal_display_t;

/* Bind an already-open view + core.  Does not take ownership of fds. */
void terminal_display_init(terminal_display_t *d, gfx_handle_t *gfx,
                           term_core_t *core, const psf2_t *font,
                           const struct fb_info *info, uint64_t generation,
                           uint32_t fg, uint32_t bg);

/* Synchronize owner_generation and foreground state with the underlying display. */
int terminal_display_sync_owner(terminal_display_t *d);

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

/* Task 3 unified entry: terminal_display_present() and the redraw path
 * inside terminal_display_refresh() both flow through a single private
 * `display_submit()` (defined as `static` in terminal_display.c).  In
 * Task 3 it submits ONE full-frame GFX_PRESENT ioctl and updates the
 * terminal_present_metrics_t counters; Task 5 will replace the body
 * with damage-aware selection while keeping the signature.
 *
 * Tests drive display_submit() through the public entry points:
 *   - ordinary present: terminal_display_present()
 *   - refresh redraw : terminal_display_refresh() (which calls redraw()
 *                           → terminal_display_present())
 * Both paths must increment the SAME metrics struct. */

#endif
