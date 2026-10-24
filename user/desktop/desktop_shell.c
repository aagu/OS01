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
/* 应用名标签向两侧多伸出的像素：让 "Terminal" 这类长名字不被截断。 */
#define LAUNCHER_CAPTION_OVERHANG 16
#define PANEL_WIDTH        480

/* ── 外壳状态 ────────────────────────────────────────────────────── */

struct desktop_shell {
    gui_platform_t *platform;
    gui_input_t   *input;   /* 借用：系统应用用它报告键鼠是否存在 */
    lv_display_t  *disp;

    desktop_state_t state;
    desktop_intent_t pending;
    char pending_app[DESKTOP_APP_ID_MAX];
    char current_app[DESKTOP_APP_ID_MAX];

    desktop_wallpaper_t wallpaper;
    desktop_density_t   density;
    bool               clock_24h;
    desktop_setting_t   pending_setting;
    time_t             start_time;

    desktop_app_state_t *app_state; /* 跨应用存活的应用状态 */

    lv_obj_t   *root;        /* 当前表面的根对象 */
    lv_obj_t   *clock_label;
    lv_obj_t   *status_bar;
    lv_timer_t *clock_timer;

    /*
     * 桌面自己的焦点序列。
     *
     * libgui 的焦点组是私有的，公共 API 没有「登记可聚焦控件」的入口，
     * 而业务应用只能用公共 API —— 因此外壳用 LVGL 的 LV_STATE_FOCUSED
     * 状态自己实现同一语义：Tab / Shift+Tab 移动，Enter 激活。
     * 序列随表面重建：destroy_surface() 先清空它，不会残留已销毁对象。
     */
    lv_obj_t   *focus_items[DESKTOP_FOCUS_MAX];
    uint32_t    focus_count;
    int32_t     focus_index;  /* -1 表示当前没有焦点 */
};

static void apply_pending_setting(desktop_shell_t *shell, desktop_setting_t setting);

desktop_app_id_t desktop_shell_current_app_id(const desktop_shell_t *shell)
{
    desktop_app_id_t id = DESKTOP_APP_COUNT;
    if (shell == NULL || !desktop_app_id_from_name(shell->current_app, &id)) {
        return DESKTOP_APP_COUNT;
    }
    return id;
}

lv_obj_t *desktop_shell_surface_root(const desktop_shell_t *shell)
{
    return shell ? shell->root : NULL;
}

time_t desktop_shell_start_time(const desktop_shell_t *shell)
{
    return shell ? shell->start_time : (time_t)0;
}

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

/* ── 焦点模型 ────────────────────────────────────────────────────── */

/* 清空焦点序列：控件随表面一起没了，序列不能留着野指针。 */
static void focus_clear(desktop_shell_t *shell)
{
    for (uint32_t i = 0; i < shell->focus_count; i++) {
        if (shell->focus_items[i] != NULL) {
            lv_obj_remove_state(shell->focus_items[i], LV_STATE_FOCUSED);
        }
        shell->focus_items[i] = NULL;
    }
    shell->focus_count = 0;
    shell->focus_index = -1;
}

