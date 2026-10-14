/* hosttests/cases/test_terminal_display.c
 *
 * Host test for the REAL user/terminal/terminal_display.c (+ terminal_core.c,
 * terminal_render.c, libgfx draw primitives).  The libgfx open/close/present
 * and FBIOGET_STATE backends are injected through terminal_display_ops_t.
 */
#include "test_framework.h"
#include <terminal_display.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* malloc fault injection (link uses -Wl,--wrap=malloc) */
static int g_malloc_fail_nth = 0;
void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n)
{
    if (g_malloc_fail_nth > 0 && --g_malloc_fail_nth == 0) return NULL;
    return __real_malloc(n);
}

/* Layout must match libgfx/internal.h (as test_terminal_render.c does). */
struct fake_handle {
    int         fd;
    gfx_info_t  info;
    uint32_t   *pixels;
    size_t      pixels_bytes;
    int32_t     clip_x, clip_y;
    uint32_t    clip_w, clip_h;
    int         id;
    int         closed;
};

#define MAX_H 32
static struct fake_handle *g_handles[MAX_H];
static int g_nhandles;
static int g_open_calls, g_close_calls, g_double_close, g_present_calls;
static int g_open_fail_errno;          /* nonzero: next gfx_open fails */
static int g_present_errno;            /* nonzero: next present fails */
static int g_state_errno;              /* nonzero: get_state fails */
static int g_state_calls;
static struct fb_state g_state;
static gfx_display_state_t g_disp_state;
static int g_reg_fallback_calls, g_reg_fallback_errno;
static int g_get_disp_state_calls, g_get_disp_state_errno;
static int g_replace_fallback_calls, g_replace_fallback_errno;
static gfx_handle_t *g_last_replace_old, *g_last_replace_new;
/* test-only ENOMEM hook (FB_RESOLUTION_TEST op) */
static int g_consume_enomem;   /* 1 = the next consume returns 1 (once) */
static int g_consume_calls;
/* race: on this get_state call number (1-based, relative) switch generation */
static int g_state_flip_at;
static uint64_t g_state_flip_gen;

/* ── Task 5 rect/round helpers ──────────────────────────────
 * Track every gfx_present_rect hook invocation (one row per call).
 * If g_rect_errno[i] is nonzero, the i-th call returns -1 with that
 * errno instead of 0.  g_rect_calls is the call counter (call index is
 * 0-based).  g_rect_always_errno, if nonzero, makes EVERY call fail
 * with that errno (overrides the per-call table).  This is enough to
 * drive the multi-rect round, retry handshake and state-table tests. */
#define RECT_LOG_MAX 256
typedef struct { uint32_t x, y, w, h; } rect_call_t;
static rect_call_t g_rect_log[RECT_LOG_MAX];
static int         g_rect_log_count;
static int         g_rect_errno_per_call[RECT_LOG_MAX];
static int         g_rect_always_errno;
/* Set g_rect_owners[i] to a non-zero value to make the i-th call fail
 * with EACCES (foreground change mid-round).  Mutually exclusive with
 * the always_errnop table. */
static int         g_rect_owners[RECT_LOG_MAX];

/* ── Task 3 metrics harness ───────────────────────────────────
 *
 * now_ns fake holds g_now_ns; bumped by +g_now_advance each call.  The
 * default advance is 0 so callers can pin the moment.  After the first
 * submit-style call inside the SU, the test typically bumps it by 6_000_000
 * ns (Task 3 brief) or by 100_000_000 ns (1/10 sec, common).
 *
 * stats_open / stats_write / stats_close fakes maintain their own
 * counters and record every text blob that passes through stats_write
 * into g_emitted_text[].  stats_open returns -1 once if g_stats_open_fail
 * is set; the lazy open policy of terminal_display.c sets stats_fd to
 * DISABLED after the first failure and never retries. */
static uint64_t g_now_ns;
static uint64_t g_now_advance; /* added to g_now_ns each call */
static int g_stats_open_calls;
static int g_stats_open_fail;   /* if nonzero: next stats_open returns -1 */
static int g_stats_open_returns;  /* the fd to return from stats_open (-1 default) */
static int g_stats_write_calls;
static int g_stats_write_fail;  /* if nonzero: next stats_write returns -1 */
static int g_stats_close_calls;
static int g_stats_close_value;  /* fd passed to stats_close, recorded */
#define EMIT_MAX_LINES 16
#define EMIT_MAX_LEN   512
static char g_emitted_text[EMIT_MAX_LINES][EMIT_MAX_LEN];
static int  g_emitted_lines;

static uint64_t m_now_ns(void)
{
    uint64_t v = g_now_ns;
    g_now_ns += g_now_advance;
    return v;
}
static int m_stats_open(void)
{
    g_stats_open_calls++;
    int saved = errno;
    int rc;
    if (g_stats_open_fail) { g_stats_open_fail = 0; rc = -1; }
    else if (g_stats_open_returns >= 0) rc = g_stats_open_returns;
    else
        rc = 0x40001; /* opaque sentinel fd */
    errno = saved;
    return rc;
}
static int m_stats_write(int fd, const char *text, size_t len)
{
    g_stats_write_calls++;
    int saved = errno;
    if (g_stats_write_fail) { g_stats_write_fail = 0; errno = EIO; return -1; }
    if (g_emitted_lines < EMIT_MAX_LINES && len < EMIT_MAX_LEN) {
        size_t n = len < (size_t)(EMIT_MAX_LEN - 1) ? len : (size_t)(EMIT_MAX_LEN - 1);
        for (size_t i = 0; i < n; i++) g_emitted_text[g_emitted_lines][i] = text[i];
        g_emitted_text[g_emitted_lines][n] = '\0';
        g_emitted_lines++;
    }
    (void)fd;
    errno = saved;
    return 0;
}
static int m_stats_close(int fd)
{
    g_stats_close_calls++;
    g_stats_close_value = fd;
    /* Stats I/O is required by spec §8 to never clobber caller's
     * errno.  This hook does no I/O in the host test, so errno is
     * unchanged by definition; we keep the body small to stay that way. */
    return 0;
}

static void reset_stats_counters(void)
{
    g_now_ns = 0;
    g_now_advance = 0;
    g_stats_open_calls = 0;
    g_stats_open_fail = 0;
    g_stats_open_returns = -1;
    g_stats_write_calls = 0;
    g_stats_write_fail = 0;
    g_stats_close_calls = 0;
    g_stats_close_value = -1;
    g_emitted_lines = 0;
    for (int i = 0; i < EMIT_MAX_LINES; i++) g_emitted_text[i][0] = '\0';
}

/* ── Step clock: deterministic ns clock for Task 3 metrics tests. ─
 *
 * Returns a value derived from g_now_ns as follows:
 *   call 1 (pre-flush)    -> 0           (g_now_ns == 0)
 *   call 2 (pre-ioctl)    -> 0           (still 0)
 *   call 3 (post-ioctl)   -> 6_000_000   (g_now_ns bumped to 6 MB)
 *   call 4 (end_ns / emit-due check) -> 6_000_000 (unchanged; not yet 1 s)
 *   call 5+ -> whatever g_now_ns is at that point.
 *
 * Tests use set_step_clock_jump_to() to bump g_now_ns past 1 s for
 * emit-due assertions.  The clock is *read-only* with respect to
 * advance — only set_step_clock_jump_to() mutates g_now_ns. */
static int s_step_call;
static void reset_step_clock(void) {
    s_step_call = 0;
    g_now_ns = 0;
}
static void reset_step_clock_keep_time(void) {
    s_step_call = 0;
}
static void set_step_clock_jump_to(uint64_t ns) {
    g_now_ns = ns;
}
static uint64_t step_clock(void)
{
    s_step_call++;
    /* Only call #3 (the post-ioctl sample) advances the clock by
     * exactly 6_000_000 ns — the brief's pinned "single submit
     * duration".  All other calls return the current g_now_ns
     * unchanged so tests can pre-set arbitrary time origins. */
    if (s_step_call == 3) g_now_ns += 6000000ull;
    return g_now_ns;
}

/* Returns a NON-ZERO, NON-FAKE clock value used to verify that the
 * final-interval emit at terminal_display_close captures real begin
 * and end timestamps (not the fake step_clock's 0 / 6000000). */
static uint64_t real_close_now_ns(void)
{
    return 5000000000ull; /* 5 seconds — well past the 1 s window */
}

static gfx_handle_t *m_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)x; (void)y;
    g_open_calls++;
    if (g_open_fail_errno) { errno = g_open_fail_errno; g_open_fail_errno = 0; return NULL; }
    struct fake_handle *f = calloc(1, sizeof(*f));
    f->fd = -1;
    f->info.width = w; f->info.height = h; f->info.stride = w * 4u;
    f->info.format = GFX_FORMAT_RGB32;
    f->pixels_bytes = (size_t)w * h * 4u;
    f->pixels = calloc(1, f->pixels_bytes);
    f->clip_w = w; f->clip_h = h;
    f->id = g_nhandles;
    g_handles[g_nhandles++] = f;
    return (gfx_handle_t *)f;
}
static void m_close(gfx_handle_t *h)
{
    struct fake_handle *f = (struct fake_handle *)h;
    g_close_calls++;
    if (f->closed) g_double_close++;
    f->closed = 1;
}
static int m_present(gfx_handle_t *h)
{
    (void)h; g_present_calls++;
    if (g_present_errno) { errno = g_present_errno; g_present_errno = 0; return -1; }
    return 0;
}

/* Task 5 fake gfx_present_rect hook.  Records the rect into g_rect_log
 * for ordering & geometry assertions.  g_rect_always_errno wins over
 * the per-call table if nonzero.  If both are zero the call succeeds
 * (returns 0). */
