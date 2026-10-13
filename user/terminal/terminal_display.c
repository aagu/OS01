// user/terminal/terminal_display.c — display-mode transaction helper for terminal.elf.
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
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef FB_RESOLUTION_TEST
#include <fcntl.h>
#include <unistd.h>
#include <uapi/fb_test.h>

/* Test-only fault hook.  Reads the one-shot terminal-ENOMEM fault armed for
 * this PID from /dev/fbtest.  The fd is opened lazily and kept for the
 * process lifetime; a missing device simply disables the hook.  The real
 * terminal.elf (compiled with -DFB_RESOLUTION_TEST) sets this as its
 * consume_terminal_enomem op; production never compiles it. */
static int real_consume_terminal_enomem(void)
{
    static int fd = -2; /* -2 = uninitialised */
    if (fd == -2) {
        fd = open("/dev/fbtest", O_RDWR);
    }
    if (fd < 0) {
        return 0;
    }

    struct fb_test_req req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.target_pid = (uint32_t)getpid();
    int rc = ioctl(fd, FBIOTEST_CONSUME_TERMINAL_ENOMEM, &req);
    /* The ioctl returns 1 when this PID must fail its next prepare once. */
    return rc > 0 ? 1 : 0;
}
#endif

/* ── Production hook defaults (Task 3) ─────────────────────────────────
 *
 * now_ns uses CLOCK_MONOTONIC; production terminal.elf always has access
 * to it via the libc syscall layer (terminal.c uses the same function
 * today).  stats_open/write/close open /dev/serial on demand and write
 * the per-second metrics dump without disturbing the caller's errno. */
static uint64_t real_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int real_stats_open(void)
{
    /* Open lazily — the file may not exist on first boot or during
     * kernel bring-up; treat absence as "metrics disabled" (return -1)
     * and never retry (the device is either present at startup or
     * missing for the entire lifetime of the process). */
    int saved = errno;
    int fd = open("/dev/serial", O_WRONLY);
    int err = errno;
    errno = saved;
    if (fd < 0) {
        errno = err;
        return -1;
    }
    return fd;
}

static int real_stats_write(int fd, const char *text, size_t len)
{
    if (fd < 0 || !text || len == 0) return -1;
    int saved = errno;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            int err = errno;
            if (err == EINTR) continue;
            errno = saved;
            return -1;
        }
        if (n == 0) {
            errno = saved;
            return -1;
        }
        off += (size_t)n;
    }
    errno = saved;
    return 0;
}

static int real_stats_close(int fd)
{
    if (fd < 0) return -1;
    int saved = errno;
    int r = close(fd);
    int err = errno;
    errno = saved;
    return r < 0 ? -1 : 0;
}

/* Internal emit entry: write one `[term-present]` record.  Caller has
 * already snapshotted begin/end_ns; this fn just serialises one
 * canonical line and emits it.  Stats fd lifecycle is preserved
 * (open/write/close are no-ops when disabled).  errno on entry is
 * preserved across the call. */
static int emit_present_record(terminal_display_t *d,
                               uint64_t begin_ns, uint64_t end_ns);

static gfx_handle_t *real_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return gfx_open(x, y, w, h);
}
static void real_close(gfx_handle_t *h) { gfx_close(h); }
static int real_present(gfx_handle_t *h) { return gfx_present(h); }
/* Task 3: the rect hook field exists on terminal_display_ops_t but is
 * NOT wired to libgfx/gfx_present_rect yet.  Task 5 will.  Until then
 * the missing hook falls back to the OLD full present inside
 * display_submit() so the existing tests that only inject gfx_present
 * keep passing. */
static int real_present_rect(gfx_handle_t *h, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h_)
{
    return gfx_present(h);
}
static int real_register_fallback(gfx_handle_t *h) { return gfx_register_fallback(h); }
static int real_get_display_state(gfx_handle_t *h, gfx_display_state_t *st) { return gfx_get_display_state(h, st); }
static int real_replace_fallback(gfx_handle_t *old_h, gfx_handle_t *new_h) { return gfx_replace_fallback(old_h, new_h); }
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
    d->ops.gfx_present_rect = real_present_rect;
    d->ops.gfx_register_fallback = real_register_fallback;
    d->ops.gfx_get_display_state = real_get_display_state;
    d->ops.gfx_replace_fallback = real_replace_fallback;
    d->ops.get_state = real_state;
    d->ops.now_ns = real_now_ns;
    d->ops.stats_open = real_stats_open;
    d->ops.stats_write = real_stats_write;
    d->ops.stats_close = real_stats_close;
