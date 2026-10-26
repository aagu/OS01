/*
 * test_desktop_shell.c — 桌面外壳像素契约测试（GUI-3 Task 4 / GUI-2b Task 10）
 *
 * 链接**真实的** user/desktop/desktop_shell.c + desktop_registry.c +
 * desktop_state.c，配真实 LVGL 与真实离屏 platform，用
 * gui_platform_get_pixel() 读回像素，验证：
 *   - 壁纸区呈现壁纸令牌色
 *   - 状态栏呈现 elevation 1 表面色
 *   - 欢迎卡片与启动器呈现 elevation 2 表面色
 *   - 切换表面后旧表面不残留（像素回到主屏采样）
 *
 * 取样坐标由 desktop_shell_probe_point() 给出：它与外壳构建共用同一套
 * 布局常量，因此取样点始终落在目标区域内；被验证的是「该区域呈现的
 * 是正确的令牌色」，这才是设计系统的契约。
 *
 * GUI-2b：按键路径走**真实**的合成输入（解码器 → FIFO → step），不再
 * 手调桌面键函数假装完整集成；焦点断言走 libgui 的唯一焦点引擎
 * （gui_focus_get），桌面不再有第二套焦点序列。
 */

#include "test_framework.h"
#include <gui/gui_focus.h>
#include <gui/gui_layout.h>
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <gui/gui_widget.h>
#include "desktop_registry.h"
#include "desktop_shell.h"
#include "desktop_state.h"
#include "gui_native_fixture.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* libgui/src/platform.c 在 OS01_HOST_TEST 下引用的注入开关。 */
bool g_gui_test_tamper_header = false;

#define TEST_W 640
#define TEST_H 480

/* Set 1 扫描码 */
#define KEY_ESC_DOWN    0x01u
#define KEY_ESC_UP      0x81u
#define KEY_TAB_DOWN    0x0Fu
#define KEY_TAB_UP      0x8Fu
#define KEY_LSHIFT_DOWN 0x2Au
#define KEY_LSHIFT_UP   0xAAu
#define KEY_A_DOWN      0x1Eu
#define KEY_A_UP        0x9Eu

/* ── fixture 辅助 ─────────────────────────────────────────────────── */

/*
 * 渲染一帧。
 *
 * 必须先推进时钟：LVGL 的刷新定时器周期是 33ms，不推进时间的话第二次
 * step() 里定时器还没到期，屏幕不会重绘，断言就会读到上一帧的旧像素。
 */
static void render(gui_native_fixture_t *fx)
{
    assert_eq(0, gui_native_fixture_step(fx, 50));
}

/* 桌面的原生按键回调：与 user/desktop/desktop.c 里的 on_native_key 同形。 */
static bool on_native_key(const gui_key_event_t *ev, void *userdata)
{
    return desktop_shell_handle_key((desktop_shell_t *)userdata, ev);
}

static void attach_shell(gui_native_fixture_t *fx, desktop_shell_t *shell)
{
    assert_not_null(shell);
    gui_input_set_key_handler(fx->input, on_native_key, shell);
}

/* 注入一次完整的按下 / 抬起。 */
static void press_key(gui_native_fixture_t *fx, uint8_t down, uint8_t up)
{
    assert_eq(0, gui_native_fixture_keys(fx, &down, 1));
    assert_eq(0, gui_native_fixture_step(fx, 20));
    assert_eq(0, gui_native_fixture_keys(fx, &up, 1));
    assert_eq(0, gui_native_fixture_step(fx, 20));
}

/* 注入一次 Shift+Tab（组合键必须是真的按下 Shift 再按 Tab）。 */
static void press_shift_tab(gui_native_fixture_t *fx)
{
    assert_eq(0, gui_native_fixture_keys(fx, (const uint8_t[]){ KEY_LSHIFT_DOWN }, 1));
    assert_eq(0, gui_native_fixture_step(fx, 20));
    assert_eq(0, gui_native_fixture_keys(fx, (const uint8_t[]){ KEY_TAB_DOWN }, 1));
    assert_eq(0, gui_native_fixture_step(fx, 20));
    assert_eq(0, gui_native_fixture_keys(fx, (const uint8_t[]){ KEY_TAB_UP }, 1));
    assert_eq(0, gui_native_fixture_step(fx, 20));
    assert_eq(0, gui_native_fixture_keys(fx, (const uint8_t[]){ KEY_LSHIFT_UP }, 1));
    assert_eq(0, gui_native_fixture_step(fx, 20));
}

