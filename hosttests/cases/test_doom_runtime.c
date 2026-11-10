/*
 * test_doom_runtime.c — Doom platform runtime & shutdown host tests (T3)
 *
 * Verifies:
 *   - failure_at_each_stage: cleanup releases resources in order unbind -> close kbd -> release -> close gfx, exactly once
 *   - double_cleanup: repeated cleanup is idempotent with no double close or free
 *   - signal_handler_only_flag: SIGINT/SIGTERM handlers only set volatile sig_atomic_t flag, no IO
 *   - clock_failure_fatal: clock failure terminates rather than faking time
 *   - sleep_eintr_remaining: nanosleep resumes on EINTR with remaining time, returns early on exit signal
 */

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_framework.h"
#include <uapi/fb.h>
#include <gfx.h>
#include "doom_runtime.h"
#include "doom_os01.h"

int DG_GetKey(int *pressed, unsigned char *key);
uint32_t *DG_ScreenBuffer = NULL;

#define FAKE_FB_FD  555
#define FAKE_KBD_FD 777
#define DUMMY_HANDLE ((gfx_handle_t *)(uintptr_t)0x12340000)

typedef enum {
    TRACE_UNBIND = 1,
    TRACE_CLOSE_KBD,
    TRACE_RELEASE,
    TRACE_CLOSE_GFX
} trace_step_t;

static trace_step_t g_trace[32];
static int          g_trace_count;

static bool g_fail_fb_open;
static bool g_fail_gfx_open;
static bool g_fail_acquire;
static bool g_fail_kbd_open;
static bool g_fail_bind;

static bool g_clock_fail;
static int  g_nanosleep_eintr_count;
static int  g_nanosleep_call_count;

static struct fb_info g_fb_info;

static uint32_t g_mock_display_flags;

static void reset_runtime_mocks(void)
{
    memset(g_trace, 0, sizeof(g_trace));
    g_trace_count = 0;

    g_mock_display_flags = GFX_DISPLAY_STATE_FOREGROUND;

    g_fail_fb_open = false;
    g_fail_gfx_open = false;
    g_fail_acquire = false;
    g_fail_kbd_open = false;
    g_fail_bind = false;

    g_clock_fail = false;
    g_nanosleep_eintr_count = 0;
    g_nanosleep_call_count = 0;
    doom_runtime_install_signals();

    memset(&g_fb_info, 0, sizeof(g_fb_info));
    g_fb_info.width = 1024;
    g_fb_info.height = 768;
    g_fb_info.stride = 1024 * 4;
    g_fb_info.bpp = 32;
    g_fb_info.format = FB_FORMAT_RGB32;
}

static void record_trace(trace_step_t step)
{
    if (g_trace_count < (int)(sizeof(g_trace) / sizeof(g_trace[0]))) {
        g_trace[g_trace_count++] = step;
    }
}

/* Wrapped libc functions */
int __wrap_open(const char *path, int flags, ...)
{
    if (path && strcmp(path, "/dev/fb") == 0) {
        if (g_fail_fb_open) {
            errno = ENODEV;
            return -1;
        }
        return FAKE_FB_FD;
    }
    if (path && strcmp(path, "/dev/keyboard") == 0) {
        if (g_fail_kbd_open) {
            errno = EACCES;
            return -1;
        }
        return FAKE_KBD_FD;
    }
    return -1;
}

ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    if (fd == FAKE_FB_FD) {
        size_t n = count < sizeof(g_fb_info) ? count : sizeof(g_fb_info);
        memcpy(buf, &g_fb_info, n);
        return (ssize_t)n;
    }
    if (fd == FAKE_KBD_FD) {
        errno = EAGAIN;
        return -1;
    }
    return -1;
}

int __wrap_close(int fd)
{
    if (fd == FAKE_KBD_FD) {
        record_trace(TRACE_CLOSE_KBD);
        return 0;
    }
    return 0;
}

int __wrap_nanosleep(const struct timespec *req, struct timespec *rem)
{
    g_nanosleep_call_count++;
    if (g_nanosleep_eintr_count > 0) {
        g_nanosleep_eintr_count--;
        if (rem) {
            rem->tv_sec = req->tv_sec / 2;
            rem->tv_nsec = req->tv_nsec / 2;
        }
        errno = EINTR;
        return -1;
    }
    return 0;
}

int __wrap_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    (void)clk_id;
    if (g_clock_fail) {
        errno = EINVAL;
        return -1;
    }
    if (tp) {
        tp->tv_sec = 100;
        tp->tv_nsec = 500000000L;
    }
    return 0;
}

/* Mock gfx functions */
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)x; (void)y; (void)w; (void)h;
    if (g_fail_gfx_open) {
        errno = ENOMEM;
        return NULL;
    }
    return DUMMY_HANDLE;
}

void gfx_close(gfx_handle_t *h)
{
    if (h) {
        record_trace(TRACE_CLOSE_GFX);
    }
}

