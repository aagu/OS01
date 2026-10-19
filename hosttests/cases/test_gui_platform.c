#include "test_framework.h"
#include <gui/gui_platform.h>
#include <gui_lvgl_private.h>
#include <gui_tick.h>
#include "gui_test_runtime.h"
#include <src/display/lv_display_private.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern bool gui_platform_checked_mul_size(size_t a, size_t b, size_t *out);
bool g_gui_test_tamper_header = false;

TEST_FUNC(test_config_validation)
{
    gui_test_reset();

    /* NULL cfg -> onscreen default */
    gui_platform_t *p = gui_platform_create(NULL);
    assert_not_null(p);
    assert_eq(640, gui_platform_get_width(p));
    assert_eq(480, gui_platform_get_height(p));
    assert_eq(GUI_MODE_ONSCREEN, gui_platform_get_mode(p));
    gui_platform_destroy(p);

    /* Invalid mode */
    gui_config_t cfg = { .mode = (gui_mode_t)42 };
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EINVAL, errno);

    /* Offscreen zero dimensions */
    cfg.mode = GUI_MODE_OFFSCREEN;
    cfg.width = 0;
    cfg.height = 100;
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EINVAL, errno);

    cfg.width = 100;
    cfg.height = 0;
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EINVAL, errno);

    /* Onscreen non-zero dimensions */
    cfg.mode = GUI_MODE_ONSCREEN;
    cfg.width = 100;
    cfg.height = 0;
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EINVAL, errno);

    cfg.width = 0;
    cfg.height = 100;
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EINVAL, errno);

    /* Width boundary: 16383 accepted, 16384 rejected */
    cfg.mode = GUI_MODE_OFFSCREEN;
    cfg.width = 16383;
    cfg.height = 10;
    cfg.buffer_lines = 10;
    p = gui_platform_create(&cfg);
    assert_not_null(p);
    gui_platform_destroy(p);

    cfg.width = 16384;
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EOVERFLOW, errno);

    /* Height boundary: 65535 accepted, 65536 rejected */
    cfg.width = 10;
    cfg.height = 65535;
    cfg.buffer_lines = 10;
    p = gui_platform_create(&cfg);
    assert_not_null(p);
    gui_platform_destroy(p);

    cfg.height = 65536;
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EOVERFLOW, errno);

    /* buffer_lines: 0 defaults to 40 and clamped to height */
    cfg.width = 100;
    cfg.height = 25;
    cfg.buffer_lines = 0;
    p = gui_platform_create(&cfg);
    assert_not_null(p);
    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);
    assert_eq(25, disp->buf_1->header.h);
    gui_platform_destroy(p);

    /* buffer_lines: UINT32_MAX clamped to height */
    cfg.buffer_lines = UINT32_MAX;
    p = gui_platform_create(&cfg);
    assert_not_null(p);
    disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);
    assert_eq(25, disp->buf_1->header.h);
    gui_platform_destroy(p);
}

TEST_FUNC(test_capacity_and_header)
{
    gui_test_reset();

    /* Checked multiply overflow detection */
    size_t res = 0;
    assert_false(gui_platform_checked_mul_size(SIZE_MAX / 2, 3, &res));
    assert_true(gui_platform_checked_mul_size(320, 200, &res));
    assert_eq(64000, res);

    /* Real header fields */
    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 320,
        .height = 240,
        .buffer_lines = 30
    };
    gui_platform_t *p = gui_platform_create(&cfg);
    assert_not_null(p);
    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);
    assert_eq(320, disp->buf_1->header.w);
    assert_eq(30, disp->buf_1->header.h);
    assert_eq(320 * 4, disp->buf_1->header.stride);
    gui_platform_destroy(p);

    /* Abnormal header rollback */
    g_gui_test_tamper_header = true;
    errno = 0;
    p = gui_platform_create(&cfg);
    assert_null(p);
    assert_eq(EINVAL, errno);
    g_gui_test_tamper_header = false;
}