#ifdef FB_RESOLUTION_TEST
    d->ops.consume_terminal_enomem = real_consume_terminal_enomem;
#endif
    d->gfx = gfx;
    d->core = core;
    d->font = font;
    d->info = *info;
    d->generation = generation;
    d->owner_generation = 1;
    d->foreground = true;
    d->fg = fg;
    d->bg = bg;
    d->present_retry_pending = false;
    d->stats_fd = TERMINAL_STATS_FD_UNOPENED;
    d->metrics_begin_ns = UINT64_MAX; /* "no interval yet" sentinel */
    term_render_init(&d->render, gfx, font, core, fg, bg);
}

/* Lazy open + emit-eligibility check.  Returns true iff the interval has
 * crossed >= 1 second AND the per-second emit should fire NOW.  Side
 * effect: lazily opens the stats fd on first emit-eligibility transition
 * (after that, an open failure permanently disables the path).
 *
 * `metrics_begin_ns == UINT64_MAX` is the "no interval open yet" sentinel
 * (terminal_display_init sets it that way); we never emit before the
 * first metrics call seeds it.  Real clock values are bounded by
 * CLOCK_MONOTONIC + uptime, well below UINT64_MAX. */
static bool stats_emit_due(terminal_display_t *d, uint64_t now_ns)
{
    if (d->metrics_begin_ns == UINT64_MAX) return false;
    if (d->metrics_begin_ns > now_ns) return false;
    if (now_ns - d->metrics_begin_ns < 1000000000ull) return false;
    /* Lazy open: try once on the first emit-due tick.  A subsequent
     * failure sets stats_fd to DISABLED and never retries. */
    if (d->stats_fd == TERMINAL_STATS_FD_UNOPENED && d->ops.stats_open) {
        d->stats_fd = d->ops.stats_open();
        if (d->stats_fd < 0) {
            d->stats_fd = TERMINAL_STATS_FD_DISABLED;
        }
    }
    return d->stats_fd >= 0;
}

/* Reset counters and seed a new interval anchored at now_ns.  Always
 * preserves the caller's errno across the call. */
static void metrics_reset(terminal_display_t *d, uint64_t now_ns)
{
    int saved = errno;
    memset(&d->metrics, 0, sizeof(d->metrics));
    d->metrics_begin_ns = now_ns;
    errno = saved;
}

/* Append one canonical `[term-present] ...` line.  v7 protocol — Task 3
 * only fills the full_* / success_* / failed_* / noop_* counters;
 * partial_frames / failed_completed_rect_bytes / rect_calls are zero in
 * Task 3 (always emitted with the literal sentinel per spec §8).
 *
 * Returns 0 on success, -1 on failure.  On failure the interval
 * counters are KEPT so the next emit can merge them (spec §8). */
static int emit_present_record(terminal_display_t *d,
                               uint64_t begin_ns, uint64_t end_ns)
{
    if (d->stats_fd < 0 || !d->ops.stats_write) return -1;
    char line[512];
    int n = snprintf(line, sizeof line,
        "[term-present] version=1 begin_ns=%llu end_ns=%llu "
        "full_frames=%llu partial_frames=%llu "
        "success_ns=%llu success_bytes=%llu success_max_ns=%llu "
        "noop_calls=%llu skipped_calls=%llu "
        "failed_attempts=%llu failed_ns=%llu failed_max_ns=%llu "
        "failed_completed_rect_bytes=%llu failed_prefix=unknown "
        "rect_calls=%llu\r\n",
        (unsigned long long)begin_ns,
        (unsigned long long)end_ns,
        (unsigned long long)d->metrics.full_frames,
        (unsigned long long)d->metrics.partial_frames,
        (unsigned long long)d->metrics.success_ns,
        (unsigned long long)d->metrics.success_bytes,
        (unsigned long long)d->metrics.success_max_ns,
        (unsigned long long)d->metrics.noop_calls,
        (unsigned long long)d->metrics.skipped_calls,
        (unsigned long long)d->metrics.failed_attempts,
        (unsigned long long)d->metrics.failed_ns,
        (unsigned long long)d->metrics.failed_max_ns,
        (unsigned long long)d->metrics.failed_completed_rect_bytes,
        (unsigned long long)d->metrics.rect_calls);
    if (n <= 0 || (size_t)n >= sizeof line) return -1;
    if (d->ops.stats_write(d->stats_fd, line, (size_t)n) != 0) return -1;
    return 0;
}

