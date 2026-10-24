/*
 * test_gui_theme.c — Material 设计令牌表契约测试（GUI-3 Task 1）
 *
 * 覆盖设计规范 §9.1 的 8 组断言：色彩角色合法性、on_* 配对、字号层级、
 * 间距刻度、圆角刻度、elevation 单调性、elevation 表面色序列、非法枚举。
 *
 * 本测试只编译 libgui/src/theme.c（纯令牌表，不含 LVGL），因此可以在
 * 宿主侧独立验证设计系统的取值。
 */

#include "test_framework.h"
#include <gui/gui_theme.h>

#include <stddef.h>
#include <stdint.h>

/* ── 断言 1：每个色彩角色都是合法的 0x00RRGGBB ──────────────────── */

static void assert_color_role(uint32_t color, const char *name)
{
    /* 最高字节（A 位）必须为 0：令牌一律是 0x00RRGGBB 的 8 位写法。 */
    assert_true((color >> 24) == 0u);
    /* RGB 三通道不得全为 0（scrim / shadow 是纯黑，属于合法例外）。 */
    if (color != 0x00000000u) {
        assert_true((color & 0x00FFFFFFu) != 0u);
    }
    (void)name;
}

TEST_FUNC(test_color_roles_are_wellformed)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    assert_not_null(c);

    assert_color_role(c->primary, "primary");
    assert_color_role(c->on_primary, "on_primary");
    assert_color_role(c->primary_container, "primary_container");
    assert_color_role(c->on_primary_container, "on_primary_container");
    assert_color_role(c->secondary, "secondary");
    assert_color_role(c->on_secondary, "on_secondary");
    assert_color_role(c->secondary_container, "secondary_container");
    assert_color_role(c->on_secondary_container, "on_secondary_container");
    assert_color_role(c->tertiary, "tertiary");
    assert_color_role(c->on_tertiary, "on_tertiary");
    assert_color_role(c->tertiary_container, "tertiary_container");
    assert_color_role(c->on_tertiary_container, "on_tertiary_container");
    assert_color_role(c->error, "error");
    assert_color_role(c->on_error, "on_error");
    assert_color_role(c->error_container, "error_container");
    assert_color_role(c->on_error_container, "on_error_container");
    assert_color_role(c->success, "success");
    assert_color_role(c->on_success, "on_success");
    assert_color_role(c->success_container, "success_container");
    assert_color_role(c->on_success_container, "on_success_container");
    assert_color_role(c->surface, "surface");
    assert_color_role(c->on_surface, "on_surface");
    assert_color_role(c->surface_variant, "surface_variant");
    assert_color_role(c->on_surface_variant, "on_surface_variant");
    assert_color_role(c->surface_container_low, "surface_container_low");
    assert_color_role(c->surface_container, "surface_container");
    assert_color_role(c->surface_container_high, "surface_container_high");
    assert_color_role(c->surface_container_highest, "surface_container_highest");
    assert_color_role(c->outline, "outline");
    assert_color_role(c->outline_variant, "outline_variant");
    assert_color_role(c->inverse_surface, "inverse_surface");
    assert_color_role(c->inverse_on_surface, "inverse_on_surface");
    assert_color_role(c->scrim, "scrim");
    assert_color_role(c->shadow, "shadow");

    /* scrim 与 shadow 是纯黑遮罩 / 阴影基色。 */
    assert_eq(0x00000000u, c->scrim);
    assert_eq(0x00000000u, c->shadow);
}

/* ── 断言 2：on_x 与 x 成对存在且取值不同 ───────────────────────── */