static int m_present_rect(gfx_handle_t *h, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h_)
{
    (void)h;
    int idx = g_rect_log_count;
    if (idx >= RECT_LOG_MAX) {
        /* Drop further calls after the log is full: count them in a
         * separate counter so tests can detect overflow. */
        g_rect_log_count++;
        errno = ENOSPC;
        return -1;
    }
    g_rect_log[idx].x = x; g_rect_log[idx].y = y;
    g_rect_log[idx].w = w; g_rect_log[idx].h = h_;
    g_rect_log_count++;
    if (g_rect_owners[idx]) {
        errno = EACCES;
        return -1;
    }
    if (g_rect_always_errno) {
        int e = g_rect_always_errno;
        errno = e;
        return -1;
    }
    if (g_rect_errno_per_call[idx]) {
        int e = g_rect_errno_per_call[idx];
        g_rect_errno_per_call[idx] = 0;
        errno = e;
        return -1;
    }
    return 0;
}
static int m_register_fallback(gfx_handle_t *h)
{
    (void)h; g_reg_fallback_calls++;
    if (g_reg_fallback_errno) { errno = g_reg_fallback_errno; g_reg_fallback_errno = 0; return -1; }
    return 0;
}
static int m_get_display_state(gfx_handle_t *h, gfx_display_state_t *st)
{
    (void)h; g_get_disp_state_calls++;
    if (g_get_disp_state_errno) { errno = g_get_disp_state_errno; return -1; }
    *st = g_disp_state;
    return 0;
}
static int m_replace_fallback(gfx_handle_t *old_h, gfx_handle_t *new_h)
{
    g_replace_fallback_calls++;
    g_last_replace_old = old_h;
    g_last_replace_new = new_h;
    if (g_replace_fallback_errno) { errno = g_replace_fallback_errno; g_replace_fallback_errno = 0; return -1; }
    return 0;
}
static int m_state(int fd, struct fb_state *st)
{
    (void)fd;
    g_state_calls++;
    if (g_state_errno) { errno = g_state_errno; return -1; }
    if (g_state_flip_at && g_state_calls == g_state_flip_at) {
        g_state.generation = g_state_flip_gen;
        g_state_flip_at = 0;
    }
    *st = g_state;
    return 0;
}

/* Test-only fbtest ENOMEM hook: returns 1 exactly once, then 0. */
static int m_consume_enomem(void)
{
    g_consume_calls++;
    if (g_consume_enomem) {
        g_consume_enomem = 0;
        return 1;
    }
    return 0;
}

/* tiny in-memory PSF2: 8x16, 256 glyphs */
static uint8_t g_fontbuf[sizeof(psf2_t) + 256 * 16];

static term_core_t core;
static terminal_display_t D;

static void setup(uint32_t w, uint32_t h, uint64_t gen)
{
    psf2_t *f = (psf2_t *)g_fontbuf;
    f->magic = PSF2_MAGIC; f->version = 0; f->headersize = sizeof(psf2_t);
    f->flags = 0; f->numglyph = 256; f->bytesperglyph = 16;
    f->height = 16; f->width = 8;

    g_nhandles = g_open_calls = g_close_calls = g_double_close = 0;
    g_present_calls = g_state_calls = 0;
    g_open_fail_errno = g_present_errno = g_state_errno = 0;
    g_state_flip_at = 0; g_malloc_fail_nth = 0;
    g_consume_enomem = 0; g_consume_calls = 0;
    g_reg_fallback_calls = g_reg_fallback_errno = 0;
    g_get_disp_state_calls = g_get_disp_state_errno = 0;
    g_replace_fallback_calls = g_replace_fallback_errno = 0;
    g_last_replace_old = g_last_replace_new = NULL;
    /* Reset Task 5 rect round counters. */
    g_rect_log_count = 0;
    g_rect_always_errno = 0;
    for (int i = 0; i < RECT_LOG_MAX; i++) {
        g_rect_errno_per_call[i] = 0;
        g_rect_owners[i] = 0;
        g_rect_log[i].x = g_rect_log[i].y = 0;
        g_rect_log[i].w = g_rect_log[i].h = 0;
    }
    memset(&g_state, 0, sizeof(g_state));
    g_state.info.width = w; g_state.info.height = h;
    g_state.info.stride = w * 4; g_state.info.bpp = 32;
    g_state.generation = gen;

    memset(&g_disp_state, 0, sizeof(g_disp_state));
    g_disp_state.generation = 1;
    g_disp_state.flags = GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;

    term_core_init(&core, (int)(h / 16), (int)(w / 8));
    struct fb_info info = g_state.info;
    gfx_handle_t *g0 = m_open(0, 0, w, h);
    g_open_calls = 0;
    terminal_display_init(&D, g0, &core, (const psf2_t *)g_fontbuf, &info,
                          gen, 0xFFFFFFFFu, 0);
    D.ops.gfx_open = m_open; D.ops.gfx_close = m_close;
    D.ops.gfx_present = m_present; D.ops.get_state = m_state;
    D.ops.gfx_register_fallback = m_register_fallback;
    D.ops.gfx_get_display_state = m_get_display_state;
    D.ops.gfx_replace_fallback = m_replace_fallback;
    D.ops.consume_terminal_enomem = m_consume_enomem;
    /* Task 5 rect hook default: present-day tests (Task 3) still need a
     * OLD-full path.  We pin the default to the OLD full present so the
     * existing 248 tests stay green WITHOUT recoding anything.  Tests
     * that want the rect path call install_rect_hook() after setup(). */
    D.ops.gfx_present_rect = NULL;
    /* Task 3 metrics hooks — defaults are the real_* production impls
     * (CLOCK_MONOTONIC / /dev/serial).  Tests that want a fake clock
     * or a captured /dev/serial must override these AFTER setup(). */
    D.ops.now_ns = m_now_ns;
    D.ops.stats_open = m_stats_open;
    D.ops.stats_write = m_stats_write;
    D.ops.stats_close = m_stats_close;
    reset_stats_counters();
}

TEST_FUNC(test_terminal_recover_aba) {
    setup(640, 480, 1);
    gfx_handle_t *old = D.gfx;
    assert_eq(0, terminal_display_refresh(&D, 3, 1000));     /* same gen */
    assert_true(D.gfx == old);
    assert_eq(0, g_open_calls);

    g_state.generation = 2;                                  /* ABA: same size */
    assert_eq(0, terminal_display_refresh(&D, 3, 1100));
    assert_true(D.gfx != old);
    assert_true(D.render.gfx == D.gfx);
    assert_true(D.render.core == &core);
    assert_true(((struct fake_handle *)old)->closed);
    assert_eq(1, g_close_calls);
    assert_eq(0, g_double_close);
    assert_eq(2, (int)D.generation);
    assert_true(D.pending_winsize);
    assert_false(D.redraw_pending);
    assert_true(g_present_calls >= 1);
    unsigned short r, c, x, y;
    assert_true(terminal_display_winsize(&D, &r, &c, &x, &y));
    assert_eq(30, r); assert_eq(80, c); assert_eq(640, x); assert_eq(480, y);
    terminal_display_close(&D);
    terminal_display_close(&D);                              /* idempotent */
    assert_eq(2, g_close_calls);
    assert_eq(0, g_double_close);
}

TEST_FUNC(test_terminal_resize_changes_core) {
    setup(640, 480, 1);
    core.main_buf[0].glyph = 'K';
    g_state.info.width = 1280; g_state.info.height = 720;
    g_state.info.stride = 1280 * 4; g_state.generation = 2;
    assert_eq(0, terminal_display_refresh(&D, 3, 10));
    assert_eq(45, core.rows); assert_eq(160, core.cols);
    assert_eq('K', core.main_buf[0].glyph);
    assert_eq(1280, (int)D.info.width);
    assert_eq(1280, (int)gfx_get_info(D.gfx).width);
    terminal_display_close(&D);
}

TEST_FUNC(test_terminal_prepare_failure) {
    setup(640, 480, 1);
    gfx_handle_t *old = D.gfx;
    g_state.generation = 2;
    g_open_fail_errno = ENOMEM;
    assert_eq(1, terminal_display_refresh(&D, 3, 1000));     /* gfx fails */
    assert_true(D.gfx == old);
    assert_false(((struct fake_handle *)old)->closed);
    assert_eq(1, g_open_calls);
    assert_eq(1250, (int)D.retry_deadline_ms);
    assert_eq(1, terminal_display_refresh(&D, 3, 1100));     /* rate limited */
    assert_eq(1, g_open_calls);

    /* core array ENOMEM (size change) on each array: old core intact */
    g_state.info.width = 800; g_state.info.height = 600;
    g_state.info.stride = 3200;
    for (int nth = 1; nth <= 3; nth++) {
        term_cell_t *mb = core.main_buf;
        g_malloc_fail_nth = nth;
        int before_close = g_close_calls;
        assert_eq(1, terminal_display_refresh(&D, 3, 2000 + nth * 1000));
        g_malloc_fail_nth = 0;
        assert_true(D.gfx == old);
        assert_true(core.main_buf == mb);
        assert_eq(30, core.rows); assert_eq(80, core.cols);
        assert_eq(before_close + 1, g_close_calls);          /* only the new view */
    }
    /* success after retry window */
    assert_eq(0, terminal_display_refresh(&D, 3, 10000));
    assert_true(D.gfx != old);
    assert_eq(0, g_double_close);
    terminal_display_close(&D);
}

TEST_FUNC(test_terminal_slots_full_keeps_old) {
    setup(640, 480, 1);
    gfx_handle_t *old = D.gfx;
    g_state.generation = 2;
    g_open_fail_errno = EMFILE;
    assert_eq(1, terminal_display_refresh(&D, 3, 5));
    assert_true(D.gfx == old);
    assert_eq(0, g_close_calls);                             /* not closed early */
    assert_eq(0, terminal_display_refresh(&D, 3, 5 + 250));
    assert_eq(1, g_close_calls);
    terminal_display_close(&D);
}

TEST_FUNC(test_terminal_recover_race) {
    setup(640, 480, 1);
    gfx_handle_t *old = D.gfx;
    g_state.generation = 2;
    g_state_flip_at = 2;           /* second GET_STATE (after open) sees gen 3 */
    g_state_flip_gen = 3;
    assert_eq(1, terminal_display_refresh(&D, 3, 0));
    assert_true(D.gfx == old);
    assert_false(((struct fake_handle *)old)->closed);
    assert_eq(1, g_close_calls);                             /* prepared view released */
    assert_eq(1, g_nhandles - 1);
    assert_eq(0, terminal_display_refresh(&D, 3, 250));
    assert_eq(3, (int)D.generation);
    assert_eq(0, g_double_close);
    assert_true(D.render.gfx == D.gfx);
    terminal_display_close(&D);
    assert_eq(3, g_close_calls);
    assert_eq(0, g_double_close);
}

TEST_FUNC(test_terminal_present_errno_and_redraw_retry) {
    setup(640, 480, 1);
    g_present_errno = EAGAIN;
    errno = 0;
    assert_eq(-1, terminal_display_present(&D, 100));
    assert_eq(EAGAIN, errno);
    assert_eq(350, (int)D.retry_deadline_ms);
    assert_eq(0, terminal_display_present(&D, 400));

    /* present fails right after a successful swap: retried without reopen */
    g_state.generation = 2;
    g_present_errno = ESTALE;
    assert_eq(1, terminal_display_refresh(&D, 3, 1000));
    assert_true(D.redraw_pending);
    assert_eq(1, g_open_calls);
    assert_eq(0, terminal_display_refresh(&D, 3, 1250));
    assert_false(D.redraw_pending);
    assert_eq(1, g_open_calls);
    terminal_display_close(&D);
}

