/*
 * test_gui_theme_apply.c — Material 主题落地契约测试（GUI-3 Task 2）
 *
 * 验证 Material 令牌表确实变成了 LVGL 主题：
 *   - gui_platform_create() 之后 display 已挂上主题，屏幕已套用主题（壁纸）
 *   - 容器 / 按钮 / 标签三类控件的底色、圆角、边框、字号全部来自令牌表
 *   - hover / pressed / focus / disabled / checked 五类状态各自有对应样式
 *   - gui_platform_get_lv_display(NULL) 返回 NULL 且 errno == EINVAL
 *
 * 与 test_gui_theme.c 的分工：后者只验证令牌**取值**（不链接 LVGL），
 * 本测试验证令牌**落地**（链接真实 LVGL 与真实 platform）。
 *
 * 状态断言**一次只加一个状态**：LVGL 的样式仲裁在多个状态同时存在时
 * 依赖具体优先级，这里要验证的是「每个状态都有对应样式」，而不是仲裁
 * 顺序；组合状态的仲裁由 LVGL 自身保证。
 */

#include "test_framework.h"
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <gui_lvgl_private.h>
#include "gui_test_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* libgui/src/platform.c 在 OS01_HOST_TEST 下引用的注入开关。 */
bool g_gui_test_tamper_header = false;

/*
 * LVGL 内部的颜色值最高字节是 0xFF（0xFFRRGGBB），而令牌表用的是
 * 0x00RRGGBB（与 gui_platform_get_pixel() 的返回格式一致）。比较时统一
 * 掩掉最高字节，只比对 RGB。
 */
static uint32_t style_color_u32(lv_color_t c)
{
    return lv_color_to_u32(c) & 0x00FFFFFFu;
}

/* ── 断言：平台创建后主题已就位、屏幕已套用 ─────────────────────── */

TEST_FUNC(test_theme_is_attached_and_screen_is_themed)
{
    gui_test_reset();

    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 64,
        .height = 48,
        .buffer_lines = 8
    };

    gui_platform_t *p = gui_platform_create(&cfg);
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    /* 平台一律以 Material 设计系统出场。 */
    assert_not_null(lv_display_get_theme(disp));

    /* 屏幕已套用主题：壁纸 = surface。 */
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    assert_not_null(scr);
    assert_eq(gui_theme_colors()->surface,
              style_color_u32(lv_obj_get_style_bg_color(scr, LV_PART_MAIN)));

    gui_platform_destroy(p);
}

/* ── 断言：控件样式全部来自令牌表 ───────────────────────────────── */

TEST_FUNC(test_widget_styles_come_from_tokens)
{
    gui_test_reset();

    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 64,
        .height = 48,
        .buffer_lines = 8
    };

    gui_platform_t *p = gui_platform_create(&cfg);
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    const gui_theme_colors_t *c = gui_theme_colors();
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    assert_not_null(scr);

    /* 容器：elevation 2 表面 + radius_lg + 1px outline_variant 边框。 */
    lv_obj_t *card = lv_obj_create(scr);
    assert_not_null(card);
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2),
              style_color_u32(lv_obj_get_style_bg_color(card, LV_PART_MAIN)));
    assert_eq(gui_theme_radius(GUI_THEME_RADIUS_LG),
              lv_obj_get_style_radius(card, LV_PART_MAIN));
    assert_eq(1, lv_obj_get_style_border_width(card, LV_PART_MAIN));
    assert_eq(c->outline_variant,
              style_color_u32(lv_obj_get_style_border_color(card, LV_PART_MAIN)));

    /* 按钮：primary 底 + on_primary 文字 + radius_md + title 字号。 */
    lv_obj_t *btn = lv_button_create(scr);
    assert_not_null(btn);
    assert_eq(c->primary, style_color_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)));
    assert_eq(c->on_primary, style_color_u32(lv_obj_get_style_text_color(btn, LV_PART_MAIN)));
    assert_eq(gui_theme_radius(GUI_THEME_RADIUS_MD),
              lv_obj_get_style_radius(btn, LV_PART_MAIN));
    assert_true(lv_obj_get_style_text_font(btn, LV_PART_MAIN) == &lv_font_montserrat_16);

    /* 标签：on_surface 文字 + body 字号。 */
    lv_obj_t *label = lv_label_create(scr);
    assert_not_null(label);
    assert_eq(c->on_surface, style_color_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)));
    assert_true(lv_obj_get_style_text_font(label, LV_PART_MAIN) == &lv_font_montserrat_14);

    gui_platform_destroy(p);
}

/* ── 断言：按钮的 hover / pressed / disabled 三态 ────────────────── */