/* ── 断言：主屏呈现各自的 elevation ─────────────────────────────── */

TEST_FUNC(test_home_surface_matches_the_token_table)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);
    render(&fx);

    uint32_t x = 0;
    uint32_t y = 0;

    /* 壁纸区：壁纸色由根容器自己绘制。 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_WALLPAPER, &x, &y));
    assert_eq(gui_theme_colors()->surface_dim, gui_platform_get_pixel(fx.platform, x, y));

    /* 状态栏：elevation 1。 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_STATUS_BAR, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_1),
              gui_platform_get_pixel(fx.platform, x, y));

    /* 欢迎卡片：elevation 2。 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_CARD, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2),
              gui_platform_get_pixel(fx.platform, x, y));

    /* 启动器卡片：elevation 2。 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_LAUNCHER, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2),
              gui_platform_get_pixel(fx.platform, x, y));

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：迁移后旧表面不残留 ───────────────────────────────────── */

TEST_FUNC(test_surface_transition_leaves_no_residue)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);
    render(&fx);

    uint32_t x = 0;
    uint32_t y = 0;
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_CARD, &x, &y));
    uint32_t before = gui_platform_get_pixel(fx.platform, x, y);

    /* 启动一个应用：旧表面的控件树被销毁；回到主屏后像素必须恢复原值。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "files");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    render(&fx);
    assert_eq(before, gui_platform_get_pixel(fx.platform, x, y));

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：应用不允许叠加 ───────────────────────────────────────── */

TEST_FUNC(test_app_cannot_stack_another_app)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);

    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "files");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    /* 已经在一个应用里，再开一个应用是非法迁移：状态不变。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：注册表与启动器子集 ───────────────────────────────────── */

TEST_FUNC(test_registry_and_launcher_subset)
{
    assert_eq(4u, desktop_app_count());
    assert_eq(3u, desktop_app_launcher_count());

    /* 启动器里的每个应用都必须能在注册表里找到，且标记一致。 */
    for (uint32_t i = 0; i < desktop_app_launcher_count(); i++) {
        const desktop_app_info_t *info = desktop_app_launcher_entry(i);
        assert_not_null(info);
        assert_true(info->in_launcher);
        assert_true(desktop_app_info(info->id) == info);
    }
    assert_null(desktop_app_launcher_entry(desktop_app_launcher_count()));

    /* 强调色必须来自令牌表，不得是自造颜色。 */
    const gui_theme_colors_t *c = gui_theme_colors();
    assert_eq(c->primary, desktop_app_accent_color(DESKTOP_APP_FILES));
    assert_eq(c->secondary, desktop_app_accent_color(DESKTOP_APP_TERMINAL));
    assert_eq(c->tertiary, desktop_app_accent_color(DESKTOP_APP_SYSTEM));
    assert_eq(c->success, desktop_app_accent_color(DESKTOP_APP_SETTINGS));

    /*
     * 强调色**角色**必须与强调色一一对应：桌面只用公共 API 表达外观，
     * 公共 API 接受的是角色而不是颜色值。
     */
    assert_eq(GUI_COLOR_PRIMARY, desktop_app_accent_role(DESKTOP_APP_FILES));
    assert_eq(GUI_COLOR_SECONDARY, desktop_app_accent_role(DESKTOP_APP_TERMINAL));
    assert_eq(GUI_COLOR_TERTIARY, desktop_app_accent_role(DESKTOP_APP_SYSTEM));
    assert_eq(GUI_COLOR_SUCCESS, desktop_app_accent_role(DESKTOP_APP_SETTINGS));
    assert_eq(GUI_COLOR_COUNT, desktop_app_accent_role(DESKTOP_APP_COUNT));

    /* 标识串往返一致。 */
    for (uint32_t i = 0; i < desktop_app_count(); i++) {
        desktop_app_id_t id = (desktop_app_id_t)i;
        desktop_app_id_t back = DESKTOP_APP_COUNT;
        assert_true(desktop_app_id_from_name(desktop_app_id_name(id), &back));
        assert_eq(id, back);
    }
    desktop_app_id_t dummy = DESKTOP_APP_COUNT;
    assert_false(desktop_app_id_from_name("nope", &dummy));
    assert_false(desktop_app_id_from_name(NULL, &dummy));
}