/* Task 3 unified entry: ordinary present and redraw both flow through
 * here.  In Task 3 the body submits ONE full-frame GFX_PRESENT ioctl
 * and updates terminal_present_metrics_t accordingly.  Task 5 will
 * replace the body with damage-aware selection.
 *
 * Caller contract:
 *   - `d` is a valid terminal_display_t (not serial_only).  Background
 *     / serial_only are handled by terminal_display_present() and the
 *     redraw wrapper BEFORE reaching this fn.
 *   - `now_ms` is the wall-clock timestamp from terminal.c's
 *     `clock_gettime(CLOCK_MONOTONIC)` and is only used as a fallback
 *     for the metrics clock if ops.now_ns is NULL.
 *   - `*submitted` is set to true iff exactly one ioctl was issued AND
 *     that ioctl returned success.  Spec §7 ack rule: only ALL-true
 *     frames count as full_frames.
 *
 * Return: 0 = no error (success or no-op submitted frame); the ioctl's
 * errno is left in errno (terminal_display_present() preserves it for
 * its caller).  Metrics-clock reads happen ONCE each at the start and
 * immediately after the ioctl returns — no syscalls between the two
 * samples. */
static int display_submit(terminal_display_t *d, uint64_t now_ms,
                          bool *submitted)
{
    if (submitted) *submitted = false;

    /* Ensure we have an interval open before incrementing anything.
     * The sentinel is UINT64_MAX (never a real clock value) so the
     * check below also works when the first interval legitimately
     * starts at clock=0.  Once set, the only way back to UNSET is
     * through metrics_reset() at a successful emit. */
    uint64_t t0_ns;
    if (d->ops.now_ns) {
        t0_ns = d->ops.now_ns();
    } else {
        t0_ns = (uint64_t)now_ms * 1000000ull;
    }
    if (d->metrics_begin_ns == UINT64_MAX) d->metrics_begin_ns = t0_ns;

    /* Flush dirty cells to the view's private buffer.  This is part of
     * the measured window — present_cost depends on what we drew. */
    term_render_flush(&d->render);

    /* Full-frame path (Task 3): one GFX_PRESENT ioctl.  Task 5 will
     * branch on pending damage here. */
    uint64_t before = d->ops.now_ns ? d->ops.now_ns() : t0_ns;
    int rc = d->ops.gfx_present(d->gfx);
    uint64_t after = d->ops.now_ns ? d->ops.now_ns() : before;
    uint64_t dt = after - before;

    d->metrics.rect_calls++; /* one ioctl attempted */

    if (rc != 0) {
        d->metrics.failed_attempts++;
        d->metrics.failed_ns += dt;
        if (dt > d->metrics.failed_max_ns) d->metrics.failed_max_ns = dt;
        /* On failure the failed ioctl's byte prefix is unknown; we
         * MUST keep the success_* and rect_* counters for the next
         * interval to merge (spec §8). */
    } else {
        /* Success path.  Bytes for a full present are the entire
         * configured view (info.width * info.height * 4 == info.stride
         * * info.height for RGB32).  Use stride*height as the canonical
         * byte count — it's what the kernel wrote. */
        uint64_t bytes = (uint64_t)d->info.stride * d->info.height;
        d->metrics.full_frames++;
        d->metrics.success_ns += dt;
        d->metrics.success_bytes += bytes;
        if (dt > d->metrics.success_max_ns) d->metrics.success_max_ns = dt;
        if (submitted) *submitted = true;
    }

    /* Per-second emit decision: lazy-open fd once, then emit.  On
     * successful emit we close the interval (reset counters + new
     * begin).  On emit failure the counters are retained so the
     * next interval can merge them (spec §8). */
    uint64_t end_ns = d->ops.now_ns ? d->ops.now_ns() : t0_ns;
    if (stats_emit_due(d, end_ns)) {
        uint64_t begin_ns = d->metrics_begin_ns;
        if (emit_present_record(d, begin_ns, end_ns) == 0) {
            metrics_reset(d, end_ns);
        }
        /* On emit failure (stats_write returned -1) the counters are
         * kept; the next emit_due attempt will merge. */
    }
    return rc;
}