int gfx_acquire(gfx_handle_t *h)
{
    (void)h;
    if (g_fail_acquire) {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

int gfx_release(gfx_handle_t *h)
{
    (void)h;
    record_trace(TRACE_RELEASE);
    return 0;
}

int gfx_bind_input(gfx_handle_t *h, int keyboard_fd, int mouse_fd,
                   gfx_input_state_t *out_state)
{
    (void)h; (void)keyboard_fd; (void)mouse_fd;
    if (g_fail_bind) {
        errno = EACCES;
        return -1;
    }
    if (out_state) {
        out_state->generation = 1;
        out_state->initial_modifiers = 0;
        out_state->reserved = 0;
    }
    return 0;
}

int gfx_unbind_input(gfx_handle_t *h)
{
    (void)h;
    record_trace(TRACE_UNBIND);
    return 0;
}

int gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out)
{
    (void)h;
    if (out) {
        out->generation = 1;
        out->flags = g_mock_display_flags;
        out->reserved = 0;
    }
    return 0;
}

void gfx__fill_rect_impl(gfx_handle_t *h, int32_t x, int32_t y,
                         uint32_t w, uint32_t h_, uint32_t color)
{
    (void)h; (void)x; (void)y; (void)w; (void)h_; (void)color;
}

void gfx_sprite_blit(gfx_handle_t *h, int32_t dx, int32_t dy,
                     const uint32_t *src, uint32_t src_stride,
                     uint32_t src_w, uint32_t src_h,
                     bool use_color_key, uint32_t color_key)
{
    (void)h; (void)dx; (void)dy; (void)src; (void)src_stride;
    (void)src_w; (void)src_h; (void)use_color_key; (void)color_key;
}

int gfx_present(gfx_handle_t *h)
{
    (void)h;
    return 0;
}

int gfx_present_rect(gfx_handle_t *h, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h_)
{
    (void)h; (void)x; (void)y; (void)w; (void)h_;
    return 0;
}

/* ── Test Cases ─────────────────────────────────────────────── */

static void test_failure_at_each_stage(void)
{
    doom_runtime_t rt;

    /* 1. Failure at open(/dev/fb) */
    reset_runtime_mocks();
    g_fail_fb_open = true;
    assert_eq(-1, doom_runtime_open(&rt, 0));
    assert_eq(0, g_trace_count); /* no resources acquired */

    /* 2. Failure at gfx_open */
    reset_runtime_mocks();
    g_fail_gfx_open = true;
    assert_eq(-1, doom_runtime_open(&rt, 0));
    assert_eq(0, g_trace_count);

    /* 3. Failure at gfx_acquire */
    reset_runtime_mocks();
    g_fail_acquire = true;
    assert_eq(-1, doom_runtime_open(&rt, 0));
    /* gfx_open succeeded, so gfx_close must have been called */
    assert_eq(1, g_trace_count);
    assert_eq(TRACE_CLOSE_GFX, g_trace[0]);

    /* 4. Failure at open(/dev/keyboard) */
    reset_runtime_mocks();
    g_fail_kbd_open = true;
    assert_eq(-1, doom_runtime_open(&rt, 0));
    /* video acquired, so release -> close gfx */
    assert_eq(2, g_trace_count);
    assert_eq(TRACE_RELEASE, g_trace[0]);
    assert_eq(TRACE_CLOSE_GFX, g_trace[1]);

    /* 5. Failure at gfx_bind_input */
    reset_runtime_mocks();
    g_fail_bind = true;
    assert_eq(-1, doom_runtime_open(&rt, 0));
    /* kbd opened, video acquired -> close kbd -> release -> close gfx */
    assert_eq(3, g_trace_count);
    assert_eq(TRACE_CLOSE_KBD, g_trace[0]);
    assert_eq(TRACE_RELEASE, g_trace[1]);
    assert_eq(TRACE_CLOSE_GFX, g_trace[2]);

    /* 6. Success: all stages pass, then cleanup */
    reset_runtime_mocks();
    assert_eq(0, doom_runtime_open(&rt, 0));
    assert_eq(0, g_trace_count); /* acquired, not released yet */

    doom_runtime_cleanup();
    /* Cleanup sequence must be: UNBIND -> CLOSE_KBD -> RELEASE -> CLOSE_GFX */
    assert_eq(4, g_trace_count);
    assert_eq(TRACE_UNBIND, g_trace[0]);
    assert_eq(TRACE_CLOSE_KBD, g_trace[1]);
    assert_eq(TRACE_RELEASE, g_trace[2]);
    assert_eq(TRACE_CLOSE_GFX, g_trace[3]);
}

static void test_double_cleanup(void)
{
    reset_runtime_mocks();
    doom_runtime_t rt;
    assert_eq(0, doom_runtime_open(&rt, 0));

    doom_runtime_cleanup();
    assert_eq(4, g_trace_count);

    /* Second cleanup call must be a safe no-op */
    doom_runtime_cleanup();
    assert_eq(4, g_trace_count);
}

static void test_signal_handler_only_flag(void)
{
    reset_runtime_mocks();
    doom_runtime_install_signals();
    assert_eq(0, doom_runtime_should_exit());

    /* Raise SIGINT directly */
    raise(SIGINT);
    assert_eq(1, doom_runtime_should_exit());

    /* Verify handler did not perform any cleanup / trace */
    assert_eq(0, g_trace_count);
}

static jmp_buf g_abort_jmp;
static bool    g_abort_caught;

void __wrap_abort(void)
{
    g_abort_caught = true;
    longjmp(g_abort_jmp, 1);
}

static void test_clock_failure_fatal(void)
{
    reset_runtime_mocks();
    g_clock_fail = true;
    g_abort_caught = false;

    if (setjmp(g_abort_jmp) == 0) {
        doom_runtime_now_ms();
    }
    assert_true(g_abort_caught);
}

static void test_sleep_eintr_remaining(void)
{
    reset_runtime_mocks();
    /* Force 2 EINTR returns before succeeding */
    g_nanosleep_eintr_count = 2;
    g_nanosleep_call_count = 0;

    doom_runtime_sleep_ms(100);
    /* It should have called nanosleep 3 times (2 retries + 1 success) */
    assert_eq(3, g_nanosleep_call_count);

    /* Now test that exit signal aborts the sleep early */
    reset_runtime_mocks();
    g_nanosleep_eintr_count = 5;
    g_nanosleep_call_count = 0;
    doom_runtime_request_exit();

    doom_runtime_sleep_ms(100);
    /* Should have stopped after 1 call because exit was requested */
    assert_eq(1, g_nanosleep_call_count);
}

static jmp_buf g_exit_jmp;
static bool    g_exit_caught;
static int     g_exit_status;

void __wrap_exit(int status)
{
    g_exit_caught = true;
    g_exit_status = status;
    longjmp(g_exit_jmp, 1);
}

static void test_reject_unsafe_engine_args(void)
{
    doom_os01_config_t cfg;
    int out_argc = 0;
    char **out_argv = NULL;

    /* -scaling rejected */
    char *args1[] = { "doom", "-scaling", "2" };
    assert_eq(-1, doom_os01_parse_args(3, args1, &cfg, &out_argc, &out_argv));

    /* -gfxmode rejected */
    char *args2[] = { "doom", "-gfxmode", "640x400" };
    assert_eq(-1, doom_os01_parse_args(3, args2, &cfg, &out_argc, &out_argv));

    /* @response-file rejected */
    char *args3[] = { "doom", "@args.txt" };
    assert_eq(-1, doom_os01_parse_args(2, args3, &cfg, &out_argc, &out_argv));

    /* --os01-scale without argument rejected */
    char *args4[] = { "doom", "--os01-scale" };
    assert_eq(-1, doom_os01_parse_args(2, args4, &cfg, &out_argc, &out_argv));

    /* --os01-scale 4 (invalid range) rejected */
    char *args5[] = { "doom", "--os01-scale", "4" };
    assert_eq(-1, doom_os01_parse_args(3, args5, &cfg, &out_argc, &out_argv));

    /* --os01-data-dir without argument rejected */
    char *args6[] = { "doom", "--os01-data-dir" };
    assert_eq(-1, doom_os01_parse_args(2, args6, &cfg, &out_argc, &out_argv));
}

static void test_iwad_before_chdir(void)
{
    doom_os01_config_t cfg;
    int out_argc = 0;
    char **out_argv = NULL;

    char *args[] = { "doom", "-iwad", "relative/game.wad", "--os01-scale", "2" };
    assert_eq(0, doom_os01_parse_args(5, args, &cfg, &out_argc, &out_argv));
    assert_eq(2, cfg.scale);
    assert_true(cfg.iwad_path[0] == '/'); /* Must be absolute! */
    assert_true(strstr(cfg.iwad_path, "relative/game.wad") != NULL);
}

static void test_missing_iwad_no_acquire(void)
{
    reset_runtime_mocks();
    /* Non-existent IWAD path */
    const char *non_existent = "/tmp/does_not_exist_doom_12345.wad";
    int rc = doom_os01_verify_iwad(non_existent);
    assert_eq(-1, rc);

    /* Verify no gfx acquisition occurred */
    assert_eq(0, g_trace_count);
}

static void test_dg_getkey_error_exits(void)
{
    reset_runtime_mocks();
    doom_runtime_t rt;
    assert_eq(0, doom_runtime_open(&rt, 0));

    /* Revoke foreground owner */
    g_mock_display_flags = 0;

    int pressed = 0;
    unsigned char key = 0;
    g_exit_caught = false;

    if (setjmp(g_exit_jmp) == 0) {
        DG_GetKey(&pressed, &key);
    }

    assert_true(g_exit_caught);
    assert_eq(1, g_exit_status);

    doom_runtime_cleanup();
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_failure_at_each_stage),
    TEST_ENTRY(test_double_cleanup),
    TEST_ENTRY(test_signal_handler_only_flag),
    TEST_ENTRY(test_clock_failure_fatal),
    TEST_ENTRY(test_sleep_eintr_remaining),
    TEST_ENTRY(test_reject_unsafe_engine_args),
    TEST_ENTRY(test_iwad_before_chdir),
    TEST_ENTRY(test_missing_iwad_no_acquire),
    TEST_ENTRY(test_dg_getkey_error_exits),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
