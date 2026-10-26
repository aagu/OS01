/*
 * desktop_shell.c — 桌面外壳实现（GUI-3 / GUI-2b Task 10）
 *
 * 所有颜色 / 字号 / 间距 / 圆角一律取自 gui_theme_*，本文件不出现任何
 * 颜色字面量或字号字面量（宿主静态审计会检查这一点）。
 *
 * GUI-2b：本文件**只用 libgui 的公共 API**。
 *   - 控件身份是 `gui_widget_t`（值类型），不再持有任何后端指针；
 *   - 焦点由 libgui 的**唯一焦点引擎**持有：桌面只负责「登记」
 *     （`gui_focus_register`）与「请求移动」（`gui_focus_move`），
 *     不再维护第二套焦点序列，也不再手工投递 CLICKED / FOCUSED；
 *   - 几何、布局、外观分别由 `gui_widget_set_geometry` /
 *     `gui_widget_set_layout` / `gui_widget_set_appearance` 表达；
 *   - 表面根容器**自己**绘制壁纸，不再修改后端的 screen 对象。
 *
 * 几何常量集中在这里，构建与取样共用，保证「取样点落在目标区域内」。
 */

#include "desktop_shell.h"

#include <gui/gui_event.h>
#include <gui/gui_focus.h>
#include <gui/gui_input.h>
#include <gui/gui_layout.h>
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <gui/gui_widget.h>

#include <errno.h>
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

/* Set 1 扫描码 */
#define KEY_ESC   0x01
#define KEY_TAB   0x0F

/* ── 业务上下文 ──────────────────────────────────────────────────────
 *
 * 控件回调的 userdata **只**放业务上下文地址（不是值句柄，也不是后端
 * 指针）。上下文由外壳持有，生命周期覆盖它创建的全部控件。
 */

/* 启动器图标 / 抽屉行：打开某个应用。 */
typedef struct {
    desktop_shell_t *shell;
    desktop_app_id_t app_id;
} desktop_app_ctx_t;

/* 设置行：切换某个设置项。 */
typedef struct {
    desktop_shell_t *shell;
    desktop_setting_t setting;
} desktop_setting_ctx_t;

/*
 * 文件导航意图。
 *
 * 只**按值复制**意图：路径是完整副本，调用方的缓冲不被借用；意图本身也
 * 不延长任何控件或行上下文的寿命（行上下文在旧控件销毁之后才可复用）。
 */
typedef struct {
    bool     pending;                        /* 导航槽是否被占用 */
    char     path[DESKTOP_FILES_PATH_MAX];   /* 完整绝对路径副本 */
    uint64_t version;                        /* 发起方看到的目录版本 */
    uint64_t surface_version;                /* 请求时的表面版本 */
} desktop_nav_t;

/* ── 外壳状态 ────────────────────────────────────────────────────── */

struct desktop_shell {
    gui_platform_t *platform;
    gui_input_t   *input;   /* 借用：焦点引擎与系统应用都用它 */

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

    gui_widget_t root;        /* 当前表面的根控件 */
    gui_widget_t clock_label; /* 状态栏里的时钟 */

    /* 业务上下文：控件回调只拿业务数据。 */
    desktop_app_ctx_t     app_ctx[DESKTOP_APP_COUNT];
    desktop_setting_ctx_t setting_ctx[3];

    /* 时钟节流：时钟每秒更新一次，终端刷新不受它影响。 */
    time_t last_clock;

    /* 文件导航：意图槽与表面版本。目录版本由文件应用的状态持有。 */
    desktop_nav_t nav;
    uint64_t surface_version;
};

/* ── 令牌化外观辅助 ───────────────────────────────────────────────── */

static gui_appearance_t make_appearance(gui_variant_t variant,
                                        gui_color_role_t bg,
                                        gui_color_role_t text,
                                        gui_theme_font_t font,
                                        gui_theme_radius_t radius,
                                        gui_theme_elevation_t elevation,
                                        bool filled, bool rounded, bool bordered)
{
    gui_appearance_t ap;

    ap.variant = variant;
    ap.bg_role = bg;
    ap.text_role = text;
    ap.border_role = GUI_COLOR_OUTLINE_VARIANT;
    ap.font = font;
    ap.radius = radius;
    ap.elevation = elevation;
    ap.filled = filled;
    ap.rounded = rounded;
    ap.bordered = bordered;
    return ap;
}

/* elevation 层级 → 表面色角色（M3 的容器表面刻度）。 */
static gui_color_role_t elevation_role(gui_theme_elevation_t elevation)
{
    switch (elevation) {
    case GUI_THEME_ELEVATION_0: return GUI_COLOR_SURFACE;
    case GUI_THEME_ELEVATION_1: return GUI_COLOR_SURFACE_CONTAINER_LOW;
    case GUI_THEME_ELEVATION_2: return GUI_COLOR_SURFACE_CONTAINER;
    case GUI_THEME_ELEVATION_3: return GUI_COLOR_SURFACE_CONTAINER_HIGH;
    case GUI_THEME_ELEVATION_4:
    case GUI_THEME_ELEVATION_5:
    default:                    return GUI_COLOR_SURFACE_CONTAINER_HIGHEST;
    }
}

/* 表面卡片：底色 + 圆角 + 边框 + 阴影（elevation）。 */
static gui_appearance_t card_appearance(gui_theme_elevation_t elevation,
                                        gui_color_role_t bg_role)
{
    return make_appearance(GUI_VARIANT_CARD, bg_role, GUI_COLOR_ON_SURFACE,
                           GUI_THEME_FONT_BODY, GUI_THEME_RADIUS_LG, elevation,
                           true, true, true);
}

/* ── 控件构建辅助 ─────────────────────────────────────────────────── */

static gui_widget_t make_widget(desktop_shell_t *shell, gui_widget_t parent,
                                gui_widget_kind_t kind)
{
    gui_widget_t widget = GUI_WIDGET_NONE;

    if (gui_widget_create(shell->platform, parent, kind, &widget) != 0) {
        return GUI_WIDGET_NONE;
    }
    return widget;
}

static bool apply_appearance(desktop_shell_t *shell, gui_widget_t widget,
                             const gui_appearance_t *appearance)
{
    int rc = gui_widget_set_appearance(shell->platform, widget, appearance);
    return rc == 0;
}

/* 绝对几何：位置相对父控件的内容区左上角。 */
static bool set_geometry(desktop_shell_t *shell, gui_widget_t widget,
                         int32_t x, int32_t y, int32_t width, int32_t height)
{
    gui_geometry_t geometry;

    geometry.x = x;
    geometry.y = y;
    geometry.width = GUI_SIZE_PX(width);
    geometry.height = GUI_SIZE_PX(height);
    int rc = gui_widget_set_geometry(shell->platform, widget, &geometry);
    return rc == 0;
}

