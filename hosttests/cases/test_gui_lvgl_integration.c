#include "test_framework.h"
#include <gui/gui_platform.h>
#include <gui_lvgl_private.h>
#include <gui_tick.h>
#include "gui_test_runtime.h"
#include <src/display/lv_display_private.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

bool g_gui_test_tamper_header = false;

TEST_FUNC(test_real_lvgl_rendering)
{
    gui_test_reset();

    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 40,
        .height = 30,
        .buffer_lines = 7
    };

    gui_platform_t *p = gui_platform_create(&cfg);
    assert_not_null(p);

    /* 真实渲染后核对 header 字段 */
    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);
    assert_not_null(disp->buf_1);
    assert_eq(40, disp->buf_1->header.w);
    assert_eq(7, disp->buf_1->header.h);
    assert_eq(40 * 4, disp->buf_1->header.stride);
    assert_eq(LV_COLOR_FORMAT_XRGB8888, disp->buf_1->header.cf);
    assert_eq(LV_IMAGE_HEADER_MAGIC, disp->buf_1->header.magic);

    /* 用私有 display 的 active screen 绘制纯背景 0x181825 */
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    assert_not_null(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x181825), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 无边框/圆角/阴影/透明/文字覆盖的 0x1A73E8 矩形 (覆盖 x: 8..23, y: 4..19) */
    lv_obj_t *rect = lv_obj_create(scr);
    assert_not_null(rect);
    lv_obj_remove_style_all(rect);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rect, 8, 4);
    lv_obj_set_size(rect, 16, 16);
    lv_obj_set_style_bg_color(rect, lv_color_hex(0x1A73E8), 0);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);

    /* 白块 0xFFFFFF (覆盖 x: 26..35, y: 5..16) */
    lv_obj_t *white_box = lv_obj_create(scr);
    assert_not_null(white_box);
    lv_obj_remove_style_all(white_box);
    lv_obj_clear_flag(white_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(white_box, 26, 5);
    lv_obj_set_size(white_box, 10, 12);
    lv_obj_set_style_bg_color(white_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(white_box, LV_OPA_COVER, 0);

    /* 执行单步真实渲染 */
    uint32_t next_delay = 0;
    gui_status_t status = gui_platform_step(p, &next_delay);
    assert_eq(GUI_STATUS_OK, status);
    /* 稳定时钟下验证正等待；允许有到期任务时返回 0 */
    assert_true(next_delay >= 0);

    /* 1. 画布四角采样 (背景色 0x181825) */
    assert_eq(0x00181825, gui_platform_get_pixel(p, 0, 0));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 39, 0));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 0, 29));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 39, 29));

    /* 2. 背景采样点 */
    assert_eq(0x00181825, gui_platform_get_pixel(p, 2, 10));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 38, 10));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 20, 2));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 20, 28));

    /* 3. 矩形内部采样 (0x001A73E8) */
    assert_eq(0x001A73E8, gui_platform_get_pixel(p, 12, 10));
    assert_eq(0x001A73E8, gui_platform_get_pixel(p, 8, 4));
    assert_eq(0x001A73E8, gui_platform_get_pixel(p, 23, 19));

    /* 4. 白块内部采样 */
    assert_eq(0x00FFFFFF, gui_platform_get_pixel(p, 26, 5));
    assert_eq(0x00FFFFFF, gui_platform_get_pixel(p, 30, 10));
    assert_eq(0x00FFFFFF, gui_platform_get_pixel(p, 35, 16));

    /* 5. 跨 partial 边界 (buffer_lines = 7, 边界 y=6/7, 13/14) */
    /* 背景列 (x = 2) */
    assert_eq(0x00181825, gui_platform_get_pixel(p, 2, 6));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 2, 7));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 2, 13));
    assert_eq(0x00181825, gui_platform_get_pixel(p, 2, 14));

    /* 蓝色矩形列 (x = 12) */
    assert_eq(0x001A73E8, gui_platform_get_pixel(p, 12, 6));
    assert_eq(0x001A73E8, gui_platform_get_pixel(p, 12, 7));
    assert_eq(0x001A73E8, gui_platform_get_pixel(p, 12, 13));
    assert_eq(0x001A73E8, gui_platform_get_pixel(p, 12, 14));

    /* 白块列 (x = 30) */
    assert_eq(0x00FFFFFF, gui_platform_get_pixel(p, 30, 6));
    assert_eq(0x00FFFFFF, gui_platform_get_pixel(p, 30, 7));
    assert_eq(0x00FFFFFF, gui_platform_get_pixel(p, 30, 13));
    assert_eq(0x00FFFFFF, gui_platform_get_pixel(p, 30, 14));

    /* 6. 像素 X 掩码核对 */
    uint32_t px = gui_platform_get_pixel(p, 12, 10);
    assert_eq(0, px & 0xFF000000u);

    /* 7. 越界查询返回 0xFFFFFFFF 且设置 EINVAL */
    errno = 0;
    assert_eq(0xFFFFFFFFu, gui_platform_get_pixel(p, 40, 0));
    assert_eq(EINVAL, errno);

    gui_platform_destroy(p);
}

