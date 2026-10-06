/* user/test_lvgl.c — LVGL v9.5.0 graphical showcase & compatibility test for OS01 */

#include <lvgl/lvgl.h>
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <gfx.h>
#include <uapi/fb.h>
#include "gfx_client_policy.h"

#define DISP_FALLBACK_HOR 320
#define DISP_FALLBACK_VER 240
#define MAX_HOR_RES       1920
#define BUF_LINES         40

#define CLR_EXPECT_BG  0x00181825u  /* Catppuccin Mantle dark background */
#define CLR_EXPECT_BTN 0x001A73E8u  /* Google Blue 600 */

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

static gfx_handle_t *gfx = NULL;
static uint32_t screen_w = DISP_FALLBACK_HOR;
static uint32_t screen_h = DISP_FALLBACK_VER;

/* Task 8: shared stale-view policy state (monotonic ms retry deadline). */
static uint64_t gfx_retry_deadline_ms = 0;

static uint64_t client_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Present the frame under the shared policy.  Returns false when the
 * view is stale/permanently failed: the caller closes gfx and returns
 * non-zero.  EAGAIN keeps the view and arms a >=250ms retry (this call
 * then skips presenting until the deadline). */
static bool gfx_present_ok(void)
{
    if (!gfx) return true;
    uint64_t now = client_now_ms();
    if (gfx_retry_deadline_ms != 0 && now < gfx_retry_deadline_ms) return true;
    errno = 0;
    if (gfx_present(gfx) == 0) {
        gfx_retry_deadline_ms = 0;
        return true;
    }
    int saved = errno;
    if (gfx_client_present_policy(saved) == 1) {     /* EAGAIN: keep view */
        gfx_retry_deadline_ms = now + GFX_CLIENT_RETRY_MS;
        return true;
    }
    const char *why = (saved == ESTALE)
        ? "display mode changed, restart the app"
        : "display device failure";
    fprintf(stderr, "[test_lvgl] %s (errno=%d); exiting\n", why, saved);
    gfx_close(gfx);
    gfx = NULL;
    errno = saved;                   /* cleanup must not clobber it */
    return false;
}

static int flush_count = 0;
static bool bg_sampled = false;
static bool btn_sampled = false;

static int32_t btn_sample_x = 100;
static int32_t btn_sample_y = 70;

static void test_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    TEST_ASSERT(area->x1 <= area->x2);
    TEST_ASSERT(area->y1 <= area->y2);
    TEST_ASSERT(area->x1 >= 0 && area->y1 >= 0);
    TEST_ASSERT((uint32_t)area->x2 < screen_w && (uint32_t)area->y2 < screen_h);

    int32_t area_w = area->x2 - area->x1 + 1;
    int32_t area_h = area->y2 - area->y1 + 1;
    uint32_t stride_bytes = lv_draw_buf_width_to_stride((uint32_t)area_w, LV_COLOR_FORMAT_XRGB8888);
    TEST_ASSERT(stride_bytes == (uint32_t)(area_w * 4));

    if (gfx) {
        gfx_sprite_blit(gfx, area->x1, area->y1, (const uint32_t *)px_map,
                        stride_bytes, (uint32_t)area_w, (uint32_t)area_h, false, 0);
    }

    /* Sample background at (10, 10) */
    if (10 >= area->x1 && 10 <= area->x2 && 10 >= area->y1 && 10 <= area->y2) {
        const uint8_t *row = px_map + (10 - area->y1) * stride_bytes;
        uint32_t px = *(const uint32_t *)(row + (10 - area->x1) * 4);
        TEST_ASSERT((px & 0x00FFFFFFu) == CLR_EXPECT_BG);
        bg_sampled = true;
    }

    /* Sample primary button */
    if (btn_sample_x >= area->x1 && btn_sample_x <= area->x2 &&
        btn_sample_y >= area->y1 && btn_sample_y <= area->y2) {
        const uint8_t *row = px_map + (btn_sample_y - area->y1) * stride_bytes;
        uint32_t px = *(const uint32_t *)(row + (btn_sample_x - area->x1) * 4);
        TEST_ASSERT((px & 0x00FFFFFFu) == CLR_EXPECT_BTN);
        btn_sampled = true;
    }

    flush_count++;
    lv_display_flush_ready(disp);
}