/* 只定位：尺寸由内容决定（标签的默认行为）。 */
static bool set_position(desktop_shell_t *shell, gui_widget_t widget,
                         int32_t x, int32_t y)
{
    gui_geometry_t geometry;

    geometry.x = x;
    geometry.y = y;
    geometry.width = GUI_SIZE_CONTENT;
    geometry.height = GUI_SIZE_CONTENT;
    int rc = gui_widget_set_geometry(shell->platform, widget, &geometry);
    return rc == 0;
}

/* 定位 + 固定宽度：高度由内容决定（折行的标签）。 */
static bool set_geometry_width(desktop_shell_t *shell, gui_widget_t widget,
                               int32_t x, int32_t y, int32_t width)
{
    gui_geometry_t geometry;

    geometry.x = x;
    geometry.y = y;
    geometry.width = GUI_SIZE_PX(width);
    geometry.height = GUI_SIZE_CONTENT;
    int rc = gui_widget_set_geometry(shell->platform, widget, &geometry);
    return rc == 0;
}

/* 锚点定位：偏移相对锚点计算，尺寸来源不动。 */
static bool set_alignment(desktop_shell_t *shell, gui_widget_t widget,
                          gui_align_t anchor, int32_t x_offset, int32_t y_offset)
{
    gui_alignment_t alignment;

    alignment.anchor = anchor;
    alignment.x_offset = x_offset;
    alignment.y_offset = y_offset;
    int rc = gui_widget_set_alignment(shell->platform, widget, &alignment);
    return rc == 0;
}

/*
 * 布局：只有 layout 控制 padding / gap。
 *
 * 控件创建时带的是**默认外观**，而外观不写内边距：主题的卡片样式会给
 * 容器 16px 内边距。桌面的每一处内边距都必须在这里显式给出，否则子项
 * 会贴边，与原桌面的像素取样不符。
 */
static bool set_layout_none(desktop_shell_t *shell, gui_widget_t widget,
                            int32_t pad_top, int32_t pad_bottom,
                            int32_t pad_left, int32_t pad_right)
{
    gui_layout_t layout;

    layout.flow = GUI_FLOW_NONE;
    layout.main_align = GUI_FLOW_ALIGN_START;
    layout.cross_align = GUI_FLOW_ALIGN_START;
    layout.grow = 0u;
    layout.gap = 0;
    layout.pad_top = pad_top;
    layout.pad_bottom = pad_bottom;
    layout.pad_left = pad_left;
    layout.pad_right = pad_right;
    int rc = gui_widget_set_layout(shell->platform, widget, &layout);
    return rc == 0;
}

static bool set_layout_column(desktop_shell_t *shell, gui_widget_t widget,
                              int32_t gap, int32_t pad_top, int32_t pad_bottom,
                              int32_t pad_left, int32_t pad_right)
{
    gui_layout_t layout;

    layout.flow = GUI_FLOW_COLUMN;
    layout.main_align = GUI_FLOW_ALIGN_START;
    layout.cross_align = GUI_FLOW_ALIGN_START;
    layout.grow = 0u;
    layout.gap = gap;
    layout.pad_top = pad_top;
    layout.pad_bottom = pad_bottom;
    layout.pad_left = pad_left;
    layout.pad_right = pad_right;
    int rc = gui_widget_set_layout(shell->platform, widget, &layout);
    return rc == 0;
}

/* 滚动：容器 / 按钮只使用滚动字段，文本字段必须是默认。 */
static bool set_scroll_both(desktop_shell_t *shell, gui_widget_t widget)
{
    gui_scroll_t scroll;

    scroll.horizontal = true;
    scroll.vertical = true;
    scroll.text_mode = GUI_TEXT_MODE_WRAP;
    scroll.text_align = GUI_TEXT_ALIGN_LEFT;
    int rc = gui_widget_set_scroll(shell->platform, widget, &scroll);
    return rc == 0;
}

static bool set_scroll_none(desktop_shell_t *shell, gui_widget_t widget)
{
    gui_scroll_t scroll;

    scroll.horizontal = false;
    scroll.vertical = false;
    scroll.text_mode = GUI_TEXT_MODE_WRAP;
    scroll.text_align = GUI_TEXT_ALIGN_LEFT;
    return gui_widget_set_scroll(shell->platform, widget, &scroll) == 0;
}

/* 标签的换行与对齐。 */
static bool set_label_scroll(desktop_shell_t *shell, gui_widget_t widget,
                             gui_text_mode_t mode, gui_text_align_t align)
{
    gui_scroll_t scroll;

    scroll.horizontal = false;
    scroll.vertical = false;
    scroll.text_mode = mode;
    scroll.text_align = align;
    int rc = gui_widget_set_scroll(shell->platform, widget, &scroll);
    return rc == 0;
}

static gui_widget_t make_label(desktop_shell_t *shell, gui_widget_t parent,
                               const char *text, gui_theme_font_t font,
                               gui_color_role_t color)
{
    gui_widget_t label = make_widget(shell, parent, GUI_WIDGET_LABEL);
    gui_appearance_t appearance;

    if (label.id == 0) {
        return GUI_WIDGET_NONE;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, GUI_COLOR_SURFACE, color,
                                 font, GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 false, false, false);
    if (!apply_appearance(shell, label, &appearance)) {
        return GUI_WIDGET_NONE;
    }
    if (!set_scroll_none(shell, label)) {
        return GUI_WIDGET_NONE;
    }
    if (text != NULL && gui_widget_set_text(shell->platform, label, text) != 0) {
        return GUI_WIDGET_NONE;
    }
    return label;
}

/* ── 焦点 ────────────────────────────────────────────────────────────
 *
 * 桌面**不**再维护焦点序列：登记与移动都交给 libgui 的唯一焦点引擎。
 * 登记一律用 STRONG：桌面上的每一个可聚焦项都是明确的键盘可达目标。
 */

static bool focus_register(desktop_shell_t *shell, gui_widget_t widget)
{
    if (widget.id == 0) {
        return false;
    }
    int rc = gui_focus_register(shell->input, widget, GUI_FOCUS_STRONG);
    return rc == 0;
}

/* 表面切换前的显式清空：必须成功，否则不动表面。 */
static bool focus_clear(desktop_shell_t *shell)
{
    return gui_focus_set(shell->input, GUI_WIDGET_NONE) == 0;
}

/* ── 控件回调 ─────────────────────────────────────────────────────── */

static void on_app_activate(const gui_event_t *event, void *userdata)
{
    desktop_app_ctx_t *ctx = (desktop_app_ctx_t *)userdata;

    (void)event;
    if (ctx == NULL || ctx->shell == NULL) {
        return;
    }
    desktop_shell_request(ctx->shell, DESKTOP_INTENT_OPEN_APP,
                          desktop_app_id_name(ctx->app_id));
}

static void on_setting_activate(const gui_event_t *event, void *userdata)
{
    desktop_setting_ctx_t *ctx = (desktop_setting_ctx_t *)userdata;

    (void)event;
    if (ctx == NULL || ctx->shell == NULL) {
        return;
    }
    if (ctx->setting == DESKTOP_SETTING_NONE) {
        return;
    }
    desktop_shell_request_setting(ctx->shell, ctx->setting);
}

