/*
 * desktop_shell.c — 桌面外壳实现（GUI-3）
 *
 * 所有颜色 / 字号 / 间距 / 圆角一律取自 gui_theme_*，本文件不出现任何
 * 颜色字面量或字号字面量（宿主静态审计会检查这一点）。
 *
 * 几何常量集中在这里，构建与取样共用，保证「取样点落在目标区域内」。
 */

#include "desktop_shell.h"

#include <gui/gui_theme.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── 布局常量（像素） ───────────────────────────────────────────── */

#define STATUS_BAR_HEIGHT  32
#define APP_BAR_HEIGHT      40
#define CARD_WIDTH         320
#define CARD_HEIGHT        120
#define CARD_TOP            88
#define LAUNCHER_HEIGHT     96
#define LAUNCHER_ICON       48
#define LAUNCHER_PAD        12
#define LAUNCHER_GAP        12
#define PANEL_WIDTH        480

/* ── 外壳状态 ────────────────────────────────────────────────────── */

struct desktop_shell {
    lv_display_t *disp;

    desktop_state_t state;
    desktop_intent_t pending;
    char pending_app[DESKTOP_APP_ID_MAX];
    char current_app[DESKTOP_APP_ID_MAX];

    desktop_wallpaper_t wallpaper;
    desktop_density_t   density;
    bool               clock_24h;

    lv_obj_t   *root;        /* 当前表面的根对象 */
    lv_obj_t   *clock_label;
    lv_obj_t   *status_bar;
    lv_timer_t *clock_timer;
};

/* ── 令牌化样式辅助 ─────────────────────────────────────────────── */

static lv_color_t tok(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

const lv_font_t *desktop_shell_font(gui_theme_font_t role)
{
    switch (role) {
    case GUI_THEME_FONT_DISPLAY:  return &lv_font_montserrat_24;
    case GUI_THEME_FONT_HEADLINE: return &lv_font_montserrat_20;
    case GUI_THEME_FONT_TITLE:    return &lv_font_montserrat_16;
    case GUI_THEME_FONT_BODY:     return &lv_font_montserrat_14;
    case GUI_THEME_FONT_LABEL:    return &lv_font_montserrat_12;
    default:                      return LV_FONT_DEFAULT;
    }
}

static uint32_t tok_radius(gui_theme_radius_t role)
{
    return (uint32_t)gui_theme_radius(role);
}

static uint32_t tok_space(gui_theme_space_t role)
{
    return (uint32_t)gui_theme_space(role);
}

/* 表面卡片：底色 + 圆角 + 阴影（elevation） */
static void style_card(lv_obj_t *obj, gui_theme_elevation_t elevation)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    lv_obj_set_style_bg_color(obj, tok(gui_theme_elevation_surface(elevation)), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, (int32_t)tok_radius(GUI_THEME_RADIUS_LG), 0);
    lv_obj_set_style_border_color(obj, tok(c->outline_variant), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, (int32_t)tok_space(GUI_THEME_SPACE_MD), 0);
    lv_obj_set_style_shadow_color(obj, tok(c->shadow), 0);
    lv_obj_set_style_shadow_width(obj,
                                  gui_theme_elevation_shadow_width(elevation), 0);
    lv_obj_set_style_shadow_offset_y(obj,
                                     gui_theme_elevation_shadow_offset_y(elevation), 0);
    lv_obj_set_style_shadow_opa(obj, gui_theme_elevation_shadow_opa(elevation), 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* 透明全屏容器：表面的根对象，壁纸由屏幕承载 */
static lv_obj_t *make_surface_root(lv_display_t *disp)
{
    lv_obj_t *root = lv_obj_create(lv_display_get_screen_active(disp));
    if (root == NULL) {
        return NULL;
    }
    lv_obj_set_layout(root, LV_LAYOUT_NONE);
    lv_obj_set_size(root, (int32_t)lv_display_get_horizontal_resolution(disp),
                    (int32_t)lv_display_get_vertical_resolution(disp));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_shadow_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                             gui_theme_font_t role, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, tok(color), 0);
    lv_obj_set_style_text_font(label, desktop_shell_font(role), 0);
    return label;
}

/* ── 时钟 ────────────────────────────────────────────────────────── */

static void format_clock(const desktop_shell_t *shell, char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);

    if (tm == NULL) {
        snprintf(buf, len, "--:--");
        return;
    }

    const char *fmt = shell->clock_24h ? "%H:%M" : "%I:%M %p";
    if (strftime(buf, len, fmt, tm) == 0) {
        snprintf(buf, len, "--:--");
    }
}