/* 把焦点环画到当前项上：LV_STATE_FOCUSED 的样式由 Material 主题给出。 */
static void focus_apply(desktop_shell_t *shell)
{
    for (uint32_t i = 0; i < shell->focus_count; i++) {
        if (shell->focus_items[i] == NULL) {
            continue;
        }
        if ((int32_t)i == shell->focus_index) {
            lv_obj_add_state(shell->focus_items[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_remove_state(shell->focus_items[i], LV_STATE_FOCUSED);
        }
    }
}

bool desktop_shell_focus_add(desktop_shell_t *shell, lv_obj_t *obj)
{
    if (shell == NULL || obj == NULL) {
        return false;
    }
    if (shell->focus_count >= DESKTOP_FOCUS_MAX) {
        return false;
    }
    shell->focus_items[shell->focus_count++] = obj;
    return true;
}

void desktop_shell_focus_remove_children(desktop_shell_t *shell, lv_obj_t *parent)
{
    if (shell == NULL || parent == NULL) {
        return;
    }

    /* 保留顺序地剔除：焦点项的父对象即将随容器一起销毁。 */
    uint32_t keep = 0;
    for (uint32_t i = 0; i < shell->focus_count; i++) {
        lv_obj_t *obj = shell->focus_items[i];
        if (obj == NULL || lv_obj_get_parent(obj) == parent) {
            continue;
        }
        shell->focus_items[keep++] = obj;
    }
    for (uint32_t i = keep; i < shell->focus_count; i++) {
        shell->focus_items[i] = NULL;
    }
    shell->focus_count = keep;

    if (shell->focus_index >= (int32_t)keep) {
        shell->focus_index = (int32_t)keep - 1;
    }
    focus_apply(shell);
}

uint32_t desktop_shell_focus_count(const desktop_shell_t *shell)
{
    return shell ? shell->focus_count : 0u;
}

int32_t desktop_shell_focus_index(const desktop_shell_t *shell)
{
    return shell ? shell->focus_index : -1;
}

/* Tab / Shift+Tab：在序列里前进 / 后退。第一次 Tab 落在首项上。 */
static bool focus_move(desktop_shell_t *shell, bool forward)
{
    if (shell == NULL || shell->focus_count == 0) {
        return false;
    }
    if (shell->focus_index < 0) {
        shell->focus_index = forward ? 0 : (int32_t)shell->focus_count - 1;
    } else if (forward) {
        shell->focus_index = (int32_t)((shell->focus_index + 1) % shell->focus_count);
    } else {
        shell->focus_index = (int32_t)((shell->focus_index + (int32_t)shell->focus_count - 1) %
                                       shell->focus_count);
    }
    focus_apply(shell);
    return true;
}

/* Enter：激活当前焦点控件（复用它自己的点击回调）。 */
static bool focus_activate(desktop_shell_t *shell)
{
    if (shell == NULL || shell->focus_index < 0 ||
        (uint32_t)shell->focus_index >= shell->focus_count) {
        return false;
    }
    lv_obj_t *obj = shell->focus_items[shell->focus_index];
    if (obj == NULL) {
        return false;
    }
    lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    return true;
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
    case DESKTOP_WALLPAPER_SURFACE:            return c->surface;
    case DESKTOP_WALLPAPER_CONTAINER_LOW:      return c->surface_container_low;
    case DESKTOP_WALLPAPER_CONTAINER_HIGH:     return c->surface_container_high;
    case DESKTOP_WALLPAPER_CONTAINER_HIGHEST:  return c->surface_container_highest;
    case DESKTOP_WALLPAPER_SURFACE_DIM:
    default:                                   return c->surface_dim;
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

/* 启动器条的宽度：两侧留出应用名标签的余量（标签比图标宽）。 */
static int32_t launcher_bar_width(uint32_t count)
{
    return (int32_t)((LAUNCHER_PAD + LAUNCHER_CAPTION_OVERHANG) * 2 +
                     count * LAUNCHER_ICON +
                     (count ? (count - 1) * LAUNCHER_GAP : 0));
}

static lv_obj_t *build_launcher(desktop_shell_t *shell, lv_obj_t *parent)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    uint32_t count = desktop_app_launcher_count();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);
    int32_t h = lv_display_get_vertical_resolution(shell->disp);

    int32_t bar_w = launcher_bar_width(count);
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

        int32_t ix = (int32_t)(LAUNCHER_PAD + LAUNCHER_CAPTION_OVERHANG +
                               i * (LAUNCHER_ICON + LAUNCHER_GAP));

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

        /* 文字标签：图标下方的应用名。标签比图标宽一点并居中：应用名
         * （"Terminal" / "Settings"）比 48px 的图标还宽，固定成图标宽度
         * 会被截断。 */
        lv_obj_t *cap = make_label(bar, info->name, GUI_THEME_FONT_LABEL, c->on_surface_variant);
        if (cap != NULL) {
            lv_obj_set_width(cap, LAUNCHER_ICON + LAUNCHER_CAPTION_OVERHANG * 2);
            lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(cap, ix - LAUNCHER_CAPTION_OVERHANG,
                           LAUNCHER_PAD + LAUNCHER_ICON + 4);
        }

        /* 点击图标 = 启动应用 */
        lv_obj_set_user_data(icon, (void *)desktop_app_id_name(info->id));
        lv_obj_add_event_cb(icon, launcher_icon_cb, LV_EVENT_CLICKED, shell);

        /* 启动器图标也是键盘焦点项：Tab 可依次走到它们。 */
        desktop_shell_focus_add(shell, icon);
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
        lv_obj_t *desc = make_label(card, "Material single-process desktop shell",
                                    GUI_THEME_FONT_BODY, c->on_surface_variant);
        if (desc != NULL) {
            lv_obj_set_pos(desc, 16, 52);
        }
        lv_obj_t *hint = make_label(card, "Click a launcher icon to open an app; ESC returns home",
                                    GUI_THEME_FONT_LABEL, c->on_surface_variant);
        if (hint != NULL) {
            lv_obj_set_pos(hint, 16, 80);
            /* 提示比卡片宽：让它折行，而不是被截断。 */
            lv_obj_set_width(hint, CARD_WIDTH - 32);
            lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        }
    }

    build_launcher(shell, root);
    apply_wallpaper(shell);
}