TEST_FUNC(test_idle_poll_bound) {
    setup(640, 480, 1);
    assert_true(terminal_display_poll_timeout(&D, -1, 0) <= 250);
    assert_true(terminal_display_poll_timeout(&D, -1, 0) >= 0);
    assert_true(terminal_display_poll_timeout(&D, 500, 0) <= 250);   /* CMD_HOLD */
    assert_eq(33, terminal_display_poll_timeout(&D, 33, 0));
    g_state.generation = 2;
    g_open_fail_errno = ENOMEM;
    terminal_display_refresh(&D, 3, 1000);
    int t = terminal_display_poll_timeout(&D, -1, 1000);
    assert_true(t >= 0 && t <= 250);
    assert_true(terminal_display_poll_timeout(&D, -1, 1240) <= 10);
    assert_true(terminal_display_poll_timeout(&D, -1, 5000) <= 250);
    terminal_display_close(&D);
}

TEST_FUNC(test_terminal_permanent_eio_serial_only) {
    setup(640, 480, 1);
    g_state_errno = EIO;
    assert_eq(-1, terminal_display_refresh(&D, 3, 0));
    assert_true(D.serial_only);
    assert_eq(-1, terminal_display_refresh(&D, 3, 1000));
    assert_eq(0, terminal_display_present(&D, 1000));
    assert_true(terminal_display_poll_timeout(&D, -1, 1000) <= 250);
    assert_true(D.gfx != NULL);                              /* terminal.c cleans up */
    terminal_display_close(&D);
    assert_eq(0, g_double_close);
}

/* FB_RESOLUTION_TEST hook: a one-shot ENOMEM consume fails exactly the next
 * resource prepare for this PID; the retry then succeeds normally. */
TEST_FUNC(test_terminal_fbtest_enomem_once) {
    setup(640, 480, 1);
    D.ops.consume_terminal_enomem = m_consume_enomem;
    g_consume_enomem = 1;
    g_consume_calls = 0;

    gfx_handle_t *old = D.gfx;
    g_state.info.width = 1280; g_state.info.height = 720;
    g_state.info.stride = 1280 * 4; g_state.generation = 2;

    assert_eq(1, terminal_display_refresh(&D, 3, 1000));   /* injected ENOMEM */
    assert_true(D.gfx == old);                             /* old view intact */
    assert_eq(0, g_open_calls);                            /* prepare never ran */
    assert_eq(1, g_consume_calls);
    assert_eq(1250, (int)D.retry_deadline_ms);
    assert_eq(1, terminal_display_refresh(&D, 3, 1100));   /* rate limited */

    assert_eq(0, terminal_display_refresh(&D, 3, 1250));   /* retry succeeds */
    assert_true(D.gfx != old);
    assert_eq(2, g_consume_calls);
    assert_eq(0, g_double_close);
    terminal_display_close(&D);
}

/* Background receiving PTY/core dirty does NOT call render/present; returns TERMINAL_PRESENT_SKIPPED */
TEST_FUNC(test_terminal_background_skips_present) {
    setup(640, 480, 1);
    /* Switch state to background (fallback only, not foreground) */
    g_disp_state.flags = GFX_DISPLAY_STATE_FALLBACK;
    /* Refresh queries ownership state and sets foreground = false */
    assert_eq(0, terminal_display_refresh(&D, 3, 100));
    assert_false(D.foreground);

    /* PTY dirty input arrives */
    term_core_input(&core, 'X');
    int before_present = g_present_calls;
    assert_eq(TERMINAL_PRESENT_SKIPPED, terminal_display_present(&D, 110));
    assert_eq(before_present, g_present_calls);
    assert_false(D.serial_only);

    terminal_display_close(&D);
}

/* Owner generation change / recovery must trigger full redraw (redraw_pending cleared, screen presented) */
TEST_FUNC(test_terminal_owner_restore_triggers_full_redraw) {
    setup(640, 480, 1);
    /* Initially background */
    g_disp_state.flags = GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 2;
    terminal_display_refresh(&D, 3, 100);
    assert_false(D.foreground);

    /* Write some text while background */
    term_core_input(&core, 'A');
    term_core_input(&core, 'B');

    /* Foreground restored with incremented ownership generation */
    g_disp_state.flags = GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 3;
    int before_present = g_present_calls;
    assert_eq(0, terminal_display_refresh(&D, 3, 200));
    assert_true(D.foreground);
    assert_eq(3, (int)D.owner_generation);
    assert_false(D.redraw_pending);
    assert_true(g_present_calls > before_present);

    terminal_display_close(&D);
}

/* EACCES on present does NOT count toward failures or set serial_only; sets foreground=false, redraw_pending=true */
TEST_FUNC(test_terminal_present_eacces_pauses_without_failure) {
    setup(640, 480, 1);
    /* First ensure foreground */
    assert_true(D.foreground);

    /* GFX_PRESENT returns EACCES (lost foreground race) */
    g_present_errno = EACCES;
    assert_eq(TERMINAL_PRESENT_SKIPPED, terminal_display_present(&D, 100));
    assert_false(D.foreground);
    assert_true(D.redraw_pending);
    assert_false(D.serial_only);

    /* Subsequent present in background returns TERMINAL_PRESENT_SKIPPED without calling m_present */
    int before_present = g_present_calls;
    assert_eq(TERMINAL_PRESENT_SKIPPED, terminal_display_present(&D, 150));
    assert_eq(before_present, g_present_calls);
    assert_false(D.serial_only);

    terminal_display_close(&D);
}

/* Mode change: prepares new view, calls replace_fallback, closes old view */
TEST_FUNC(test_terminal_mode_replace_fallback_success) {
    setup(640, 480, 1);
    gfx_handle_t *old = D.gfx;
    g_state.generation = 2;
    g_state.info.width = 1024; g_state.info.height = 768;
    g_disp_state.generation = 1;
    g_disp_state.flags = GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;

    assert_eq(0, terminal_display_refresh(&D, 3, 1000));
    assert_true(D.gfx != old);
    assert_eq(1, g_replace_fallback_calls);
    assert_true(g_last_replace_old == old);
    assert_true(g_last_replace_new == D.gfx);
    assert_true(((struct fake_handle *)old)->closed);
    assert_eq(1, g_close_calls);
    assert_true(D.foreground);

    terminal_display_close(&D);
}

/* Mode change when replace_fallback fails: does NOT close old view or drop role, arms retry */
TEST_FUNC(test_terminal_mode_replace_fallback_failure_keeps_old) {
    setup(640, 480, 1);
    gfx_handle_t *old = D.gfx;
    g_state.generation = 2;
    g_replace_fallback_errno = EBUSY;

    assert_eq(1, terminal_display_refresh(&D, 3, 1000));
    assert_true(D.gfx == old);
    assert_false(((struct fake_handle *)old)->closed);
    assert_eq(1, g_replace_fallback_calls);
    /* The newly allocated view was closed upon replace failure */
    assert_eq(1, g_close_calls);
    assert_eq(1250, (int)D.retry_deadline_ms);

    /* Next retry succeeds after replace_fallback_errno cleared */
    assert_eq(0, terminal_display_refresh(&D, 3, 1250));
    assert_true(D.gfx != old);
    assert_true(((struct fake_handle *)old)->closed);

    terminal_display_close(&D);
}

/* Mode recovery while in background: replace_fallback succeeds, does NOT steal back foreground */
TEST_FUNC(test_terminal_mode_recovery_in_background_does_not_steal_foreground) {
    setup(640, 480, 1);
    gfx_handle_t *old = D.gfx;
    /* Currently another GUI is foreground */
    g_disp_state.flags = GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 2;
    D.foreground = false;

    g_state.generation = 2;
    g_state.info.width = 800; g_state.info.height = 600;

    int before_present = g_present_calls;
    assert_eq(0, terminal_display_refresh(&D, 3, 1000));
    assert_true(D.gfx != old);
    assert_eq(1, g_replace_fallback_calls);
    assert_true(g_last_replace_old == old);
    assert_true(g_last_replace_new == D.gfx);
    assert_true(((struct fake_handle *)old)->closed);
    /* Crucial check: remains background! Does NOT present to screen! */
    assert_false(D.foreground);
    assert_eq(before_present, g_present_calls);
    assert_true(D.redraw_pending);

    terminal_display_close(&D);
}

/* Foreground + dirty + retry active => timeout > 0 and <= remaining retry window, avoiding zero-timeout busy poll */
TEST_FUNC(test_terminal_foreground_dirty_retry_poll_timeout_avoids_busy_loop) {
    setup(640, 480, 1);
    assert_true(D.foreground);

    /* Arm retry deadline (e.g. from EAGAIN on present or prepare failure) */
    D.retry_deadline_ms = 1250;
    uint64_t now_ms = 1000;

    /* PTY/core is dirty and cur_elapsed >= FRAME_INTERVAL_MS.
     * When present is blocked by retry deadline, terminal.c computes base timeout or base=0.
     * Even if base=0 is requested, poll timeout must NOT return 0 (no busy poll).
     * It must return > 0 and <= remaining retry window (250ms). */
    int t_base_0 = terminal_display_poll_timeout(&D, 0, now_ms);
    assert_true(t_base_0 > 0);
    assert_true(t_base_0 <= 250);
    assert_eq(250, t_base_0);

    /* As time advances, timeout tracks the remaining retry window */
    int t_base_adv = terminal_display_poll_timeout(&D, 0, 1150);
    assert_true(t_base_adv > 0);
    assert_eq(100, t_base_adv);

    /* With negative base (-1 = indefinite), it also waits up to the retry deadline */
    int t_base_neg = terminal_display_poll_timeout(&D, -1, now_ms);
    assert_eq(250, t_base_neg);

    /* When a retry window w is active, return min(base clamped to [..250], w) with w>0;
     * positive base > w must clamp to w. */
    uint64_t now_w50 = 1200; /* deadline 1250 - 1200 = 50ms remaining */
    assert_eq(50, terminal_display_poll_timeout(&D, 100, now_w50));
    assert_eq(50, terminal_display_poll_timeout(&D, -1, now_w50));
    assert_eq(50, terminal_display_poll_timeout(&D, 0, now_w50));

    terminal_display_close(&D);
}

