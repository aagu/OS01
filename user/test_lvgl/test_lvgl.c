/* user/test_lvgl.c — LVGL v9.5.0 graphical showcase & compatibility test for OS01 */

#include <gui/gui.h>
#include <gui_lvgl_private.h>

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CLR_EXPECT_BG  0x00181825u  /* Catppuccin Mantle dark background */
#define CLR_EXPECT_BTN 0x001A73E8u  /* Google Blue 600 */

static int32_t btn_sample_x = 100;
static int32_t btn_sample_y = 70;

static int get_monotonic_ms(uint64_t *out_ms)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    *out_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    return 0;
}

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERTION FAILED (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
        ret = 1; \
        goto cleanup; \
    } \
} while (0)


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
    int ret = 0;
    gui_platform_t *platform = NULL;

    uint64_t app_start_ms = 0;
    if (get_monotonic_ms(&app_start_ms) != 0) {
        fprintf(stderr, "[test_lvgl] clock_gettime failed at startup\n");
        return 1;
    }

    bool wait_mode = false;
    bool watch_display = false;
    bool smoke_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0 || strcmp(argv[i], "smoke") == 0) {
            smoke_mode = true;
        } else if (strcmp(argv[i], "--watch-display") == 0) {
            watch_display = true;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "wait") == 0 ||
                   strcmp(argv[i], "--wait") == 0 || strcmp(argv[i], "gui") == 0) {
            wait_mode = true;
        }
    }

    if (smoke_mode) {
        printf("[test_lvgl] Starting LVGL v9.5.0 smoke test...\n");
    } else {
        printf("[test_lvgl] Starting LVGL v9.5.0 showcase...\n");
    }

    gui_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (smoke_mode) {
        cfg.mode = GUI_MODE_OFFSCREEN;
        cfg.width = 320;
        cfg.height = 240;
        cfg.buffer_lines = 40;
    } else {
        cfg.mode = GUI_MODE_ONSCREEN;
        cfg.width = 0;
        cfg.height = 0;
        cfg.buffer_lines = 40;
    }

    platform = gui_platform_create(&cfg);
    if (!platform) {
        int err = errno;
        if (err == EBUSY) {
            fprintf(stderr, "[test_lvgl] display busy (another GUI active); exiting\n");
        } else {
            fprintf(stderr, "[test_lvgl] gui_platform_create failed (errno=%d): %s\n", err, strerror(err));
        }
        ret = 1;
        goto cleanup;
    }

    uint32_t screen_w = gui_platform_get_width(platform);
    uint32_t screen_h = gui_platform_get_height(platform);

    lv_display_t *disp = gui_platform_get_lv_display(platform);
    TEST_ASSERT(disp != NULL);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    TEST_ASSERT(scr != NULL);

    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_EXPECT_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if (screen_w >= 640 && screen_h >= 480) {
        build_rich_ui(scr, screen_w, screen_h);
    } else {
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

    if (smoke_mode) {
        bool pixel_assert_passed = false;
        while (1) {
            uint64_t now_ms = 0;
            if (get_monotonic_ms(&now_ms) != 0) {
                fprintf(stderr, "[test_lvgl] monotonic clock error\n");
                ret = 2;
                goto cleanup;
            }
            if (now_ms - app_start_ms >= 1000ULL) {
                fprintf(stderr, "[test_lvgl] smoke test timed out within 1s budget\n");
                ret = 2;
                goto cleanup;
            }

            uint32_t delay_ms = 100;
            gui_status_t status = gui_platform_step(platform, &delay_ms);
            if (status != GUI_STATUS_OK) {
                fprintf(stderr, "[test_lvgl] step failed (status=%d, errno=%d)\n", (int)status, errno);
                ret = 1;
                goto cleanup;
            }

            uint32_t px_bg = gui_platform_get_pixel(platform, 10, 10);
            uint32_t px_btn = gui_platform_get_pixel(platform, (uint32_t)btn_sample_x, (uint32_t)btn_sample_y);
            if (px_bg == CLR_EXPECT_BG && px_btn == CLR_EXPECT_BTN) {
                pixel_assert_passed = true;
                break;
            }

            uint64_t elapsed = now_ms - app_start_ms;
            uint64_t rem = (elapsed < 1000ULL) ? (1000ULL - elapsed) : 0;
            if (delay_ms > rem) delay_ms = (uint32_t)rem;
            int pret = poll(NULL, 0, (int)delay_ms);
            if (pret < 0 && errno != EINTR) {
                fprintf(stderr, "[test_lvgl] poll failed (errno=%d)\n", errno);
                ret = 1;
                goto cleanup;
            }
        }

        if (!pixel_assert_passed) {
            fprintf(stderr, "[test_lvgl] pixel assertions failed\n");
            ret = 1;
            goto cleanup;
        }

        printf("[test_lvgl] Rendering and pixel assertions passed.\n");

        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        printf("[test_lvgl] Memory pool: total=%u, free=%u, used_pct=%u%%\n",
               mon.total_size, mon.free_size, (unsigned)mon.used_pct);

        TEST_ASSERT(mon.total_size >= (4 * 1024 * 1024 - 65536));
        TEST_ASSERT(mon.used_pct > 0 && mon.used_pct < 50);
        TEST_ASSERT(mon.free_size > 0 && mon.free_size < mon.total_size);

        printf("[TEST PASS] LVGL compatibility smoke test succeeded.\n");
        fflush(stdout);

        ret = 0;
        goto cleanup;
    }

    /* Onscreen mode: render initial frames */
    for (int frame = 0; frame < 5; frame++) {
        uint32_t delay_ms = 20;
        gui_status_t status = gui_platform_step(platform, &delay_ms);
        if (status == GUI_STATUS_LOST_OWNERSHIP ||
            status == GUI_STATUS_DISPLAY_CHANGED ||
            status == GUI_STATUS_ERROR) {
            const char *why = (status == GUI_STATUS_LOST_OWNERSHIP) ? "lost display foreground ownership" :
                              (status == GUI_STATUS_DISPLAY_CHANGED) ? "display mode changed, restart the app" :
                              "display device failure";
            fprintf(stderr, "[test_lvgl] %s (errno=%d); exiting\n", why, errno);
            ret = 1;
            goto cleanup;
        }
        int pret = poll(NULL, 0, (int)delay_ms);
        if (pret < 0 && errno != EINTR) {
            fprintf(stderr, "[test_lvgl] poll failed (errno=%d)\n", errno);
            ret = 1;
            goto cleanup;
        }
    }

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("[test_lvgl] Memory pool: total=%u, free=%u, used_pct=%u%%\n",
           mon.total_size, mon.free_size, (unsigned)mon.used_pct);

    TEST_ASSERT(mon.total_size >= (4 * 1024 * 1024 - 65536));
    TEST_ASSERT(mon.used_pct > 0 && mon.used_pct < 50);
    TEST_ASSERT(mon.free_size > 0 && mon.free_size < mon.total_size);

    printf("[TEST PASS] LVGL compatibility smoke test succeeded.\n");
    fflush(stdout);

    if (wait_mode || watch_display) {
        if (watch_display) {
            printf("[test_lvgl] Watching display changes (stdin unused)...\n");
        } else {
            printf("[test_lvgl] Interactive mode: Press 'q' or ESC to exit...\n");
        }
        while (1) {
            /* Resolution E2E needs real PRESENT calls even for this static
             * showcase. The dedicated watcher never reads background stdin
             * (ash supplies /dev/null), using a 250 ms polling interval. */
            if (watch_display) {
                lv_obj_invalidate(scr);
            }
            uint32_t delay_ms = 100;
            gui_status_t status = gui_platform_step(platform, &delay_ms);
            if (status == GUI_STATUS_LOST_OWNERSHIP ||
                status == GUI_STATUS_DISPLAY_CHANGED ||
                status == GUI_STATUS_ERROR) {
                const char *why = (status == GUI_STATUS_LOST_OWNERSHIP) ? "lost display foreground ownership" :
                                  (status == GUI_STATUS_DISPLAY_CHANGED) ? "display mode changed, restart the app" :
                                  "display device failure";
                fprintf(stderr, "[test_lvgl] %s (errno=%d); exiting\n", why, errno);
                ret = 1;
                goto cleanup;
            }

            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
            int pret = watch_display ? poll(NULL, 0, 250)
                                     : poll(&pfd, 1, (int)delay_ms);
            if (pret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "[test_lvgl] poll stdin failed (errno=%d)\n", errno);
                ret = 1;
                goto cleanup;
            }
            if (pret > 0) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    break;
                }
                if (pfd.revents & POLLIN) {
                    char ch = 0;
                    ssize_t n = read(STDIN_FILENO, &ch, 1);
                    if (n > 0) {
                        if (ch == 'q' || ch == 'Q' || ch == 27 || ch == 3) {
                            break;
                        }
                    } else if (n == 0) {
                        break;
                    } else if (errno != EINTR && errno != EAGAIN) {
                        break;
                    }
                }
            }
        }
    } else {
        /* Default mode: hold display for 1 second, preserving stdin */
        uint64_t hold_start_ms = 0;
        if (get_monotonic_ms(&hold_start_ms) != 0) {
            ret = 1;
            goto cleanup;
        }
        while (1) {
            uint64_t hold_now_ms = 0;
            if (get_monotonic_ms(&hold_now_ms) != 0) {
                ret = 1;
                goto cleanup;
            }
            if (hold_now_ms - hold_start_ms >= 1000ULL) {
                break;
            }
            uint32_t delay_ms = 100;
            gui_status_t status = gui_platform_step(platform, &delay_ms);
            if (status == GUI_STATUS_LOST_OWNERSHIP ||
                status == GUI_STATUS_DISPLAY_CHANGED ||
                status == GUI_STATUS_ERROR) {
                const char *why = (status == GUI_STATUS_LOST_OWNERSHIP) ? "lost display foreground ownership" :
                                  (status == GUI_STATUS_DISPLAY_CHANGED) ? "display mode changed, restart the app" :
                                  "display device failure";
                fprintf(stderr, "[test_lvgl] %s (errno=%d); exiting\n", why, errno);
                ret = 1;
                goto cleanup;
            }
            uint64_t elapsed = hold_now_ms - hold_start_ms;
            uint64_t rem = (elapsed < 1000ULL) ? (1000ULL - elapsed) : 0;
            if (delay_ms > rem) delay_ms = (uint32_t)rem;
            int pret = poll(NULL, 0, (int)delay_ms);
            if (pret < 0 && errno != EINTR) {
                fprintf(stderr, "[test_lvgl] poll failed (errno=%d)\n", errno);
                ret = 1;
                goto cleanup;
            }
        }
    }

    ret = 0;

cleanup:
    if (platform) {
        gui_platform_destroy(platform);
        platform = NULL;
    }
    return ret;
}