/* ── 断言：字号层级与 libgui 的令牌一致 ─────────────────────────── */

TEST_FUNC(test_shell_font_mapping_matches_the_token_table)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t label;
    const gui_theme_font_t roles[] = {
        GUI_THEME_FONT_DISPLAY,
        GUI_THEME_FONT_HEADLINE,
        GUI_THEME_FONT_TITLE,
        GUI_THEME_FONT_BODY,
        GUI_THEME_FONT_LABEL,
    };
    int previous = 0;

    assert_eq(0, gui_native_fixture_create(&fx, true));

    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE,
                                   GUI_WIDGET_CONTAINER, &root));
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    /*
     * 桌面不再有自己的字号映射：它只把**主题层级**交给公共 API。因此这条
     * 契约验的是「公共 API 落地的字体与令牌表给的是同一个字号」——
     * 比旧的映射表断言更强：它读的是真实后端对象上生效的字体。
     */
    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        gui_appearance_t appearance = GUI_APPEARANCE_DEFAULT;
        int line_height = 0;

        /* 标签只能用 PLAIN 形态。 */
        appearance.variant = GUI_VARIANT_PLAIN;
        appearance.filled = false;
        appearance.font = roles[i];
        assert_eq(0, gui_widget_set_appearance(fx.platform, label, &appearance));
        assert_eq(0, gui_native_fixture_query_font_line_height(&fx, label,
                                                               &line_height));
        /* LVGL 的 line height 比字号略大，用它做一致性判定。 */
        assert_true(line_height >= gui_theme_font_size(roles[i]));
        assert_true(line_height <= gui_theme_font_size(roles[i]) + 8);
        /* 层级必须严格递减：层级表与字号表一一对应。 */
        if (i > 0) {
            assert_true(line_height < previous);
        }
        previous = line_height;
    }

    assert_eq(0, gui_widget_destroy(fx.platform, root));
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：抽屉与设置表面呈现各自的 elevation ────────────────────── */

TEST_FUNC(test_drawer_and_settings_surfaces)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);
    render(&fx);

    uint32_t x = 0;
    uint32_t y = 0;

    /* 抽屉：面板是 elevation 3（浮层）。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_DRAWER, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_DRAWER, desktop_shell_state(shell));
    render(&fx);
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_DRAWER, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_3),
              gui_platform_get_pixel(fx.platform, x, y));

    /* 抽屉 → 主屏。 */
    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    /* 设置：面板是 elevation 2（卡片层）。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_SETTINGS, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_SETTINGS, desktop_shell_state(shell));
    render(&fx);
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_SETTINGS, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2),
              gui_platform_get_pixel(fx.platform, x, y));

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：设置项变更立即生效并重建表面 ────────────────────────── */