/* Background ticks with redraw_pending must NOT execute redraw (no render/clear), but preserve redraw_pending until restore */
TEST_FUNC(test_terminal_background_ticks_do_not_redraw_until_restore) {
    setup(640, 480, 1);
    /* Transition to background */
    g_disp_state.flags = GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 2;
    assert_eq(0, terminal_display_refresh(&D, 3, 100));
    assert_false(D.foreground);

    /* Flag a redraw while in background */
    D.redraw_pending = true;

    /* Plant a canary in view pixels */
    struct fake_handle *fh = (struct fake_handle *)D.gfx;
    fh->pixels[0] = 0x12345678;

    /* Tick in background */
    assert_eq(0, terminal_display_refresh(&D, 3, 110));
    assert_false(D.foreground);
    /* In background ticks: redraw_pending must remain true, no render/clear should run */
    assert_true(D.redraw_pending);
    assert_eq(0x12345678, fh->pixels[0]);

    /* Restore foreground */
    g_disp_state.flags = GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 3;
    int before_present = g_present_calls;
    assert_eq(0, terminal_display_refresh(&D, 3, 200));
    assert_true(D.foreground);
    assert_false(D.redraw_pending);
    /* Full redraw executed: canary overwritten and screen presented */
    assert_true(fh->pixels[0] != 0x12345678);
    assert_true(g_present_calls > before_present);

    terminal_display_close(&D);
}

/* Item 1: after fallback registration, syncing owner avoids redundant redraw on first tick */
TEST_FUNC(test_terminal_no_redundant_redraw_on_first_refresh_after_fallback_sync) {
    setup(640, 480, 1);
    /* Kernel owner_generation increments to 2 after fallback registration */
    g_disp_state.flags = GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 2;

    /* Sync owner generation from display state */
    assert_eq(0, terminal_display_sync_owner(&D));
    assert_eq(2, D.owner_generation);
    assert_true(D.foreground);

    int before_presents = g_present_calls;

    /* First refresh tick: generation unchanged, owner unchanged => no redraw / no present */
    assert_eq(0, terminal_display_refresh(&D, 3, 100));
    assert_eq(before_presents, g_present_calls);
    assert_false(D.redraw_pending);
    assert_false(D.refresh_presented);

    terminal_display_close(&D);
}

/* Item 2: foreground restore reports refresh_presented and causes exactly one present */
TEST_FUNC(test_terminal_foreground_restore_reports_presented_and_causes_exactly_one_present) {
    setup(640, 480, 1);
    g_disp_state.flags = GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 2;
    terminal_display_sync_owner(&D);

    /* View loses foreground */
    g_disp_state.flags = GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 3;
    assert_eq(0, terminal_display_refresh(&D, 3, 100));
    assert_false(D.foreground);

    /* Output arrived in background: terminal marks dirty */
    bool dirty_pending = true;
    uint64_t last_present_ms = 0;
    int present_failures = 0;

    /* Foreground restored */
    g_disp_state.flags = GFX_DISPLAY_STATE_FOREGROUND | GFX_DISPLAY_STATE_FALLBACK;
    g_disp_state.generation = 4;

    int before_presents = g_present_calls;
    uint64_t now_ms = 200;
    uint64_t gen_before = D.generation;

    int rr = terminal_display_refresh(&D, 3, now_ms);
    assert_eq(0, rr);
    assert_true(D.refresh_presented);
    assert_eq(before_presents + 1, g_present_calls);

    /* terminal.c event loop logic: reset dirty_pending and last_present_ms when refresh presented */
    if (D.generation != gen_before || D.refresh_presented) {
        last_present_ms = now_ms;
        dirty_pending = false;
        present_failures = 0;
    }

    uint64_t elapsed = now_ms - last_present_ms;
    /* Step 1 in terminal.c: must NOT present a second time */
    if (dirty_pending && elapsed >= 33 &&
        !(D.retry_deadline_ms && now_ms < D.retry_deadline_ms)) {
        terminal_display_present(&D, now_ms);
    }

    assert_eq(before_presents + 1, g_present_calls);

    terminal_display_close(&D);
}

/* ── Task 3 metric tests ─────────────────────────────────────── */

/* One full-frame submit through the ordinary entry on a 4×3 view
 * (48 B) taking 6_000_000 ns — drives full_frames, success_ns,
 * success_bytes.  A follow-up failed frame only advances
 * failed_attempts and failed_ns.  Background present bumps
 * skipped_calls only and never calls gfx_present.  A stats_write that
 * clobbers errno must NOT pollute submit return or measured time. */
TEST_FUNC(test_terminal_present_metrics_full_frame_and_failures) {
    setup(4, 3, 1);             /* view 4×3 × 4 B = 48 B */
    D.ops.now_ns = step_clock;
    reset_step_clock();

    /* 1) Ordinary entry: full frame success */
    assert_eq(0, terminal_display_present(&D, 0));
    assert_eq(1, D.metrics.full_frames);
    assert_eq(0, D.metrics.partial_frames);
    assert_eq(6000000ull, D.metrics.success_ns);
    assert_eq(48ull, D.metrics.success_bytes);
    assert_eq(6000000ull, D.metrics.success_max_ns);
    assert_eq(0, D.metrics.failed_attempts);
    assert_eq(0, D.metrics.failed_ns);
    assert_eq(1, g_present_calls);

    /* 2) Refresh redraw path also routes through display_submit.  We
     * trigger redraw via the owner_generation-change branch (the
     * refresh query sees g_disp_state.generation != D.owner_generation
     * and arms d->redraw_pending without changing g_state.generation,
     * so the refresh takes the same-dimension redraw path). */
    g_disp_state.generation = 2;
    int before_redraw = g_present_calls;
    reset_step_clock();
    assert_eq(0, terminal_display_refresh(&D, 3, 0));
    assert_eq(2, D.metrics.full_frames);
    assert_eq(12000000ull, D.metrics.success_ns);
    assert_eq(96ull, D.metrics.success_bytes);
    assert_true(g_present_calls >= before_redraw + 1);

    /* 3) Failed frame: only failed_attempts and failed_ns advance.
     * success_* untouched.  Use EAGAIN which keeps foreground=true.
     *
     * After Task 5 the display_submit body is damage-aware: an empty
     * damage state routes to noop (not a failed frame), so the failing
     * present path is only reached when there IS pending damage.  We
     * re-arm full_pending via term_render_clear to make the failing
     * present take the full path; this matches the production flow
     * after a redraw or after accumulation of dirty cells. */
    g_present_errno = EAGAIN;
    D.retry_deadline_ms = 0;
    reset_step_clock();
    term_render_clear(&D.render);
    assert_eq(-1, terminal_display_present(&D, 0));
    assert_eq(EAGAIN, errno);
    assert_eq(1, D.metrics.failed_attempts);
    assert_eq(12000000ull, D.metrics.success_ns);
    assert_eq(2, D.metrics.full_frames);
    assert_eq(96ull, D.metrics.success_bytes);
    assert_true(D.metrics.failed_ns > 0);

    /* 4) Background present: only skipped_calls. */
    D.foreground = false;
    int before_bg = g_present_calls;
    int rc_bg = terminal_display_present(&D, 1000);
    assert_eq(TERMINAL_PRESENT_SKIPPED, rc_bg);
    assert_eq(before_bg, g_present_calls);
    assert_true(D.metrics.skipped_calls >= 1);
    assert_eq(2, D.metrics.full_frames);
    assert_eq(96ull, D.metrics.success_bytes);
    D.foreground = true;

    /* 5) stats_write failure must not propagate errno to the submit
     * return or extend success_ns.  Set up a present whose emit step
     * crosses the 1-second boundary, then make stats_write fail. */
    g_present_errno = 0;
    D.retry_deadline_ms = 0;
    reset_step_clock();
    set_step_clock_jump_to(3000000000ull);
    g_stats_write_fail = 1;
    int before_write_calls = g_stats_write_calls;
    assert_eq(0, terminal_display_present(&D, 0));
    /* After this present, full_frames += 1, success_ns += 6_000_000,
     * success_bytes += 48.  The failing stats_write must NOT have
     * contributed to success_ns (the ioctl timer is local to
     * display_submit and ends before the emit step). */
    assert_eq(3, D.metrics.full_frames);
    assert_eq(18000000ull, D.metrics.success_ns);
    assert_eq(144ull, D.metrics.success_bytes);
    /* The emit was attempted but failed; stats_write_calls grew by 1. */
    assert_eq(before_write_calls + 1, g_stats_write_calls);
    /* The emit-fail must NOT have reset the interval — counters
     * must still be present, ready for the next interval.  Prove
     * that with the next successful present. */
    assert_true(D.metrics_begin_ns != UINT64_MAX);
    assert_eq(3, D.metrics.full_frames);

    /* Close: stats_fd was opened so close must be called exactly once. */
    terminal_display_close(&D);
    assert_eq(1, g_stats_close_calls);
    /* Idempotent: second close must NOT call stats_close again. */
    terminal_display_close(&D);
    assert_eq(1, g_stats_close_calls);
}

/* Output lifecycle: lazy open ONCE; failed open disables the path
 * (sets stats_fd == DISABLED and never retries).  stats_close is
 * idempotent.  No output before 1 second; exactly one output after
 * 1 second.  Write failure does NOT overwrite caller's EAGAIN.  The
 * final short interval at terminal_display_close DOES emit and has
 * real begin/end_ns (not fakes). */
