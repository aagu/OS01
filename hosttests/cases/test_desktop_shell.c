/*
 * test_desktop_shell.c — 桌面外壳像素契约测试（GUI-3 Task 4）
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
 */

#include "test_framework.h"
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <gui_lvgl_private.h>
#include "desktop_registry.h"
#include "desktop_shell.h"
#include "desktop_state.h"
#include "gui_test_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* libgui/src/platform.c 在 OS01_HOST_TEST 下引用的注入开关。 */
bool g_gui_test_tamper_header = false;

#define TEST_W 640
#define TEST_H 480

static gui_platform_t *make_platform(void)
{
    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = TEST_W,
        .height = TEST_H,
        .buffer_lines = 8
    };
    return gui_platform_create(&cfg);
}

/*
 * 渲染一帧。
 *
 * 必须先推进时钟：LVGL 的刷新定时器周期是 33ms，不推进时间的话第二次
 * step() 里定时器还没到期，屏幕不会重绘，断言就会读到上一帧的旧像素。
 */
static void render(gui_platform_t *p)
{
    gui_test_set_now(gui_test_get_now() + 50);
    uint32_t delay = 0;
    gui_status_t status = gui_platform_step(p, &delay);
    assert_eq(GUI_STATUS_OK, status);
}

/* ── 断言：主屏四个区域的颜色都来自令牌表 ───────────────────────── */

TEST_FUNC(test_home_surface_matches_the_token_table)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    render(p);

    const gui_theme_colors_t *c = gui_theme_colors();
    uint32_t x = 0;
    uint32_t y = 0;

    /* 壁纸区 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_WALLPAPER, &x, &y));
    assert_eq(c->surface_dim, gui_platform_get_pixel(p, x, y));

    /* 状态栏区 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_STATUS_BAR, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_1),
              gui_platform_get_pixel(p, x, y));

    /* 欢迎卡片区 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_CARD, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2),
              gui_platform_get_pixel(p, x, y));

    /* 启动器卡片区 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_LAUNCHER, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2),
              gui_platform_get_pixel(p, x, y));

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
}

/* ── 断言：迁移后旧表面不残留 ───────────────────────────────────── */

TEST_FUNC(test_surface_transition_leaves_no_residue)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);
    render(p);

    uint32_t x = 0;
    uint32_t y = 0;
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_CARD, &x, &y));
    uint32_t before = gui_platform_get_pixel(p, x, y);

    /* 启动一个应用：应用表面尚未实现（Task 6），此时状态机判定迁移合法，
     * 外壳会销毁主屏控件树；回到主屏后像素必须恢复原值。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "files");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    render(p);
    assert_eq(before, gui_platform_get_pixel(p, x, y));

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
}

/* ── 断言：应用不允许叠加 ───────────────────────────────────────── */

TEST_FUNC(test_app_cannot_stack_another_app)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);

    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "files");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    /* 已经在一个应用里，再开一个应用是非法迁移：状态不变。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
}

/* ── 断言：注册表与启动器子集 ───────────────────────────────────── */

TEST_FUNC(test_registry_and_launcher_subset)
{
    gui_test_reset();

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

/* ── 断言：外壳的字号映射与 libgui 的令牌一致 ───────────────────── */

TEST_FUNC(test_shell_font_mapping_matches_the_token_table)
{
    gui_test_reset();

    const gui_theme_font_t roles[] = {
        GUI_THEME_FONT_DISPLAY,
        GUI_THEME_FONT_HEADLINE,
        GUI_THEME_FONT_TITLE,
        GUI_THEME_FONT_BODY,
        GUI_THEME_FONT_LABEL,
    };

    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); i++) {
        const lv_font_t *font = desktop_shell_font(roles[i]);
        assert_not_null(font);
        /* LVGL 的 line height 比字号略大，用它做一致性判定：
         * 外壳的映射必须与令牌表给的是同一个字号。 */
        assert_true((int)font->line_height >= gui_theme_font_size(roles[i]));
        assert_true((int)font->line_height <= gui_theme_font_size(roles[i]) + 8);
    }
}


/* ── 断言：抽屉与设置表面呈现各自的 elevation ────────────────────── */

TEST_FUNC(test_drawer_and_settings_surfaces)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);
    render(p);

    uint32_t x = 0;
    uint32_t y = 0;

    /* 抽屉：面板是 elevation 3（浮层）。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_DRAWER, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_DRAWER, desktop_shell_state(shell));
    render(p);
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_DRAWER, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_3),
              gui_platform_get_pixel(p, x, y));

    /* 抽屉 → 主屏。 */
    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    /* 设置：面板是 elevation 2（卡片层）。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_SETTINGS, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_SETTINGS, desktop_shell_state(shell));
    render(p);
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_SETTINGS, &x, &y));
    assert_eq(gui_theme_elevation_surface(GUI_THEME_ELEVATION_2),
              gui_platform_get_pixel(p, x, y));

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
}

/* ── 断言：设置项变更立即生效并重建表面 ────────────────────────── */

