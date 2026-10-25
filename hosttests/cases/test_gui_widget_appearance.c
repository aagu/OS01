/*
 * test_gui_widget_appearance.c — Material 角色表达原生外观（GUI-2b Task 5）
 *
 * 本文件是**公共契约**测试：它只包含 gui 目录下的公共头、标准库、测试
 * 框架和宿主私有 fixture 头。断言全部通过公共 API 与值句柄表达；需要
 * 观察后端状态时，用的是 fixture 提供的宿主私有观察桥。
 *
 * 断言策略：外观是**看得见**的东西，因此这里的断言全部落在**像素**上：
 * 底色、文字色、边框、圆角、elevation 与焦点环都必须真的画出来。
 */

#include "test_framework.h"

#include <gui/gui.h>
#include <gui/gui_layout.h>
#include <gui/gui_theme.h>
#include <gui/gui_widget.h>

#include "gui_native_fixture.h"
#include "gui_test_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* The platform TU's host-only header tamper switch.  Every GUI host
 * fixture defines it; this fixture never flips it. */
bool g_gui_test_tamper_header = false;

/* ── 像素工具 ──────────────────────────────────────────────────────── */

#define REGION_W 200
#define REGION_H 40

static uint32_t g_region[REGION_W * REGION_H];

static void capture_region(gui_platform_t *platform, int32_t x, int32_t y, uint32_t *out)
{
    for (uint32_t row = 0; row < REGION_H; row++) {
        for (uint32_t col = 0; col < REGION_W; col++) {
            out[row * REGION_W + col] =
                gui_platform_get_pixel(platform, (uint32_t)(x + (int32_t)col),
                                       (uint32_t)(y + (int32_t)row));
        }
    }
}

/** 区域里有多少个像素正好是某个颜色。 */
static uint32_t count_color(const uint32_t *region, uint32_t color)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < REGION_W * REGION_H; i++) {
        if (region[i] == color) {
            n++;
        }
    }
    return n;
}

/* ── 类型契约 ────────────────────────────────────────────────────── */

TEST_FUNC(test_appearance_public_types_compile)
{
    /* 哨兵不得省略。 */
    assert_eq(5, GUI_VARIANT_COUNT);
    assert_eq(35, GUI_COLOR_COUNT);

    /* 角色必须覆盖令牌表的每一个颜色字段：令牌表的字段全是 32 位颜色，
     * 因此字段数就是 sizeof/4。 */
    assert_eq((int)(sizeof(gui_theme_colors_t) / sizeof(uint32_t)), GUI_COLOR_COUNT);

    /* 默认构造：面板形态 + 卡片表面 / 主要文字 / 细分割线 + 正文 / 中圆角 /
     * 无 elevation + 填充 + 圆角 + 不画普通边框。 */
    gui_appearance_t def = GUI_APPEARANCE_DEFAULT;
    assert_eq(GUI_VARIANT_PANEL, def.variant);
    assert_eq(GUI_COLOR_SURFACE_CONTAINER, def.bg_role);
    assert_eq(GUI_COLOR_ON_SURFACE, def.text_role);
    assert_eq(GUI_COLOR_OUTLINE_VARIANT, def.border_role);
    assert_eq(GUI_THEME_FONT_BODY, def.font);
    assert_eq(GUI_THEME_RADIUS_MD, def.radius);
    assert_eq(GUI_THEME_ELEVATION_0, def.elevation);
    assert_true(def.filled);
    assert_true(def.rounded);
    assert_false(def.bordered);

    /* 外观结构是可聚合的值类型。 */
    gui_appearance_t built = { GUI_VARIANT_CARD, GUI_COLOR_SURFACE, GUI_COLOR_ON_SURFACE,
                               GUI_COLOR_OUTLINE, GUI_THEME_FONT_TITLE, GUI_THEME_RADIUS_LG,
                               GUI_THEME_ELEVATION_2, true, true, true };
    assert_eq(GUI_VARIANT_CARD, built.variant);
    assert_eq(GUI_COLOR_SURFACE, built.bg_role);
    assert_eq(GUI_THEME_ELEVATION_2, built.elevation);
    assert_true(built.bordered);
}