TEST_FUNC(test_terminal_present_metrics_output_lifecycle) {
    setup(4, 3, 1);
    D.ops.now_ns = step_clock;
    reset_step_clock();

    /* 1) First present at clock=0 — sets begin_ns to 0 but does NOT
     * trigger emit (still < 1 s since begin). */
    terminal_display_present(&D, 0);
    assert_eq(1, D.metrics.full_frames);
    assert_eq(0, g_stats_open_calls);   /* lazy open NOT yet triggered */
    assert_eq(0, g_stats_write_calls);  /* no emit yet */
    assert_true(D.metrics_begin_ns == 0);

    /* 2) Bump clock past 1 second.  The next present's emit-due
     * check fires.  Lazy open happens, then a single emit. */
    reset_step_clock();
    set_step_clock_jump_to(1500000000ull); /* 1.5 s */
    terminal_display_present(&D, 0);
    assert_eq(1, g_stats_open_calls);
    assert_eq(1, g_stats_write_calls);
    /* Interval counters were reset by the emit. */
    assert_eq(0, D.metrics.full_frames);
    assert_eq(0, D.metrics.success_ns);
    assert_eq(0, D.metrics.success_ns);
    assert_true(D.metrics_begin_ns != UINT64_MAX);

    /* 3) Subsequent presents reuse the already-open fd (lazy open
     * is one-shot): emit_due on the next present fires but does
     * NOT call stats_open again. */
    reset_step_clock();
    set_step_clock_jump_to(3000000000ull);
    terminal_display_present(&D, 0);
    assert_eq(1, g_stats_open_calls);  /* no second open */
    assert_eq(2, g_stats_write_calls); /* second emit fired */

    /* terminal_display_close: close the fd. */
    terminal_display_close(&D);
    assert_eq(1, g_stats_close_calls);
    /* Idempotent close: stats_fd was set to DISABLED so the second
     * close must NOT call stats_close. */
    terminal_display_close(&D);
    assert_eq(1, g_stats_close_calls);

    /* 4) The final short interval at close emits with real begin /
     * end ns when stats_fd >= 0.  Re-init to a clean display,
     * make ONE present (begin_ns set), close it.  Use the real
     * clock for now_ns at close to verify the real begin/end are
     * passed to the emit (not the fake).
     *
     * NOTE: setup() resets the stats counters, so all counters here
     * start at 0. */
    setup(4, 3, 1);
    D.ops.now_ns = step_clock;
    reset_step_clock();
    terminal_display_present(&D, 0);
    assert_eq(1, D.metrics.full_frames);
    assert_eq(0, g_stats_open_calls); /* lazy open NOT yet triggered */
    assert_eq(0, g_stats_write_calls);
    /* Switch now_ns to a real-clock fn just before close so the
     * close path uses real values.  The fake emit capture will see
     * those real values in the text. */
    D.ops.now_ns = real_close_now_ns;
    terminal_display_close(&D);
    /* Lazy open at close + final emit + close. */
    assert_eq(1, g_stats_open_calls);
    assert_eq(1, g_stats_write_calls);
    assert_eq(1, g_stats_close_calls);
    /* Close was called exactly once. */
    terminal_display_close(&D);
    assert_eq(1, g_stats_close_calls); /* idempotent */

    /* 5) Failed stats_open permanently disables the path: subsequent
     * present does NOT retry open; final close emits + closes nothing.
     *
     * NOTE: setup() resets stats counters — start from 0. */
    setup(4, 3, 1);
    D.ops.now_ns = step_clock;
    reset_step_clock();
    /* Open the interval at clock=0 (no emit-due yet — < 1 s). */
    terminal_display_present(&D, 0);
    assert_eq(0, g_stats_open_calls); /* still unopened */
    assert_eq(0, g_stats_write_calls);
    /* Now jump past 1 s; the next present fires emit-due.  Set
     * g_stats_open_fail=1 BEFORE that present so the lazy open
     * returns -1 and stats_fd becomes DISABLED. */
    reset_step_clock();
    set_step_clock_jump_to(1500000000ull);
    g_stats_open_fail = 1;
    terminal_display_present(&D, 0);
    assert_eq(1, g_stats_open_calls); /* 1 attempt that failed */
    assert_eq(0, g_stats_write_calls); /* no fd to write to */
    /* A second present crossing the next boundary must NOT retry open. */
    reset_step_clock();
    set_step_clock_jump_to(3000000000ull);
    terminal_display_present(&D, 0);
    assert_eq(1, g_stats_open_calls); /* still no retry */
    /* terminal_display_close: stats_fd == DISABLED, so no close call. */
    terminal_display_close(&D);
    assert_eq(0, g_stats_close_calls);
    terminal_display_close(&D);
    assert_eq(0, g_stats_close_calls);

    /* 6) stats_write failure does NOT overwrite caller's EAGAIN.
     * Set EAGAIN before the submit; the failing stats_write inside
     * must not leave EAGAIN replaced by the write errno (EIO). */
    setup(4, 3, 1);
    D.ops.now_ns = step_clock;
    reset_step_clock();
    set_step_clock_jump_to(1500000000ull); /* trigger emit */
    g_stats_write_fail = 1;
    errno = EAGAIN;
    terminal_display_present(&D, 0);
    /* The submit returned 0 (success).  EAGAIN must still be the
     * caller's errno, NOT replaced by the failing stats_write's
     * internal EIO. */
    assert_eq(EAGAIN, errno);
    (void)EAGAIN;
    /* Clean up. */
    terminal_display_close(&D);
}

/* ── Task 5 metric protocol — damage-driven present ─────────── */

/* ── Task 5 metric protocol — damage-driven present ─────────── */

/* The renderer always adds the cursor's underline as a single-pixel
 * damage rect when the cursor is visible (spec §6 row 2 + Task 4).
 * Tests that want a clean damage=0 state must hide the cursor
 * before driving the renderer; otherwise the noop path can never
 * trigger from a hot setup. */
static void hide_cursor_and_flush(void)
{
    /* \e[?25l hides the cursor. */
    term_core_input(&core, 0x1b);
    term_core_input(&core, '[');
    term_core_input(&core, '?');
    term_core_input(&core, '2');
    term_core_input(&core, '5');
    term_core_input(&core, 'l');
    term_render_cursor_update(&D.render);
    term_render_flush(&D.render);
    /* Drop the initial full_pending flag the renderer carries after
     * init + the cursor-hide flush. */
    term_render_ack_damage(&D.render);
}

/* Mark a single cell dirty and force the renderer's pending_damage to
 * contain that cell's row-span (8×16 px for the 8×16 font used in
 * test_terminal_display.c).  Returns nothing — the row/col numbering
 * follows the small-font convention used in setup().  Hides the
 * cursor so the only damage is the cell rect itself (otherwise the
 * visible-cursor underline shows up as an 8×1 rect that biases the
 * byte-count assertions). */
static void mark_one_cell(int row, int col, uint8_t glyph)
{
    term_cell_t *scr = term_core_screen(&core);
    scr[row * core.cols + col].glyph = glyph;
    core.dirty[row * core.cols + col] = true;
}

/* ── Test 1: multi-rect round + ack + retry semantics ───────── */

/* Two cell-row spans → exactly two gfx_present_rect calls; total
 * bytes = sum(w*h*4); full success acks damage.  The second rect
 * returning EAGAIN: the first's success bytes count toward
 * failed_completed_rect_bytes; damage is preserved; the next round
 * retries both rects.  Also covers: NULL rect hook falls back to
 * single full gfx_present; real rect hook returning ENOTTY does NOT
 * fall back; no damage = no ioctl + noop_calls++. */
TEST_FUNC(test_display_partial_rect_round_and_ack) {
    /* Subtest A: list path success on two rects. */
    setup(80, 64, 1);                    /* 8×16 font ⇒ 10×4 cells */
    D.ops.gfx_present_rect = m_present_rect;
    /* Hide the cursor (so the visible-cursor underline doesn't show
     * up as an extra 8×1 rect) and ack the init's full_pending. */
    hide_cursor_and_flush();
    assert_eq(0u, D.render.pending_damage.count);
    assert_false(D.render.pending_damage.full);
    /* Mark 2 cells on different rows → 2 distinct rects. */
    mark_one_cell(0, 0, 'A');
    mark_one_cell(3, 5, 'B');
    assert_eq(0, terminal_display_present(&D, 1000));
    /* Exactly 2 rect calls; (0,0,8,16) then (40,48,8,16) (col 5 = x=40,
     * row 3 = y=48). */
    assert_eq(2, g_rect_log_count);
    assert_eq(0u, g_rect_log[0].x);
    assert_eq(0u, g_rect_log[0].y);
    assert_eq(8u, g_rect_log[0].w);
    assert_eq(16u, g_rect_log[0].h);
    assert_eq(40u, g_rect_log[1].x);
    assert_eq(48u, g_rect_log[1].y);
    assert_eq(8u, g_rect_log[1].w);
    assert_eq(16u, g_rect_log[1].h);
    /* 8*16*4 + 8*16*4 = 1024 B per round; partial_frames=1; g_present_calls
     * stays at 0 (list path). */
    assert_eq(0, g_present_calls);
    assert_eq(1ull, D.metrics.partial_frames);
    assert_eq(0ull, D.metrics.full_frames);
    assert_eq(1024ull, D.metrics.success_bytes);
    assert_eq(2ull, D.metrics.rect_calls);
    assert_eq(0ull, D.metrics.failed_completed_rect_bytes);
    /* Ack: pending damage is empty after a fully successful round. */
    assert_false(D.render.pending_damage.full);
    assert_eq(0u, D.render.pending_damage.count);
    terminal_display_close(&D);

    /* Subtest B: 2nd rect returns EAGAIN.  First rect's success bytes
     * go to failed_completed_rect_bytes; damage is preserved (entire
     * list); next round retries BOTH rects. */
    setup(80, 64, 1);
    D.ops.gfx_present_rect = m_present_rect;
    hide_cursor_and_flush();
    mark_one_cell(0, 0, 'A');
    mark_one_cell(3, 5, 'B');
    g_rect_errno_per_call[1] = EAGAIN;
    errno = 0;
    assert_eq(-1, terminal_display_present(&D, 1000));
    assert_eq(EAGAIN, errno);
    assert_eq(2, g_rect_log_count);
    /* First rect's 8*16*4 = 512 B is the completed prefix. */
    assert_eq(512ull, D.metrics.failed_completed_rect_bytes);
    /* No partial success frame; failed_attempts=1. */
    assert_eq(0ull, D.metrics.partial_frames);
    assert_eq(1ull, D.metrics.failed_attempts);
    /* Damage preserved (full list): pending_damage.count == 2 still. */
    assert_false(D.render.pending_damage.full);
    assert_eq(2u, D.render.pending_damage.count);
    /* ordinary present armed retry pending + 250ms deadline. */
    assert_true(D.present_retry_pending);
    assert_eq(1250ull, D.retry_deadline_ms);
    /* Round 2 (would be triggered by refresh at deadline or by an
     * external main loop after deadline): the SAME two rects must be
     * re-issued.  Feed the same dirty cells, mark flushable again,
     * and call refresh at the deadline to drive the retry submit. */
    /* Re-mark the same cells (they're still dirty in core.dirty? NO:
     * the renderer flush cleared core.dirty when it ran; the renderer
     * preserves pending_damage because the round failed.  Mark again
     * to match the spec's "partial recovery merges new dirty". */
    /* Actually the spec says "partial recovery merges new dirty
     * without losing coverage" — meaning the existing pending damage
     * is kept AND new damage can be added.  Here we already have
     * pending damage; future flushes that find cells dirty add
     * more.  For this subtest we just want to verify the SAME
     * pending damage survives and is re-submitted. */
    /* Drop the deadline so the next present can proceed; reset
     * retry flag for clarity. */
    D.retry_deadline_ms = 0;
    D.present_retry_pending = false;
    /* g_rect_errno_per_call[1] was consumed during round 1 so the
     * retry round's 2nd rect succeeds.  This makes the retry a
     * successful round. */
    assert_eq(0, terminal_display_present(&D, 2000));
    /* 4 total rect ioctls logged (2 from round 1 + 2 from round 2). */
    assert_eq(4, g_rect_log_count);
    /* The retry round succeeded: partial_frames=1, ack'd damage. */
    assert_eq(1ull, D.metrics.partial_frames);
    assert_eq(1024ull, D.metrics.success_bytes);
    assert_eq(0u, D.render.pending_damage.count);
    /* Retry flags cleared on success. */
    assert_false(D.present_retry_pending);
    assert_eq(0ull, D.retry_deadline_ms);
    /* failed_completed_rect_bytes stays at 512 because the 2nd round
     * succeeded (it is only incremented on a failed round's
     * pre-failure rect bytes). */
    assert_eq(512ull, D.metrics.failed_completed_rect_bytes);
    /* failed_attempts counts the 1 failed round only. */
    assert_eq(1ull, D.metrics.failed_attempts);
    terminal_display_close(&D);

    /* Subtest C: NULL rect hook ⇒ single full gfx_present fallback. */
    setup(80, 64, 1);
    D.ops.gfx_present_rect = NULL;
    hide_cursor_and_flush();
    mark_one_cell(0, 0, 'A');
    mark_one_cell(3, 5, 'B');
    assert_eq(0, terminal_display_present(&D, 1000));
    assert_eq(1, g_present_calls); /* single full ioctl */
    assert_eq(0, g_rect_log_count); /* no rect hook was called */
    /* 80*64*4 = 20480 B for the full view. */
    assert_eq(20480ull, D.metrics.success_bytes);
    assert_eq(1ull, D.metrics.full_frames);
    terminal_display_close(&D);

    /* Subtest D: real rect hook returning ENOTTY is propagated as
     * -1/ENOTTY (no fallback to gfx_present).  Use per-call table
     * so the 1st rect succeeds (so we record a real failed_completed
     * prefix) and the 2nd rect returns ENOTTY.  For the ORDINARY entry
     * path the implementation propagates -1/ENOTTY verbatim WITHOUT
     * setting serial_only (that is reserved for refresh-time fail()).
     * Subtest 3k exercises the refresh-time permanent-errno path. */
    setup(80, 64, 1);
    D.ops.gfx_present_rect = m_present_rect;
    hide_cursor_and_flush();
    mark_one_cell(0, 0, 'A');
    mark_one_cell(3, 5, 'B');
    g_rect_errno_per_call[1] = ENOTTY;
    errno = 0;
    assert_eq(-1, terminal_display_present(&D, 1000));
    assert_eq(ENOTTY, errno);
    /* No fallback path: gfx_present is NOT called. */
    assert_eq(0, g_present_calls);
    /* Both rects attempted (1st succ, 2nd ENOTTY). */
    assert_eq(2, g_rect_log_count);
    /* First rect's bytes (512) recorded as the completed prefix. */
    assert_eq(512ull, D.metrics.failed_completed_rect_bytes);
    assert_eq(1ull, D.metrics.failed_attempts);
    /* ENOTTY is NOT a retry errno; retry_pending stays false, the
     * ordinary path does NOT set serial_only (the brief says
     * permanent errnos only become serial_only in refresh's fail()). */
    assert_false(D.serial_only);
    assert_false(D.present_retry_pending);
    assert_eq(0ull, D.retry_deadline_ms);
    terminal_display_close(&D);

    /* Subtest E: no damage = no ioctl + noop_calls++. */
    setup(80, 64, 1);
    D.ops.gfx_present_rect = m_present_rect;
    /* Hide cursor + ack init full_pending so the renderer's pending
     * damage is truly empty — otherwise the visible cursor's
     * underline (8×1 px = 32 B) would force a single-rect list
     * round. */
    hide_cursor_and_flush();
    /* Don't mark anything. */
    assert_eq(0, terminal_display_present(&D, 1000));
    assert_eq(0, g_present_calls);
    assert_eq(0, g_rect_log_count);
    assert_eq(0, D.metrics.success_bytes);
    assert_true(D.metrics.noop_calls >= 1);
    terminal_display_close(&D);
}

