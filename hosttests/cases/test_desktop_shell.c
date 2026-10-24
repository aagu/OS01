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

static void render(gui_platform_t *p)
{
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

    desktop_shell_t *shell = desktop_shell_create(disp);
    assert_not_null(shell);
    assert_eq(DESKTOP_STATE_HOME, desktop_shell_state(shell));

    render(p);

    const gui_theme_colors_t *c = gui_theme_colors();
    uint32_t x = 0;
    uint32_t y = 0;

    /* 壁纸区 */
    assert_true(desktop_shell_probe_point(shell, DESKTOP_REGION_WALLPAPER, &x, &y));
    assert_eq(c->surface, gui_platform_get_pixel(p, x, y));

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

    desktop_shell_t *shell = desktop_shell_create(disp);
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

    desktop_shell_t *shell = desktop_shell_create(disp);
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

TEST_LIST_BEGIN
    TEST_ENTRY(test_home_surface_matches_the_token_table),
    TEST_ENTRY(test_surface_transition_leaves_no_residue),
    TEST_ENTRY(test_app_cannot_stack_another_app),
    TEST_ENTRY(test_registry_and_launcher_subset),
    TEST_ENTRY(test_shell_font_mapping_matches_the_token_table),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