/* ── 应用抽屉 ────────────────────────────────────────────────────── */

static void drawer_row_cb(lv_event_t *event)
{
    desktop_shell_t *shell = (desktop_shell_t *)lv_event_get_user_data(event);
    if (shell == NULL) {
        return;
    }
    const char *app_id = (const char *)lv_obj_get_user_data(lv_event_get_target(event));
    if (app_id == NULL) {
        return;
    }
    desktop_shell_request(shell, DESKTOP_INTENT_OPEN_APP, app_id);
}

static void build_drawer(desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);
    int32_t h = lv_display_get_vertical_resolution(shell->disp);

    lv_obj_t *root = make_surface_root(shell->disp);
    if (root == NULL) {
        return;
    }
    shell->root = root;
    shell->clock_label = NULL;

    build_status_bar(shell, root);

    lv_obj_t *title = make_label(root, "Apps", GUI_THEME_FONT_HEADLINE, c->on_surface);
    if (title != NULL) {
        lv_obj_set_pos(title, 16, STATUS_BAR_HEIGHT + 12);
    }

    lv_obj_t *count = make_label(root, "", GUI_THEME_FONT_LABEL, c->on_surface_variant);
    if (count != NULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u apps", desktop_app_count());
        lv_label_set_text(count, buf);
        lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -16, STATUS_BAR_HEIGHT + 16);
    }

    /* 抽屉面板：elevation 3（浮层）。面板留出内边距，取样点落在面板本身。 */
    lv_obj_t *panel = lv_obj_create(root);
    if (panel == NULL) {
        return;
    }
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_set_size(panel, w - 32, h - STATUS_BAR_HEIGHT - 64);
    lv_obj_set_pos(panel, 16, STATUS_BAR_HEIGHT + 48);
    style_card(panel, GUI_THEME_ELEVATION_3);
    lv_obj_set_style_pad_all(panel, (int32_t)tok_space(GUI_THEME_SPACE_MD), 0);

    /* 可滚动列表：flex 纵向排列，行高自然撑开内容高度。 */
    lv_obj_t *list = lv_obj_create(panel);
    if (list == NULL) {
        return;
    }
    lv_obj_set_layout(list, LV_LAYOUT_NONE);
    lv_obj_set_size(list, w - 32 - 2 * (int32_t)tok_space(GUI_THEME_SPACE_MD),
                    h - STATUS_BAR_HEIGHT - 64 - 2 * (int32_t)tok_space(GUI_THEME_SPACE_MD));
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, (int32_t)tok_space(GUI_THEME_SPACE_SM), 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_shadow_width(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (uint32_t i = 0; i < desktop_app_count(); i++) {
        desktop_app_id_t id = (desktop_app_id_t)i;
        const desktop_app_info_t *info = desktop_app_info(id);
        if (info == NULL) {
            break;
        }

        lv_obj_t *row = lv_obj_create(list);
        if (row == NULL) {
            continue;
        }
        style_card(row, GUI_THEME_ELEVATION_1);
        lv_obj_set_style_bg_color(row, tok(c->surface_container_low), 0);
        lv_obj_set_width(row, w - 32);
        lv_obj_set_height(row, 64);
        lv_obj_set_flex_grow(row, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        /* 图标 */
        lv_obj_t *icon = lv_obj_create(row);
        if (icon != NULL) {
            lv_obj_set_size(icon, 40, 40);
            lv_obj_set_pos(icon, 12, 12);
            lv_obj_set_style_bg_color(icon, tok(desktop_app_accent_color(id)), 0);
            lv_obj_set_style_radius(icon, (int32_t)tok_radius(GUI_THEME_RADIUS_FULL), 0);
            lv_obj_set_style_border_width(icon, 0, 0);
            lv_obj_set_style_shadow_width(icon, 0, 0);
            lv_obj_set_style_pad_all(icon, 0, 0);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *glyph = make_label(icon, info->glyph, GUI_THEME_FONT_TITLE, c->on_primary);
            if (glyph != NULL) {
                lv_obj_center(glyph);
            }
        }

        lv_obj_t *name = make_label(row, info->name, GUI_THEME_FONT_TITLE, c->on_surface);
        if (name != NULL) {
            lv_obj_set_pos(name, 64, 10);
        }
        lv_obj_t *desc = make_label(row, info->desc, GUI_THEME_FONT_BODY, c->on_surface_variant);
        if (desc != NULL) {
            lv_obj_set_pos(desc, 64, 34);
        }

        lv_obj_set_user_data(row, (void *)desktop_app_id_name(id));
        lv_obj_add_event_cb(row, drawer_row_cb, LV_EVENT_CLICKED, shell);

        /* 抽屉条目也是键盘焦点项：Tab 选中后回车即可启动应用。 */
        desktop_shell_focus_add(shell, row);
    }

    lv_obj_t *hint = make_label(root, "ESC returns home", GUI_THEME_FONT_LABEL, c->on_surface_variant);
    if (hint != NULL) {
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 16, -12);
    }

    apply_wallpaper(shell);
}