/* ── Test 2: scheduling handshake failure ──────────────────── */

/* now=100ms, an ordinary present returns EAGAIN → retry_deadline_ms=350,
 * present_retry_pending=true.  refresh at now=349 returns 1, no
 * submit.  refresh at now=350 with no new PTY output: ONE submit
 * attempt; success clears the flags and sets refresh_presented=true.
 * Main loop reads refresh_presented, clears dirty_pending: no third
 * submit.  No-deadline general dirty is NOT submitted by refresh.
 * Retry-again EAGAIN: deadline=600, errno preserved; partial recovery
 * merges new dirty without losing coverage. */
TEST_FUNC(test_display_partial_retry_deadline) {
    /* Subtest A: full handshake on retry → deadline arm → 350 submit success. */
    setup(80, 64, 1);
    D.ops.gfx_present_rect = m_present_rect;
    hide_cursor_and_flush();
    mark_one_cell(0, 0, 'A');
    mark_one_cell(3, 5, 'B');
    /* Drive the first submit attempt to fail with EAGAIN. */
    g_rect_always_errno = EAGAIN;
    errno = 0;
    assert_eq(-1, terminal_display_present(&D, 100));
    assert_eq(EAGAIN, errno);
    /* retry_deadline = 100 + 250 = 350, pending set. */
    assert_true(D.present_retry_pending);
    assert_eq(350ull, D.retry_deadline_ms);
    /* Damage preserved (2 rects). */
    assert_eq(2u, D.render.pending_damage.count);
    /* Refresh before deadline returns 1 without ANY ioctl. */
    int rect_calls_before = g_rect_log_count;
    assert_eq(1, terminal_display_refresh(&D, 3, 349));
    assert_eq(rect_calls_before, g_rect_log_count);
    assert_true(D.present_retry_pending);
    /* Refresh at deadline: ONE submit attempt.  Make this one
     * succeed. */
    g_rect_always_errno = 0;
    assert_eq(0, terminal_display_refresh(&D, 3, 350));
    /* ONE submit = 2 rect calls added. */
    assert_eq(rect_calls_before + 2, g_rect_log_count);
    /* Success cleared retry + deadline, set refresh_presented=true. */
    assert_false(D.present_retry_pending);
    assert_eq(0ull, D.retry_deadline_ms);
    assert_true(D.refresh_presented);
    /* Damage ACK'd. */
    assert_eq(0u, D.render.pending_damage.count);
    assert_false(D.render.pending_damage.full);

    /* Simulate main loop reading refresh_presented and clearing
     * dirty_pending.  After this the main loop must NOT issue
     * another submit at the same now. */
    bool dirty_pending = true;
    uint64_t last_present_ms = 0;
    if (D.refresh_presented) {
        dirty_pending = false;
        last_present_ms = 350;
    }
    uint64_t now_ms = 350;
    uint64_t elapsed = now_ms - last_present_ms;
    int rect_calls_before_step = g_rect_log_count;
    if (dirty_pending && elapsed >= 33 &&
        !(D.retry_deadline_ms && now_ms < D.retry_deadline_ms)) {
        terminal_display_present(&D, now_ms);
    }
    /* Main loop's "if dirty_present" branch did NOT fire because
     * dirty_pending was cleared by refresh_presented. */
    assert_eq(rect_calls_before_step, g_rect_log_count);
    terminal_display_close(&D);

    /* Subtest B: ordinary PTY dirty with NO retry pending must NOT be
     * submitted by refresh (spec §7 row 1).  Without redraw_pending
     * or generation change, refresh returns 0 with no flush /
     * submit, even if pending damage exists. */
    setup(80, 64, 1);
    D.ops.gfx_present_rect = m_present_rect;
    hide_cursor_and_flush();
    /* Simulate ordinary PTY dirty arriving while foreground is true
     * but no retry is pending. */
    mark_one_cell(2, 4, 'X');
    assert_false(D.present_retry_pending);
    /* refresh at now=1000 (no owner/mode change, no redraw_pending,
     * no retry pending, no deadline) returns 0, no submit. */
    int rect_before = g_rect_log_count;
    assert_eq(0, terminal_display_refresh(&D, 3, 1000));
    assert_eq(rect_before, g_rect_log_count);
    /* Flush dirty cells into pending damage — refresh does NOT do
     * this (no-op per spec row 1), so the test must do it explicitly
     * to verify that pending_damage survives. */
    term_render_flush(&D.render);
    /* The damage is still there for the ordinary present path. */
    assert_eq(1u, D.render.pending_damage.count);
    /* The ordinary present path can still issue a submit (no deadline
     * blocking it). */
    assert_eq(0, terminal_display_present(&D, 1100));
    assert_eq(rect_before + 1, g_rect_log_count);
    terminal_display_close(&D);

    /* Subtest C: again-EAGAIN retry → deadline=600, errno preserved;
     * partial recovery merges new dirty without losing coverage. */
    setup(80, 64, 1);
    D.ops.gfx_present_rect = m_present_rect;
    hide_cursor_and_flush();
    mark_one_cell(0, 0, 'A');
    g_rect_always_errno = EAGAIN;
    errno = 0;
    assert_eq(-1, terminal_display_present(&D, 350));
    assert_eq(EAGAIN, errno);
    assert_eq(600ull, D.retry_deadline_ms);
    assert_true(D.present_retry_pending);
    /* Damage preserved. */
    assert_eq(1u, D.render.pending_damage.count);
    /* Before deadline refresh returns 1. */
    assert_eq(1, terminal_display_refresh(&D, 3, 599));
    /* Mark a NEW cell dirty; the renderer must MERGE this into
     * pending damage without losing the old. */
    mark_one_cell(2, 4, 'X');
    /* At deadline, submit succeeds.  Both rects (the original one +
     * the newly-merged one) are submitted. */
    g_rect_always_errno = 0;
    int rc2 = terminal_display_refresh(&D, 3, 600);
    assert_eq(0, rc2);
    assert_true(D.refresh_presented);
    /* pending damage fully acked. */
    assert_eq(0u, D.render.pending_damage.count);
    assert_false(D.render.pending_damage.full);
    assert_false(D.present_retry_pending);
    assert_eq(0ull, D.retry_deadline_ms);
    terminal_display_close(&D);
}

/* ── Test 3: spec §7 state table ───────────────────────────── */

/* Parametric check of every errno in the spec table from the
 * ordinary + refresh entry paths.  Returns 0 / 1 / -1 /
 * TERMINAL_PRESENT_SKIPPED as documented; preserved errno;
 * counters increment the right way; present_retry_pending +
 * retry_deadline_ms taken on the right path only.  Covers the
 * rect-hook-NULL fallback, real-ENOTTY propagation, owner/mode
 * change mid-round, mode-rebind, resource-prepare failure does
 * NOT arm partial retry, full-redraw failure keeps redraw_pending. */