TEST_FUNC(test_setting_change_rebuilds_the_surface)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);
    render(&fx);

    desktop_wallpaper_t wallpaper = DESKTOP_WALLPAPER_SURFACE_DIM;
    desktop_density_t density = DESKTOP_DENSITY_COMFORTABLE;
    bool clock_24h = false;

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_SURFACE_DIM, wallpaper);
    assert_eq(DESKTOP_DENSITY_COMFORTABLE, density);
    assert_true(clock_24h);

    uint32_t x = 0;
    uint32_t y = 0;
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_WALLPAPER, &x, &y));
    assert_eq(gui_theme_colors()->surface_dim, gui_platform_get_pixel(fx.platform, x, y));

    /* 换壁纸：变更在主循环里执行，并立即重建当前表面。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_SURFACE, wallpaper);
    render(&fx);
    assert_eq(gui_theme_colors()->surface, gui_platform_get_pixel(fx.platform, x, y));

    /* 再换一次：走到 container_low。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_LOW, wallpaper);
    render(&fx);
    assert_eq(gui_theme_colors()->surface_container_low,
              gui_platform_get_pixel(fx.platform, x, y));

    /* 再换一次：走到 container_high。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_HIGH, wallpaper);
    render(&fx);
    assert_eq(gui_theme_colors()->surface_container_high,
              gui_platform_get_pixel(fx.platform, x, y));

    /* 再换一次：走到 container_highest —— 五种壁纸走完一圈。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_HIGHEST, wallpaper);
    render(&fx);
    assert_eq(gui_theme_colors()->surface_container_highest,
              gui_platform_get_pixel(fx.platform, x, y));

    /* 换时钟格式：只改设置，不改壁纸。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_CLOCK_FORMAT);
    desktop_shell_apply_pending(shell);
    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_false(clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_HIGHEST, wallpaper);

    /* 换密度。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_DENSITY);
    desktop_shell_apply_pending(shell);
    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_DENSITY_COMPACT, density);

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：四个内置应用都能启动、占满显示区、能退出 ─────────────── */

TEST_FUNC(test_every_builtin_app_starts_fullscreen_and_exits)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);
    render(&fx);

    /* 主屏的初始采样：退出应用后必须回到它。 */
    uint32_t x = 0;
    uint32_t y = 0;
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_CARD, &x, &y));
    uint32_t home_pixel = gui_platform_get_pixel(fx.platform, x, y);

    for (uint32_t i = 0; i < desktop_app_count(); i++) {
        desktop_app_id_t id = (desktop_app_id_t)i;
        const char *name = desktop_app_id_name(id);
        gui_widget_t root;
        gui_rect_t rect;
        assert_not_null(name);

        desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, name);
        desktop_shell_apply_pending(shell);

        /* ① 状态变为 APP */
        assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));
        assert_eq(id, desktop_shell_current_app_id(shell));

        /* ② 应用表面根控件非空 */
        root = desktop_shell_surface_root(shell);
        assert_true(root.id != 0);

        /* 布局要先跑一遍，尺寸才落在对象上。 */
        render(&fx);

        /* ③ 根控件覆盖整个显示区：应用表面恒为全屏 */
        assert_eq(0, gui_widget_get_bounds(fx.platform, root, &rect));
        assert_eq((int32_t)TEST_W, rect.width);
        assert_eq((int32_t)TEST_H, rect.height);
        assert_eq(0, rect.x);
        assert_eq(0, rect.y);

        /* ④ 退出后回到主屏，主屏采样回到初始值 */
        desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
        desktop_shell_apply_pending(shell);
        assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

        render(&fx);
        assert_eq(home_pixel, gui_platform_get_pixel(fx.platform, x, y));
    }

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：终端消费键入字符（走真实的合成输入路径） ─────────────── */

TEST_FUNC(test_terminal_consumes_typed_characters)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;
    desktop_app_state_t *state;
    char before[DESKTOP_TERM_LINE_MAX + 32];
    size_t before_len;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);

    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    state = desktop_shell_app_state(shell);
    assert_not_null(state);
    assert_true(state->term_input_label.id != 0);

    /* 刷新一轮，输入行先带上提示符。 */
    desktop_shell_tick(shell);

    snprintf(before, sizeof(before), "%s", state->term_input_shown);
    before_len = strlen(before);
    assert_true(before_len > 0);

    /* 键入 'a'：真实的解码器 → FIFO → step 路径。 */
    assert_eq(0, gui_native_fixture_keys(&fx, (const uint8_t[]){ KEY_A_DOWN }, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 20));

    /* 按键回调只改状态；界面由主循环刷新。 */
    desktop_shell_tick(shell);

    assert_eq(before_len + 1, strlen(state->term_input_shown));
    assert_true(strncmp(before, state->term_input_shown, before_len) == 0);

    /* 释放：字符不会重复插入。 */
    assert_eq(0, gui_native_fixture_keys(&fx, (const uint8_t[]){ KEY_A_UP }, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 20));
    desktop_shell_tick(shell);
    assert_eq(before_len + 1, strlen(state->term_input_shown));

    /* 终端之外的应用不消费字符键。 */
    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));
    assert_false(desktop_shell_handle_key(
        shell, &(gui_key_event_t){ .physical_key = KEY_A_DOWN,
                                   .codepoint = (uint32_t)'a',
                                   .action = GUI_KEY_DOWN }));

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：终端退出后焦点仍能导航到启动器第一个图标 ─────────────── */