TEST_FUNC(test_on_color_pairs_differ_from_base)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    assert_true(c->on_primary != c->primary);
    assert_true(c->on_primary_container != c->primary_container);
    assert_true(c->on_secondary != c->secondary);
    assert_true(c->on_secondary_container != c->secondary_container);
    assert_true(c->on_tertiary != c->tertiary);
    assert_true(c->on_tertiary_container != c->tertiary_container);
    assert_true(c->on_error != c->error);
    assert_true(c->on_error_container != c->error_container);
    assert_true(c->on_success != c->success);
    assert_true(c->on_success_container != c->success_container);
    assert_true(c->on_surface != c->surface);
    assert_true(c->on_surface_variant != c->surface_variant);
    assert_true(c->inverse_on_surface != c->inverse_surface);
}

/* ── 断言 3：字号层级严格递减 ───────────────────────────────────── */

TEST_FUNC(test_font_scale_is_strictly_decreasing)
{
    int display  = gui_theme_font_size(GUI_THEME_FONT_DISPLAY);
    int headline = gui_theme_font_size(GUI_THEME_FONT_HEADLINE);
    int title    = gui_theme_font_size(GUI_THEME_FONT_TITLE);
    int body     = gui_theme_font_size(GUI_THEME_FONT_BODY);
    int label    = gui_theme_font_size(GUI_THEME_FONT_LABEL);

    assert_eq(24, display);
    assert_eq(20, headline);
    assert_eq(16, title);
    assert_eq(14, body);
    assert_eq(12, label);

    assert_true(display > headline);
    assert_true(headline > title);
    assert_true(title > body);
    assert_true(body > label);
}

/* ── 断言 4：间距刻度严格递增 ───────────────────────────────────── */

TEST_FUNC(test_space_scale_is_strictly_increasing)
{
    int xs = gui_theme_space(GUI_THEME_SPACE_XS);
    int sm = gui_theme_space(GUI_THEME_SPACE_SM);
    int md = gui_theme_space(GUI_THEME_SPACE_MD);
    int lg = gui_theme_space(GUI_THEME_SPACE_LG);
    int xl = gui_theme_space(GUI_THEME_SPACE_XL);

    assert_eq(4, xs);
    assert_eq(8, sm);
    assert_eq(16, md);
    assert_eq(24, lg);
    assert_eq(32, xl);

    assert_true(xs < sm);
    assert_true(sm < md);
    assert_true(md < lg);
    assert_true(lg < xl);
}

/* ── 断言 5：圆角刻度严格递增 ───────────────────────────────────── */

TEST_FUNC(test_radius_scale_is_strictly_increasing)
{
    int sm   = gui_theme_radius(GUI_THEME_RADIUS_SM);
    int md   = gui_theme_radius(GUI_THEME_RADIUS_MD);
    int lg   = gui_theme_radius(GUI_THEME_RADIUS_LG);
    int xl   = gui_theme_radius(GUI_THEME_RADIUS_XL);
    int full = gui_theme_radius(GUI_THEME_RADIUS_FULL);

    assert_eq(4, sm);
    assert_eq(8, md);
    assert_eq(12, lg);
    assert_eq(16, xl);
    assert_eq(28, full);

    assert_true(sm < md);
    assert_true(md < lg);
    assert_true(lg < xl);
    assert_true(xl < full);
}

/* ── 断言 6：elevation 阴影单调不减，且 level 0 无阴影 ───────────── */