TEST_FUNC(test_rollback_each_stage)
{
    /* 1. calloc failure */
    gui_test_reset();
    g_gui_test_fail_calloc = true;
    errno = 0;
    gui_platform_t *p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(ENOMEM, errno);
    assert_eq(0, g_gui_test_counts.munmap);
    assert_eq(0, g_gui_test_counts.close);

    /* 2. fb open failure */
    gui_test_reset();
    g_gui_test_fail_fb_open = true;
    g_gui_test_fail_fb_open_errno = ENOENT;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(ENOENT, errno);

    /* 3. fb read failure */
    gui_test_reset();
    g_gui_test_fail_fb_read = true;
    g_gui_test_fail_fb_read_errno = EIO;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EIO, errno);

    /* 4. fb short read */
    gui_test_reset();
    g_gui_test_short_fb_read = true;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EINVAL, errno);

    /* 5. fb invalid zero dimensions */
    gui_test_reset();
    g_gui_test_fb_info.width = 0;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EINVAL, errno);

    /* 6. fb wrong format */
    gui_test_reset();
    g_gui_test_fb_info.format = 99;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EINVAL, errno);

    /* 7. initial clock failure */
    gui_test_reset();
    g_gui_test_fail_clock = true;
    g_gui_test_fail_clock_errno = EPERM;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EPERM, errno);

    /* 8. gfx open failure */
    gui_test_reset();
    g_gui_test_fail_gfx_open = true;
    g_gui_test_fail_gfx_open_errno = ENXIO;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(ENXIO, errno);

    /* 9. gfx info failure */
    gui_test_reset();
    g_gui_test_fail_gfx_info = true;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EINVAL, errno);
    assert_eq(1, g_gui_test_counts.close);

    /* 10. gfx info mismatch */
    gui_test_reset();
    g_gui_test_mismatch_gfx_info = true;
    g_gui_test_gfx_info_override = (gfx_info_t){
        .width = 100, .height = 100, .stride = 400, .format = GFX_FORMAT_RGB32
    };
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EINVAL, errno);
    assert_eq(1, g_gui_test_counts.close);

    /* 11. draw_buf mmap failure (MAP_FAILED must not be munmapped) */
    gui_test_reset();
    g_gui_test_fail_mmap_n = 1;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(ENOMEM, errno);
    assert_eq(0, g_gui_test_counts.munmap);
    assert_eq(1, g_gui_test_counts.close);

    /* 12. shadow_canvas mmap failure in offscreen mode */
    gui_test_reset();
    g_gui_test_fail_mmap_n = 2;
    gui_config_t off_cfg = { .mode = GUI_MODE_OFFSCREEN, .width = 320, .height = 240 };
    errno = 0;
    p = gui_platform_create(&off_cfg);
    assert_null(p);
    assert_eq(ENOMEM, errno);
    assert_eq(1, g_gui_test_counts.munmap); /* draw_buf unmapped, shadow was MAP_FAILED */

    /* 13. lv_display_create failure */
    gui_test_reset();
    g_gui_test_fail_lv_display_create = true;
    errno = 0;
    p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(ENOMEM, errno);
    assert_eq(1, g_gui_test_counts.munmap);
    assert_eq(1, g_gui_test_counts.close);

    /* Create must never acquire */
    assert_eq(0, g_gui_test_counts.acquire);

    /* Verify a clean create works again */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    gui_platform_destroy(p);
}

TEST_FUNC(test_singleton_and_queries)
{
    gui_test_reset();

    gui_platform_t *p1 = gui_platform_create(NULL);
    assert_not_null(p1);

    /* Second create returns EBUSY */
    errno = 0;
    gui_platform_t *p2 = gui_platform_create(NULL);
    assert_null(p2);
    assert_eq(EBUSY, errno);

    /* Simulate tick callback latching EIO while p1 active */
    g_gui_test_fail_clock = true;
    g_gui_test_fail_clock_errno = EIO;
    uint32_t tk = gui_tick_get_cb();
    (void)tk;
    g_gui_test_fail_clock = false;

    /* Before step, second create still returns EBUSY with no device/LVGL calls */
    uint32_t close_before = g_gui_test_counts.close;
    errno = 0;
    gui_platform_t *p3 = gui_platform_create(NULL);
    assert_null(p3);
    assert_eq(EBUSY, errno);
    assert_eq(close_before, g_gui_test_counts.close);

    /* Subsequent step on p1 returns ERROR and latched EIO */
    uint32_t delay = 0;
    errno = 0;
    gui_status_t st = gui_platform_step(p1, &delay);
    assert_eq(GUI_STATUS_ERROR, st);
    assert_eq(EIO, errno);

    /* Destroy NULL is safe */
    gui_platform_destroy(NULL);

    /* Destroy p1 */
    gui_platform_destroy(p1);

    /* Can create again after destroy */
    gui_platform_t *p4 = gui_platform_create(NULL);
    assert_not_null(p4);

    /* NULL getters */
    errno = 0;
    assert_eq(0, gui_platform_get_width(NULL));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(0, gui_platform_get_height(NULL));
    assert_eq(EINVAL, errno);

    errno = 0;
    assert_eq(GUI_MODE_ONSCREEN, gui_platform_get_mode(NULL));
    assert_eq(EINVAL, errno);

    delay = 0;
    errno = 0;
    assert_eq(GUI_STATUS_ERROR, gui_platform_step(NULL, &delay));
    assert_eq(EINVAL, errno);
    assert_eq(100, delay);

    errno = 0;
    assert_eq(0xFFFFFFFFu, gui_platform_get_pixel(NULL, 0, 0));
    assert_eq(EINVAL, errno);

    /* get_pixel on onscreen mode returns 0xFFFFFFFF and EINVAL */
    errno = 0;
    assert_eq(0xFFFFFFFFu, gui_platform_get_pixel(p4, 0, 0));
    assert_eq(EINVAL, errno);
    gui_platform_destroy(p4);

    /* Offscreen get_pixel */
    gui_config_t off_cfg = { .mode = GUI_MODE_OFFSCREEN, .width = 320, .height = 240, .buffer_lines = 40 };
    gui_platform_t *off = gui_platform_create(&off_cfg);
    assert_not_null(off);

    /* Out of bounds */
    errno = 0;
    assert_eq(0xFFFFFFFFu, gui_platform_get_pixel(off, 320, 0));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(0xFFFFFFFFu, gui_platform_get_pixel(off, 0, 240));
    assert_eq(EINVAL, errno);

    /* Valid pixel sampling with high byte masked out */
    lv_area_t area = { .x1 = 10, .y1 = 10, .x2 = 10, .y2 = 10 };
    uint32_t white_pixel = 0xFFFFFFFF; /* X=0xFF, RGB=0xFFFFFF */
    gui_test_queue_flush(&area, &white_pixel);
    gui_platform_step(off, &delay);
    assert_eq(0x00FFFFFFu, gui_platform_get_pixel(off, 10, 10));

    gui_platform_destroy(off);
}