TEST_FUNC(test_real_lvgl_lifecycle)
{
    gui_test_reset();

    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 64,
        .height = 32,
        .buffer_lines = 8
    };

    /* 基准轮：测定初始池可用量 */
    gui_platform_t *p0 = gui_platform_create(&cfg);
    assert_not_null(p0);
    lv_mem_monitor_t mon0;
    lv_mem_monitor(&mon0);
    size_t baseline_free = mon0.free_size;
    gui_platform_destroy(p0);
    assert_false(lv_is_initialized());
    assert_null(lv_display_get_next(NULL));

    /* 50 次 create -> 控件 -> step -> destroy */
    for (int iter = 0; iter < 50; iter++) {
        uint32_t mmap_before = g_gui_test_counts.mmap;
        uint32_t munmap_before = g_gui_test_counts.munmap;

        gui_platform_t *p = gui_platform_create(&cfg);
        assert_not_null(p);
        assert_true(lv_is_initialized());

        /* 下轮重新 init 后 pool 可用量回到相同基线 */
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        assert_eq(baseline_free, mon.free_size);

        /* 构造控件 */
        lv_display_t *disp = gui_platform_get_lv_display(p);
        assert_not_null(disp);
        lv_obj_t *scr = lv_display_get_screen_active(disp);
        assert_not_null(scr);
        lv_obj_t *btn = lv_obj_create(scr);
        assert_not_null(btn);
        lv_obj_set_pos(btn, 10, 5);
        lv_obj_set_size(btn, 20, 10);

        /* step */
        uint32_t next_delay = 0;
        gui_status_t st = gui_platform_step(p, &next_delay);
        assert_eq(GUI_STATUS_OK, st);

        /* destroy */
        gui_platform_destroy(p);

        /* destroy 后 display 列表为空、LVGL 全局 deinit */
        assert_false(lv_is_initialized());
        assert_null(lv_display_get_next(NULL));

        /* 逐轮追踪平台映射/释放对数 (draw_buf + shadow_canvas) */
        uint32_t delta_mmap = g_gui_test_counts.mmap - mmap_before;
        uint32_t delta_munmap = g_gui_test_counts.munmap - munmap_before;
        assert_eq(2, delta_mmap);
        assert_eq(2, delta_munmap);
        assert_eq(g_gui_test_counts.mmap, g_gui_test_counts.munmap);

        /* 继续推进时钟和 handler 不得触达旧映射 */
        gui_test_advance_now(50);
        uint32_t res_handler = lv_timer_handler();
        assert_eq(1, res_handler);
    }

    /* 离屏所有设备 open/ownership 计数必须为 0 */
    assert_eq(0, g_gui_test_counts.acquire);
    assert_eq(0, g_gui_test_counts.present);
    assert_eq(0, g_gui_test_counts.release);
    assert_eq(0, g_gui_test_counts.close);
}

TEST_FUNC(test_real_lvgl_pool_oom_fatal)
{
    gui_test_reset();

    pid_t pid = fork();
    assert_true(pid >= 0);

    if (pid == 0) {
        /* 子进程：在独立地址空间内耗尽池并触发 fatal 断言 */
        gui_config_t cfg = {
            .mode = GUI_MODE_OFFSCREEN,
            .width = 100,
            .height = 50,
            .buffer_lines = 10
        };
        gui_platform_t *p = gui_platform_create(&cfg);
        if (!p) {
            _exit(3);
        }

        /* 依次用 4096、256、16、1 字节调用实际 lv_malloc，每档分配到 NULL 才进入下一档，全部成功块不释放 */
        size_t chunk_sizes[] = { 4096, 256, 16, 1 };
        for (int i = 0; i < 4; i++) {
            size_t sz = chunk_sizes[i];
            while (1) {
                void *ptr = lv_malloc(sz);
                if (!ptr) {
                    break;
                }
            }
        }

        /* 确认 1 字节分配也失败 */
        void *probe = lv_malloc(1);
        if (probe != NULL) {
            _exit(4);
        }

        /* 调用真实 lv_timer_create(NULL, 1, NULL)，其首个 lv_ll_ins_head() 失败后的 LV_ASSERT_MALLOC 必须触发 fatal (_exit(1)) */
        lv_timer_create(NULL, 1, NULL);

        /* 若此调用意外返回，子进程以 2 退出并使测试 FAIL */
        _exit(2);
    }

    /* 父进程限时 waitpid，5 秒内必须 WIFEXITED 且 WEXITSTATUS=1；超时 kill 并 wait 回收 */
    int status = 0;
    int waited_ms = 0;
    pid_t ret = 0;
    while (waited_ms < 5000) {
        ret = waitpid(pid, &status, WNOHANG);
        if (ret != 0) break;
        usleep(10000); /* 10 ms */
        waited_ms += 10;
    }

    if (ret == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        assert_true(false); /* 超时未能正常触发 fatal 退出 */
    }

    assert_eq(pid, ret);
    assert_true(WIFEXITED(status));
    assert_eq(1, WEXITSTATUS(status));

    /* 子进程不 destroy；父进程仍可创建和销毁自己的平台 */
    gui_config_t cfg_parent = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 40,
        .height = 30,
        .buffer_lines = 10
    };
    gui_platform_t *p_parent = gui_platform_create(&cfg_parent);
    assert_not_null(p_parent);
    gui_platform_destroy(p_parent);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_real_lvgl_rendering),
    TEST_ENTRY(test_real_lvgl_lifecycle),
    TEST_ENTRY(test_real_lvgl_pool_oom_fatal),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