/* ── 基本设置 ────────────────────────────────────────────────────── */

static void setting_row_cb(lv_event_t *event)
{
    desktop_shell_t *shell = (desktop_shell_t *)lv_event_get_user_data(event);
    if (shell == NULL) {
        return;
    }
    /* 设置项标识挂在行对象的 user data 上；回调的 user data 是外壳。 */
    desktop_setting_t setting =
        (desktop_setting_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target(event));
    if (setting == DESKTOP_SETTING_NONE) {
        return;
    }
    desktop_shell_request_setting(shell, setting);
}

static const char *wallpaper_label(const desktop_shell_t *shell)
{
    switch (shell->wallpaper) {
    case DESKTOP_WALLPAPER_SURFACE:            return "Standard";
    case DESKTOP_WALLPAPER_CONTAINER_LOW:      return "Light grey";
    case DESKTOP_WALLPAPER_CONTAINER_HIGH:     return "Grey";
    case DESKTOP_WALLPAPER_CONTAINER_HIGHEST:  return "Dark grey";
    case DESKTOP_WALLPAPER_SURFACE_DIM:
    default:                                   return "Deepest";
    }
}

static const char *density_label(const desktop_shell_t *shell)
{
    return (shell->density == DESKTOP_DENSITY_COMPACT) ? "Compact" : "Comfortable";
}

static lv_obj_t *make_setting_row(desktop_shell_t *shell, lv_obj_t *list,
                                   const char *name, const char *value,
                                   desktop_setting_t setting)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);

    lv_obj_t *row = lv_obj_create(list);
    if (row == NULL) {
        return NULL;
    }
    style_card(row, GUI_THEME_ELEVATION_1);
    lv_obj_set_style_bg_color(row, tok(c->surface_container_low), 0);
    lv_obj_set_width(row, w - 32);
    lv_obj_set_height(row, 56);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = make_label(row, name, GUI_THEME_FONT_TITLE, c->on_surface);
    if (label != NULL) {
        lv_obj_set_pos(label, 16, 18);
    }

    /* 右侧的当前值：点击整行即切换。 */
    lv_obj_t *val = make_label(row, value, GUI_THEME_FONT_BODY, c->primary);
    if (val != NULL) {
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    lv_obj_set_user_data(row, (void *)(intptr_t)setting);
    lv_obj_add_event_cb(row, setting_row_cb, LV_EVENT_CLICKED, shell);

    /* 设置项也是键盘焦点项：Tab 选中后回车即可切换。 */
    desktop_shell_focus_add(shell, row);
    return row;
}