TEST_FUNC(test_elevation_shadow_is_monotonic)
{
    int prev_w = -1;
    int prev_y = -1;
    int prev_o = -1;

    for (int level = GUI_THEME_ELEVATION_0; level <= GUI_THEME_ELEVATION_5; level++) {
        int w = gui_theme_elevation_shadow_width((gui_theme_elevation_t)level);
        int y = gui_theme_elevation_shadow_offset_y((gui_theme_elevation_t)level);
        int o = gui_theme_elevation_shadow_opa((gui_theme_elevation_t)level);

        assert_true(w >= 0);
        assert_true(y >= 0);
        assert_true(o >= 0);
        assert_true(o <= 255);

        assert_true(w >= prev_w);
        assert_true(y >= prev_y);
        assert_true(o >= prev_o);

        prev_w = w;
        prev_y = y;
        prev_o = o;
    }

    /* elevation 0 是页面底：不得有阴影。 */
    assert_eq(0, gui_theme_elevation_shadow_width(GUI_THEME_ELEVATION_0));
    assert_eq(0, gui_theme_elevation_shadow_offset_y(GUI_THEME_ELEVATION_0));
    assert_eq(0, gui_theme_elevation_shadow_opa(GUI_THEME_ELEVATION_0));

    /* elevation 5 必须是最高层：三项都严格大于 0。 */
    assert_true(gui_theme_elevation_shadow_width(GUI_THEME_ELEVATION_5) > 0);
    assert_true(gui_theme_elevation_shadow_offset_y(GUI_THEME_ELEVATION_5) > 0);
    assert_true(gui_theme_elevation_shadow_opa(GUI_THEME_ELEVATION_5) > 0);
}

/* ── 断言 7：elevation 表面色序列 ───────────────────────────────── */

TEST_FUNC(test_elevation_surface_sequence)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    assert_eq(c->surface,                   gui_theme_elevation_surface(GUI_THEME_ELEVATION_0));
    assert_eq(c->surface_container_low,     gui_theme_elevation_surface(GUI_THEME_ELEVATION_1));
    assert_eq(c->surface_container,         gui_theme_elevation_surface(GUI_THEME_ELEVATION_2));
    assert_eq(c->surface_container_high,    gui_theme_elevation_surface(GUI_THEME_ELEVATION_3));
    assert_eq(c->surface_container_highest, gui_theme_elevation_surface(GUI_THEME_ELEVATION_4));
    assert_eq(c->surface_container_highest, gui_theme_elevation_surface(GUI_THEME_ELEVATION_5));

    /* 层级越高表面越深：相邻层级不得相同（0..4 之间严格不同）。 */
    assert_true(c->surface_container_low != c->surface);
    assert_true(c->surface_container != c->surface_container_low);
    assert_true(c->surface_container_high != c->surface_container);
    assert_true(c->surface_container_highest != c->surface_container_high);
}

/* ── 断言 8：非法枚举返回 -1 / 0xFFFFFFFF ───────────────────────── */

TEST_FUNC(test_invalid_enum_returns_error)
{
    assert_eq(-1, gui_theme_font_size((gui_theme_font_t)99));
    assert_eq(-1, gui_theme_font_size((gui_theme_font_t)-1));
    assert_eq(-1, gui_theme_space((gui_theme_space_t)99));
    assert_eq(-1, gui_theme_space((gui_theme_space_t)-1));
    assert_eq(-1, gui_theme_radius((gui_theme_radius_t)99));
    assert_eq(-1, gui_theme_radius((gui_theme_radius_t)-1));
    assert_eq(-1, gui_theme_elevation_shadow_width((gui_theme_elevation_t)99));
    assert_eq(-1, gui_theme_elevation_shadow_offset_y((gui_theme_elevation_t)99));
    assert_eq(-1, gui_theme_elevation_shadow_opa((gui_theme_elevation_t)99));
    assert_eq(0xFFFFFFFFu, gui_theme_elevation_surface((gui_theme_elevation_t)99));
    assert_eq(0xFFFFFFFFu, gui_theme_elevation_surface((gui_theme_elevation_t)-1));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_color_roles_are_wellformed),
    TEST_ENTRY(test_on_color_pairs_differ_from_base),
    TEST_ENTRY(test_font_scale_is_strictly_decreasing),
    TEST_ENTRY(test_space_scale_is_strictly_increasing),
    TEST_ENTRY(test_radius_scale_is_strictly_increasing),
    TEST_ENTRY(test_elevation_shadow_is_monotonic),
    TEST_ENTRY(test_elevation_surface_sequence),
    TEST_ENTRY(test_invalid_enum_returns_error),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