/* ── 时钟 ──────────────────────────────────────────────────────────── */

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

/* ── 壁纸 ──────────────────────────────────────────────────────────── */

static gui_color_role_t wallpaper_role(const desktop_shell_t *shell)
{
    switch (shell->wallpaper) {
    case DESKTOP_WALLPAPER_SURFACE:            return GUI_COLOR_SURFACE;
    case DESKTOP_WALLPAPER_CONTAINER_LOW:      return GUI_COLOR_SURFACE_CONTAINER_LOW;
    case DESKTOP_WALLPAPER_CONTAINER_HIGH:     return GUI_COLOR_SURFACE_CONTAINER_HIGH;
    case DESKTOP_WALLPAPER_CONTAINER_HIGHEST:  return GUI_COLOR_SURFACE_CONTAINER_HIGHEST;
    case DESKTOP_WALLPAPER_SURFACE_DIM:
    default:                                   return GUI_COLOR_SURFACE_DIM;
    }
}

/*
 * 根容器是**透明底板**：ROOT 形态强制零填充、零边框、零圆角、零内边距，
 * 铺满整个视口。
 *
 * 壁纸因此由根的**图层**绘制（见 build_wallpaper），而不是修改后端的
 * screen 对象：后端的 screen 保持主题自己的底色，桌面不再去改它。
 */
static bool apply_root_appearance(desktop_shell_t *shell, gui_widget_t root)
{
    gui_appearance_t appearance =
        make_appearance(GUI_VARIANT_ROOT, GUI_COLOR_SURFACE,
                        GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                        GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                        false, false, false);

    if (!apply_appearance(shell, root, &appearance)) {
        return false;
    }
    return set_layout_none(shell, root, 0, 0, 0, 0);
}

/*
 * 壁纸图层：根的第一个子项，铺满根的内容区。
 *
 * 它是普通容器：不拦截命中（容器默认不可点击），也不登记焦点，因此它
 * 只贡献底色。
 */
static bool build_wallpaper(desktop_shell_t *shell, gui_widget_t root)
{
    gui_widget_t layer = make_widget(shell, root, GUI_WIDGET_CONTAINER);
    gui_appearance_t appearance;
    gui_geometry_t geometry;

    if (layer.id == 0) {
        return false;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, wallpaper_role(shell),
                                 GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 true, false, false);
    if (!apply_appearance(shell, layer, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, layer, 0, 0, 0, 0)) {
        return false;
    }
    if (!set_scroll_none(shell, layer)) {
        return false;
    }
    geometry.x = 0;
    geometry.y = 0;
    geometry.width = GUI_SIZE_FILL;
    geometry.height = GUI_SIZE_FILL;
    if (gui_widget_set_geometry(shell->platform, layer, &geometry) != 0) {
        return false;
    }
    return true;
}

/* ── 状态栏 ────────────────────────────────────────────────────────── */

static bool build_status_bar(desktop_shell_t *shell, gui_widget_t parent)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    gui_widget_t bar = make_widget(shell, parent, GUI_WIDGET_CONTAINER);
    gui_appearance_t appearance;

    if (bar.id == 0) {
        return false;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN,
                                 elevation_role(GUI_THEME_ELEVATION_1),
                                 GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 true, false, false);
    if (!apply_appearance(shell, bar, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, bar, 0, 0, 0, 0)) {
        return false;
    }
    if (!set_scroll_none(shell, bar)) {
        return false;
    }
    if (!set_geometry(shell, bar, 0, 0, w, STATUS_BAR_HEIGHT)) {
        return false;
    }

    /* 左侧：时钟 */
    shell->clock_label = make_label(shell, bar, "--:--", GUI_THEME_FONT_LABEL,
                                    GUI_COLOR_ON_SURFACE);
    if (shell->clock_label.id == 0) {
        return false;
    }
    if (!set_position(shell, shell->clock_label, 12, 9)) {
        return false;
    }

    /* 中间：系统名 */
    gui_widget_t sysname = make_label(shell, bar, "OS01", GUI_THEME_FONT_BODY,
                                      GUI_COLOR_ON_SURFACE);
    if (sysname.id == 0) {
        return false;
    }
    if (!set_alignment(shell, sysname, GUI_ALIGN_TOP_MID, 0, 8)) {
        return false;
    }

    /* 右侧：指示区 */
    gui_widget_t ind = make_label(shell, bar, "x86_64  lvgl", GUI_THEME_FONT_LABEL,
                                  GUI_COLOR_ON_SURFACE_VARIANT);
    if (ind.id == 0) {
        return false;
    }
    if (!set_alignment(shell, ind, GUI_ALIGN_TOP_RIGHT, -12, 9)) {
        return false;
    }

    /* 时钟立刻显示真实时间：它不等第一次 tick。 */
    {
        char buf[16];
        format_clock(shell, buf, sizeof(buf));
        if (gui_widget_set_text(shell->platform, shell->clock_label, buf) != 0) {
            return false;
        }
    }
    shell->last_clock = time(NULL);

    (void)c;
    return true;
}

/* ── 启动器 ────────────────────────────────────────────────────────── */

/* 启动器条的宽度：两侧留出应用名标签的余量（标签比图标宽）。 */
static int32_t launcher_bar_width(uint32_t count)
{
    return (int32_t)((LAUNCHER_PAD + LAUNCHER_CAPTION_OVERHANG) * 2 +
                     count * LAUNCHER_ICON +
                     (count ? (count - 1) * LAUNCHER_GAP : 0));
}