/*
 * 设置项的**唯一实现**：设置表面与设置应用都调用它。
 *
 * 这样「设置表面」与「设置应用」不是两套界面，而是同一份界面的两个入口：
 * 读同一份 shell 设置状态，写同一条投递通道。
 */
static void build_settings_rows(desktop_shell_t *shell, lv_obj_t *list)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);

    if (list == NULL) {
        return;
    }

    make_setting_row(shell, list, "Clock format",
                     shell->clock_24h ? "24-hour" : "12-hour",
                     DESKTOP_SETTING_CLOCK_FORMAT);
    make_setting_row(shell, list, "Wallpaper colour", wallpaper_label(shell),
                     DESKTOP_SETTING_WALLPAPER);
    make_setting_row(shell, list, "Card density", density_label(shell),
                     DESKTOP_SETTING_DENSITY);

    /* 「关于」是只读展示，不可点击。 */
    lv_obj_t *about = lv_obj_create(list);
    if (about != NULL) {
        style_card(about, GUI_THEME_ELEVATION_1);
        lv_obj_set_style_bg_color(about, tok(c->surface_container_low), 0);
        lv_obj_set_width(about, w - 32);
        lv_obj_set_height(about, 88);
        lv_obj_t *name = make_label(about, "About", GUI_THEME_FONT_TITLE, c->on_surface);
        if (name != NULL) {
            lv_obj_set_pos(name, 16, 14);
        }
        lv_obj_t *detail = make_label(about,
                                      "OS01 Material desktop shell\n"
                                      "Kernel x86_64 / LVGL 9 / libgui",
                                      GUI_THEME_FONT_BODY, c->on_surface_variant);
        if (detail != NULL) {
            lv_obj_set_pos(detail, 16, 40);
        }
    }
}

static void build_settings(desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);
    int32_t h = lv_display_get_vertical_resolution(shell->disp);

    lv_obj_t *root = make_surface_root(shell->disp);
    if (root == NULL) {
        return;
    }
    shell->root = root;
    shell->clock_label = NULL;

    build_status_bar(shell, root);

    lv_obj_t *title = make_label(root, "Settings", GUI_THEME_FONT_HEADLINE, c->on_surface);
    if (title != NULL) {
        lv_obj_set_pos(title, 16, STATUS_BAR_HEIGHT + 12);
    }

    /* 设置面板：elevation 2（卡片层）。 */
    lv_obj_t *panel = lv_obj_create(root);
    if (panel == NULL) {
        return;
    }
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_set_size(panel, w - 32, h - STATUS_BAR_HEIGHT - 64);
    lv_obj_set_pos(panel, 16, STATUS_BAR_HEIGHT + 48);
    style_card(panel, GUI_THEME_ELEVATION_2);
    lv_obj_set_style_pad_all(panel, (int32_t)tok_space(GUI_THEME_SPACE_MD), 0);

    lv_obj_t *list = lv_obj_create(panel);
    if (list == NULL) {
        return;
    }
    lv_obj_set_layout(list, LV_LAYOUT_NONE);
    lv_obj_set_size(list, w - 32 - 2 * (int32_t)tok_space(GUI_THEME_SPACE_MD),
                    h - STATUS_BAR_HEIGHT - 64 - 2 * (int32_t)tok_space(GUI_THEME_SPACE_MD));
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, (int32_t)tok_space(GUI_THEME_SPACE_SM), 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_shadow_width(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    build_settings_rows(shell, list);

    apply_wallpaper(shell);
}

/*
 * 设置**应用**的设置体：与应用表面共用同一份设置项实现。
 *
 * 与设置表面的差别只在承载方式：应用表面自带顶栏（返回 + 标题），
 * 因此设置体从顶栏之下开始，没有状态栏也没有标题行。
 */