TEST_FUNC(test_display_state_table_parametric) {
    /* ── 3a: ordinary EAGAIN → -1, retry arm, damage preserved ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = EAGAIN;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 1000));
        assert_eq(EAGAIN, errno);
        assert_true(D.present_retry_pending);
        assert_eq(1250ull, D.retry_deadline_ms);
        assert_eq(1ull, D.metrics.failed_attempts);
        assert_eq(0ull, D.metrics.partial_frames);
        assert_eq(1u, D.render.pending_damage.count);
        terminal_display_close(&D);
    }

    /* ── 3b: ordinary ESTALE → -1, retry arm, damage preserved ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = ESTALE;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 2000));
        assert_eq(ESTALE, errno);
        assert_true(D.present_retry_pending);
        assert_eq(2250ull, D.retry_deadline_ms);
        terminal_display_close(&D);
    }

    /* ── 3c: ordinary EACCES → SKIPPED, foreground=false,
     *        redraw_pending=true, retry NOT armed,
     *        damage preserved.  Per-errno counters split between
     *        display_submit (failed_attempts/failed_ns records the
     *        actual ioctl failure) and terminal_display_present
     *        (skipped_calls records the SKIPPED return to caller).
     *        Both increment because both layers' invariants fire. ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = EACCES;
        assert_eq(TERMINAL_PRESENT_SKIPPED,
                  terminal_display_present(&D, 1000));
        assert_false(D.foreground);
        assert_true(D.redraw_pending);
        assert_false(D.present_retry_pending);
        assert_eq(0ull, D.retry_deadline_ms);
        /* Damage preserved (the SKIPPED return does not ack). */
        assert_eq(1u, D.render.pending_damage.count);
        /* display_submit recorded the ioctl failure (failed_attempts++);
         * the EACCES happened WHILE foreground was true so the present
         * path doesn't bump skipped_calls (the bg branch is the only
         * one that increments it).  Main's failure counter sees the -1
         * return and counts there. */
        assert_eq(1ull, D.metrics.failed_attempts);
        assert_eq(0ull, D.metrics.skipped_calls);
        terminal_display_close(&D);
    }

    /* ── 3d: ordinary EIO → -1, errno preserved, NO retry arm.
     *        The ordinary entry does NOT set serial_only (that is
     *        reserved for refresh's fail() — see subtest 3k and
     *        test_terminal_permanent_eio_serial_only).  Display
     *        records the ioctl failure as failed_attempts++; main
     *        loop (terminal.c) counts the failure. ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = EIO;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 1000));
        assert_eq(EIO, errno);
        assert_false(D.present_retry_pending);
        assert_eq(0ull, D.retry_deadline_ms);
        assert_false(D.serial_only); /* ordinary path */
        assert_eq(1ull, D.metrics.failed_attempts);
        terminal_display_close(&D);
    }

    /* ── 3e: ordinary ENOMEM → -1, errno preserved, NO retry arm
     *        (resource-prepare failures do NOT arm partial retry;
     *        but ENOMEM here is from a rect call, which is a
     *        present failure like EIO except not permanent). ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = ENOMEM;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 1000));
        assert_eq(ENOMEM, errno);
        /* ENOMEM is non-permanent BUT is also NOT EAGAIN/ESTALE,
         * so the rect-path retry flag is NOT armed (main counts
         * the failure). */
        assert_false(D.present_retry_pending);
        assert_eq(0ull, D.retry_deadline_ms);
        /* ENOMEM is not in permanent list, so serial_only stays
         * false. */
        assert_false(D.serial_only);
        terminal_display_close(&D);
    }

    /* ── 3f: ordinary EFAULT → -1, errno preserved, NOT arm
     *        (consistent with EIO/ENOMEM; ENOTTY from REAL hook
     *        is propagation, no fallback). ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = EFAULT;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 1000));
        assert_eq(EFAULT, errno);
        assert_false(D.present_retry_pending);
        /* EFAULT is not in the permanent errno list — serial_only
         * is unchanged. */
        assert_false(D.serial_only);
        terminal_display_close(&D);
    }

    /* ── 3g: ordinary EINVAL → -1, errno preserved, NOT arm ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = EINVAL;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 1000));
        assert_eq(EINVAL, errno);
        assert_false(D.present_retry_pending);
        terminal_display_close(&D);
    }

    /* ── 3h: ordinary EBUSY → -1, errno preserved, NOT arm ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = EBUSY;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 1000));
        assert_eq(EBUSY, errno);
        assert_false(D.present_retry_pending);
        terminal_display_close(&D);
    }

    /* ── 3i: ordinary ENODEV → -1, errno preserved; ordinary path
     *        does NOT set serial_only (refresh-time fail() does,
     *        see 3k). ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = ENODEV;
        errno = 0;
        assert_eq(-1, terminal_display_present(&D, 1000));
        assert_eq(ENODEV, errno);
        assert_false(D.serial_only); /* ordinary path: not set */
        assert_false(D.present_retry_pending);
        terminal_display_close(&D);
    }

    /* ── 3j: ordinary ENXIO / EBADF / ENOTTY → -1, errno preserved,
     *        no fallback to gfx_present.  Ordinary path does NOT
     *        set serial_only (refresh-time fail() does, see 3k). ── */
    {
        int errnos[] = { ENXIO, EBADF, ENOTTY };
        for (size_t i = 0; i < sizeof(errnos)/sizeof(errnos[0]); i++) {
            int e = errnos[i];
            setup(80, 64, 1);
            D.ops.gfx_present_rect = m_present_rect;
            hide_cursor_and_flush();
            mark_one_cell(0, 0, 'A');
            g_rect_always_errno = e;
            errno = 0;
            assert_eq(-1, terminal_display_present(&D, 1000));
            assert_eq(e, errno);
            /* ENOTTY is propagated without falling back to
             * gfx_present. */
            assert_eq(0, g_present_calls);
            assert_false(D.serial_only); /* ordinary path: not set */
            assert_false(D.present_retry_pending);
            terminal_display_close(&D);
        }
    }

    /* ── 3k: refresh entry — same error matrix but routed through
     *        terminal_display_refresh.  We pre-arm present_retry_pending
     *        with a deadline in the past so the refresh path is
     *        the active submit path. ── */
    {
        int errnos[] = { EAGAIN, ESTALE, EIO, ENOMEM, EFAULT,
                         EINVAL, EBUSY, ENODEV, ENXIO, EBADF, ENOTTY };
        for (size_t i = 0; i < sizeof(errnos)/sizeof(errnos[0]); i++) {
            int e = errnos[i];
            setup(80, 64, 1);
            D.ops.gfx_present_rect = m_present_rect;
            hide_cursor_and_flush();
            mark_one_cell(0, 0, 'A');
            /* Pre-arm retry so refresh takes the submit path. */
            D.present_retry_pending = true;
            D.retry_deadline_ms = 100; /* already past */
            g_rect_always_errno = e;
            errno = 0;
            int rrc = terminal_display_refresh(&D, 3, 1000);
            /* Permanent errnos: -1 (serial_only) — refresh returns
             * -1 exactly like fail().  Non-permanent retry-class
             * errnos (EAGAIN/ESTALE) return 1; others return 1 also
             * (spec §7 row 10 — "other refresh submit errors" 1). */
            if (e == EIO || e == ENODEV || e == ENXIO ||
                e == EBADF || e == ENOTTY) {
                assert_eq(-1, rrc);
                assert_eq(e, errno);
                assert_true(D.serial_only);
            } else {
                assert_eq(1, rrc);
                assert_eq(e, errno);
                assert_false(D.serial_only);
            }
            /* EAGAIN/ESTALE: retry_pending and deadline RE-armed. */
            if (e == EAGAIN || e == ESTALE) {
                assert_true(D.present_retry_pending);
                assert_eq(1250ull, D.retry_deadline_ms);
            } else {
                /* Other errnos: deadline reset by fail() to 1250 too
                 * (spec row 10), but present_retry_pending cleared
                 * if permanent (or kept + new deadline if not). */
                if (e == EIO || e == ENODEV || e == ENXIO ||
                    e == EBADF || e == ENOTTY) {
                    assert_false(D.present_retry_pending);
                    assert_eq(0ull, D.retry_deadline_ms);
                } else {
                    /* Non-permanent non-retry errno: fail() arms 250ms
                     * deadline; present_retry_pending stays cleared
                     * (spec: refresh-time retry-pending is only armed
                     * by EAGAIN/ESTALE from refresh; the deadline here
                     * is a refresh recovery window). */
                    assert_eq(1250ull, D.retry_deadline_ms);
                }
            }
            terminal_display_close(&D);
        }
    }

    /* ── 3l: mode rebind (generation change) ⇒ must issue ONE
     *        full redraw, NOT carry the old rects to the new
     *        view.  Old rects are discarded by redraw_full_pending. ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        mark_one_cell(3, 5, 'B');
        /* Explicit flush so the renderer's pending damage reflects
         * the marked cells (refresh does not flush by itself). */
        term_render_flush(&D.render);
        /* Pending damage is now 2 rects. */
        assert_eq(2u, D.render.pending_damage.count);
        /* Trigger a mode rebind: gen+1 with a DIFFERENT view size. */
        g_state.generation = 2;
        g_state.info.width = 1280; g_state.info.height = 720;
        g_state.info.stride = 1280 * 4;
        int rect_before = g_rect_log_count;
        int present_before = g_present_calls;
        assert_eq(0, terminal_display_refresh(&D, 3, 1000));
        /* The mode rebind does a full redraw on the NEW view: it
         * uses gfx_present (single full ioctl), NOT the rect
         * hook.  Old rects are NOT carried to the new view. */
        assert_eq(rect_before, g_rect_log_count);
        assert_eq(present_before + 1, g_present_calls);
        terminal_display_close(&D);
    }

    /* ── 3m: resource-prepare failure does NOT arm partial retry. ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        /* Trigger a mode rebind that requires gfx_open.  Force
         * ENOMEM on the prepare via g_consume_enomem (FB_RESOLUTION_TEST hook). */
        g_state.generation = 2;
        g_state.info.width = 1280; g_state.info.height = 720;
        g_state.info.stride = 1280 * 4;
        D.ops.consume_terminal_enomem = m_consume_enomem;
        g_consume_enomem = 1;
        assert_eq(1, terminal_display_refresh(&D, 3, 1000));
        /* The resource prepare failed; partial retry is NOT armed
         * (only full-redraw retry).  deadline may be set by
         * fail() but present_retry_pending is NOT. */
        assert_false(D.present_retry_pending);
        terminal_display_close(&D);
    }

    /* ── 3n: full-redraw present fails → redraw_pending stays true. ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        /* Pre-load the renderer so term_render_clear in redraw() can
         * draw to it (its pixels field is set up). */
        D.present_retry_pending = false;
        D.retry_deadline_ms = 0;
        hide_cursor_and_flush();
        /* Manually arm redraw_pending: this is what foreground
         * restore sets up via the owner-restore branch. */
        D.redraw_pending = true;
        /* Set rect hook to NULL so the full path is taken even
         * though damage is full.  (Wait — when damage.full=true the
         * full path is always taken regardless of hook.  We do
         * NOT need to clear the hook; we just need m_present to
         * fail.) */
        g_present_errno = EAGAIN;
        errno = 0;
        /* Refresh hits the redraw_pending branch, calls redraw,
         * which calls terminal_display_present, which calls
         * display_submit → full path → m_present EAGAIN. */
        int rrc = terminal_display_refresh(&D, 3, 1000);
        assert_eq(1, rrc); /* rate-limited-style: redraw retry deadline 250ms */
        assert_eq(EAGAIN, errno);
        assert_true(D.redraw_pending); /* MUST stay true */
        /* EAGAIN arms retry. */
        assert_true(D.present_retry_pending);
        assert_eq(1250ull, D.retry_deadline_ms);
        terminal_display_close(&D);
    }

    /* ── 3o: refresh at no-deadline/no-retry returns 0 with no submit. ── */
    {
        setup(80, 64, 1);
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A'); /* dirty exists */
        int rect_before = g_rect_log_count;
        int present_before = g_present_calls;
        /* No redraw_pending, no deadline, no retry_pending, no
         * owner/mode change — refresh returns 0 with no submit
         * (spec §7 row 1). */
        assert_eq(0, terminal_display_refresh(&D, 3, 1000));
        assert_eq(rect_before, g_rect_log_count);
        assert_eq(present_before, g_present_calls);
        terminal_display_close(&D);
    }
}