/* Background / serial-only pre-classifier — increments skipped_calls
 * exactly once per call and returns 0 (the present path treats
 * background as a no-op success for caller-side bookkeeping).
 * Kept inline in terminal_display_present() for clarity; no separate
 * helper. */

int terminal_display_sync_owner(terminal_display_t *d)
{
    if (!d->gfx || !d->ops.gfx_get_display_state) {
        return -1;
    }
    gfx_display_state_t dst;
    if (d->ops.gfx_get_display_state(d->gfx, &dst) != 0) {
        return -1;
    }
    d->owner_generation = dst.generation;
    d->foreground = (dst.flags & GFX_DISPLAY_STATE_FOREGROUND) != 0;
    return 0;
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
    if (d->serial_only || !d->gfx) {
        /* Spec §8: serial-only / no-handle calls increment skipped_calls
         * so the per-second record reflects "no work done". */
        uint64_t t0 = d->ops.now_ns ? d->ops.now_ns() : (uint64_t)now_ms * 1000000ull;
        if (d->metrics_begin_ns == UINT64_MAX) d->metrics_begin_ns = t0;
        d->metrics.skipped_calls++;
        return 0;
    }
    if (!d->foreground) {
        uint64_t t0 = d->ops.now_ns ? d->ops.now_ns() : (uint64_t)now_ms * 1000000ull;
        if (d->metrics_begin_ns == UINT64_MAX) d->metrics_begin_ns = t0;
        d->metrics.skipped_calls++;
        return TERMINAL_PRESENT_SKIPPED;
    }

    bool submitted = false;
    int rc = display_submit(d, now_ms, &submitted);
    if (rc == 0) {
        /* Ack: clear pending damage state and any retry deadline. */
        d->retry_deadline_ms = 0;
        d->present_retry_pending = false;
        return 0;
    }

    int e = errno;
    if (e == EACCES) {
        /* Background pause: non-owner calling present. Do not count as failure. */
        d->foreground = false;
        d->redraw_pending = true;
        return TERMINAL_PRESENT_SKIPPED;
    }
    // errno belongs to the failing present; only arm the retry (no syscalls).
    if (e == EAGAIN || e == ESTALE) {
        d->retry_deadline_ms = now_ms + TERMINAL_RETRY_MS;
        d->present_retry_pending = true;
    }
    errno = e;
    return -1;
}

// Redraw the whole screen into the current view and present it.
static int redraw(terminal_display_t *d, uint64_t now_ms)
{
    term_render_clear(&d->render);
    term_core_mark_all_dirty(d->core);
    d->redraw_pending = true;
    int rc = terminal_display_present(d, now_ms);
    if (rc == 0) {
        d->redraw_pending = false;
        d->retry_deadline_ms = 0;
        d->present_retry_pending = false;
        d->refresh_presented = true;
        return 0;
    }
    if (rc == TERMINAL_PRESENT_SKIPPED) {
        return 0;
    }
    return fail(d, errno, now_ms);
}