TEST_FUNC(test_terminal_policy)
{
    lv_area_t area = { .x1 = 0, .y1 = 0, .x2 = 10, .y2 = 10 };
    uint32_t px = 0;
    uint32_t delay = 0;

    /* acquire EBUSY */
    gui_test_reset();
    gui_platform_t *p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_acquire_errno = EBUSY;
    gui_test_queue_flush(&area, &px);
    errno = 0;
    gui_status_t st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_ERROR, st);
    assert_eq(EBUSY, errno);

    /* 3 consecutive steps: no handler, no acquire, no present */
    for (int i = 0; i < 3; i++) {
        errno = 0;
        st = gui_platform_step(p, &delay);
        assert_eq(GUI_STATUS_ERROR, st);
        assert_eq(EBUSY, errno);
    }
    assert_eq(1, g_gui_test_counts.handler);
    assert_eq(1, g_gui_test_counts.acquire);
    assert_eq(0, g_gui_test_counts.present);
    gui_platform_destroy(p);
    assert_eq(1, g_gui_test_counts.close);

    /* acquire EIO */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_acquire_errno = EIO;
    gui_test_queue_flush(&area, &px);
    errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_ERROR, st);
    assert_eq(EIO, errno);
    gui_platform_destroy(p);

    /* present EACCES */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_present_errno = EACCES;
    gui_test_queue_flush(&area, &px);
    errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_LOST_OWNERSHIP, st);
    assert_eq(EACCES, errno);
    gui_platform_destroy(p);

    /* present ESTALE */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_present_errno = ESTALE;
    gui_test_queue_flush(&area, &px);
    errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_DISPLAY_CHANGED, st);
    assert_eq(ESTALE, errno);
    gui_platform_destroy(p);

    /* present EIO */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_present_errno = EIO;
    gui_test_queue_flush(&area, &px);
    errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_ERROR, st);
    assert_eq(EIO, errno);
    gui_platform_destroy(p);

    /* Cleanup intentionally modifies errno, still returns original */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_present_errno = ESTALE;
    g_gui_test_cleanup_change_errno = true;
    gui_test_queue_flush(&area, &px);
    errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_DISPLAY_CHANGED, st);
    assert_eq(ESTALE, errno);
    gui_platform_destroy(p);
}

TEST_FUNC(test_retry_end_time)
{
    gui_test_reset();
    gui_test_set_now(100);

    gui_platform_t *p = gui_platform_create(NULL);
    assert_not_null(p);

    g_gui_test_present_blocking_ms = 80; /* advances now from 100 to 180 */
    g_gui_test_fail_present_errno = EAGAIN;

    lv_area_t area = { .x1 = 0, .y1 = 0, .x2 = 10, .y2 = 10 };
    uint32_t px = 0;
    gui_test_queue_flush(&area, &px);

    uint32_t delay = 0;
    gui_status_t st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_EAGAIN, st);
    assert_eq(180, gui_test_get_now());
    assert_eq(1, g_gui_test_counts.present);

    /* Deadline is 180 + 250 = 430 ms. At 429 ms, no present */
    gui_test_set_now(429);
    gui_test_queue_flush(&area, &px);
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_EAGAIN, st);
    assert_eq(1, g_gui_test_counts.present);

    /* At 430 ms, deadline reached -> presents */
    gui_test_set_now(430);
    g_gui_test_present_blocking_ms = 0;
    g_gui_test_fail_present_errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, st);
    assert_eq(2, g_gui_test_counts.present);

    gui_platform_destroy(p);
}