/* ── 色彩角色 ────────────────────────────────────────────────────── */

TEST_FUNC(test_all_color_roles_map_to_tokens)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    const gui_theme_colors_t *c = gui_theme_colors();

    /* 令牌表的颜色字段顺序就是角色枚举的顺序：这张表是"枚举覆盖每一
     * 字段"的可执行版本。 */
    const uint32_t expected[35] = {
        c->primary,                   c->on_primary,
        c->primary_container,         c->on_primary_container,
        c->secondary,                 c->on_secondary,
        c->secondary_container,       c->on_secondary_container,
        c->tertiary,                  c->on_tertiary,
        c->tertiary_container,        c->on_tertiary_container,
        c->error,                     c->on_error,
        c->error_container,           c->on_error_container,
        c->success,                   c->on_success,
        c->success_container,         c->on_success_container,
        c->surface,                   c->surface_dim,
        c->on_surface,                c->surface_variant,
        c->on_surface_variant,        c->surface_container_low,
        c->surface_container,         c->surface_container_high,
        c->surface_container_highest, c->outline,
        c->outline_variant,           c->inverse_surface,
        c->inverse_on_surface,        c->scrim,
        c->shadow,
    };

    /* 35 个角色排成 7 x 5 的网格，每个 80x80，互不重叠。 */
    for (uint32_t i = 0; i < 35u; i++) {
        gui_widget_t box;
        assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &box));

        gui_geometry_t g;
        g.x = (int32_t)((i % 7u) * 84u);
        g.y = (int32_t)((i / 7u) * 84u);
        g.width = GUI_SIZE_PX(80);
        g.height = GUI_SIZE_PX(80);
        assert_eq(0, gui_widget_set_geometry(fx.platform, box, &g));

        gui_appearance_t ap = GUI_APPEARANCE_DEFAULT;
        ap.variant = GUI_VARIANT_PLAIN;
        ap.bg_role = (gui_color_role_t)i;
        ap.filled = true;
        ap.rounded = false;
        ap.bordered = false;
        assert_eq(0, gui_widget_set_appearance(fx.platform, box, &ap));
    }

    assert_eq(0, gui_native_fixture_step(&fx, 50));

    /* 每一个角色的底色都必须真的画成令牌表里的那个颜色。 */
    for (uint32_t i = 0; i < 35u; i++) {
        uint32_t x = (i % 7u) * 84u + 40u;
        uint32_t y = (i / 7u) * 84u + 40u;
        uint32_t pixel = gui_platform_get_pixel(fx.platform, x, y);
        assert_eq(expected[i], pixel);
    }

    gui_native_fixture_destroy(&fx);
}

/* ── 创建时的默认外观 ────────────────────────────────────────────── */

TEST_FUNC(test_appearance_defaults_per_kind)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    assert_eq(0, gui_native_fixture_step(&fx, 50));

    const gui_theme_colors_t *c = gui_theme_colors();

    /* 根是透明的全屏底板：它身后的壁纸（surface）透出来。 */
    assert_eq(c->surface, gui_platform_get_pixel(fx.platform, 600, 400));

    /* 按钮取主题里按钮的现有默认：primary 底 + on_primary 文字。 */
    gui_rect_t b;
    assert_eq(0, gui_widget_get_bounds(fx.platform, button, &b));
    assert_eq(c->primary, gui_platform_get_pixel(fx.platform, (uint32_t)(b.x + b.width / 2),
                                                  (uint32_t)(b.y + b.height / 2)));

    /* 标签是素面：不画底，文字是主要文字色。 */
    assert_eq(0, gui_widget_set_text(fx.platform, label, "Hi"));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    gui_rect_t l;
    assert_eq(0, gui_widget_get_bounds(fx.platform, label, &l));
    capture_region(fx.platform, l.x, l.y, g_region);
    assert_true(count_color(g_region, c->on_surface) > 0);

    gui_native_fixture_destroy(&fx);
}