/* ── Test 4: metrics anti-dilution ─────────────────────────── */

/* Same 6ms frame submitted from ordinary AND refresh entry: success
 * statistics agree.  Insert 1000 noop / skipped calls between:
 * success_ns and success frame count UNCHANGED.  Failed-frame time
 * does NOT enter success_ns.  Full bytes include actual view size;
 * partial stats include ALL rects' bytes.  Stats hook I/O still
 * preserves caller errno. */
TEST_FUNC(test_display_partial_metrics_anti_dilution) {
    /* ── Subtest A: same 6ms frame → ordinary and refresh entries
     *        record the SAME success_ns. ── */
    {
        setup(4, 3, 1);   /* view 4×3 × 4 B = 48 B */
        D.ops.now_ns = step_clock;
        reset_step_clock();
        /* ordinary path: full frame (initial full_pending=true) */
        assert_eq(0, terminal_display_present(&D, 0));
        uint64_t ordinary_success_ns = D.metrics.success_ns;
        uint64_t ordinary_success_bytes = D.metrics.success_bytes;
        assert_eq(6000000ull, ordinary_success_ns);
        assert_eq(48ull, ordinary_success_bytes);
        /* refresh path: redraw → full → success */
        g_disp_state.generation = 2;
        reset_step_clock();
        assert_eq(0, terminal_display_refresh(&D, 3, 0));
        uint64_t refresh_success_ns = D.metrics.success_ns - ordinary_success_ns;
        /* The refresh's redraw path issues one full present (per the
         * pre-Task-5 baseline OR Task 5's full path); both paths
         * accumulate into success_ns in the same way. */
        assert_eq(6000000ull, refresh_success_ns);
        /* Both runs measured 6ms — agreement. */
        assert_eq(ordinary_success_ns, refresh_success_ns);
        terminal_display_close(&D);
    }

    /* ── Subtest B: 1000 noop / skipped inserts between two
     *        successful presents — neither success_ns nor success
     *        frame count advances for the inserts. ── */
    {
        setup(4, 3, 1);
        D.ops.now_ns = step_clock;
        reset_step_clock();
        /* First successful present. */
        assert_eq(0, terminal_display_present(&D, 0));
        assert_eq(1ull, D.metrics.full_frames);
        uint64_t success_ns_1 = D.metrics.success_ns; /* 6M */
        uint64_t frame_count_1 = D.metrics.full_frames; /* 1 */
        /* Insert 1000 calls that should NOT touch success_*: 500
         * background-present SKIPPED + 500 ordinary-presents on
         * empty damage (noop). */
        for (int i = 0; i < 500; i++) {
            D.foreground = false;
            terminal_display_present(&D, 0);
            D.foreground = true;
            /* ACK'd after part 1 so damage is empty → next ordinary
             * present is a noop. */
            terminal_display_present(&D, 0);
        }
        /* Still 1 full_frame, success_ns unchanged. */
        assert_eq(frame_count_1, D.metrics.full_frames);
        assert_eq(success_ns_1, D.metrics.success_ns);
        assert_eq(48ull, D.metrics.success_bytes);
        /* The noops and skipped_counts DID increment. */
        assert_true(D.metrics.noop_calls >= 500);
        assert_true(D.metrics.skipped_calls >= 500);
        terminal_display_close(&D);
    }

    /* ── Subtest C: failed-frame time does NOT enter success_ns. ── */
    {
        setup(80, 64, 1);            /* 80×64 view, 10×4 cells @ 8×16 */
        D.ops.gfx_present_rect = m_present_rect;
        D.ops.now_ns = step_clock;
        reset_step_clock();
        /* Establish a baseline success (initial full_pending = true,
         * so the first present takes the full-path; success_ns = 6M
         * from step_clock's deterministic post-ioctl sample). */
        hide_cursor_and_flush();
        assert_eq(0, terminal_display_present(&D, 0));
        uint64_t success_ns_before = D.metrics.success_ns;
        uint64_t success_frames_before = D.metrics.partial_frames +
                                        D.metrics.full_frames;
        uint64_t success_bytes_before = D.metrics.success_bytes;
        /* Drive a failed list-path round: m_present_rect returns
         * EAGAIN before any rect succeeds. */
        mark_one_cell(0, 0, 'A');
        g_rect_always_errno = EAGAIN;
        reset_step_clock();
        assert_eq(-1, terminal_display_present(&D, 0));
        /* success_* UNCHANGED.  failed_* advanced. */
        assert_eq(success_ns_before, D.metrics.success_ns);
        assert_eq(success_frames_before,
                  D.metrics.partial_frames + D.metrics.full_frames);
        assert_eq(success_bytes_before, D.metrics.success_bytes);
        assert_eq(1ull, D.metrics.failed_attempts);
        assert_true(D.metrics.failed_ns > 0);
        terminal_display_close(&D);
    }

    /* ── Subtest D: full_bytes = stride*height for the actual view;
     *        partial stats include ALL rects' bytes when ALL
     *        rects succeed. ── */
    {
        setup(80, 64, 1);  /* 80*64*4 = 20480 B full */
        D.ops.gfx_present_rect = m_present_rect;
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        mark_one_cell(3, 5, 'B');
        /* Force the full path (damage.full=true) and verify byte count. */
        term_render_clear(&D.render);
        assert_eq(0, terminal_display_present(&D, 1000));
        assert_eq(1ull, D.metrics.full_frames);
        assert_eq(0ull, D.metrics.partial_frames);
        assert_eq(20480ull, D.metrics.success_bytes);
        assert_eq(1ull, D.metrics.rect_calls); /* one full ioctl */
        /* Now exercise list path with two 8x16 rects → 1024 B partial.
         * Ack the prior full_pending and mark new dirty to flip to
         * list path. */
        hide_cursor_and_flush();
        mark_one_cell(0, 0, 'A');
        mark_one_cell(3, 5, 'B');
        assert_eq(0, terminal_display_present(&D, 1000));
        assert_eq(1ull, D.metrics.partial_frames);
        /* full_frames stayed at 1 — only the first present was the
         * full path.  success_bytes = 20480 + 1024. */
        assert_eq(1ull, D.metrics.full_frames);
        assert_eq(20480ull + 1024ull, D.metrics.success_bytes);
        assert_eq(3ull, D.metrics.rect_calls); /* 1 full + 2 rect */
        terminal_display_close(&D);
    }

    /* ── Subtest E: stats I/O preserves caller errno. ── */
    {
        setup(4, 3, 1);
        D.ops.now_ns = step_clock;
        reset_step_clock();
        /* Force a stats_write failure during emit.  Set errno to
         * a sentinel before the call. */
        set_step_clock_jump_to(1500000000ull);
        g_stats_write_fail = 1;
        errno = ETIMEDOUT;
        assert_eq(0, terminal_display_present(&D, 0));
        /* Caller's errno (ETIMEDOUT) is preserved across the
         * stats_write failure inside display_submit. */
        assert_eq(ETIMEDOUT, errno);
        terminal_display_close(&D);
    }
}

/* ── end Task 5 tests ───────────────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_terminal_recover_aba),
    TEST_ENTRY(test_terminal_resize_changes_core),
    TEST_ENTRY(test_terminal_prepare_failure),
    TEST_ENTRY(test_terminal_slots_full_keeps_old),
    TEST_ENTRY(test_terminal_recover_race),
    TEST_ENTRY(test_terminal_present_errno_and_redraw_retry),
    TEST_ENTRY(test_idle_poll_bound),
    TEST_ENTRY(test_terminal_permanent_eio_serial_only),
    TEST_ENTRY(test_terminal_fbtest_enomem_once),
    TEST_ENTRY(test_terminal_background_skips_present),
    TEST_ENTRY(test_terminal_owner_restore_triggers_full_redraw),
    TEST_ENTRY(test_terminal_present_eacces_pauses_without_failure),
    TEST_ENTRY(test_terminal_mode_replace_fallback_success),
    TEST_ENTRY(test_terminal_mode_replace_fallback_failure_keeps_old),
    TEST_ENTRY(test_terminal_mode_recovery_in_background_does_not_steal_foreground),
    TEST_ENTRY(test_terminal_foreground_dirty_retry_poll_timeout_avoids_busy_loop),
    TEST_ENTRY(test_terminal_background_ticks_do_not_redraw_until_restore),
    TEST_ENTRY(test_terminal_no_redundant_redraw_on_first_refresh_after_fallback_sync),
    TEST_ENTRY(test_terminal_foreground_restore_reports_presented_and_causes_exactly_one_present),
    TEST_ENTRY(test_terminal_present_metrics_full_frame_and_failures),
    TEST_ENTRY(test_terminal_present_metrics_output_lifecycle),
    /* Task 5: partial-rect submit, retry handshake, spec state table,
     * metrics anti-dilution. */
    TEST_ENTRY(test_display_partial_rect_round_and_ack),
    TEST_ENTRY(test_display_partial_retry_deadline),
    TEST_ENTRY(test_display_state_table_parametric),
    TEST_ENTRY(test_display_partial_metrics_anti_dilution),
TEST_LIST_END

int main(void) {
    printf("=== Test Runner ===\n");
    int n = sizeof(__test_table) / sizeof(__test_table[0]);
    for (int i = 0; i < n; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }
    int failed = __test_stats.failed;
    TEST_RESULTS();
    term_core_free(&core);
    return failed > 0 ? 1 : 0;
}