TEST_FUNC(test_focus_survives_terminal_exit)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;
    gui_widget_t got;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);

    /* 启动终端并消费一个字符。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    assert_eq(0, gui_native_fixture_keys(&fx, (const uint8_t[]){ KEY_A_DOWN }, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 20));

    /* 退出应用回到主屏。 */
    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    /* 焦点序列是主屏的启动器图标，没有残留已销毁对象。 */
    {
        gui_widget_t order[64];
        int count = gui_native_fixture_query_focus_order(&fx, order, 64);
        assert_eq((int)desktop_app_launcher_count(), count);
    }

    /* Tab 落到第一个启动器图标上。 */
    press_key(&fx, KEY_TAB_DOWN, KEY_TAB_UP);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_true(got.id != 0);

    /* 再按一次 Tab 前进一项，Shift+Tab 退回来。 */
    {
        gui_widget_t first = got;
        press_key(&fx, KEY_TAB_DOWN, KEY_TAB_UP);
        assert_eq(0, gui_focus_get(fx.input, &got));
        assert_true(got.id != first.id);

        press_shift_tab(&fx);
        assert_eq(0, gui_focus_get(fx.input, &got));
        assert_eq(first.id, got.id);
    }

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：外壳必须拿到有效且关联的输入 ─────────────────────────── */

TEST_FUNC(test_shell_requires_matching_input)
{
    gui_native_fixture_t fx;
    gui_input_t *foreign = NULL;

    assert_eq(0, gui_native_fixture_create(&fx, true));

    /* 平台缺失。 */
    errno = 0;
    assert_null(desktop_shell_create(NULL, fx.input));
    assert_eq(EINVAL, errno);

    /* 输入缺失。 */
    errno = 0;
    assert_null(desktop_shell_create(fx.platform, NULL));
    assert_eq(EINVAL, errno);

    /* 输入与平台不匹配：焦点登记会被判成 EINVAL，主屏建不起来。 */
    assert_eq(0, gui_native_fixture_create_foreign_input(&foreign));
    assert_not_null(foreign);
    errno = 0;
    assert_null(desktop_shell_create(fx.platform, foreign));
    assert_eq(EINVAL, errno);
    gui_native_fixture_destroy_foreign_input(foreign);

    /* 有效且关联：创建成功。 */
    {
        desktop_shell_t *shell = desktop_shell_create(fx.platform, fx.input);
        assert_not_null(shell);
        desktop_shell_destroy(shell);
    }

    gui_native_fixture_destroy(&fx);
}

/* ── 断言：时钟节流不影响终端的每轮刷新 ─────────────────────────── */