lv_obj_t *desktop_shell_build_settings_body(desktop_shell_t *shell, lv_obj_t *parent)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = lv_display_get_horizontal_resolution(shell->disp);
    int32_t h = lv_display_get_vertical_resolution(shell->disp);

    if (shell == NULL || parent == NULL) {
        return NULL;
    }

    lv_obj_t *list = lv_obj_create(parent);
    if (list == NULL) {
        return NULL;
    }
    lv_obj_set_layout(list, LV_LAYOUT_NONE);
    lv_obj_set_size(list, w - 32, h - DESKTOP_APP_BAR_HEIGHT - 32);
    lv_obj_set_pos(list, 16, DESKTOP_APP_BAR_HEIGHT + 16);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, (int32_t)tok_space(GUI_THEME_SPACE_SM), 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_shadow_width(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    build_settings_rows(shell, list);

    /* 设置应用里的「关于」额外给出只读的系统信息入口。 */
    lv_obj_t *hint = make_label(parent, "Shares one configuration with the home screen",
                                GUI_THEME_FONT_LABEL, c->on_surface_variant);
    if (hint != NULL) {
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 16, -10);
    }

    return list;
}

/* ── 迁移执行 ────────────────────────────────────────────────────── */

static void destroy_surface(desktop_shell_t *shell)
{
    /* 顺序要紧：焦点项随控件树一起没了，必须在删除控件树**之前**
     * 把它们从序列里摘掉，否则就是往已销毁对象上写状态。 */
    focus_clear(shell);

    if (shell->root != NULL) {
        lv_obj_delete(shell->root);
        shell->root = NULL;
    }
    shell->clock_label = NULL;
    shell->status_bar = NULL;

    /* 界面句柄随控件树一起没了：应用状态里存的指针必须同时失效。 */
    if (shell->app_state != NULL) {
        shell->app_state->files_list = NULL;
        shell->app_state->term_output_label = NULL;
        shell->app_state->term_input_label = NULL;
    }
}

/* 构建当前应用的全屏表面（应用自己构造界面，外壳只持有根对象）。 */
static void build_app_surface(desktop_shell_t *shell)
{
    desktop_app_id_t id = desktop_shell_current_app_id(shell);
    if (id == DESKTOP_APP_COUNT) {
        return;
    }
    lv_obj_t *root = desktop_app_build(shell->disp, id, shell);
    if (root != NULL) {
        shell->root = root;
        shell->clock_label = NULL;
    }
}

void desktop_shell_apply_pending(desktop_shell_t *shell)
{
    if (shell == NULL) {
        return;
    }

    /* 设置项变更先处理：它不改变表面状态，但要重建当前表面。 */
    if (shell->pending_setting != DESKTOP_SETTING_NONE) {
        desktop_setting_t setting = shell->pending_setting;
        shell->pending_setting = DESKTOP_SETTING_NONE;
        apply_pending_setting(shell, setting);
        return;
    }

    if (shell->pending == DESKTOP_INTENT_NONE) {
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
        build_drawer(shell);
        break;
    case DESKTOP_STATE_SETTINGS:
        build_settings(shell);
        break;
    case DESKTOP_STATE_APP: {
        snprintf(shell->current_app, sizeof(shell->current_app), "%s", app_id);
        /* 应用表面由应用自己构造：它占满整个显示区。 */
        build_app_surface(shell);
        break;
    }
    default:
        break;
    }
}

/* ── 设置项变更 ──────────────────────────────────────────────────── */

static void apply_pending_setting(desktop_shell_t *shell, desktop_setting_t setting)
{
    switch (setting) {
    case DESKTOP_SETTING_CLOCK_FORMAT:
        shell->clock_24h = !shell->clock_24h;
        break;
    case DESKTOP_SETTING_WALLPAPER:
        shell->wallpaper = (desktop_wallpaper_t)((shell->wallpaper + 1) %
                          (int)(DESKTOP_WALLPAPER_CONTAINER_HIGHEST + 1));
        break;
    case DESKTOP_SETTING_DENSITY:
        shell->density = (shell->density == DESKTOP_DENSITY_COMFORTABLE)
                             ? DESKTOP_DENSITY_COMPACT
                             : DESKTOP_DENSITY_COMFORTABLE;
        break;
    case DESKTOP_SETTING_NONE:
    default:
        return;
    }

    /* 设置项的可见效果在当前表面上：重建当前表面。 */
    desktop_state_t state = shell->state;
    destroy_surface(shell);
    switch (state) {
    case DESKTOP_STATE_HOME:     build_home(shell);     break;
    case DESKTOP_STATE_DRAWER:   build_drawer(shell);   break;
    case DESKTOP_STATE_SETTINGS: build_settings(shell); break;
    case DESKTOP_STATE_APP:      build_app_surface(shell); break;
    default:                     build_home(shell);     break;
    }
}