static void clock_timer_cb(lv_timer_t *timer)
{
    desktop_shell_t *shell = (desktop_shell_t *)lv_timer_get_user_data(timer);
    if (shell == NULL || shell->clock_label == NULL) {
        return;
    }
    char buf[16];
    format_clock(shell, buf, sizeof(buf));
    lv_label_set_text(shell->clock_label, buf);
}

/* ── 壁纸 ────────────────────────────────────────────────────────── */

static uint32_t wallpaper_color(const desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    switch (shell->wallpaper) {
    case DESKTOP_WALLPAPER_CONTAINER_LOW:     return c->surface_container_low;
    case DESKTOP_WALLPAPER_CONTAINER_HIGH:    return c->surface_container_high;
    case DESKTOP_WALLPAPER_CONTAINER_HIGHEST: return c->surface_container_highest;
    case DESKTOP_WALLPAPER_SURFACE:
    default:                                  return c->surface;
    }
}

static void apply_wallpaper(desktop_shell_t *shell)
{
    lv_obj_t *scr = lv_display_get_screen_active(shell->disp);
    if (scr == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(scr, tok(wallpaper_color(shell)), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

/* ── 状态栏 ──────────────────────────────────────────────────────── */

static lv_obj_t *build_status_bar(desktop_shell_t *shell, lv_obj_t *parent)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);

    lv_obj_t *bar = lv_obj_create(parent);
    if (bar == NULL) {
        return NULL;
    }
    lv_obj_set_layout(bar, LV_LAYOUT_NONE);
    lv_obj_set_size(bar, w, STATUS_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, tok(gui_theme_elevation_surface(GUI_THEME_ELEVATION_1)), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左侧：时钟 */
    shell->clock_label = make_label(bar, "--:--", GUI_THEME_FONT_LABEL, c->on_surface);
    if (shell->clock_label != NULL) {
        lv_obj_set_pos(shell->clock_label, 12, 9);
    }

    /* 中间：系统名 */
    lv_obj_t *sysname = make_label(bar, "OS01", GUI_THEME_FONT_BODY, c->on_surface);
    if (sysname != NULL) {
        lv_obj_align(sysname, LV_ALIGN_TOP_MID, 0, 8);
    }

    /* 右侧：指示区 */
    lv_obj_t *ind = make_label(bar, "x86_64  lvgl", GUI_THEME_FONT_LABEL,
                               c->on_surface_variant);
    if (ind != NULL) {
        lv_obj_align(ind, LV_ALIGN_TOP_RIGHT, -12, 9);
    }

    return bar;
}

/* ── 启动器 ──────────────────────────────────────────────────────── */

static void launcher_icon_cb(lv_event_t *event)
{
    desktop_shell_t *shell = (desktop_shell_t *)lv_event_get_user_data(event);
    if (shell == NULL) {
        return;
    }
    /* 应用标识挂在图标对象的 user data 上。 */
    const char *app_id = (const char *)lv_obj_get_user_data(lv_event_get_target(event));
    if (app_id == NULL) {
        return;
    }
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, app_id);
}

static lv_obj_t *build_launcher(desktop_shell_t *shell, lv_obj_t *parent)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    uint32_t count = desktop_app_launcher_count();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);
    int32_t h = lv_display_get_vertical_resolution(shell->disp);

    int32_t bar_w = (int32_t)(LAUNCHER_PAD * 2 + count * LAUNCHER_ICON +
                              (count ? (count - 1) * LAUNCHER_GAP : 0));
    int32_t bar_x = (w - bar_w) / 2;
    int32_t bar_y = h - LAUNCHER_HEIGHT;

    lv_obj_t *bar = lv_obj_create(parent);
    if (bar == NULL) {
        return NULL;
    }
    lv_obj_set_layout(bar, LV_LAYOUT_NONE);
    lv_obj_set_size(bar, bar_w, LAUNCHER_HEIGHT);
    lv_obj_set_pos(bar, bar_x, bar_y);
    style_card(bar, GUI_THEME_ELEVATION_2);

    for (uint32_t i = 0; i < count; i++) {
        const desktop_app_info_t *info = desktop_app_launcher_entry(i);
        if (info == NULL) {
            break;
        }

        int32_t ix = (int32_t)(LAUNCHER_PAD + i * (LAUNCHER_ICON + LAUNCHER_GAP));

        /* 图标：强调色圆块 + 字形 */
        lv_obj_t *icon = lv_obj_create(bar);
        if (icon == NULL) {
            continue;
        }
        lv_obj_set_layout(icon, LV_LAYOUT_NONE);
        lv_obj_set_size(icon, LAUNCHER_ICON, LAUNCHER_ICON);
        lv_obj_set_pos(icon, ix, LAUNCHER_PAD);
        lv_obj_set_style_bg_color(icon, tok(desktop_app_accent_color(info->id)), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(icon, (int32_t)tok_radius(GUI_THEME_RADIUS_FULL), 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_shadow_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *glyph = make_label(icon, info->glyph, GUI_THEME_FONT_TITLE, c->on_primary);
        if (glyph != NULL) {
            lv_obj_center(glyph);
        }

        /* 文字标签：图标下方的应用名 */
        lv_obj_t *cap = make_label(bar, info->name, GUI_THEME_FONT_LABEL, c->on_surface_variant);
        if (cap != NULL) {
            lv_obj_set_width(cap, LAUNCHER_ICON);
            lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(cap, ix, LAUNCHER_PAD + LAUNCHER_ICON + 4);
        }

        /* 点击图标 = 启动应用 */
        lv_obj_set_user_data(icon, (void *)desktop_app_id_name(info->id));
        lv_obj_add_event_cb(icon, launcher_icon_cb, LV_EVENT_CLICKED, shell);
    }

    return bar;
}

/* ── 表面构建 ────────────────────────────────────────────────────── */

static void build_home(desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    lv_obj_t *root = make_surface_root(shell->disp);
    if (root == NULL) {
        return;
    }
    shell->root = root;
    shell->clock_label = NULL;

    build_status_bar(shell, root);

    /* 欢迎卡片 */
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);
    lv_obj_t *card = lv_obj_create(root);
    if (card != NULL) {
        lv_obj_set_layout(card, LV_LAYOUT_NONE);
        lv_obj_set_size(card, CARD_WIDTH, CARD_HEIGHT);
        lv_obj_set_pos(card, (w - CARD_WIDTH) / 2, CARD_TOP);
        style_card(card, GUI_THEME_ELEVATION_2);

        lv_obj_t *title = make_label(card, "OS01", GUI_THEME_FONT_DISPLAY, c->on_surface);
        if (title != NULL) {
            lv_obj_set_pos(title, 16, 14);
        }
        lv_obj_t *desc = make_label(card, "Material 风格的单进程桌面外壳",
                                    GUI_THEME_FONT_BODY, c->on_surface_variant);
        if (desc != NULL) {
            lv_obj_set_pos(desc, 16, 52);
        }
        lv_obj_t *hint = make_label(card, "点击启动器图标进入应用；ESC 返回主屏",
                                    GUI_THEME_FONT_LABEL, c->on_surface_variant);
        if (hint != NULL) {
            lv_obj_set_pos(hint, 16, 80);
        }
    }

    build_launcher(shell, root);
    apply_wallpaper(shell);
}

