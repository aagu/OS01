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
/* Real gfx_present_rect hook (Task 5): wired to libgfx's wrapper
 * (Task 2).  A NULL handle / invalid geometry inside the wrapper
 * returns -1/EINVAL without issuing any ioctl; the failure is
 * propagated naturally to display_submit's list path. */
static int real_present_rect(gfx_handle_t *h, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h_)
{
    return gfx_present_rect(h, x, y, w, h_);
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

/* ── Path-selected present (Task 5 spec §7) ─────────────────────
 *
 * `display_submit` selects between the full / list / noop / fallback
 * paths based on the renderer's pending damage and the ops-installed
 * rect hook.  It is the single entry point used by both
 * terminal_display_present() (the ordinary path) and the refresh-time
 * retry submit (spec §7 row 4-5).
 *
 * Path selection (binding for Task 5):
 *   1) damage.full OR (damage.count > 0 AND rect hook NULL):
 *      single GFX_PRESENT ioctl (full view); success → full_frames++;
 *      failure → failed_attempts/failed_ns; damage preserved on failure.
 *   2) damage.count == 0: noop → noop_calls++, no ioctl.
 *   3) otherwise (damage.count > 0 AND rect hook present):
 *      iterate damage.rects[] in their (y,x) sorted order, calling
 *      gfx_present_rect once each; stop on first non-success;
 *      successful rects' bytes → partial_frames / success_bytes on
 *      full-round success, or failed_completed_rect_bytes on partial
 *      round failure.
 *
 * Caller-observable invariants:
 *   - `*submitted` is set true ONLY when at least one ioctl was issued
 *     AND ALL issued ioctls returned 0 (spec §7 ack rule).
 *   - On full success: term_render_ack_damage clears pending damage,
 *     present_retry_pending clears, retry_deadline_ms clears.
 *   - On failure: pending damage is preserved; emit-helpers preserve
 *     errno; the ioctl's errno is restored at the end so terminal_display_present
 *     can classify it.
 *   - All rect hook failures stop the round (no "all-or-nothing"
 *     upgrade to full present). */
static int display_submit(terminal_display_t *d, uint64_t now_ms,
                          bool *submitted)
{
    if (submitted) *submitted = false;

    /* Interval seed (Task 3 contract). */
    uint64_t t0_ns;
    if (d->ops.now_ns) {
        t0_ns = d->ops.now_ns();
    } else {
        t0_ns = (uint64_t)now_ms * 1000000ull;
    }
    if (d->metrics_begin_ns == UINT64_MAX) d->metrics_begin_ns = t0_ns;

    /* Flush dirty cells into pending damage and snapshot it.  The
     * snapshot is local; the renderer keeps its own copy.  A later
     * flush that finds more dirty cells will extend pending_damage
     * for the next round (Task 4 contract). */
    term_render_flush(&d->render);
    term_damage_t damage;
    term_render_get_damage(&d->render, &damage);

    /* Path selection.  The NULL-hook fallback keeps tests that
     * don't supply the new op green without changing any production
     * behaviour (real terminals always wire gfx_present_rect). */
    bool want_full = damage.full;
    if (!want_full && damage.count > 0 && d->ops.gfx_present_rect == NULL) {
        want_full = true;
    }

    int rc = 0;
    int rc_errno = 0;

    if (want_full) {
        /* ── Full path: one GFX_PRESENT ioctl ────────────── */
        uint64_t before = d->ops.now_ns ? d->ops.now_ns() : t0_ns;
        rc = d->ops.gfx_present(d->gfx);
        uint64_t after = d->ops.now_ns ? d->ops.now_ns() : before;
        uint64_t dt = after - before;

        d->metrics.rect_calls++;

        if (rc != 0) {
            d->metrics.failed_attempts++;
            d->metrics.failed_ns += dt;
            if (dt > d->metrics.failed_max_ns)
                d->metrics.failed_max_ns = dt;
            rc_errno = errno;
        } else {
            /* Success.  full_bytes = stride * height (== width*height*4
             * for RGB32); the kernel wrote the entire view. */
            uint64_t bytes = (uint64_t)d->info.stride * d->info.height;
            d->metrics.full_frames++;
            d->metrics.success_ns += dt;
            d->metrics.success_bytes += bytes;
            if (dt > d->metrics.success_max_ns)
                d->metrics.success_max_ns = dt;
            term_render_ack_damage(&d->render);
            d->present_retry_pending = false;
            d->retry_deadline_ms = 0;
            if (submitted) *submitted = true;
        }
    } else if (damage.count == 0) {
        /* ── Noop path: no ioctl, no failure, no ack needed ─ */
        d->metrics.noop_calls++;
        rc = 0;
    } else {
        /* ── List path: per-rect gfx_present_rect ────────── */
        uint64_t total_dt = 0;
        uint64_t succ_bytes = 0;
        bool round_failed = false;
        int failed_errno = 0;
        for (uint32_t i = 0; i < damage.count; i++) {
            uint32_t x = damage.rects[i].x, y = damage.rects[i].y;
            uint32_t w = damage.rects[i].w, h = damage.rects[i].h;
            uint64_t before = d->ops.now_ns ? d->ops.now_ns() : t0_ns;
            int rrc = d->ops.gfx_present_rect(d->gfx, x, y, w, h);
            uint64_t after = d->ops.now_ns ? d->ops.now_ns() : before;
            uint64_t dt = after - before;
            total_dt += dt;
            d->metrics.rect_calls++;
            if (rrc == 0) {
                /* Per-rect bytes = w*h*4 (the kernel writes exactly
                 * that for a partial rect). */
                succ_bytes += (uint64_t)w * h * 4u;
                continue;
            }
            /* First failure: stop the round.  Pending damage is
             * preserved; retry will re-issue the same rects. */
            round_failed = true;
            failed_errno = errno;
            break;
        }

        if (!round_failed) {
            d->metrics.partial_frames++;
            d->metrics.success_ns += total_dt;
            d->metrics.success_bytes += succ_bytes;
            if (total_dt > d->metrics.success_max_ns)
                d->metrics.success_max_ns = total_dt;
            term_render_ack_damage(&d->render);
            d->present_retry_pending = false;
            d->retry_deadline_ms = 0;
            if (submitted) *submitted = true;
            rc = 0;
        } else {
            /* Pre-failure successful rects' bytes are a LOWER BOUND
             * on the kernel-written prefix.  total_dt already
             * includes only the time spent up to the first failure,
             * so it IS the wall-clock cost of the failed round. */
            d->metrics.failed_attempts++;
            d->metrics.failed_ns += total_dt;
            if (total_dt > d->metrics.failed_max_ns)
                d->metrics.failed_max_ns = total_dt;
            d->metrics.failed_completed_rect_bytes += succ_bytes;
            rc = -1;
            rc_errno = failed_errno;
        }
    }

    /* Per-second emit decision (Task 3 contract).  Save errno so
     * the emit helpers cannot clobber the ioctl's errno that we
     * restore below. */
    int saved_emit_errno = errno;
    uint64_t end_ns = d->ops.now_ns ? d->ops.now_ns() : t0_ns;
    if (stats_emit_due(d, end_ns)) {
        uint64_t begin_ns = d->metrics_begin_ns;
        if (emit_present_record(d, begin_ns, end_ns) == 0) {
            metrics_reset(d, end_ns);
        }
    }

    /* Restore the ioctl errno for the caller.  On success, the
     * ioctl didn't touch errno; we restore saved_emit_errno in case
     * snprintf inside the emit step clobbered it.  On failure, we
     * restore the failing ioctl's errno. */
    if (rc != 0) errno = rc_errno;
    else errno = saved_emit_errno;
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
// Term_render_clear() promotes pending_damage to full=true (spec §6),
// so the present below takes the full GFX_PRESENT path even when the
// rect hook is installed; the OLD rects do NOT leak across a full
// redraw.  The caller (refresh) must already have armed
// redraw_pending if it was the reason for calling.
//
// Returns the conventional terminal_display codes:
//   0            = present succeeded (ack); refresh_presented=true
//   1            = retryable failure; fail() set the deadline
//  -1            = permanent failure; serial_only=true
//
// On background drain (foreground=false; present returns SKIPPED),
// redraw_pending STAYS true so the foreground-restore refresh will
// re-trigger this path.
static int redraw(terminal_display_t *d, uint64_t now_ms)
{
    term_render_clear(&d->render);
    term_core_mark_all_dirty(d->core);
    /* Arm redraw_pending; the success branch clears it below.
     * Background drain (SKIPPED) and failure (-1) keep it set. */
    d->redraw_pending = true;
    int rc = terminal_display_present(d, now_ms);
    if (rc == 0) {
        d->redraw_pending = false;
        /* terminal_display_present already cleared retry_deadline_ms +
         * present_retry_pending on a SUCCESSFUL present (display_submit
         * did the actual ack), but be defensive in case someone
         * short-circuits the path later. */
        d->retry_deadline_ms = 0;
        d->present_retry_pending = false;
        d->refresh_presented = true;
        return 0;
    }
    if (rc == TERMINAL_PRESENT_SKIPPED) {
        /* Background drain: keep redraw_pending=true, leave retry
         * flags alone (terminal_display_present has already managed
         * them when the foreground was lost). */
        return 0;
    }
    return fail(d, errno, now_ms);
}

// Refresh-time submit helper (spec §7 row 4-5).  Pre-condition: the
// caller has already verified (a) deadline reached, (b) generation
// unchanged, (c) foreground=true, (d) present_retry_pending=true.
//
// Issues exactly ONE flush+submit attempt via display_submit.  Maps
// the resulting state to the spec's row 4-10 outcomes:
//
//   - full success → ack (done by display_submit), refresh_presented=true,
//     return 0.
//   - noop (no damage) → clear retry flags, return 0 WITHOUT setting
//     refresh_presented (spec row "无 damage 的 no-op").
//   - EAGAIN / ESTALE → re-arm retry (deadline=now+250), return 1.
//   - EACCES → foreground=false, redraw_pending=true, clear retry
//     flags, return 0 (spec row 8).
//   - permanent errnos (EIO/ENODEV/ENXIO/EBADF/ENOTTY) → serial_only,
//     clear retry flags, return -1 (spec row 9).
//   - other errnos (ENOMEM/EFAULT/EINVAL/EBUSY) → keep retry flags,
//     fail() sets deadline, return 1 (spec row 10).
//
// In all cases the caller's errno (the ioctl errno) is preserved.
static int refresh_submit_attempt(terminal_display_t *d, uint64_t now_ms)
{
    if (d->serial_only || !d->gfx || !d->foreground) return 0;

    bool submitted = false;
    int rc = display_submit(d, now_ms, &submitted);
    if (rc == 0) {
        if (submitted) {
            /* display_submit already cleared retry flags + ack'd. */
            d->refresh_presented = true;
            return 0;
        }
        /* Noop path (damage was empty when the deadline fired).
         * Clear the stale retry flag so the refresh path won't
         * submit again on every tick; do NOT set refresh_presented. */
        d->present_retry_pending = false;
        d->retry_deadline_ms = 0;
        return 0;
    }

    /* Failure classification per spec §7. */
    int e = errno;
    if (e == EAGAIN || e == ESTALE) {
        d->present_retry_pending = true;
        d->retry_deadline_ms = now_ms + TERMINAL_RETRY_MS;
        errno = e;
        return 1;
    }
    if (e == EACCES) {
        d->foreground = false;
        d->redraw_pending = true;
        d->present_retry_pending = false;
        d->retry_deadline_ms = 0;
        return 0;
    }
    if (errno_is_permanent(e)) {
        d->serial_only = true;
        d->present_retry_pending = false;
        d->retry_deadline_ms = 0;
        errno = e;
        return -1;
    }
    /* ENOMEM / EFAULT / EINVAL / EBUSY: keep retry flag, fail() arms
     * 250ms deadline. */
    d->present_retry_pending = true;
    d->retry_deadline_ms = now_ms + TERMINAL_RETRY_MS;
    errno = e;
    return 1;
}

int terminal_display_refresh(terminal_display_t *d, int fb_fd, uint64_t now_ms)
{
    /* Spec §7 row 0: refresh enters with refresh_presented=false; the
     * flag stays false unless a successful submit happened this tick. */
    d->refresh_presented = false;

    /* Spec §7 row 9 (serial_only): permanent device failure, no graphics. */
    if (d->serial_only) return -1;

    /* Spec §7 row "deadline 未到": rate-limit.  No submit, no state
     * change, no refresh_presented. */
    if (d->retry_deadline_ms && now_ms < d->retry_deadline_ms) return 1;

    /* Check ownership state.  get_display_state failure goes through
     * fail() which arms a 250ms deadline (no present_retry_pending —
     * spec: resource-prepare failures do NOT arm partial retry). */
    if (d->gfx && d->ops.gfx_get_display_state) {
        gfx_display_state_t dst;
        if (d->ops.gfx_get_display_state(d->gfx, &dst) != 0) {
            int e = errno;
            return fail(d, e, now_ms);
        }
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

    struct fb_state st;
    if (d->ops.get_state(fb_fd, &st) != 0) {
        int e = errno;
        return fail(d, e, now_ms);
    }

    /* Generation unchanged: limited submit paths (spec §7 row 1 / 3 / 4-5). */
    if (st.generation == d->generation) {
        /* redraw_pending has priority over retry_pending; foreground
         * is required to actually run redraw (otherwise we keep
         * redraw_pending set and return 0 — the foreground-restore
         * refresh will pick it up). */
        if (d->redraw_pending) {
            if (!d->foreground) return 0;
            return redraw(d, now_ms);
        }
        /* Retry-at-deadline: only when present_retry_pending is set
         * AND foreground AND we passed the deadline guard above. */
        if (d->present_retry_pending && d->foreground) {
            return refresh_submit_attempt(d, now_ms);
        }
        /* Spec §7 row 1: no submit, return 0.  Pending damage must
         * NOT be auto-promoted to a present here (Task 5 brief). */
        return 0;
    }

    /* ── Generation changed: mode rebind path (spec §7 row 3) ── */
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
    /* Mode rebind invalidates the OLD geometry: drop any in-flight
     * partial retry that targeted the old view.  The new view will
     * get a full redraw. */
    d->present_retry_pending = false;
    d->retry_deadline_ms = 0;
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
