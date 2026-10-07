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
/* test-only ENOMEM hook (FB_RESOLUTION_TEST op) */
static int g_consume_enomem;   /* 1 = the next consume returns 1 (once) */
static int g_consume_calls;
/* race: on this get_state call number (1-based, relative) switch generation */
static int g_state_flip_at;
static uint64_t g_state_flip_gen;

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
    memset(&g_state, 0, sizeof(g_state));
    g_state.info.width = w; g_state.info.height = h;
    g_state.info.stride = w * 4; g_state.info.bpp = 32;
    g_state.generation = gen;

    term_core_init(&core, (int)(h / 16), (int)(w / 8));
    struct fb_info info = g_state.info;
    gfx_handle_t *g0 = m_open(0, 0, w, h);
    g_open_calls = 0;
    terminal_display_init(&D, g0, &core, (const psf2_t *)g_fontbuf, &info,
                          gen, 0xFFFFFFFFu, 0);
    D.ops.gfx_open = m_open; D.ops.gfx_close = m_close;
    D.ops.gfx_present = m_present; D.ops.get_state = m_state;
    D.ops.consume_terminal_enomem = m_consume_enomem;
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