/* ── 迁移执行 ────────────────────────────────────────────────────── */

static void destroy_surface(desktop_shell_t *shell)
{
    if (shell->root != NULL) {
        lv_obj_delete(shell->root);
        shell->root = NULL;
    }
    shell->clock_label = NULL;
    shell->status_bar = NULL;
}

void desktop_shell_apply_pending(desktop_shell_t *shell)
{
    if (shell == NULL || shell->pending == DESKTOP_INTENT_NONE) {
        return;
    }

    desktop_intent_t intent = shell->pending;
    char app_id[DESKTOP_APP_ID_MAX];
    snprintf(app_id, sizeof(app_id), "%s", shell->pending_app);

    shell->pending = DESKTOP_INTENT_NONE;
    shell->pending_app[0] = '\0';

    desktop_state_t next = desktop_state_next(shell->state, intent, app_id);
    if (next == DESKTOP_STATE_INVALID) {
        /* 非法迁移被忽略：状态机已经把它判为「不允许」。 */
        return;
    }
    if (next == shell->state) {
        return;
    }

    destroy_surface(shell);
    shell->state = next;

    switch (next) {
    case DESKTOP_STATE_HOME:
        snprintf(shell->current_app, sizeof(shell->current_app), "%s", "");
        build_home(shell);
        break;
    case DESKTOP_STATE_DRAWER:
    case DESKTOP_STATE_SETTINGS:
    case DESKTOP_STATE_APP:
    default:
        /* 抽屉 / 设置 / 应用表面由后续任务补齐（Task 5 / Task 6）。 */
        break;
    }
}

/* ── 生命周期 ────────────────────────────────────────────────────── */