TEST_FUNC(test_terminal_refreshes_within_clock_throttle)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;
    desktop_app_state_t *state;
    char before[DESKTOP_TERM_OUTPUT_MAX];

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);

    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    state = desktop_shell_app_state(shell);
    assert_not_null(state);

    /* 先跑一轮：输入与输出都被提交到控件。 */
    desktop_shell_tick(shell);
    snprintf(before, sizeof(before), "%s", state->term_output_shown);
    assert_true(strlen(before) > 0);

    /*
     * 键入一个字符并回车：时钟节流窗口内（同一秒）终端也必须刷新。
     */
    assert_eq(0, gui_native_fixture_keys(&fx, (const uint8_t[]){ KEY_A_DOWN }, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 20));
    assert_eq(0, gui_native_fixture_keys(&fx, (const uint8_t[]){ KEY_A_UP }, 1));
    assert_eq(0, gui_native_fixture_step(&fx, 20));
    desktop_shell_tick(shell);
    assert_true(strlen(state->term_input_shown) > strlen("OS01 $ "));

    /* 回车：命令被执行，输出区在**同一秒**内必须刷新。 */
    {
        const uint8_t enter_down[] = { 0x1Cu };
        const uint8_t enter_up[] = { 0x9Cu };

        assert_eq(0, gui_native_fixture_keys(&fx, enter_down, 1));
        assert_eq(0, gui_native_fixture_step(&fx, 20));
        assert_eq(0, gui_native_fixture_keys(&fx, enter_up, 1));
        assert_eq(0, gui_native_fixture_step(&fx, 20));
    }
    desktop_shell_tick(shell);

    /* 输出变了：命令回显进了输出区。 */
    assert_true(strlen(state->term_output_shown) > strlen(before));
    assert_true(strstr(state->term_output_shown, "command not found") != NULL);

    /* 输入行被清空（提示符还在）。 */
    assert_str_eq("OS01 $ ", state->term_input_shown);

    /* ESC 回到主屏，终端的输出状态保留。 */
    press_key(&fx, KEY_ESC_DOWN, KEY_ESC_UP);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));
    desktop_shell_tick(shell);
    assert_true(strstr(state->term_output_shown, "command not found") != NULL);

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

/* ── 断言：重建后的表面没有焦点，第一次 Tab 才落到首项 ───────────── */

TEST_FUNC(test_rebuilt_surface_starts_without_focus)
{
    gui_native_fixture_t fx;
    desktop_shell_t *shell;
    gui_widget_t got;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    shell = desktop_shell_create(fx.platform, fx.input);
    assert_not_null(shell);
    attach_shell(&fx, shell);

    /* 主屏建好之后没有焦点（初始策略是 MANUAL）。 */
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    /* 第一次正 Tab 命中主屏的第一个可聚焦项。 */
    press_key(&fx, KEY_TAB_DOWN, KEY_TAB_UP);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_true(got.id != 0);
    {
        gui_widget_t first = got;

        /* 第一次反 Tab 命中最后一个可聚焦项。 */
        press_shift_tab(&fx);
        assert_eq(0, gui_focus_get(fx.input, &got));
        assert_true(got.id != first.id);

        press_key(&fx, KEY_TAB_DOWN, KEY_TAB_UP);
        assert_eq(0, gui_focus_get(fx.input, &got));
        assert_eq(first.id, got.id);
    }

    /* 整表面切换：显式清空跨新登记保留，重建后的主屏**没有**焦点。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_DRAWER, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_DRAWER, desktop_shell_state(shell));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    /* 重建后的主屏：正 Tab 命中首项，反 Tab 命中末项。 */
    press_key(&fx, KEY_TAB_DOWN, KEY_TAB_UP);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_true(got.id != 0);
    {
        gui_widget_t first = got;

        press_shift_tab(&fx);
        assert_eq(0, gui_focus_get(fx.input, &got));
        assert_true(got.id != first.id);
    }

    desktop_shell_destroy(shell);
    gui_native_fixture_destroy(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_home_surface_matches_the_token_table),
    TEST_ENTRY(test_surface_transition_leaves_no_residue),
    TEST_ENTRY(test_app_cannot_stack_another_app),
    TEST_ENTRY(test_registry_and_launcher_subset),
    TEST_ENTRY(test_shell_font_mapping_matches_the_token_table),
    TEST_ENTRY(test_drawer_and_settings_surfaces),
    TEST_ENTRY(test_setting_change_rebuilds_the_surface),
    TEST_ENTRY(test_every_builtin_app_starts_fullscreen_and_exits),
    TEST_ENTRY(test_terminal_consumes_typed_characters),
    TEST_ENTRY(test_focus_survives_terminal_exit),
    TEST_ENTRY(test_shell_requires_matching_input),
    TEST_ENTRY(test_terminal_refreshes_within_clock_throttle),
    TEST_ENTRY(test_rebuilt_surface_starts_without_focus),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