TEST_FUNC(test_setting_change_rebuilds_the_surface)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    lv_display_t *disp = gui_platform_get_lv_display(p);
    assert_not_null(disp);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);
    render(p);

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
    assert_eq(gui_theme_colors()->surface_dim, gui_platform_get_pixel(p, x, y));

    /* 换壁纸：变更在主循环里执行，并立即重建当前表面。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_SURFACE, wallpaper);
    render(p);
    assert_eq(gui_theme_colors()->surface, gui_platform_get_pixel(p, x, y));

    /* 再换一次：走到 container_low。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_WALLPAPER);
    desktop_shell_apply_pending(shell);

    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_LOW, wallpaper);
    render(p);
    assert_eq(gui_theme_colors()->surface_container_low,
              gui_platform_get_pixel(p, x, y));

    /* 换时钟格式：只改设置，不改壁纸。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_CLOCK_FORMAT);
    desktop_shell_apply_pending(shell);
    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_false(clock_24h);
    assert_eq(DESKTOP_WALLPAPER_CONTAINER_LOW, wallpaper);

    /* 换密度。 */
    desktop_shell_request_setting(shell, DESKTOP_SETTING_DENSITY);
    desktop_shell_apply_pending(shell);
    desktop_shell_get_settings(shell, &wallpaper, &density, &clock_24h);
    assert_eq(DESKTOP_DENSITY_COMPACT, density);

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
}

/* ── 断言：四个内置应用都能启动、占满显示区、能退出 ─────────────── */

TEST_FUNC(test_every_builtin_app_starts_fullscreen_and_exits)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);
    render(p);

    /* 主屏的初始采样：退出应用后必须回到它。 */
    uint32_t x = 0;
    uint32_t y = 0;
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_CARD, &x, &y));
    uint32_t home_pixel = gui_platform_get_pixel(p, x, y);

    for (uint32_t i = 0; i < desktop_app_count(); i++) {
        desktop_app_id_t id = (desktop_app_id_t)i;
        const char *name = desktop_app_id_name(id);
        assert_not_null(name);

        desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, name);
        desktop_shell_apply_pending(shell);

        /* ① 状态变为 APP */
        assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));
        assert_eq(id, desktop_shell_current_app_id(shell));

        /* ② 应用表面根对象非空 */
        lv_obj_t *root = desktop_shell_surface_root(shell);
        assert_not_null(root);

        /* 布局要先跑一遍，尺寸才落在对象上。 */
        render(p);

        /* ③ 根对象覆盖整个显示区：应用表面恒为全屏 */
        assert_eq((int32_t)TEST_W, lv_obj_get_width(root));
        assert_eq((int32_t)TEST_H, lv_obj_get_height(root));

        /* ④ 退出后回到主屏，主屏采样回到初始值 */
        desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
        desktop_shell_apply_pending(shell);
        assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

        render(p);
        assert_eq(home_pixel, gui_platform_get_pixel(p, x, y));
    }

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
}

/* ── 断言：终端消费键入字符 ──────────────────────────────────────── */

TEST_FUNC(test_terminal_consumes_typed_characters)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);

    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    desktop_app_state_t *state = desktop_shell_app_state(shell);
    assert_not_null(state);
    assert_not_null(state->term_input_label);

    /* 刷新一轮，输入行先带上提示符。 */
    desktop_shell_tick(shell);

    const char *before = lv_label_get_text(state->term_input_label);
    assert_not_null(before);
    size_t before_len = strlen(before);

    gui_key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.physical_key = 0x1E;        /* Set 1 扫描码：'a' */
    ev.codepoint = (uint32_t)'a';
    ev.action = GUI_KEY_DOWN;
    assert_true(desktop_shell_handle_key(shell, &ev));

    /* 按键回调只改状态；界面由主循环刷新。 */
    desktop_shell_tick(shell);

    const char *after = lv_label_get_text(state->term_input_label);
    assert_not_null(after);
    assert_eq(before_len + 1, strlen(after));
    assert_true(strncmp(before, after, before_len) == 0);

    /* 终端之外的应用不消费字符键。 */
    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));
    assert_false(desktop_shell_handle_key(shell, &ev));

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
}

/* ── 断言：终端退出后焦点仍能导航到启动器第一个图标 ─────────────── */

TEST_FUNC(test_focus_survives_terminal_exit)
{
    gui_test_reset();

    gui_platform_t *p = make_platform();
    assert_not_null(p);

    desktop_shell_t *shell = desktop_shell_create(p);
    assert_not_null(shell);

    /* 启动终端并消费一个字符。 */
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, "terminal");
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_APP, desktop_shell_state(shell));

    gui_key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.physical_key = 0x1E;
    ev.codepoint = (uint32_t)'a';
    ev.action = GUI_KEY_DOWN;
    assert_true(desktop_shell_handle_key(shell, &ev));

    /* 退出应用回到主屏。 */
    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
    desktop_shell_apply_pending(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    /* 焦点序列是主屏的启动器图标，没有残留已销毁对象。 */
    assert_eq(desktop_app_launcher_count(), desktop_shell_focus_count(shell));

    /* Tab 落到第一个启动器图标上。 */
    gui_key_event_t tab;
    memset(&tab, 0, sizeof(tab));
    tab.physical_key = 0x0F;        /* Tab */
    tab.action = GUI_KEY_DOWN;
    assert_true(desktop_shell_handle_key(shell, &tab));
    assert_eq(0, desktop_shell_focus_index(shell));

    /* 再按一次 Tab 前进一项，Shift+Tab 退回来。 */
    assert_true(desktop_shell_handle_key(shell, &tab));
    assert_eq(1, desktop_shell_focus_index(shell));

    tab.modifiers = GUI_MOD_LSHIFT;
    assert_true(desktop_shell_handle_key(shell, &tab));
    assert_eq(0, desktop_shell_focus_index(shell));

    desktop_shell_destroy(shell);
    gui_platform_destroy(p);
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
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