/* ── 表面形态 ────────────────────────────────────────────────────── */

TEST_FUNC(test_card_plain_root_and_action_pixels)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t card;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &card));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, card, &g));

    const gui_theme_colors_t *c = gui_theme_colors();

    /* 卡片：卡片表面底 + 1px 细分割线边框 + 中圆角 + 零 elevation。 */
    gui_appearance_t ap = GUI_APPEARANCE_DEFAULT;
    ap.variant = GUI_VARIANT_CARD;
    ap.bg_role = GUI_COLOR_SURFACE_CONTAINER;
    ap.text_role = GUI_COLOR_ON_SURFACE;
    ap.border_role = GUI_COLOR_OUTLINE_VARIANT;
    ap.font = GUI_THEME_FONT_BODY;
    ap.radius = GUI_THEME_RADIUS_MD;
    ap.elevation = GUI_THEME_ELEVATION_0;
    ap.filled = true;
    ap.rounded = true;
    ap.bordered = true;
    assert_eq(0, gui_widget_set_appearance(fx.platform, card, &ap));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, card, &r));
    /* 中心是卡片表面。 */
    assert_eq(c->surface_container,
              gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 60), (uint32_t)(r.y + 30)));
    /* 边缘一圈是 1px 的普通边框。 */
    assert_eq(c->outline_variant, gui_platform_get_pixel(fx.platform, (uint32_t)r.x,
                                                          (uint32_t)(r.y + 30)));

    /* 圆角：卡片左上角那个像素被圆角切掉，属于卡片背后的背景。 */
    assert_eq(c->surface, gui_platform_get_pixel(fx.platform, (uint32_t)r.x, (uint32_t)r.y));

    /* 零圆角：把圆角关掉之后，那个角上的像素就是卡片自己的边框。 */
    ap.rounded = false;
    assert_eq(0, gui_widget_set_appearance(fx.platform, card, &ap));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    assert_eq(c->outline_variant, gui_platform_get_pixel(fx.platform, (uint32_t)r.x,
                                                          (uint32_t)r.y));

    /* 素面：同样的控件改成透明底、无边框。 */
    ap.variant = GUI_VARIANT_PLAIN;
    ap.filled = false;
    ap.bordered = false;
    assert_eq(0, gui_widget_set_appearance(fx.platform, card, &ap));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    /* 透明底：中心透出根容器（根也是透明的）身后的壁纸。 */
    assert_eq(c->surface, gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 60),
                                                  (uint32_t)(r.y + 30)));

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_appearance_variant_kind_rules)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t container;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &container));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    gui_appearance_t ap = GUI_APPEARANCE_DEFAULT;

    /* 根容器：ROOT 合法，但必须是透明、无边框、无圆角、无 elevation。 */
    ap.variant = GUI_VARIANT_ROOT;
    ap.filled = false;
    ap.rounded = false;
    ap.bordered = false;
    ap.elevation = GUI_THEME_ELEVATION_0;
    assert_eq(0, gui_widget_set_appearance(fx.platform, root, &ap));

    ap.filled = true;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, root, &ap));
    assert_eq(EINVAL, errno);
    ap.filled = false;
    ap.bordered = true;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, root, &ap));
    assert_eq(EINVAL, errno);
    ap.bordered = false;
    ap.rounded = true;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, root, &ap));
    assert_eq(EINVAL, errno);
    ap.rounded = false;
    ap.elevation = GUI_THEME_ELEVATION_2;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, root, &ap));
    assert_eq(EINVAL, errno);

    /* 普通容器：PANEL / CARD / PLAIN 合法，ROOT 与 ACTION 不合法。 */
    ap.variant = GUI_VARIANT_PANEL;
    assert_eq(0, gui_widget_set_appearance(fx.platform, container, &ap));
    ap.variant = GUI_VARIANT_CARD;
    assert_eq(0, gui_widget_set_appearance(fx.platform, container, &ap));
    ap.variant = GUI_VARIANT_PLAIN;
    assert_eq(0, gui_widget_set_appearance(fx.platform, container, &ap));
    ap.variant = GUI_VARIANT_ROOT;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, container, &ap));
    assert_eq(EINVAL, errno);
    ap.variant = GUI_VARIANT_ACTION;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, container, &ap));
    assert_eq(EINVAL, errno);

    /* 按钮：ACTION / CARD / PLAIN 合法，PANEL 与 ROOT 不合法。 */
    ap.variant = GUI_VARIANT_ACTION;
    assert_eq(0, gui_widget_set_appearance(fx.platform, button, &ap));
    ap.variant = GUI_VARIANT_CARD;
    assert_eq(0, gui_widget_set_appearance(fx.platform, button, &ap));
    ap.variant = GUI_VARIANT_PLAIN;
    assert_eq(0, gui_widget_set_appearance(fx.platform, button, &ap));
    ap.variant = GUI_VARIANT_PANEL;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, button, &ap));
    assert_eq(EINVAL, errno);

    /* 标签：只有 PLAIN。 */
    ap.variant = GUI_VARIANT_PLAIN;
    assert_eq(0, gui_widget_set_appearance(fx.platform, label, &ap));
    ap.variant = GUI_VARIANT_CARD;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, label, &ap));
    assert_eq(EINVAL, errno);

    gui_native_fixture_destroy(&fx);
}