int terminal_display_refresh(terminal_display_t *d, int fb_fd, uint64_t now_ms)
{
    d->refresh_presented = false;
    if (d->serial_only) return -1;
    if (d->retry_deadline_ms && now_ms < d->retry_deadline_ms) return 1;

    /* Check ownership state */
    if (d->gfx && d->ops.gfx_get_display_state) {
        gfx_display_state_t dst;
        if (d->ops.gfx_get_display_state(d->gfx, &dst) == 0) {
            bool was_fg = d->foreground;
            bool is_fg = (dst.flags & GFX_DISPLAY_STATE_FOREGROUND) != 0;
            d->foreground = is_fg;

            if (dst.generation != d->owner_generation || (!was_fg && is_fg)) {
                d->owner_generation = dst.generation;
                if (is_fg) {
                    d->redraw_pending = true;
                }
            }
        }
    }

    struct fb_state st;
    if (d->ops.get_state(fb_fd, &st) != 0) {
        int e = errno;
        return fail(d, e, now_ms);
    }
    if (st.generation == d->generation) {
        if (d->redraw_pending && d->foreground) return redraw(d, now_ms);
        d->retry_deadline_ms = 0;
        return 0;
    }

    uint32_t w = st.info.width, h = st.info.height;
    if (w == 0 || h == 0 || d->font->width == 0 || d->font->height == 0)
        return fail(d, EINVAL, now_ms);
    int cols = (int)(w / d->font->width);
    int rows = (int)(h / d->font->height);
    if (cols <= 0 || rows <= 0) return fail(d, EINVAL, now_ms);

#ifdef FB_RESOLUTION_TEST
    /* Test-only: fail this resource prepare once, exactly as a gfx_open /
     * core-array ENOMEM would.  Nothing has been allocated yet, so the old
     * view/core stay intact and the 250 ms retry re-runs the prepare. */
    if (d->ops.consume_terminal_enomem && d->ops.consume_terminal_enomem()) {
        return fail(d, ENOMEM, now_ms);
    }
#endif

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

    // Transfer fallback role from old view to new view before closing old
    if (d->gfx && d->ops.gfx_replace_fallback) {
        if (d->ops.gfx_replace_fallback(d->gfx, nh) != 0) {
            int e = errno;
            d->ops.gfx_close(nh);
            return fail(d, e, now_ms);
        }
    }

    // Update display ownership state for the new view
    if (d->ops.gfx_get_display_state) {
        gfx_display_state_t dst2;
        if (d->ops.gfx_get_display_state(nh, &dst2) == 0) {
            d->owner_generation = dst2.generation;
            d->foreground = (dst2.flags & GFX_DISPLAY_STATE_FOREGROUND) != 0;
        }
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
    if (!d->serial_only && d->retry_deadline_ms && d->retry_deadline_ms > now_ms) {
        int w = (int)(d->retry_deadline_ms - now_ms);
        if (w > (int)TERMINAL_RETRY_MS) w = (int)TERMINAL_RETRY_MS;
        if (w <= 0) w = 1;
        if (base_timeout_ms <= 0) {
            return w;
        }
        int b = base_timeout_ms;
        if (b > (int)TERMINAL_RETRY_MS) b = (int)TERMINAL_RETRY_MS;
        return (b < w) ? b : w;
    }

    int t = base_timeout_ms;
    if (t < 0 || t > (int)TERMINAL_RETRY_MS) t = (int)TERMINAL_RETRY_MS;
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
    /* Spec §8: a final short interval after the last emit IS emitted
     * with complete begin/end boundaries.  Ensure stats_fd is opened
     * (best effort) and the final interval is drained before close. */
    if (d->metrics_begin_ns != UINT64_MAX) {
        uint64_t now_ns = d->ops.now_ns ? d->ops.now_ns() : 0;
        if (d->stats_fd == TERMINAL_STATS_FD_UNOPENED && d->ops.stats_open) {
            int saved = errno;
            int f = d->ops.stats_open();
            int err = errno;
            errno = saved;
            d->stats_fd = (f < 0) ? TERMINAL_STATS_FD_DISABLED : f;
            (void)err;
        }
        if (d->stats_fd >= 0 && now_ns != 0) {
            /* Best-effort final emit; do not retry on failure — the
             * process is exiting.  Mark interval drained regardless
             * so the next close() is idempotent. */
            emit_present_record(d, d->metrics_begin_ns, now_ns);
        }
        /* Mark interval as drained; do NOT mutate counters here — the
         * caller is about to free `d` anyway. */
        d->metrics_begin_ns = UINT64_MAX;
    }
    if (d->stats_fd >= 0 && d->ops.stats_close) {
        int saved = errno;
        d->ops.stats_close(d->stats_fd);
        errno = saved;
        d->stats_fd = TERMINAL_STATS_FD_DISABLED;
    }
}