static void build_rich_ui(lv_obj_t *scr, uint32_t w, uint32_t h)
{
    /* ── 1. Top Header Bar ────────────────────────────────────────── */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_pos(header, 20, 16);
    lv_obj_set_size(header, w - 40, 64);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x11111B), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x313244), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 10, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "OS01 x86_64  |  LVGL v9.5.0 Native GUI Showcase");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCDD6F4), 0);
    lv_obj_set_pos(title, 12, 6);

    lv_obj_t *subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "Ring 3 Userspace  •  EEVDF SMP Multicore  •  /dev/gfx0 2D Accelerated");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9399B2), 0);
    lv_obj_set_pos(subtitle, 12, 28);

    /* Google 4-Color Identity Indicators on right */
    static const uint32_t g_colors[4] = { 0x1A73E8, 0xEA4335, 0xFBBC05, 0x34A853 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(header);
        lv_obj_set_pos(dot, (int32_t)(w - 140 + i * 20), 18);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_bg_color(dot, lv_color_hex(g_colors[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 7, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── 2. Three Main Cards Layout ───────────────────────────────── */
    int32_t card_w = (int32_t)(w - 40 - 32) / 3;
    int32_t card_h = 490;
    int32_t card_y = 96;

    /* ── CARD 1: Interactive Widgets ─────────────────────────────── */
    lv_obj_t *card1 = lv_obj_create(scr);
    lv_obj_set_pos(card1, 20, card_y);
    lv_obj_set_size(card1, card_w, card_h);
    lv_obj_set_style_bg_color(card1, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_bg_opa(card1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card1, lv_color_hex(0x313244), 0);
    lv_obj_set_style_border_width(card1, 1, 0);
    lv_obj_set_style_radius(card1, 10, 0);
    lv_obj_clear_flag(card1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c1_title = lv_label_create(card1);
    lv_label_set_text(c1_title, "INTERACTIVE CONTROLS");
    lv_obj_set_style_text_color(c1_title, lv_color_hex(0x89B4FA), 0);
    lv_obj_set_pos(c1_title, 10, 8);

    /* Buttons row */
    lv_obj_t *btn1 = lv_button_create(card1);
    lv_obj_set_pos(btn1, 16, 45);
    lv_obj_set_size(btn1, 110, 40);
    lv_obj_set_style_bg_color(btn1, lv_color_hex(CLR_EXPECT_BTN), 0);
    lv_obj_set_style_bg_opa(btn1, LV_OPA_COVER, 0);
    lv_obj_t *btn1_lbl = lv_label_create(btn1);
    lv_label_set_text(btn1_lbl, "Primary");
    lv_obj_center(btn1_lbl);

    lv_obj_update_layout(scr);
    lv_area_t btn_coords;
    lv_obj_get_coords(btn1, &btn_coords);
    btn_sample_x = btn_coords.x1 + 10;
    btn_sample_y = btn_coords.y1 + 10;

    lv_obj_t *btn2 = lv_button_create(card1);
    lv_obj_set_pos(btn2, 136, 45);
    lv_obj_set_size(btn2, 110, 40);
    lv_obj_set_style_bg_color(btn2, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_bg_opa(btn2, LV_OPA_COVER, 0);
    lv_obj_t *btn2_lbl = lv_label_create(btn2);
    lv_label_set_text(btn2_lbl, "Success");
    lv_obj_center(btn2_lbl);

    lv_obj_t *btn3 = lv_button_create(card1);
    lv_obj_set_pos(btn3, 256, 45);
    lv_obj_set_size(btn3, 110, 40);
    lv_obj_set_style_bg_color(btn3, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_bg_opa(btn3, LV_OPA_COVER, 0);
    lv_obj_t *btn3_lbl = lv_label_create(btn3);
    lv_label_set_text(btn3_lbl, "Danger");
    lv_obj_center(btn3_lbl);

    /* Switches */
    lv_obj_t *sw1 = lv_switch_create(card1);
    lv_obj_set_pos(sw1, 16, 110);
    lv_obj_add_state(sw1, LV_STATE_CHECKED);
    lv_obj_t *sw1_lbl = lv_label_create(card1);
    lv_label_set_text(sw1_lbl, "Wi-Fi Interface (Active)");
    lv_obj_set_style_text_color(sw1_lbl, lv_color_hex(0xCDD6F4), 0);
    lv_obj_set_pos(sw1_lbl, 80, 115);

    lv_obj_t *sw2 = lv_switch_create(card1);
    lv_obj_set_pos(sw2, 16, 155);
    lv_obj_t *sw2_lbl = lv_label_create(card1);
    lv_label_set_text(sw2_lbl, "Low-Power Suspend (Off)");
    lv_obj_set_style_text_color(sw2_lbl, lv_color_hex(0xA6ADC8), 0);
    lv_obj_set_pos(sw2_lbl, 80, 160);

    /* Checkboxes */
    lv_obj_t *cb1 = lv_checkbox_create(card1);
    lv_checkbox_set_text(cb1, "Enable 2D Blitter (/dev/gfx0)");
    lv_obj_set_style_text_color(cb1, lv_color_hex(0xCDD6F4), 0);
    lv_obj_set_pos(cb1, 16, 205);
    lv_obj_add_state(cb1, LV_STATE_CHECKED);

    lv_obj_t *cb2 = lv_checkbox_create(card1);
    lv_checkbox_set_text(cb2, "Strict TLSF Bounds Checking");
    lv_obj_set_style_text_color(cb2, lv_color_hex(0xCDD6F4), 0);
    lv_obj_set_pos(cb2, 16, 240);
    lv_obj_add_state(cb2, LV_STATE_CHECKED);

    /* Sliders */
    lv_obj_t *sl1_lbl = lv_label_create(card1);
    lv_label_set_text(sl1_lbl, "Master Volume: 75%");
    lv_obj_set_style_text_color(sl1_lbl, lv_color_hex(0xBAC2DE), 0);
    lv_obj_set_pos(sl1_lbl, 16, 285);

    lv_obj_t *sl1 = lv_slider_create(card1);
    lv_obj_set_pos(sl1, 16, 310);
    lv_obj_set_size(sl1, card_w - 50, 14);
    lv_slider_set_value(sl1, 75, LV_ANIM_OFF);

    lv_obj_t *sl2_lbl = lv_label_create(card1);
    lv_label_set_text(sl2_lbl, "Display Brightness: 60%");
    lv_obj_set_style_text_color(sl2_lbl, lv_color_hex(0xBAC2DE), 0);
    lv_obj_set_pos(sl2_lbl, 16, 340);

    lv_obj_t *sl2 = lv_slider_create(card1);
    lv_obj_set_pos(sl2, 16, 365);
    lv_obj_set_size(sl2, card_w - 50, 14);
    lv_slider_set_value(sl2, 60, LV_ANIM_OFF);

    /* Dropdown */
    lv_obj_t *dd = lv_dropdown_create(card1);
    lv_dropdown_set_options(dd, "Catppuccin Mocha\nTokyo Night Dark\nNord Glacier\nGruvbox Dark");
    lv_obj_set_pos(dd, 16, 405);
    lv_obj_set_size(dd, card_w - 50, 42);

    /* ── CARD 2: Gauges & Indicators ─────────────────────────────── */
    lv_obj_t *card2 = lv_obj_create(scr);
    lv_obj_set_pos(card2, 20 + card_w + 16, card_y);
    lv_obj_set_size(card2, card_w, card_h);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_bg_opa(card2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card2, lv_color_hex(0x313244), 0);
    lv_obj_set_style_border_width(card2, 1, 0);
    lv_obj_set_style_radius(card2, 10, 0);
    lv_obj_clear_flag(card2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c2_title = lv_label_create(card2);
    lv_label_set_text(c2_title, "DIALS & PROGRESS GAUGES");
    lv_obj_set_style_text_color(c2_title, lv_color_hex(0xA6E3A1), 0);
    lv_obj_set_pos(c2_title, 10, 8);

    /* Arc 1 */
    lv_obj_t *arc1 = lv_arc_create(card2);
    lv_obj_set_pos(arc1, 30, 40);
    lv_obj_set_size(arc1, 150, 150);
    lv_arc_set_value(arc1, 72);
    lv_obj_t *arc1_lbl = lv_label_create(arc1);
    lv_label_set_text(arc1_lbl, "72%");
    lv_obj_set_style_text_color(arc1_lbl, lv_color_hex(0xCDD6F4), 0);
    lv_obj_center(arc1_lbl);
    lv_obj_t *arc1_sub = lv_label_create(card2);
    lv_label_set_text(arc1_sub, "Storage Used");
    lv_obj_set_style_text_color(arc1_sub, lv_color_hex(0xA6ADC8), 0);
    lv_obj_set_pos(arc1_sub, 65, 195);

    /* Arc 2 */
    lv_obj_t *arc2 = lv_arc_create(card2);
    lv_obj_set_pos(arc2, card_w - 190, 40);
    lv_obj_set_size(arc2, 150, 150);
    lv_arc_set_value(arc2, 45);
    lv_obj_t *arc2_lbl = lv_label_create(arc2);
    lv_label_set_text(arc2_lbl, "45°C");
    lv_obj_set_style_text_color(arc2_lbl, lv_color_hex(0xF9E2AF), 0);
    lv_obj_center(arc2_lbl);
    lv_obj_t *arc2_sub = lv_label_create(card2);
    lv_label_set_text(arc2_sub, "Core Temp");
    lv_obj_set_style_text_color(arc2_sub, lv_color_hex(0xA6ADC8), 0);
    lv_obj_set_pos(arc2_sub, card_w - 150, 195);

    /* Progress bars */
    lv_obj_t *bar1_lbl = lv_label_create(card2);
    lv_label_set_text(bar1_lbl, "EEVDF Runqueue Balance: 38%");
    lv_obj_set_style_text_color(bar1_lbl, lv_color_hex(0xCDD6F4), 0);
    lv_obj_set_pos(bar1_lbl, 16, 235);

    lv_obj_t *bar1 = lv_bar_create(card2);
    lv_obj_set_pos(bar1, 16, 260);
    lv_obj_set_size(bar1, card_w - 50, 16);
    lv_bar_set_value(bar1, 38, LV_ANIM_OFF);

    lv_obj_t *bar2_lbl = lv_label_create(card2);
    lv_label_set_text(bar2_lbl, "Kernel Heap Allotment: 15%");
    lv_obj_set_style_text_color(bar2_lbl, lv_color_hex(0xCDD6F4), 0);
    lv_obj_set_pos(bar2_lbl, 16, 295);

    lv_obj_t *bar2 = lv_bar_create(card2);
    lv_obj_set_pos(bar2, 16, 320);
    lv_obj_set_size(bar2, card_w - 50, 16);
    lv_bar_set_value(bar2, 15, LV_ANIM_OFF);

    lv_obj_t *bar3_lbl = lv_label_create(card2);
    lv_label_set_text(bar3_lbl, "AHCI Block Device Throughput: 84%");
    lv_obj_set_style_text_color(bar3_lbl, lv_color_hex(0xCDD6F4), 0);
    lv_obj_set_pos(bar3_lbl, 16, 355);

    lv_obj_t *bar3 = lv_bar_create(card2);
    lv_obj_set_pos(bar3, 16, 380);
    lv_obj_set_size(bar3, card_w - 50, 16);
    lv_bar_set_value(bar3, 84, LV_ANIM_OFF);

    /* Status badge */
    lv_obj_t *badge = lv_obj_create(card2);
    lv_obj_set_pos(badge, 16, 420);
    lv_obj_set_size(badge, card_w - 50, 38);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x11111B), 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0x22C55E), 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_radius(badge, 6, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge_lbl = lv_label_create(badge);
    lv_label_set_text(badge_lbl, "● SYSTEM STATUS: OPTIMAL (60 FPS)");
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(0x22C55E), 0);
    lv_obj_center(badge_lbl);

    /* ── CARD 3: System Telemetry ────────────────────────────────── */
    lv_obj_t *card3 = lv_obj_create(scr);
    lv_obj_set_pos(card3, 20 + (card_w + 16) * 2, card_y);
    lv_obj_set_size(card3, card_w, card_h);
    lv_obj_set_style_bg_color(card3, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_bg_opa(card3, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card3, lv_color_hex(0x313244), 0);
    lv_obj_set_style_border_width(card3, 1, 0);
    lv_obj_set_style_radius(card3, 10, 0);
    lv_obj_clear_flag(card3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c3_title = lv_label_create(card3);
    lv_label_set_text(c3_title, "SYSTEM TELEMETRY & SPECS");
    lv_obj_set_style_text_color(c3_title, lv_color_hex(0xFAB387), 0);
    lv_obj_set_pos(c3_title, 10, 8);

    char telem_buf[512];
    snprintf(telem_buf, sizeof(telem_buf),
             "Kernel:       OS01 x86_64 Higher-Half\n\n"
             "SMP Cores:    2 Active APs (MADT / IPI)\n\n"
             "Scheduler:    EEVDF O(log n) Latency-Aware\n\n"
             "Video View:   %u x %u (32bpp XRGB8888)\n\n"
             "Graphics API: /dev/gfx0 2D Accelerated\n\n"
             "GUI Stack:    LVGL v9.5.0 Standalone\n\n"
             "Memory:       TLSF 4096 KiB Static Pool\n\n"
             "Clock Source: CLOCK_MONOTONIC\n\n"
             "Security:     Ring 3 Freestanding Isolation\n\n"
             "Test Suite:   ALL ASSERTIONS PASS",
             w, h);

    lv_obj_t *c3_info = lv_label_create(card3);
    lv_label_set_text(c3_info, telem_buf);
    lv_obj_set_style_text_color(c3_info, lv_color_hex(0xBAC2DE), 0);
    lv_obj_set_pos(c3_info, 16, 45);

    /* ── 3. Bottom Footer Showcase Card ───────────────────────────── */
    int32_t foot_y = card_y + card_h + 16;
    int32_t foot_h = 115;

    lv_obj_t *footer = lv_obj_create(scr);
    lv_obj_set_pos(footer, 20, foot_y);
    lv_obj_set_size(footer, w - 40, foot_h);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x11111B), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(0x313244), 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_radius(footer, 10, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *foot_title = lv_label_create(footer);
    lv_label_set_text(foot_title, "DESIGN SYSTEM PALETTE & STATUS");
    lv_obj_set_style_text_color(foot_title, lv_color_hex(0xCBA6F7), 0);
    lv_obj_set_pos(foot_title, 12, 6);

    /* 8 Palette Swatches */
    static const uint32_t palette[8] = {
        0x1A73E8, 0x22C55E, 0xF59E0B, 0xEF4444,
        0x8B5CF6, 0x06B6D4, 0xEC4899, 0x64748B
    };
    static const char *pal_names[8] = {
        "Blue", "Green", "Amber", "Red", "Purple", "Cyan", "Pink", "Slate"
    };

    int32_t swatch_w = ((int32_t)w - 80) / 8;
    for (int i = 0; i < 8; i++) {
        lv_obj_t *sw = lv_obj_create(footer);
        lv_obj_set_pos(sw, 12 + i * swatch_w, 30);
        lv_obj_set_size(sw, swatch_w - 12, 32);
        lv_obj_set_style_bg_color(sw, lv_color_hex(palette[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 6, 0);
        lv_obj_set_style_border_width(sw, 0, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sw_lbl = lv_label_create(sw);
        lv_label_set_text(sw_lbl, pal_names[i]);
        lv_obj_set_style_text_color(sw_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(sw_lbl);
    }

    lv_obj_t *foot_hint = lv_label_create(footer);
    lv_label_set_text(foot_hint, "Execution complete. Displaying rendered LVGL framebuffer presentation...");
    lv_obj_set_style_text_color(foot_hint, lv_color_hex(0x6C7086), 0);
    lv_obj_set_pos(foot_hint, 12, 72);
}

int main(int argc, char **argv)
{
    bool wait_mode = (argc > 1 && (strcmp(argv[1], "-w") == 0 ||
                                  strcmp(argv[1], "wait") == 0 ||
                                  strcmp(argv[1], "--wait") == 0 ||
                                  strcmp(argv[1], "gui") == 0));
    bool smoke_mode = (argc > 1 && strcmp(argv[1], "smoke") == 0);

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

    /* 5. Probe Framebuffer and libgfx */
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd >= 0) {
        struct fb_info info;
        ssize_t r = read(fb_fd, &info, sizeof(info));
        ioctl(fb_fd, FBIOSURRENDER, NULL);
        close(fb_fd);

        if (r == (ssize_t)sizeof(info) && info.width > 0 && info.height > 0) {
            screen_w = info.width;
            screen_h = info.height;

            if (!smoke_mode) {
                /* Enter alt screen and wait out terminal CMD_HOLD_MS (500ms) */
                write(1, "\x1b[?1049h\x1b[2J", 12);
                poll(NULL, 0, 550);
            }

            gfx = gfx_open(0, 0, screen_w, screen_h);
            if (!gfx) {
                printf("[test_lvgl] WARNING: gfx_open failed, using headless memory display\n");
            }
        }
    }

    /* 6. Create display and partial draw buffer */
    static uint32_t draw_buf[MAX_HOR_RES * BUF_LINES];
    uint32_t buf_bytes = (uint32_t)(screen_w * BUF_LINES * sizeof(uint32_t));

    lv_display_t *disp = lv_display_create(screen_w, screen_h);
    TEST_ASSERT(disp != NULL);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, draw_buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, test_flush_cb);

    /* 7. Build UI scene */
    lv_obj_t *scr = lv_screen_active();
    TEST_ASSERT(scr != NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_EXPECT_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if (screen_w >= 640 && screen_h >= 480) {
        build_rich_ui(scr, screen_w, screen_h);
    } else {
        /* Fallback minimal scene */
        btn_sample_x = 100;
        btn_sample_y = 70;
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
    }

    /* 8. Render loop and present */
    for (int frame = 0; frame < 5; frame++) {
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 20000000 };
        nanosleep(&pause, NULL);
        lv_timer_handler();
        if (!gfx_present_ok()) return 1;   /* stale/permanent -> abort */
    }

    /* 9. Assertions on flush output and pixel sampling */
    TEST_ASSERT(flush_count > 0);
    TEST_ASSERT(bg_sampled);
    TEST_ASSERT(btn_sampled);
    printf("[test_lvgl] Rendering and pixel assertions passed (flush_count=%d).\n", flush_count);

    /* 10. TLSF memory pool assertions */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("[test_lvgl] Memory pool: total=%u, free=%u, used_pct=%u%%\n",
           mon.total_size, mon.free_size, (unsigned)mon.used_pct);

    TEST_ASSERT(mon.total_size >= (4 * 1024 * 1024 - 65536));
    TEST_ASSERT(mon.used_pct > 0 && mon.used_pct < 50);
    TEST_ASSERT(mon.free_size > 0 && mon.free_size < mon.total_size);

    printf("[TEST PASS] LVGL compatibility smoke test succeeded.\n");
    fflush(stdout);

    /* 11. Visual display hold / event loop */
    if (wait_mode) {
        printf("[test_lvgl] Interactive mode: Press 'q' or ESC to exit...\n");
        while (1) {
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
            int ret = poll(&pfd, 1, 100);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                char ch = 0;
                if (read(STDIN_FILENO, &ch, 1) > 0) {
                    if (ch == 'q' || ch == 'Q' || ch == 27 || ch == 3) {
                        break;
                    }
                }
            }
            lv_timer_handler();
            if (!gfx_present_ok()) return 1;
        }
    } else if (!smoke_mode) {
        /* Hold on screen for 1 second so framebuffer / QMP screendump captures it,
         * using poll(NULL) so stdin queued commands are preserved */
        for (int i = 0; i < 10; i++) {
            poll(NULL, 0, 100);
            lv_timer_handler();
            if (!gfx_present_ok()) return 1;
        }
    }

    /* 12. Cleanup */
    if (gfx) {
        gfx_close(gfx);
    }
    if (!smoke_mode && fb_fd >= 0) {
        write(1, "\x1b[?1049l", 8);
    }

    return 0;
}