static bool build_launcher(desktop_shell_t *shell, gui_widget_t parent)
{
    uint32_t count = desktop_app_launcher_count();
    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    int32_t h = (int32_t)gui_platform_get_height(shell->platform);
    int32_t bar_w = launcher_bar_width(count);
    int32_t bar_x = (w - bar_w) / 2;
    int32_t bar_y = h - LAUNCHER_HEIGHT;

    gui_widget_t bar = make_widget(shell, parent, GUI_WIDGET_CONTAINER);
    gui_appearance_t appearance;

    if (bar.id == 0) {
        return false;
    }
    appearance = card_appearance(GUI_THEME_ELEVATION_2, GUI_COLOR_SURFACE_CONTAINER);
    if (!apply_appearance(shell, bar, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, bar, (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }
    if (!set_scroll_none(shell, bar)) {
        return false;
    }
    if (!set_geometry(shell, bar, bar_x, bar_y, bar_w, LAUNCHER_HEIGHT)) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        const desktop_app_info_t *info = desktop_app_launcher_entry(i);
        gui_widget_t icon;
        gui_widget_t glyph;
        gui_widget_t cap;

        if (info == NULL) {
            return false;
        }

        int32_t ix = (int32_t)(LAUNCHER_PAD + LAUNCHER_CAPTION_OVERHANG +
                               i * (LAUNCHER_ICON + LAUNCHER_GAP));

        /* 图标：强调色圆块 + 字形。图标是**动作**：它启动应用。 */
        icon = make_widget(shell, bar, GUI_WIDGET_BUTTON);
        if (icon.id == 0) {
            return false;
        }
        appearance = make_appearance(GUI_VARIANT_PLAIN,
                                     desktop_app_accent_role(info->id),
                                     GUI_COLOR_ON_PRIMARY, GUI_THEME_FONT_TITLE,
                                     GUI_THEME_RADIUS_FULL, GUI_THEME_ELEVATION_0,
                                     true, true, false);
        if (!apply_appearance(shell, icon, &appearance)) {
            return false;
        }
        if (!set_layout_none(shell, icon, 0, 0, 0, 0)) {
            return false;
        }
        if (!set_scroll_none(shell, icon)) {
            return false;
        }
        if (!set_geometry(shell, icon, ix, LAUNCHER_PAD, LAUNCHER_ICON,
                          LAUNCHER_ICON)) {
            return false;
        }

        glyph = make_label(shell, icon, info->glyph, GUI_THEME_FONT_TITLE,
                           GUI_COLOR_ON_PRIMARY);
        if (glyph.id == 0) {
            return false;
        }
        if (!set_alignment(shell, glyph, GUI_ALIGN_MID, 0, 0)) {
            return false;
        }

        /* 文字标签：图标下方的应用名。标签比图标宽一点并居中：应用名
         * （"Terminal" / "Settings"）比 48px 的图标还宽，固定成图标宽度
         * 会被截断。 */
        cap = make_label(shell, bar, info->name, GUI_THEME_FONT_LABEL,
                         GUI_COLOR_ON_SURFACE_VARIANT);
        if (cap.id == 0) {
            return false;
        }
        if (!set_label_scroll(shell, cap, GUI_TEXT_MODE_WRAP,
                              GUI_TEXT_ALIGN_CENTER)) {
            return false;
        }
        if (!set_geometry_width(shell, cap, ix - LAUNCHER_CAPTION_OVERHANG,
                               LAUNCHER_PAD + LAUNCHER_ICON + 4,
                               LAUNCHER_ICON + LAUNCHER_CAPTION_OVERHANG * 2)) {
            return false;
        }

        /* 点击图标 = 启动应用 */
        shell->app_ctx[info->id].shell = shell;
        shell->app_ctx[info->id].app_id = info->id;
        if (gui_widget_set_event_handler(shell->platform, icon, on_app_activate,
                                         &shell->app_ctx[info->id]) != 0) {
            return false;
        }

        /* 启动器图标也是键盘焦点项：Tab 可依次走到它们。 */
        if (!focus_register(shell, icon)) {
            return false;
        }
    }

    return true;
}

/* ── 表面构建 ──────────────────────────────────────────────────────── */

static bool build_home(desktop_shell_t *shell)
{
    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    gui_widget_t root = make_widget(shell, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER);
    gui_widget_t card;
    gui_widget_t title;
    gui_widget_t desc;
    gui_widget_t hint;
    gui_appearance_t appearance;

    if (root.id == 0) {
        return false;
    }
    shell->surface_version++;
    shell->root = root;
    shell->clock_label = GUI_WIDGET_NONE;

    if (!apply_root_appearance(shell, root)) {
        return false;
    }
    if (!build_wallpaper(shell, root)) {
        return false;
    }
    if (!build_status_bar(shell, root)) {
        return false;
    }

    /* 欢迎卡片 */
    card = make_widget(shell, root, GUI_WIDGET_CONTAINER);
    if (card.id == 0) {
        return false;
    }
    appearance = card_appearance(GUI_THEME_ELEVATION_2, GUI_COLOR_SURFACE_CONTAINER);
    if (!apply_appearance(shell, card, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, card, (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }
    if (!set_scroll_none(shell, card)) {
        return false;
    }
    if (!set_geometry(shell, card, (w - CARD_WIDTH) / 2, CARD_TOP, CARD_WIDTH,
                      CARD_HEIGHT)) {
        return false;
    }

    title = make_label(shell, card, "OS01", GUI_THEME_FONT_DISPLAY,
                       GUI_COLOR_ON_SURFACE);
    if (title.id == 0) {
        return false;
    }
    if (!set_position(shell, title, 16, 14)) {
        return false;
    }

    desc = make_label(shell, card, "Material single-process desktop shell",
                      GUI_THEME_FONT_BODY, GUI_COLOR_ON_SURFACE_VARIANT);
    if (desc.id == 0) {
        return false;
    }
    if (!set_position(shell, desc, 16, 52)) {
        return false;
    }

    hint = make_label(shell, card,
                      "Click a launcher icon to open an app; ESC returns home",
                      GUI_THEME_FONT_LABEL, GUI_COLOR_ON_SURFACE_VARIANT);
    if (hint.id == 0) {
        return false;
    }
    if (!set_label_scroll(shell, hint, GUI_TEXT_MODE_WRAP, GUI_TEXT_ALIGN_LEFT)) {
        return false;
    }
    if (!set_geometry_width(shell, hint, 16, 80, CARD_WIDTH - 32)) {
        return false;
    }

    if (!build_launcher(shell, root)) {
        return false;
    }
    return true;
}

/* ── 应用抽屉 ──────────────────────────────────────────────────────── */

static bool build_drawer(desktop_shell_t *shell)
{
    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    int32_t h = (int32_t)gui_platform_get_height(shell->platform);
    gui_widget_t root = make_widget(shell, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER);
    gui_widget_t title;
    gui_widget_t count;
    gui_widget_t panel;
    gui_widget_t list;
    gui_widget_t hint;
    gui_appearance_t appearance;
    char buf[32];

    if (root.id == 0) {
        return false;
    }
    shell->surface_version++;
    shell->root = root;
    shell->clock_label = GUI_WIDGET_NONE;

    if (!apply_root_appearance(shell, root)) {
        return false;
    }
    if (!build_wallpaper(shell, root)) {
        return false;
    }
    if (!build_status_bar(shell, root)) {
        return false;
    }

    title = make_label(shell, root, "Apps", GUI_THEME_FONT_HEADLINE,
                       GUI_COLOR_ON_SURFACE);
    if (title.id == 0) {
        return false;
    }
    if (!set_position(shell, title, 16, STATUS_BAR_HEIGHT + 12)) {
        return false;
    }

    count = make_label(shell, root, "", GUI_THEME_FONT_LABEL,
                       GUI_COLOR_ON_SURFACE_VARIANT);
    if (count.id == 0) {
        return false;
    }
    snprintf(buf, sizeof(buf), "%u apps", desktop_app_count());
    if (gui_widget_set_text(shell->platform, count, buf) != 0) {
        return false;
    }
    if (!set_alignment(shell, count, GUI_ALIGN_TOP_RIGHT, -16,
                       STATUS_BAR_HEIGHT + 16)) {
        return false;
    }

    /* 抽屉面板：elevation 3（浮层）。面板留出内边距，取样点落在面板本身。 */
    panel = make_widget(shell, root, GUI_WIDGET_CONTAINER);
    if (panel.id == 0) {
        return false;
    }
    appearance = card_appearance(GUI_THEME_ELEVATION_3, GUI_COLOR_SURFACE_CONTAINER_HIGH);
    if (!apply_appearance(shell, panel, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, panel, (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }
    if (!set_scroll_none(shell, panel)) {
        return false;
    }
    if (!set_geometry(shell, panel, 16, STATUS_BAR_HEIGHT + 48, w - 32,
                      h - STATUS_BAR_HEIGHT - 64)) {
        return false;
    }

    /* 可滚动列表：纵向排列，行高自然撑开内容高度。 */
    list = make_widget(shell, panel, GUI_WIDGET_CONTAINER);
    if (list.id == 0) {
        return false;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, GUI_COLOR_SURFACE,
                                 GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 false, false, false);
    if (!apply_appearance(shell, list, &appearance)) {
        return false;
    }
    if (!set_layout_column(shell, list, (int32_t)gui_theme_space(GUI_THEME_SPACE_SM),
                           0, 0, 0, 0)) {
        return false;
    }
    if (!set_scroll_both(shell, list)) {
        return false;
    }
    if (!set_geometry(shell, list, 0, 0,
                      w - 32 - 2 * (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                      h - STATUS_BAR_HEIGHT - 64 -
                          2 * (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }

    for (uint32_t i = 0; i < desktop_app_count(); i++) {
        desktop_app_id_t id = (desktop_app_id_t)i;
        const desktop_app_info_t *info = desktop_app_info(id);
        gui_widget_t row;
        gui_widget_t icon;
        gui_widget_t glyph;
        gui_widget_t name;
        gui_widget_t desc;

        if (info == NULL) {
            return false;
        }

        row = make_widget(shell, list, GUI_WIDGET_BUTTON);
        if (row.id == 0) {
            return false;
        }
        appearance = card_appearance(GUI_THEME_ELEVATION_1,
                                     GUI_COLOR_SURFACE_CONTAINER_LOW);
        if (!apply_appearance(shell, row, &appearance)) {
            return false;
        }
        if (!set_layout_none(shell, row, (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                             (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                             (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                             (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
            return false;
        }
        if (!set_scroll_none(shell, row)) {
            return false;
        }
        if (!set_geometry(shell, row, 0, 0, w - 32, 64)) {
            return false;
        }

        /* 图标：装饰性容器，不拦截命中（点击落在行按钮上）。 */
        icon = make_widget(shell, row, GUI_WIDGET_CONTAINER);
        if (icon.id == 0) {
            return false;
        }
        appearance = make_appearance(GUI_VARIANT_PLAIN,
                                     desktop_app_accent_role(id),
                                     GUI_COLOR_ON_PRIMARY, GUI_THEME_FONT_TITLE,
                                     GUI_THEME_RADIUS_FULL, GUI_THEME_ELEVATION_0,
                                     true, true, false);
        if (!apply_appearance(shell, icon, &appearance)) {
            return false;
        }
        if (!set_layout_none(shell, icon, 0, 0, 0, 0)) {
            return false;
        }
        if (!set_scroll_none(shell, icon)) {
            return false;
        }
        if (!set_geometry(shell, icon, 12, 12, 40, 40)) {
            return false;
        }

        glyph = make_label(shell, icon, info->glyph, GUI_THEME_FONT_TITLE,
                           GUI_COLOR_ON_PRIMARY);
        if (glyph.id == 0) {
            return false;
        }
        if (!set_alignment(shell, glyph, GUI_ALIGN_MID, 0, 0)) {
            return false;
        }

        name = make_label(shell, row, info->name, GUI_THEME_FONT_TITLE,
                          GUI_COLOR_ON_SURFACE);
        if (name.id == 0) {
            return false;
        }
        if (!set_position(shell, name, 64, 10)) {
            return false;
        }

        desc = make_label(shell, row, info->desc, GUI_THEME_FONT_BODY,
                          GUI_COLOR_ON_SURFACE_VARIANT);
        if (desc.id == 0) {
            return false;
        }
        if (!set_position(shell, desc, 64, 34)) {
            return false;
        }

        shell->app_ctx[id].shell = shell;
        shell->app_ctx[id].app_id = id;
        if (gui_widget_set_event_handler(shell->platform, row, on_app_activate,
                                         &shell->app_ctx[id]) != 0) {
            return false;
        }

        /* 抽屉条目也是键盘焦点项：Tab 选中后回车即可启动应用。 */
        if (!focus_register(shell, row)) {
            return false;
        }
    }

    hint = make_label(shell, root, "ESC returns home", GUI_THEME_FONT_LABEL,
                      GUI_COLOR_ON_SURFACE_VARIANT);
    if (hint.id == 0) {
        return false;
    }
    if (!set_alignment(shell, hint, GUI_ALIGN_BOTTOM_LEFT, 16, -12)) {
        return false;
    }

    return true;
}

/* ── 基本设置 ──────────────────────────────────────────────────────── */

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

/* 设置项在设置数组里的下标：上下文与行一一对应。 */
static int setting_slot(desktop_setting_t setting)
{
    switch (setting) {
    case DESKTOP_SETTING_CLOCK_FORMAT: return 0;
    case DESKTOP_SETTING_WALLPAPER:    return 1;
    case DESKTOP_SETTING_DENSITY:      return 2;
    case DESKTOP_SETTING_NONE:
    default:                           return -1;
    }
}

static bool make_setting_row(desktop_shell_t *shell, gui_widget_t list,
                             const char *name, const char *value,
                             desktop_setting_t setting)
{
    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    gui_widget_t row = make_widget(shell, list, GUI_WIDGET_BUTTON);
    gui_widget_t label;
    gui_widget_t val;
    gui_appearance_t appearance;
    int slot = setting_slot(setting);

    if (row.id == 0 || slot < 0) {
        return false;
    }
    appearance = card_appearance(GUI_THEME_ELEVATION_1,
                                 GUI_COLOR_SURFACE_CONTAINER_LOW);
    if (!apply_appearance(shell, row, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, row, (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                         (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                         (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                         (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }
    if (!set_scroll_none(shell, row)) {
        return false;
    }
    if (!set_geometry(shell, row, 0, 0, w - 32, 56)) {
        return false;
    }

    label = make_label(shell, row, name, GUI_THEME_FONT_TITLE,
                       GUI_COLOR_ON_SURFACE);
    if (label.id == 0) {
        return false;
    }
    if (!set_position(shell, label, 16, 18)) {
        return false;
    }

    /* 右侧的当前值：点击整行即切换。 */
    val = make_label(shell, row, value, GUI_THEME_FONT_BODY, GUI_COLOR_PRIMARY);
    if (val.id == 0) {
        return false;
    }
    if (!set_alignment(shell, val, GUI_ALIGN_MID_RIGHT, -16, 0)) {
        return false;
    }

    shell->setting_ctx[slot].shell = shell;
    shell->setting_ctx[slot].setting = setting;
    if (gui_widget_set_event_handler(shell->platform, row, on_setting_activate,
                                     &shell->setting_ctx[slot]) != 0) {
        return false;
    }

    /* 设置项也是键盘焦点项：Tab 选中后回车即可切换。 */
    return focus_register(shell, row);
}

/*
 * 设置项的**唯一实现**：设置表面与设置应用都调用它。
 *
 * 这样「设置表面」与「设置应用」不是两套界面，而是同一份界面的两个入口：
 * 读同一份 shell 设置状态，写同一条投递通道。
 */
static bool build_settings_rows(desktop_shell_t *shell, gui_widget_t list)
{
    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    gui_widget_t about;
    gui_widget_t name;
    gui_widget_t detail;
    gui_appearance_t appearance;

    if (list.id == 0) {
        return false;
    }

    if (!make_setting_row(shell, list, "Clock format",
                           shell->clock_24h ? "24-hour" : "12-hour",
                           DESKTOP_SETTING_CLOCK_FORMAT)) {
        return false;
    }
    if (!make_setting_row(shell, list, "Wallpaper colour", wallpaper_label(shell),
                           DESKTOP_SETTING_WALLPAPER)) {
        return false;
    }
    if (!make_setting_row(shell, list, "Card density", density_label(shell),
                           DESKTOP_SETTING_DENSITY)) {
        return false;
    }

    /* 「关于」是只读展示，不可点击。 */
    about = make_widget(shell, list, GUI_WIDGET_CONTAINER);
    if (about.id == 0) {
        return false;
    }
    appearance = card_appearance(GUI_THEME_ELEVATION_1,
                                 GUI_COLOR_SURFACE_CONTAINER_LOW);
    if (!apply_appearance(shell, about, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, about, (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                         (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                         (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                         (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }
    if (!set_scroll_none(shell, about)) {
        return false;
    }
    if (!set_geometry(shell, about, 0, 0, w - 32, 88)) {
        return false;
    }

    name = make_label(shell, about, "About", GUI_THEME_FONT_TITLE,
                      GUI_COLOR_ON_SURFACE);
    if (name.id == 0) {
        return false;
    }
    if (!set_position(shell, name, 16, 14)) {
        return false;
    }

    detail = make_label(shell, about,
                        "OS01 Material desktop shell\n"
                        "Kernel x86_64 / LVGL 9 / libgui",
                        GUI_THEME_FONT_BODY, GUI_COLOR_ON_SURFACE_VARIANT);
    if (detail.id == 0) {
        return false;
    }
    if (!set_label_scroll(shell, detail, GUI_TEXT_MODE_WRAP, GUI_TEXT_ALIGN_LEFT)) {
        return false;
    }
    if (!set_position(shell, detail, 16, 40)) {
        return false;
    }

    return true;
}

static bool build_settings(desktop_shell_t *shell)
{
    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    int32_t h = (int32_t)gui_platform_get_height(shell->platform);
    gui_widget_t root = make_widget(shell, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER);
    gui_widget_t title;
    gui_widget_t panel;
    gui_widget_t list;
    gui_appearance_t appearance;

    if (root.id == 0) {
        return false;
    }
    shell->surface_version++;
    shell->root = root;
    shell->clock_label = GUI_WIDGET_NONE;

    if (!apply_root_appearance(shell, root)) {
        return false;
    }
    if (!build_wallpaper(shell, root)) {
        return false;
    }
    if (!build_status_bar(shell, root)) {
        return false;
    }

    title = make_label(shell, root, "Settings", GUI_THEME_FONT_HEADLINE,
                       GUI_COLOR_ON_SURFACE);
    if (title.id == 0) {
        return false;
    }
    if (!set_position(shell, title, 16, STATUS_BAR_HEIGHT + 12)) {
        return false;
    }

    /* 设置面板：elevation 2（卡片层）。 */
    panel = make_widget(shell, root, GUI_WIDGET_CONTAINER);
    if (panel.id == 0) {
        return false;
    }
    appearance = card_appearance(GUI_THEME_ELEVATION_2, GUI_COLOR_SURFACE_CONTAINER);
    if (!apply_appearance(shell, panel, &appearance)) {
        return false;
    }
    if (!set_layout_none(shell, panel, (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                          (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }
    if (!set_scroll_none(shell, panel)) {
        return false;
    }
    if (!set_geometry(shell, panel, 16, STATUS_BAR_HEIGHT + 48, w - 32,
                      h - STATUS_BAR_HEIGHT - 64)) {
        return false;
    }

    list = make_widget(shell, panel, GUI_WIDGET_CONTAINER);
    if (list.id == 0) {
        return false;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, GUI_COLOR_SURFACE,
                                 GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 false, false, false);
    if (!apply_appearance(shell, list, &appearance)) {
        return false;
    }
    if (!set_layout_column(shell, list, (int32_t)gui_theme_space(GUI_THEME_SPACE_SM),
                           0, 0, 0, 0)) {
        return false;
    }
    if (!set_scroll_both(shell, list)) {
        return false;
    }
    if (!set_geometry(shell, list, 0, 0,
                      w - 32 - 2 * (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                      h - STATUS_BAR_HEIGHT - 64 -
                          2 * (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
        return false;
    }

    return build_settings_rows(shell, list);
}

/*
 * 设置**应用**的设置体：与应用表面共用同一份设置项实现。
 *
 * 与设置表面的差别只在承载方式：应用表面自带顶栏（返回 + 标题），
 * 因此设置体从顶栏之下开始，没有状态栏也没有标题行。
 */
gui_widget_t desktop_shell_build_settings_body(desktop_shell_t *shell,
                                                gui_widget_t parent)
{
    int32_t w;
    int32_t h;
    gui_widget_t list;
    gui_widget_t hint;
    gui_appearance_t appearance;

    if (shell == NULL || parent.id == 0) {
        return GUI_WIDGET_NONE;
    }
    w = (int32_t)gui_platform_get_width(shell->platform);
    h = (int32_t)gui_platform_get_height(shell->platform);

    list = make_widget(shell, parent, GUI_WIDGET_CONTAINER);
    if (list.id == 0) {
        return GUI_WIDGET_NONE;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, GUI_COLOR_SURFACE,
                                 GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 false, false, false);
    if (!apply_appearance(shell, list, &appearance)) {
        return GUI_WIDGET_NONE;
    }
    if (!set_layout_column(shell, list, (int32_t)gui_theme_space(GUI_THEME_SPACE_SM),
                           0, 0, 0, 0)) {
        return GUI_WIDGET_NONE;
    }
    if (!set_scroll_both(shell, list)) {
        return GUI_WIDGET_NONE;
    }
    if (!set_geometry(shell, list, 16, DESKTOP_APP_BAR_HEIGHT + 16,
                      w - 32, h - DESKTOP_APP_BAR_HEIGHT - 32)) {
        return GUI_WIDGET_NONE;
    }

    if (!build_settings_rows(shell, list)) {
        return GUI_WIDGET_NONE;
    }

    /* 设置应用里的提示：与设置表面共用同一份配置。 */
    hint = make_label(shell, parent, "Shares one configuration with the home screen",
                      GUI_THEME_FONT_LABEL, GUI_COLOR_ON_SURFACE_VARIANT);
    if (hint.id == 0) {
        return GUI_WIDGET_NONE;
    }
    if (!set_alignment(shell, hint, GUI_ALIGN_BOTTOM_LEFT, 16, -10)) {
        return GUI_WIDGET_NONE;
    }

    return list;
}

/* ── 迁移执行 ──────────────────────────────────────────────────────── */

static void apply_pending_setting(desktop_shell_t *shell, desktop_setting_t setting);

/* 销毁当前表面的控件树。焦点已在调用它之前显式清空。 */
static void destroy_surface(desktop_shell_t *shell)
{
    if (shell->root.id != 0) {
        (void)gui_widget_destroy(shell->platform, shell->root);
        shell->root = GUI_WIDGET_NONE;
    }
    shell->clock_label = GUI_WIDGET_NONE;

    /* 界面句柄随控件树一起没了：应用状态里存的句柄必须同时失效。 */
    if (shell->app_state != NULL) {
        shell->app_state->files_list = GUI_WIDGET_NONE;
        shell->app_state->term_output_label = GUI_WIDGET_NONE;
        shell->app_state->term_input_label = GUI_WIDGET_NONE;
    }
}

/* 构建当前应用的全屏表面（应用自己构造界面，外壳只持有根控件）。 */
static bool build_app_surface(desktop_shell_t *shell)
{
    desktop_app_id_t id = desktop_shell_current_app_id(shell);
    gui_widget_t root;

    if (id == DESKTOP_APP_COUNT) {
        return false;
    }
    root = make_widget(shell, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER);
    if (root.id == 0) {
        return false;
    }
    shell->surface_version++;
    shell->root = root;
    shell->clock_label = GUI_WIDGET_NONE;

    if (!apply_root_appearance(shell, root)) {
        return false;
    }
    if (!build_wallpaper(shell, root)) {
        return false;
    }
    if (desktop_app_build(shell->platform, id, shell, root).id == 0) {
        return false;
    }
    return true;
}

/* 重建当前表面（设置项变更时用）。 */
static bool rebuild_surface(desktop_shell_t *shell)
{
    desktop_state_t state = shell->state;
    bool ok;

    switch (state) {
    case DESKTOP_STATE_HOME:     ok = build_home(shell);     break;
    case DESKTOP_STATE_DRAWER:   ok = build_drawer(shell);   break;
    case DESKTOP_STATE_SETTINGS: ok = build_settings(shell); break;
    case DESKTOP_STATE_APP:      ok = build_app_surface(shell); break;
    default:                     ok = build_home(shell);     break;
    }
    return ok;
}

/*
 * 执行一次文件导航意图（分期导航的外层）。
 *
 *   第一步：读快照（纯 C，不改变 cwd / 控件 / context / 焦点）。
 *   第二步：确定新版本（单调增加）。
 *   第三步：不可逆提交（销毁旧列表子树、释放旧行 context、建新行并登记）。
 *
 * 打不开或路径错误时**完整保留旧视图**：旧树还在原位，焦点不动。
 */
static int apply_file_nav(desktop_shell_t *shell)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    desktop_files_snapshot_t snapshot;
    uint64_t new_version;

    /* 外层验证：仍是同一个文件表面、且版本未变。 */
    if (shell->state != DESKTOP_STATE_APP ||
        desktop_shell_current_app_id(shell) != DESKTOP_APP_FILES) {
        return 0;
    }
    if (state == NULL) {
        return 0;
    }
    if (shell->nav.surface_version != shell->surface_version) {
        return 0;   /* 表面已经换过：丢弃 */
    }
    if (shell->nav.version != state->dir_version) {
        return 0;   /* 陈旧请求：旧行不能引用新的条目索引 */
    }

    /* 第一步：读快照。 */
    if (desktop_files_read_entries(shell->nav.path, &snapshot) != 0) {
        return -1;  /* 旧视图完整保留 */
    }

    /* 第二步：确定新版本。 */
    new_version = state->dir_version + 1u;

    /* 第三步：不可逆提交。 */
    if (desktop_files_replace_list(shell, &snapshot, new_version) != 0) {
        return -1;
    }
    return 0;
}

int desktop_shell_apply_pending(desktop_shell_t *shell)
{
    if (shell == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 设置项变更先处理：它不改变表面状态，但要重建当前表面。 */
    if (shell->pending_setting != DESKTOP_SETTING_NONE) {
        desktop_setting_t setting = shell->pending_setting;
        shell->pending_setting = DESKTOP_SETTING_NONE;
        shell->nav.pending = false;   /* 表面重建：导航槽清掉 */
        apply_pending_setting(shell, setting);
        return 0;
    }

    if (shell->pending == DESKTOP_INTENT_NONE) {
        /* 没有表面切换：处理导航意图。 */
        if (shell->nav.pending) {
            shell->nav.pending = false;
            return apply_file_nav(shell);
        }
        return 0;
    }

    desktop_intent_t intent = shell->pending;
    char app_id[DESKTOP_APP_ID_MAX];
    snprintf(app_id, sizeof(app_id), "%s", shell->pending_app);

    shell->pending = DESKTOP_INTENT_NONE;
    shell->pending_app[0] = '\0';

    desktop_state_t next = desktop_state_next(shell->state, intent, app_id);
    if (next == DESKTOP_STATE_INVALID) {
        /* 非法迁移被忽略：状态机已经把它判为「不允许」。 */
        return 0;
    }
    /* 表面切换优先：导航槽清掉。 */
    shell->nav.pending = false;

    if (next == shell->state) {
        return 0;
    }

    /*
     * 整表面切换：先把焦点**显式清空**，再删旧根、构建新表面。
     *
     * 显式清空的状态跨新登记保留：上一表面的 ever_selected / repair_pending
     * 不会让 MANUAL 在重建后的主屏上自动聚焦。
     */
    if (!focus_clear(shell)) {
        return 0;
    }
    destroy_surface(shell);
    shell->state = next;

    switch (next) {
    case DESKTOP_STATE_HOME:
        snprintf(shell->current_app, sizeof(shell->current_app), "%s", "");
        (void)build_home(shell);
        break;
    case DESKTOP_STATE_DRAWER:
        (void)build_drawer(shell);
        break;
    case DESKTOP_STATE_SETTINGS:
        (void)build_settings(shell);
        break;
    case DESKTOP_STATE_APP: {
        snprintf(shell->current_app, sizeof(shell->current_app), "%s", app_id);
        /* 应用表面由应用自己构造：它占满整个显示区。 */
        (void)build_app_surface(shell);
        break;
    }
    default:
        break;
    }
    return 0;
}

/* ── 设置项变更 ────────────────────────────────────────────────────── */

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
    if (!focus_clear(shell)) {
        return;
    }
    destroy_surface(shell);
    (void)rebuild_surface(shell);
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

int desktop_shell_request_file_nav(desktop_shell_t *shell,
                                   const char *absolute_path, uint64_t version)
{
    if (shell == NULL || absolute_path == NULL || absolute_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    /* 只接受绝对路径。 */
    if (absolute_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    /* 路径副本必须放得下：截断即拒绝。 */
    if (strlen(absolute_path) + 1 > sizeof(shell->nav.path)) {
        errno = EINVAL;
        return -1;
    }
    /* 同一轮里第一个导航意图占槽：后到的请求被忽略，不覆盖。 */
    if (shell->nav.pending) {
        return 0;
    }

    snprintf(shell->nav.path, sizeof(shell->nav.path), "%s", absolute_path);
    shell->nav.version = version;
    shell->nav.surface_version = shell->surface_version;
    shell->nav.pending = true;
    return 0;
}

uint64_t desktop_shell_dir_version(const desktop_shell_t *shell)
{
    desktop_app_state_t *state;

    if (shell == NULL) {
        return 0u;
    }
    state = desktop_shell_app_state((desktop_shell_t *)shell);
    return state ? state->dir_version : 0u;
}

int desktop_shell_files_cwd(const desktop_shell_t *shell, char *buf, size_t len)
{
    desktop_app_state_t *state;

    if (shell == NULL || buf == NULL || len == 0) {
        errno = EINVAL;
        return -1;
    }
    state = desktop_shell_app_state((desktop_shell_t *)shell);
    if (state == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strlen(state->files_cwd) + 1 > len) {
        errno = EINVAL;
        return -1;
    }
    snprintf(buf, len, "%s", state->files_cwd);
    return 0;
}

void desktop_shell_request_setting(desktop_shell_t *shell, desktop_setting_t setting)
{
    if (shell == NULL || setting == DESKTOP_SETTING_NONE) {
        return;
    }
    shell->pending_setting = setting;
}

/* ── 生命周期 ──────────────────────────────────────────────────────── */

desktop_shell_t *desktop_shell_create(gui_platform_t *platform, gui_input_t *input)
{
    desktop_shell_t *shell;

    if (platform == NULL || input == NULL) {
        errno = EINVAL;
        return NULL;
    }

    /*
     * 初始焦点策略必须在**任何**控件登记之前设定：MANUAL 表示主屏建好
     * 之后没有焦点，第一次按 Tab 才落到第一个可聚焦项。
     */
    if (gui_focus_set_initial_policy(input, GUI_FOCUS_INITIAL_MANUAL) != 0) {
        return NULL;
    }

    shell = (desktop_shell_t *)calloc(1, sizeof(*shell));
    if (shell == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    shell->platform = platform;
    shell->input = input;
    shell->root = GUI_WIDGET_NONE;
    shell->clock_label = GUI_WIDGET_NONE;
    shell->state = DESKTOP_STATE_HOME;
    shell->pending = DESKTOP_INTENT_NONE;
    shell->wallpaper = DESKTOP_WALLPAPER_SURFACE_DIM;
    shell->density = DESKTOP_DENSITY_COMFORTABLE;
    shell->clock_24h = true;
    shell->pending_setting = DESKTOP_SETTING_NONE;
    shell->start_time = time(NULL);

    shell->app_state = desktop_app_state_create();
    if (shell->app_state == NULL) {
        free(shell);
        errno = ENOMEM;
        return NULL;
    }
    /* 返回按钮的业务上下文：只借外壳。 */
    shell->app_state->back_ctx.shell = shell;

    if (!build_home(shell)) {
        /*
         * 构造失败：销毁整棵未完成的树并保留 errno。
         *
         * 这一步同时把「输入与平台不匹配」变成一次干净的失败：焦点登记
         * 在归属检查里被拒（EINVAL），主屏建不起来。
         */
        int saved = errno ? errno : EINVAL;
        (void)gui_widget_destroy(shell->platform, shell->root);
        shell->root = GUI_WIDGET_NONE;
        desktop_app_state_destroy(shell->app_state);
        free(shell);
        errno = saved;
        return NULL;
    }

    return shell;
}

void desktop_shell_destroy(desktop_shell_t *shell)
{
    if (shell == NULL) {
        return;
    }
    (void)focus_clear(shell);
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

gui_widget_t desktop_shell_surface_root(const desktop_shell_t *shell)
{
    return shell ? shell->root : GUI_WIDGET_NONE;
}

time_t desktop_shell_start_time(const desktop_shell_t *shell)
{
    return shell ? shell->start_time : 0;
}

desktop_app_id_t desktop_shell_current_app_id(const desktop_shell_t *shell)
{
    if (shell == NULL || shell->state != DESKTOP_STATE_APP) {
        return DESKTOP_APP_COUNT;
    }
    desktop_app_id_t id = DESKTOP_APP_COUNT;
    if (desktop_app_id_from_name(shell->current_app, &id)) {
        return id;
    }
    return DESKTOP_APP_COUNT;
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

    /*
     * 应用界面的刷新**无条件**进行：终端的输入与输出必须每轮都更新，
     * 不能被时钟的节流挡住。
     */
    desktop_app_refresh(shell);

    /* 时钟单独节流：一秒一次。 */
    time_t now = time(NULL);
    if (now == shell->last_clock) {
        return;
    }
    shell->last_clock = now;
    if (shell->clock_label.id != 0) {
        char buf[16];
        format_clock(shell, buf, sizeof(buf));
        (void)gui_widget_set_text(shell->platform, shell->clock_label, buf);
    }
}

bool desktop_shell_handle_key(desktop_shell_t *shell, const gui_key_event_t *ev)
{
    if (shell == NULL || ev == NULL) {
        return false;
    }

    /* 只处理按下边沿；重复与释放交给 libgui 的输入层。 */
    if (ev->action != GUI_KEY_DOWN) {
        return false;
    }

    /* 应用优先消费（终端的字符键）。 */
    if (desktop_app_handle_key(shell, ev)) {
        return true;
    }

    switch (ev->physical_key) {
    case KEY_ESC:
        desktop_shell_request(shell, DESKTOP_INTENT_ESC, NULL);
        return true;
    case KEY_TAB:
        /*
         * Tab 由**唯一焦点引擎**移动：桌面只提出方向，不持有焦点序列。
         * Shift+Tab 反向。
         */
        {
            bool forward =
                (ev->modifiers & (uint16_t)(GUI_MOD_LSHIFT | GUI_MOD_RSHIFT)) == 0u;
            (void)gui_focus_move(shell->input,
                                 forward ? GUI_FOCUS_FORWARD : GUI_FOCUS_BACKWARD);
        }
        return true;
    default:
        break;
    }

    /*
     * Enter **不**被消费：焦点引擎的组把 Enter 交给当前焦点按钮，
     * 控件的一次真实 down/up 产生一次 ACTIVATE。
     */
    return false;
}

bool desktop_shell_probe_point(const desktop_shell_t *shell, desktop_region_t region,
                                uint32_t *x, uint32_t *y)
{
    if (shell == NULL || x == NULL || y == NULL) {
        return false;
    }

    int32_t w = (int32_t)gui_platform_get_width(shell->platform);
    int32_t h = (int32_t)gui_platform_get_height(shell->platform);

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