TEST_FUNC(test_button_interaction_states)
{
    gui_test_reset();

    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 64,
        .height = 48,
        .buffer_lines = 8
    };

    gui_platform_t *p = gui_platform_create(&cfg);
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    const gui_theme_colors_t *c = gui_theme_colors();
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    assert_not_null(scr);

    lv_obj_t *btn = lv_button_create(scr);
    assert_not_null(btn);

    /* 默认态 */
    assert_eq(c->primary, style_color_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)));

    /* hover：提亮到 secondary */
    lv_obj_add_state(btn, LV_STATE_HOVERED);
    assert_eq(c->secondary, style_color_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)));
    lv_obj_remove_state(btn, LV_STATE_HOVERED);

    /* pressed：压暗到 primary_container，文字转 on_primary_container */
    lv_obj_add_state(btn, LV_STATE_PRESSED);
    assert_eq(c->primary_container,
              style_color_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)));
    assert_eq(c->on_primary_container,
              style_color_u32(lv_obj_get_style_text_color(btn, LV_PART_MAIN)));
    lv_obj_remove_state(btn, LV_STATE_PRESSED);

    /* disabled：填充转 surface_variant，文字转 on_surface_variant */
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    assert_eq(c->surface_variant,
              style_color_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)));
    assert_eq(c->on_surface_variant,
              style_color_u32(lv_obj_get_style_text_color(btn, LV_PART_MAIN)));
    lv_obj_remove_state(btn, LV_STATE_DISABLED);

    /* focused：选中容器语义（primary_container 底 + on_primary_container 文字）。
     * 默认态的底色就是 primary，焦点环也是 primary——聚焦态换用
     * primary_container，环与填充才互相可区分。 */
    lv_obj_add_state(btn, LV_STATE_FOCUSED);
    assert_eq(c->primary_container,
              style_color_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)));
    assert_eq(c->on_primary_container,
              style_color_u32(lv_obj_get_style_text_color(btn, LV_PART_MAIN)));
    lv_obj_remove_state(btn, LV_STATE_FOCUSED);

    /* FOCUS_KEY 与 FOCUSED 同义：键盘导航的焦点反馈必须一样。 */
    lv_obj_add_state(btn, LV_STATE_FOCUS_KEY);
    assert_eq(c->primary_container,
              style_color_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)));
    assert_eq(c->on_primary_container,
              style_color_u32(lv_obj_get_style_text_color(btn, LV_PART_MAIN)));
    lv_obj_remove_state(btn, LV_STATE_FOCUS_KEY);

    gui_platform_destroy(p);
}

/* ── 断言：焦点环可见且用 primary 轮廓 ──────────────────────────── */

TEST_FUNC(test_focus_ring_uses_primary_outline)
{
    gui_test_reset();

    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 64,
        .height = 48,
        .buffer_lines = 8
    };

    gui_platform_t *p = gui_platform_create(&cfg);
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    const gui_theme_colors_t *c = gui_theme_colors();
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    assert_not_null(scr);

    lv_obj_t *btn = lv_button_create(scr);
    assert_not_null(btn);

    /* 焦点态：2px primary 轮廓。 */
    lv_obj_add_state(btn, LV_STATE_FOCUSED);
    assert_eq(c->primary,
              style_color_u32(lv_obj_get_style_border_color(btn, LV_PART_MAIN)));
    assert_eq(2, lv_obj_get_style_border_width(btn, LV_PART_MAIN));

    /*
     * 环**自带**透明度：普通态把 border_opa 改成透明会让整个环不可见，
     * 主题必须自己声明"环是不透明的"。
     */
    assert_eq(LV_OPA_COVER, lv_obj_get_style_border_opa(btn, LV_PART_MAIN));

    gui_platform_destroy(p);
}

/* ── 断言：开关的 checked 态用 primary_container 语义 ───────────── */

TEST_FUNC(test_switch_checked_state)
{
    gui_test_reset();

    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = 64,
        .height = 48,
        .buffer_lines = 8
    };

    gui_platform_t *p = gui_platform_create(&cfg);
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    const gui_theme_colors_t *c = gui_theme_colors();
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    assert_not_null(scr);

    lv_obj_t *sw = lv_switch_create(scr);
    assert_not_null(sw);

    /* 未选中：surface_variant 底 + outline 旋钮。 */
    assert_eq(c->surface_variant,
              style_color_u32(lv_obj_get_style_bg_color(sw, LV_PART_MAIN)));
    assert_eq(c->outline,
              style_color_u32(lv_obj_get_style_bg_color(sw, LV_PART_KNOB)));

    /* 选中：primary 底 + on_primary 旋钮。 */
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    assert_eq(c->primary, style_color_u32(lv_obj_get_style_bg_color(sw, LV_PART_MAIN)));
    assert_eq(c->on_primary,
              style_color_u32(lv_obj_get_style_bg_color(sw, LV_PART_KNOB)));

    gui_platform_destroy(p);
}

/* ── 断言：空平台句柄返回 NULL 并设置 EINVAL ────────────────────── */

TEST_FUNC(test_null_platform_returns_einval)
{
    errno = 0;
    assert_null(gui_platform_get_lv_display(NULL));
    assert_eq(EINVAL, errno);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_theme_is_attached_and_screen_is_themed),
    TEST_ENTRY(test_widget_styles_come_from_tokens),
    TEST_ENTRY(test_button_interaction_states),
    TEST_ENTRY(test_focus_ring_uses_primary_outline),
    TEST_ENTRY(test_switch_checked_state),
    TEST_ENTRY(test_null_platform_returns_einval),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