void desktop_shell_attach_input(desktop_shell_t *shell, gui_input_t *input)
{
    if (shell != NULL) {
        shell->input = input;
    }
}

gui_input_t *desktop_shell_input(const desktop_shell_t *shell)
{
    return shell ? shell->input : NULL;
}

gui_platform_t *desktop_shell_platform(const desktop_shell_t *shell)
{
    return shell ? shell->platform : NULL;
}

desktop_app_state_t *desktop_shell_app_state(desktop_shell_t *shell)
{
    return shell ? shell->app_state : NULL;
}

void desktop_shell_request_setting(desktop_shell_t *shell, desktop_setting_t setting)
{
    if (shell == NULL || setting == DESKTOP_SETTING_NONE) {
        return;
    }
    shell->pending_setting = setting;
}

/* ── 生命周期 ────────────────────────────────────────────────────── */

desktop_shell_t *desktop_shell_create(gui_platform_t *platform)
{
    if (platform == NULL) {
        return NULL;
    }

    /* display 归平台所有；外壳只借用 LVGL 的默认 display。 */
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        return NULL;
    }

    desktop_shell_t *shell = (desktop_shell_t *)calloc(1, sizeof(*shell));
    if (shell == NULL) {
        return NULL;
    }
    shell->platform = platform;
    shell->disp = disp;
    shell->state = DESKTOP_STATE_HOME;
    shell->pending = DESKTOP_INTENT_NONE;
    shell->wallpaper = DESKTOP_WALLPAPER_SURFACE_DIM;
    shell->density = DESKTOP_DENSITY_COMFORTABLE;
    shell->clock_24h = true;
    shell->pending_setting = DESKTOP_SETTING_NONE;
    shell->start_time = time(NULL);
    /* calloc 把 focus_index 清成了 0 —— 那会让第一次 Tab 直接落到第二项。
     * 「还没有焦点」是 -1。 */
    shell->focus_index = -1;

    shell->app_state = desktop_app_state_create();
    if (shell->app_state == NULL) {
        free(shell);
        return NULL;
    }

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
    desktop_app_state_destroy(shell->app_state);
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
    if (shell == NULL) {
        return;
    }
    if (shell->clock_label != NULL) {
        char buf[16];
        format_clock(shell, buf, sizeof(buf));
        lv_label_set_text(shell->clock_label, buf);
    }
    /* 应用界面的刷新也在这里：按键回调只改状态。 */
    desktop_app_refresh(shell);
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

    /* 应用优先消费（终端的字符键）。 */
    if (desktop_app_handle_key(shell, ev)) {
        return true;
    }

    switch (ev->physical_key) {
    case 0x01: /* Esc */
        desktop_shell_request(shell, DESKTOP_INTENT_ESC, NULL);
        return true;
    case 0x0F: /* Tab */
        /* Shift+Tab 反向。 */
        focus_move(shell, (ev->modifiers & GUI_MOD_LSHIFT) == 0u);
        return true;
    case 0x1C: /* Enter */
        return focus_activate(shell);
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
    case DESKTOP_REGION_DRAWER:
    case DESKTOP_REGION_SETTINGS:
        /* 面板自身的表面：左上角内缩 16px，避开圆角弧（半径 12）。 */
        *x = 16 + 16;
        *y = (uint32_t)(STATUS_BAR_HEIGHT + 48 + 16);
        return true;
    case DESKTOP_REGION_LAUNCHER: {
        uint32_t count = desktop_app_launcher_count();
        int32_t bar_w = launcher_bar_width(count);
        /* 取样点必须避开圆角弧（半径 12）与图标（图标从 bar_x+28 开始）。 */
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
