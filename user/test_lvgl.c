/* user/test_lvgl.c — LVGL v9.5.0 compatibility smoke test for OS01 */

#include <lvgl/lvgl.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define DISP_HOR_RES 320
#define DISP_VER_RES 240
#define BUF_LINES    40

#define CLR_EXPECT_BG  0x00333333u
#define CLR_EXPECT_BTN 0x001A73E8u

static bool clock_failed = false;

static uint32_t my_tick_get_cb(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        clock_failed = true;
        return 0;
    }
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERTION FAILED (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static int flush_count = 0;
static bool bg_sampled = false;
static bool btn_sampled = false;

static void test_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    TEST_ASSERT(area->x1 <= area->x2);
    TEST_ASSERT(area->y1 <= area->y2);
    TEST_ASSERT(area->x1 >= 0 && area->y1 >= 0);
    TEST_ASSERT(area->x2 < DISP_HOR_RES && area->y2 < DISP_VER_RES);

    int32_t area_w = area->x2 - area->x1 + 1;
    uint32_t stride_bytes = lv_draw_buf_width_to_stride((uint32_t)area_w, LV_COLOR_FORMAT_XRGB8888);
    TEST_ASSERT(stride_bytes == (uint32_t)(area_w * 4));

    /* Sample background at (10, 10) */
    if (10 >= area->x1 && 10 <= area->x2 && 10 >= area->y1 && 10 <= area->y2) {
        const uint8_t *row = px_map + (10 - area->y1) * stride_bytes;
        uint32_t px = *(const uint32_t *)(row + (10 - area->x1) * 4);
        TEST_ASSERT((px & 0x00FFFFFFu) == CLR_EXPECT_BG);
        bg_sampled = true;
    }

    /* Sample button at (100, 70) */
    if (100 >= area->x1 && 100 <= area->x2 && 70 >= area->y1 && 70 <= area->y2) {
        const uint8_t *row = px_map + (70 - area->y1) * stride_bytes;
        uint32_t px = *(const uint32_t *)(row + (100 - area->x1) * 4);
        TEST_ASSERT((px & 0x00FFFFFFu) == CLR_EXPECT_BTN);
        btn_sampled = true;
    }

    flush_count++;
    lv_display_flush_ready(disp);
}

int main(void)
{
    printf("[test_lvgl] Starting LVGL v9.5.0 smoke test...\n");

    /* 1. Verify initial monotonic clock availability */
    struct timespec ts_init;
    if (clock_gettime(CLOCK_MONOTONIC, &ts_init) != 0) {
        fprintf(stderr, "clock_gettime failed at startup\n");
        return 1;
    }

    /* 2. Initialize LVGL core first */
    lv_init();

    /* 3. Register custom monotonic tick callback */
    lv_tick_set_cb(my_tick_get_cb);

    /* 4. Verify monotonic tick progression within budget */
    uint32_t t0 = lv_tick_get();
    if (clock_failed) {
        fprintf(stderr, "clock_failed set on initial tick read\n");
        return 2;
    }

    bool tick_progressed = false;
    for (int i = 0; i < 50; i++) {
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 2000000 };
        nanosleep(&pause, NULL);
        uint32_t t1 = lv_tick_get();
        if (clock_failed) {
            fprintf(stderr, "clock_gettime failed during tick polling\n");
            return 2;
        }
        uint32_t delta = t1 - t0;
        if (delta > 0 && delta <= 100) {
            tick_progressed = true;
            break;
        }
    }
    if (!tick_progressed) {
        fprintf(stderr, "tick failed to progress within budget\n");
        return 2;
    }
    printf("[test_lvgl] Monotonic tick progression verified.\n");

    /* 5. Create off-screen display and partial draw buffer */
    static uint32_t draw_buf[DISP_HOR_RES * BUF_LINES];
    uint32_t buf_bytes = (uint32_t)(DISP_HOR_RES * BUF_LINES * sizeof(uint32_t));

    lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    TEST_ASSERT(disp != NULL);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, draw_buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, test_flush_cb);

    /* 6. Build UI scene: background screen + button + label */
    lv_obj_t *scr = lv_screen_active();
    TEST_ASSERT(scr != NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_EXPECT_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *btn = lv_button_create(scr);
    TEST_ASSERT(btn != NULL);
    lv_obj_set_pos(btn, 60, 60);
    lv_obj_set_size(btn, 200, 60);
    lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_EXPECT_BTN), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    TEST_ASSERT(lbl != NULL);
    lv_label_set_text(lbl, "OS01 LVGL OK");
    lv_obj_center(lbl);

    /* 7. Run render loop */
    lv_timer_handler();

    /* 8. Assertions on flush output and pixel sampling */
    TEST_ASSERT(flush_count > 0);
    TEST_ASSERT(bg_sampled);
    TEST_ASSERT(btn_sampled);
    printf("[test_lvgl] Rendering and pixel assertions passed (flush_count=%d).\n", flush_count);

    /* 9. TLSF memory pool assertions */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("[test_lvgl] Memory pool: total=%u, free=%u, used_pct=%u%%\n",
           mon.total_size, mon.free_size, (unsigned)mon.used_pct);

    TEST_ASSERT(mon.total_size >= (4 * 1024 * 1024 - 65536));
    TEST_ASSERT(mon.used_pct > 0 && mon.used_pct < 50);
    TEST_ASSERT(mon.free_size > 0 && mon.free_size < mon.total_size);

    printf("[TEST PASS] LVGL compatibility smoke test succeeded.\n");
    return 0;
}