/* ── 焦点环 ──────────────────────────────────────────────────────── */

TEST_FUNC(test_material_focus_style_overrides_base_border)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, button, &g));

    const gui_theme_colors_t *c = gui_theme_colors();

    /* 局部外观：普通边框用 surface_dim（与焦点环的 primary 明显不同）。 */
    gui_appearance_t ap = GUI_APPEARANCE_DEFAULT;
    ap.variant = GUI_VARIANT_ACTION;
    ap.bg_role = GUI_COLOR_SURFACE_CONTAINER;
    ap.text_role = GUI_COLOR_ON_SURFACE;
    ap.border_role = GUI_COLOR_SURFACE_DIM;
    ap.filled = true;
    ap.rounded = true;
    ap.bordered = true;
    assert_eq(0, gui_widget_set_appearance(fx.platform, button, &ap));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, button, &r));

    /* 普通状态：局部边框生效（1px surface_dim），底色是局部角色。 */
    assert_eq(c->surface_dim, gui_platform_get_pixel(fx.platform, (uint32_t)r.x,
                                                      (uint32_t)(r.y + 30)));
    assert_eq(c->surface_container, gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 60),
                                                           (uint32_t)(r.y + 30)));

    /* 聚焦：主题的 2px primary 焦点环**取代**普通局部边框。 */
    assert_eq(0, gui_native_fixture_set_focus_visual(&fx, button, true));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    assert_eq(c->primary, gui_platform_get_pixel(fx.platform, (uint32_t)r.x,
                                                  (uint32_t)(r.y + 30)));
    /* 焦点环是 2px：第二圈也是 primary。 */
    assert_eq(c->primary, gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 1),
                                                  (uint32_t)(r.y + 30)));
    /* 局部边框没有渗进来。 */
    assert_false(gui_platform_get_pixel(fx.platform, (uint32_t)r.x, (uint32_t)(r.y + 30)) ==
                 c->surface_dim);

    /* 失焦：焦点环消失，普通局部边框回来。 */
    assert_eq(0, gui_native_fixture_set_focus_visual(&fx, button, false));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    assert_eq(c->surface_dim, gui_platform_get_pixel(fx.platform, (uint32_t)r.x,
                                                      (uint32_t)(r.y + 30)));

    /* 指针的位移是相对的：先把它归到视口左上角，再移到按钮上。 */
    mouse_event_t home;
    memset(&home, 0, sizeof(home));
    home.dx = INT16_MIN;
    home.dy = INT16_MIN;
    assert_eq(0, gui_native_fixture_mouse(&fx, &home, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    mouse_event_t move;
    memset(&move, 0, sizeof(move));
    move.dx = (int16_t)(r.x + 8);
    move.dy = (int16_t)(r.y + 8);
    assert_eq(0, gui_native_fixture_mouse(&fx, &move, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    /* 主题的 hover 状态没有被局部外观盖掉：悬停时底色变成 secondary。 */
    assert_eq(c->secondary, gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 60),
                                                    (uint32_t)(r.y + 30)));

    /* 按下：主题的 pressed 状态（primary_container）。 */
    mouse_event_t press;
    memset(&press, 0, sizeof(press));
    press.buttons = 0x01u;
    assert_eq(0, gui_native_fixture_mouse(&fx, &press, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    assert_eq(c->primary_container, gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 60),
                                                           (uint32_t)(r.y + 30)));

    /* 释放。 */
    mouse_event_t release;
    memset(&release, 0, sizeof(release));
    assert_eq(0, gui_native_fixture_mouse(&fx, &release, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    /* 把指针移开：局部外观的底色回来。 */
    mouse_event_t away;
    memset(&away, 0, sizeof(away));
    away.dx = INT16_MAX;
    away.dy = INT16_MAX;
    assert_eq(0, gui_native_fixture_mouse(&fx, &away, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    assert_eq(c->surface_container, gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 60),
                                                           (uint32_t)(r.y + 30)));

    /* 禁用：主题的 disabled 状态（surface_variant）。 */
    assert_eq(0, gui_widget_set_enabled(fx.platform, button, false));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    assert_eq(c->surface_variant, gui_platform_get_pixel(fx.platform, (uint32_t)(r.x + 60),
                                                          (uint32_t)(r.y + 30)));

    gui_native_fixture_destroy(&fx);
}

/* ── 参数校验 ────────────────────────────────────────────────────── */

TEST_FUNC(test_bad_appearance_is_atomic)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t card;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &card));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, card, &g));

    const gui_theme_colors_t *c = gui_theme_colors();

    /* 先落一个合法外观：卡片表面底。 */
    gui_appearance_t good = GUI_APPEARANCE_DEFAULT;
    good.variant = GUI_VARIANT_CARD;
    good.bg_role = GUI_COLOR_SURFACE_CONTAINER;
    good.filled = true;
    good.rounded = false;
    good.bordered = false;
    assert_eq(0, gui_widget_set_appearance(fx.platform, card, &good));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, card, &r));
    uint32_t center_x = (uint32_t)(r.x + 60);
    uint32_t center_y = (uint32_t)(r.y + 30);
    assert_eq(c->surface_container, gui_platform_get_pixel(fx.platform, center_x, center_y));

    /* 空指针与无效身份。 */
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, NULL));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(NULL, card, &good));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, GUI_WIDGET_NONE, &good));
    assert_eq(EINVAL, errno);

    /* 越界的角色、形态与令牌层级。 */
    gui_appearance_t bad = good;
    bad.bg_role = GUI_COLOR_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    bad = good;
    bad.text_role = GUI_COLOR_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    bad = good;
    bad.border_role = GUI_COLOR_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    bad = good;
    bad.variant = GUI_VARIANT_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    bad = good;
    bad.font = (gui_theme_font_t)(GUI_THEME_FONT_LABEL + 1);
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    bad = good;
    bad.radius = (gui_theme_radius_t)(GUI_THEME_RADIUS_FULL + 1);
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    bad = good;
    bad.elevation = (gui_theme_elevation_t)(GUI_THEME_ELEVATION_5 + 1);
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    /* 形态与种类不相容。 */
    bad = good;
    bad.variant = GUI_VARIANT_ACTION;
    errno = 0;
    assert_eq(-1, gui_widget_set_appearance(fx.platform, card, &bad));
    assert_eq(EINVAL, errno);

    /* 无部分修改：被拒绝的调用一个像素都没改。 */
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    assert_eq(c->surface_container, gui_platform_get_pixel(fx.platform, center_x, center_y));

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_appearance_preserves_layout_padding)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t panel;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &panel));
    gui_widget_t child;
    assert_eq(0, gui_widget_create(fx.platform, panel, GUI_WIDGET_CONTAINER, &child));

    gui_geometry_t panel_geo;
    panel_geo.x = 0;
    panel_geo.y = 0;
    panel_geo.width = GUI_SIZE_PX(200);
    panel_geo.height = GUI_SIZE_PX(120);
    assert_eq(0, gui_widget_set_geometry(fx.platform, panel, &panel_geo));

    gui_geometry_t child_geo;
    child_geo.x = 0;
    child_geo.y = 0;
    child_geo.width = GUI_SIZE_PX(40);
    child_geo.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, panel, &child_geo));
    assert_eq(0, gui_widget_set_geometry(fx.platform, child, &child_geo));

    /* 面板留 20px 内边距。 */
    gui_layout_t layout;
    layout.flow = GUI_FLOW_NONE;
    layout.main_align = GUI_FLOW_ALIGN_START;
    layout.cross_align = GUI_FLOW_ALIGN_START;
    layout.grow = 0;
    layout.gap = 0;
    layout.pad_top = 20;
    layout.pad_bottom = 20;
    layout.pad_left = 20;
    layout.pad_right = 20;
    assert_eq(0, gui_widget_set_layout(fx.platform, panel, &layout));

    gui_rect_t panel_rect;
    gui_rect_t child_rect;
    assert_eq(0, gui_widget_get_bounds(fx.platform, panel, &panel_rect));
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &child_rect));

    /* 内边距把子项顶开了。 */
    int32_t offset_x = child_rect.x - panel_rect.x;
    int32_t offset_y = child_rect.y - panel_rect.y;
    assert_true(offset_x >= 20);
    assert_true(offset_y >= 20);

    /* 换外观：外观**不改布局**，内边距仍在。 */
    gui_appearance_t appearance = GUI_APPEARANCE_DEFAULT;
    appearance.variant = GUI_VARIANT_CARD;
    appearance.bg_role = GUI_COLOR_SURFACE_CONTAINER;
    appearance.filled = true;
    appearance.rounded = true;
    /* 边框宽度保持与面板原样：普通边框会让内容区缩进 1px，那是边框的
     * 语义，不是"外观改了布局"。 */
    appearance.bordered = false;
    assert_eq(0, gui_widget_set_appearance(fx.platform, panel, &appearance));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t panel_after;
    gui_rect_t child_after;
    assert_eq(0, gui_widget_get_bounds(fx.platform, panel, &panel_after));
    assert_eq(0, gui_widget_get_bounds(fx.platform, child, &child_after));
    assert_eq(panel_rect.width, panel_after.width);
    assert_eq(panel_rect.height, panel_after.height);
    assert_eq(child_rect.x, child_after.x);
    assert_eq(child_rect.y, child_after.y);
    /* 内边距仍然在：子项依旧被顶开。 */
    assert_true(child_after.x - panel_after.x >= 20);
    assert_true(child_after.y - panel_after.y >= 20);

    gui_native_fixture_destroy(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_appearance_public_types_compile),
    TEST_ENTRY(test_all_color_roles_map_to_tokens),
    TEST_ENTRY(test_appearance_defaults_per_kind),
    TEST_ENTRY(test_card_plain_root_and_action_pixels),
    TEST_ENTRY(test_appearance_variant_kind_rules),
    TEST_ENTRY(test_material_focus_style_overrides_base_border),
    TEST_ENTRY(test_bad_appearance_is_atomic),
    TEST_ENTRY(test_appearance_preserves_layout_padding),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