TEST_FUNC(test_frame_submission)
{
    gui_test_reset();
    gui_platform_t *p = gui_platform_create(NULL);
    assert_not_null(p);

    /* Multiple partial flushes in one handler call -> only 1 present */
    lv_area_t a1 = { .x1 = 0, .y1 = 0, .x2 = 5, .y2 = 5 };
    lv_area_t a2 = { .x1 = 6, .y1 = 0, .x2 = 10, .y2 = 5 };
    uint32_t px = 0;
    gui_test_queue_flush(&a1, &px);
    gui_test_queue_flush(&a2, &px);

    uint32_t delay = 0;
    gui_status_t st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, st);
    assert_eq(1, g_gui_test_counts.present);

    /* No flush -> no present */
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, st);
    assert_eq(1, g_gui_test_counts.present);

    /* Flush error latches EINVAL, ready called exactly once, no present */
    lv_area_t bad_area = { .x1 = 10, .y1 = 0, .x2 = 5, .y2 = 5 }; /* x1 > x2 */
    gui_test_queue_flush(&bad_area, &px);
    uint32_t ready_before = g_gui_test_counts.ready;
    errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_ERROR, st);
    assert_eq(EINVAL, errno);
    assert_eq(ready_before + 1, g_gui_test_counts.ready);
    assert_eq(1, g_gui_test_counts.present);

    gui_platform_destroy(p);
}

TEST_FUNC(test_wait_and_null_out)
{
    gui_test_reset();
    gui_platform_t *p = gui_platform_create(NULL);
    assert_not_null(p);

    uint32_t delay = 999;
    /* handler returns 0 */
    g_gui_test_handler_delay = 0;
    gui_status_t st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, st);
    assert_eq(0, delay);

    /* handler returns 20 */
    g_gui_test_handler_delay = 20;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, st);
    assert_eq(20, delay);

    /* handler returns UINT32_MAX (LV_NO_TIMER_READY) -> 100ms cap */
    g_gui_test_handler_delay = UINT32_MAX;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, st);
    assert_eq(100, delay);

    /* handler returns 150 -> 100ms cap */
    g_gui_test_handler_delay = 150;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, st);
    assert_eq(100, delay);

    /* out == NULL advances successfully */
    st = gui_platform_step(p, NULL);
    assert_eq(GUI_STATUS_OK, st);

    gui_platform_destroy(p);
}

TEST_FUNC(test_clock_failures)
{
    /* 1. Initial clock read fails during create */
    gui_test_reset();
    g_gui_test_fail_clock_after_n = 1;
    g_gui_test_fail_clock_errno = EIO;
    errno = 0;
    gui_platform_t *p = gui_platform_create(NULL);
    assert_null(p);
    assert_eq(EIO, errno);

    /* 2. Clock read fails before step */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_clock_after_n = 1;
    g_gui_test_fail_clock_errno = EIO;
    uint32_t delay = 0;
    errno = 0;
    gui_status_t st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_ERROR, st);
    assert_eq(EIO, errno);
    gui_platform_destroy(p);

    /* 3. Tick callback clock read fails, retains last tick, step returns ERROR */
    gui_test_reset();
    p = gui_platform_create(NULL);
    assert_not_null(p);
    g_gui_test_fail_clock = true;
    g_gui_test_fail_clock_errno = EIO;
    uint32_t t = gui_tick_get_cb();
    (void)t;
    g_gui_test_fail_clock = false;
    errno = 0;
    st = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_ERROR, st);
    assert_eq(EIO, errno);
    gui_platform_destroy(p);

    /* 4. Tick wraparound */
    gui_test_reset();
    uint64_t wrap_base = 0xFFFFFFF0ULL;
    gui_test_set_now(wrap_base);
    p = gui_platform_create(NULL);
    assert_not_null(p);
    uint64_t now_val = 0;
    gui_tick_now_ms(&now_val);
    assert_eq(wrap_base, now_val);
    assert_eq((uint32_t)wrap_base, gui_tick_get_cb());

    /* Advance 32ms past 32-bit wrap */
    gui_test_set_now(wrap_base + 32ULL);
    gui_tick_now_ms(&now_val);
    assert_eq(wrap_base + 32ULL, now_val);
    assert_eq((uint32_t)(wrap_base + 32ULL), gui_tick_get_cb());

    gui_platform_destroy(p);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_config_validation),
    TEST_ENTRY(test_capacity_and_header),
    TEST_ENTRY(test_rollback_each_stage),
    TEST_ENTRY(test_singleton_and_queries),
    TEST_ENTRY(test_terminal_policy),
    TEST_ENTRY(test_retry_end_time),
    TEST_ENTRY(test_frame_submission),
    TEST_ENTRY(test_wait_and_null_out),
    TEST_ENTRY(test_clock_failures),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