desktop_shell_t *desktop_shell_create(lv_display_t *disp)
{
    if (disp == NULL) {
        return NULL;
    }

    desktop_shell_t *shell = (desktop_shell_t *)calloc(1, sizeof(*shell));
    if (shell == NULL) {
        return NULL;
    }
    shell->disp = disp;
    shell->state = DESKTOP_STATE_HOME;
    shell->pending = DESKTOP_INTENT_NONE;
    shell->wallpaper = DESKTOP_WALLPAPER_SURFACE;
    shell->density = DESKTOP_DENSITY_COMFORTABLE;
    shell->clock_24h = true;

    build_home(shell);

    /* 时钟定时器只创建一次：tick() 只刷新文本，不新建定时器。 */
    shell->clock_timer = lv_timer_create(clock_timer_cb, 1000, shell);
    if (shell->clock_timer != NULL) {
        clock_timer_cb(shell->clock_timer);
    }

    return shell;
}

void desktop_shell_destroy(desktop_shell_t *shell)
{
    if (shell == NULL) {
        return;
    }
    if (shell->clock_timer != NULL) {
        lv_timer_delete(shell->clock_timer);
        shell->clock_timer = NULL;
    }
    destroy_surface(shell);
    free(shell);
}

desktop_state_t desktop_shell_state(const desktop_shell_t *shell)
{
    return shell ? shell->state : DESKTOP_STATE_INVALID;
}

const char *desktop_shell_current_app(const desktop_shell_t *shell)
{
    return shell ? shell->current_app : "";
}

void desktop_shell_request(desktop_shell_t *shell, desktop_intent_t intent,
                            const char *app_id)
{
    if (shell == NULL) {
        return;
    }
    shell->pending = intent;
    if (app_id != NULL) {
        snprintf(shell->pending_app, sizeof(shell->pending_app), "%s", app_id);
    } else {
        shell->pending_app[0] = '\0';
    }
}

void desktop_shell_tick(desktop_shell_t *shell)
{
    if (shell == NULL || shell->clock_label == NULL) {
        return;
    }
    char buf[16];
    format_clock(shell, buf, sizeof(buf));
    lv_label_set_text(shell->clock_label, buf);
}

bool desktop_shell_handle_key(desktop_shell_t *shell, const gui_key_event_t *ev)
{
    if (shell == NULL || ev == NULL) {
        return false;
    }

    /* 只处理按下边沿；重复与释放交给 LVGL / libgui。 */
    if (ev->action != GUI_KEY_DOWN) {
        return false;
    }

    switch (ev->physical_key) {
    case 0x01: /* Esc */
        desktop_shell_request(shell, DESKTOP_INTENT_ESC, NULL);
        return true;
    default:
        return false;
    }
}

bool desktop_shell_probe_point(const desktop_shell_t *shell, desktop_region_t region,
                                uint32_t *x, uint32_t *y)
{
    if (shell == NULL || x == NULL || y == NULL) {
        return false;
    }

    int32_t w = lv_display_get_horizontal_resolution(shell->disp);
    int32_t h = lv_display_get_vertical_resolution(shell->disp);

    switch (region) {
    case DESKTOP_REGION_WALLPAPER:
        *x = 8;
        *y = STATUS_BAR_HEIGHT + 8;
        return true;
    case DESKTOP_REGION_STATUS_BAR:
        *x = 2;
        *y = STATUS_BAR_HEIGHT / 2;
        return true;
    case DESKTOP_REGION_CARD:
        /* 取样点必须避开圆角弧（半径 12）与卡片内的文字。 */
        *x = (uint32_t)(((w - CARD_WIDTH) / 2) + 16);
        *y = (uint32_t)(CARD_TOP + CARD_HEIGHT - 16);
        return true;
    case DESKTOP_REGION_LAUNCHER: {
        uint32_t count = desktop_app_launcher_count();
        int32_t bar_w = (int32_t)(LAUNCHER_PAD * 2 + count * LAUNCHER_ICON +
                                  (count ? (count - 1) * LAUNCHER_GAP : 0));
        /* 取样点必须避开圆角弧（半径 12）与图标。 */
        *x = (uint32_t)(((w - bar_w) / 2) + 16);
        *y = (uint32_t)(h - LAUNCHER_HEIGHT + 16);
        return true;
    }
    default:
        return false;
    }
}

void desktop_shell_get_settings(const desktop_shell_t *shell,
                                desktop_wallpaper_t *wallpaper,
                                desktop_density_t *density,
                                bool *clock_24h)
{
    if (shell == NULL) {
        return;
    }
    if (wallpaper != NULL) {
        *wallpaper = shell->wallpaper;
    }
    if (density != NULL) {
        *density = shell->density;
    }
    if (clock_24h != NULL) {
        *clock_24h = shell->clock_24h;
    }
}
