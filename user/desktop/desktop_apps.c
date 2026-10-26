/*
 * desktop_apps.c — 桌面内置应用实现（GUI-3 / GUI-2b Task 10）
 *
 * 四个应用都是**壳内应用**：界面由外壳创建与销毁，占满整个显示区；
 * 不 fork 子进程、不接管显示所有权。
 *
 *   文件   —— 只读目录浏览（opendir / readdir），目录在前文件在后
 *   终端   —— 壳内命令台，内建命令，不 fork
 *   系统   —— 内核与运行时只读信息
 *   设置   —— 与设置表面共用同一份实现
 *
 * 样式一律取自 gui_theme_*，本文件不出现颜色字面量与字号字面量。
 *
 * GUI-2b：只用 libgui 的公共 API —— 控件身份是 gui_widget_t，不出现任何
 * 后端类型；可聚焦项由 libgui 的唯一焦点引擎登记。
 */

#include "desktop_apps.h"

#include "desktop_registry.h"
#include "desktop_shell.h"
#include "desktop_state.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 构建 profile 由构建系统注入；宿主测试与其它构建方式下退化为 unknown。 */
#ifndef OS01_BUILD_PROFILE
#define OS01_BUILD_PROFILE "unknown"
#endif

/* 架构名：编译期决定。 */
#if defined(__x86_64__)
#define APP_ARCH "x86_64"
#elif defined(__aarch64__)
#define APP_ARCH "aarch64"
#elif defined(__i386__)
#define APP_ARCH "i386"
#else
#define APP_ARCH "unknown"
#endif

/* Set 1 扫描码 */
#define KEY_ESC        0x01
#define KEY_BACKSPACE  0x0E
#define KEY_ENTER      0x1C
#define KEY_UP         0x48
#define KEY_DOWN       0x50

/* 终端提示符 */
#define TERM_PROMPT "OS01 $ "

/* 应用内容区的左右边距 */
#define APP_MARGIN 16

/* ── 令牌化样式辅助 ─────────────────────────────────────────────── */

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

static gui_appearance_t card_appearance(gui_theme_elevation_t elevation,
                                        gui_color_role_t bg_role)
{
    return make_appearance(GUI_VARIANT_CARD, bg_role, GUI_COLOR_ON_SURFACE,
                           GUI_THEME_FONT_BODY, GUI_THEME_RADIUS_LG, elevation,
                           true, true, true);
}

/* ── 控件构建辅助 ───────────────────────────────────────────────── */

static gui_widget_t make_widget(gui_platform_t *platform, gui_widget_t parent,
                                gui_widget_kind_t kind)
{
    gui_widget_t widget = GUI_WIDGET_NONE;

    if (gui_widget_create(platform, parent, kind, &widget) != 0) {
        return GUI_WIDGET_NONE;
    }
    return widget;
}

static bool apply_appearance(gui_platform_t *platform, gui_widget_t widget,
                             const gui_appearance_t *appearance)
{
    return gui_widget_set_appearance(platform, widget, appearance) == 0;
}

static bool set_geometry(gui_platform_t *platform, gui_widget_t widget,
                         int32_t x, int32_t y, int32_t width, int32_t height)
{
    gui_geometry_t geometry;

    geometry.x = x;
    geometry.y = y;
    geometry.width = GUI_SIZE_PX(width);
    geometry.height = GUI_SIZE_PX(height);
    return gui_widget_set_geometry(platform, widget, &geometry) == 0;
}

static bool set_position(gui_platform_t *platform, gui_widget_t widget,
                         int32_t x, int32_t y)
{
    gui_geometry_t geometry;

    geometry.x = x;
    geometry.y = y;
    geometry.width = GUI_SIZE_CONTENT;
    geometry.height = GUI_SIZE_CONTENT;
    return gui_widget_set_geometry(platform, widget, &geometry) == 0;
}

static bool set_geometry_width(gui_platform_t *platform, gui_widget_t widget,
                               int32_t x, int32_t y, int32_t width)
{
    gui_geometry_t geometry;

    geometry.x = x;
    geometry.y = y;
    geometry.width = GUI_SIZE_PX(width);
    geometry.height = GUI_SIZE_CONTENT;
    return gui_widget_set_geometry(platform, widget, &geometry) == 0;
}

static bool set_alignment(gui_platform_t *platform, gui_widget_t widget,
                          gui_align_t anchor, int32_t x_offset, int32_t y_offset)
{
    gui_alignment_t alignment;

    alignment.anchor = anchor;
    alignment.x_offset = x_offset;
    alignment.y_offset = y_offset;
    return gui_widget_set_alignment(platform, widget, &alignment) == 0;
}

static bool set_layout_none(gui_platform_t *platform, gui_widget_t widget,
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
    return gui_widget_set_layout(platform, widget, &layout) == 0;
}

static bool set_layout_column(gui_platform_t *platform, gui_widget_t widget,
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
    return gui_widget_set_layout(platform, widget, &layout) == 0;
}

static bool set_scroll_both(gui_platform_t *platform, gui_widget_t widget)
{
    gui_scroll_t scroll;

    scroll.horizontal = true;
    scroll.vertical = true;
    scroll.text_mode = GUI_TEXT_MODE_WRAP;
    scroll.text_align = GUI_TEXT_ALIGN_LEFT;
    return gui_widget_set_scroll(platform, widget, &scroll) == 0;
}

static bool set_scroll_none(gui_platform_t *platform, gui_widget_t widget)
{
    gui_scroll_t scroll;

    scroll.horizontal = false;
    scroll.vertical = false;
    scroll.text_mode = GUI_TEXT_MODE_WRAP;
    scroll.text_align = GUI_TEXT_ALIGN_LEFT;
    return gui_widget_set_scroll(platform, widget, &scroll) == 0;
}

static bool set_label_scroll(gui_platform_t *platform, gui_widget_t widget,
                             gui_text_mode_t mode, gui_text_align_t align)
{
    gui_scroll_t scroll;

    scroll.horizontal = false;
    scroll.vertical = false;
    scroll.text_mode = mode;
    scroll.text_align = align;
    return gui_widget_set_scroll(platform, widget, &scroll) == 0;
}

static gui_widget_t make_label(gui_platform_t *platform, gui_widget_t parent,
                               const char *text, gui_theme_font_t font,
                               gui_color_role_t color)
{
    gui_widget_t label = make_widget(platform, parent, GUI_WIDGET_LABEL);
    gui_appearance_t appearance;

    if (label.id == 0) {
        return GUI_WIDGET_NONE;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, GUI_COLOR_SURFACE, color,
                                 font, GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 false, false, false);
    if (!apply_appearance(platform, label, &appearance)) {
        return GUI_WIDGET_NONE;
    }
    if (!set_scroll_none(platform, label)) {
        return GUI_WIDGET_NONE;
    }
    if (text != NULL && gui_widget_set_text(platform, label, text) != 0) {
        return GUI_WIDGET_NONE;
    }
    return label;
}

/* 透明容器：应用内容区的滚动容器都长这样。 */
static gui_widget_t make_pane(gui_platform_t *platform, gui_widget_t parent)
{
    gui_widget_t pane = make_widget(platform, parent, GUI_WIDGET_CONTAINER);
    gui_appearance_t appearance;

    if (pane.id == 0) {
        return GUI_WIDGET_NONE;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, GUI_COLOR_SURFACE,
                                 GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 false, false, false);
    if (!apply_appearance(platform, pane, &appearance)) {
        return GUI_WIDGET_NONE;
    }
    if (!set_layout_none(platform, pane, 0, 0, 0, 0)) {
        return GUI_WIDGET_NONE;
    }
    if (!set_scroll_both(platform, pane)) {
        return GUI_WIDGET_NONE;
    }
    return pane;
}

/* ── 应用状态 ────────────────────────────────────────────────────── */

desktop_app_state_t *desktop_app_state_create(void)
{
    desktop_app_state_t *state =
        (desktop_app_state_t *)calloc(1, sizeof(desktop_app_state_t));
    if (state == NULL) {
        return NULL;
    }
    state->term_history_cursor = -1;
    snprintf(state->files_cwd, sizeof(state->files_cwd), "%s", "/");
    snprintf(state->term_output, sizeof(state->term_output), "%s",
             "OS01 built-in terminal. Type help for the command list.\n");
    return state;
}

void desktop_app_state_destroy(desktop_app_state_t *state)
{
    if (state != NULL) {
        free(state);
    }
}

void desktop_app_request_back(desktop_shell_t *shell)
{
    desktop_shell_request(shell, DESKTOP_INTENT_BACK, NULL);
}

/* ── 顶栏与返回 ──────────────────────────────────────────────────── */

/*
 * 顶栏上的「返回」是**应用界面的一部分**，不是窗口装饰：GUI-3 不实现
 * 窗口系统，因此这里没有最小化 / 最大化 / 关闭按钮，也没有窗口边框。
 */
static void on_back_activate(const gui_event_t *event, void *userdata)
{
    desktop_back_ctx_t *ctx = (desktop_back_ctx_t *)userdata;

    (void)event;
    if (ctx == NULL || ctx->shell == NULL) {
        return;
    }

    /* 当前应用标识由外壳持有：顶栏属于当前表面，标识自然也是当前应用。 */
    desktop_app_id_t id = desktop_shell_current_app_id(ctx->shell);

    /* 文件应用的返回先回父目录，父目录是根才回主屏。 */
    if (id == DESKTOP_APP_FILES && desktop_files_go_up(ctx->shell)) {
        return;
    }

    desktop_app_request_back(ctx->shell);
}

static bool build_app_bar(gui_platform_t *platform, gui_widget_t root,
                          desktop_shell_t *shell, const char *title,
                          const char *subtitle)
{
    int32_t w = (int32_t)gui_platform_get_width(platform);
    gui_widget_t bar = make_widget(platform, root, GUI_WIDGET_CONTAINER);
    gui_widget_t back;
    gui_widget_t label;
    gui_widget_t name;
    gui_appearance_t appearance;

    if (bar.id == 0) {
        return false;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN,
                                 elevation_role(GUI_THEME_ELEVATION_1),
                                 GUI_COLOR_ON_SURFACE, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 true, false, false);
    if (!apply_appearance(platform, bar, &appearance)) {
        return false;
    }
    if (!set_layout_none(platform, bar, 0, 0, 0, 0)) {
        return false;
    }
    if (!set_scroll_none(platform, bar)) {
        return false;
    }
    if (!set_geometry(platform, bar, 0, 0, w, DESKTOP_APP_BAR_HEIGHT)) {
        return false;
    }

    /* 返回：应用表面的第一个焦点项。 */
    back = make_widget(platform, bar, GUI_WIDGET_BUTTON);
    if (back.id == 0) {
        return false;
    }
    appearance = make_appearance(GUI_VARIANT_PLAIN, GUI_COLOR_SURFACE,
                                 GUI_COLOR_PRIMARY, GUI_THEME_FONT_BODY,
                                 GUI_THEME_RADIUS_MD, GUI_THEME_ELEVATION_0,
                                 false, true, false);
    if (!apply_appearance(platform, back, &appearance)) {
        return false;
    }
    if (!set_layout_none(platform, back, 0, 0, 0, 0)) {
        return false;
    }
    if (!set_scroll_none(platform, back)) {
        return false;
    }
    if (!set_geometry(platform, back, APP_MARGIN, 6, 88, 28)) {
        return false;
    }

    label = make_label(platform, back, "< Back", GUI_THEME_FONT_BODY,
                       GUI_COLOR_PRIMARY);
    if (label.id == 0) {
        return false;
    }
    if (!set_alignment(platform, label, GUI_ALIGN_MID, 0, 0)) {
        return false;
    }

    if (gui_widget_set_event_handler(platform, back, on_back_activate,
                                     &desktop_shell_app_state(shell)->back_ctx) != 0) {
        return false;
    }
    if (gui_focus_register(desktop_shell_input(shell), back, GUI_FOCUS_STRONG) != 0) {
        return false;
    }

    name = make_label(platform, bar, title, GUI_THEME_FONT_TITLE,
                      GUI_COLOR_ON_SURFACE);
    if (name.id == 0) {
        return false;
    }
    if (!set_alignment(platform, name, GUI_ALIGN_TOP_MID, 0, 10)) {
        return false;
    }

    if (subtitle != NULL) {
        gui_widget_t sub = make_label(platform, bar, subtitle, GUI_THEME_FONT_LABEL,
                                      GUI_COLOR_ON_SURFACE_VARIANT);
        if (sub.id == 0) {
            return false;
        }
        if (!set_alignment(platform, sub, GUI_ALIGN_TOP_RIGHT, -APP_MARGIN, 13)) {
            return false;
        }
    }

    return true;
}

/* ── 文件应用 ────────────────────────────────────────────────────── */

static int files_entry_cmp(const void *a, const void *b)
{
    const desktop_files_entry_t *ea = (const desktop_files_entry_t *)a;
    const desktop_files_entry_t *eb = (const desktop_files_entry_t *)b;

    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;   /* 目录在前 */
    }
    return strcmp(ea->name, eb->name);
}

static void on_files_row_activate(const gui_event_t *event, void *userdata)
{
    desktop_files_ctx_t *ctx = (desktop_files_ctx_t *)userdata;

    (void)event;
    if (ctx == NULL || ctx->shell == NULL) {
        return;
    }
    if (ctx->path[0] == '\0') {
        return;
    }
    desktop_files_enter_path(ctx->shell, ctx->path);
}

/* 把 cwd 与名字拼成绝对路径；截断即拒绝。 */
static bool join_path(const char *cwd, const char *name, char *out, size_t out_len)
{
    size_t cwd_len;
    size_t name_len;
    int written;

    if (cwd == NULL || name == NULL || out == NULL) {
        return false;
    }
    cwd_len = strlen(cwd);
    name_len = strlen(name);
    if (cwd_len + 1 + name_len + 1 > out_len) {
        return false;
    }
    written = snprintf(out, out_len, "%s/%s", cwd, name);
    if (written < 0 || (size_t)written >= out_len) {
        return false;
    }
    return true;
}

/* 重新读取当前目录并重建条目列表。 */
static bool files_refresh(desktop_shell_t *shell)
{
    gui_platform_t *platform = desktop_shell_platform(shell);
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    gui_widget_t list;
    int32_t w;

    if (platform == NULL || state == NULL || state->files_list.id == 0) {
        return false;
    }
    list = state->files_list;
    w = (int32_t)gui_platform_get_width(platform);

    /*
     * 旧的行对象即将被删除：清空容器把整批行一次带走，焦点引擎的删除
     * 批次因此只在结束时投影一次。
     */
    if (gui_widget_clear(platform, list) != 0) {
        return false;
    }
    state->files_entry_count = 0;

    DIR *dir = opendir(state->files_cwd);
    if (dir == NULL) {
        gui_widget_t err = make_label(platform, list, "Cannot open directory",
                                      GUI_THEME_FONT_BODY, GUI_COLOR_ERROR);
        return err.id != 0;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL &&
           state->files_entry_count < DESKTOP_FILES_MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        desktop_files_entry_t *slot =
            &state->files_entries[state->files_entry_count];
        snprintf(slot->name, sizeof(slot->name), "%s", ent->d_name);
        slot->is_dir = (ent->d_type == DT_DIR);
        state->files_entry_count++;
    }
    closedir(dir);

    qsort(state->files_entries, state->files_entry_count,
          sizeof(state->files_entries[0]), files_entry_cmp);

    for (uint32_t i = 0; i < state->files_entry_count; i++) {
        const desktop_files_entry_t *entry = &state->files_entries[i];
        desktop_files_ctx_t *ctx = &state->files_ctx[i];
        gui_widget_t row;
        gui_widget_t icon;
        gui_widget_t name;
        gui_widget_t kind;
        gui_appearance_t appearance;

        row = make_widget(platform, list, GUI_WIDGET_BUTTON);
        if (row.id == 0) {
            return false;
        }
        appearance = card_appearance(GUI_THEME_ELEVATION_1,
                                     GUI_COLOR_SURFACE_CONTAINER_LOW);
        if (!apply_appearance(platform, row, &appearance)) {
            return false;
        }
        if (!set_layout_none(platform, row,
                             (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                             (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                             (int32_t)gui_theme_space(GUI_THEME_SPACE_MD),
                             (int32_t)gui_theme_space(GUI_THEME_SPACE_MD))) {
            return false;
        }
        if (!set_scroll_none(platform, row)) {
            return false;
        }
        if (!set_geometry(platform, row, 0, 0, w - 2 * APP_MARGIN, 44)) {
            return false;
        }

        icon = make_widget(platform, row, GUI_WIDGET_CONTAINER);
        if (icon.id == 0) {
            return false;
        }
        appearance = make_appearance(GUI_VARIANT_PLAIN,
                                     entry->is_dir ? GUI_COLOR_PRIMARY
                                                   : GUI_COLOR_OUTLINE,
                                     GUI_COLOR_ON_PRIMARY, GUI_THEME_FONT_TITLE,
                                     GUI_THEME_RADIUS_FULL, GUI_THEME_ELEVATION_0,
                                     true, true, false);
        if (!apply_appearance(platform, icon, &appearance)) {
            return false;
        }
        if (!set_layout_none(platform, icon, 0, 0, 0, 0)) {
            return false;
        }
        if (!set_scroll_none(platform, icon)) {
            return false;
        }
        if (!set_geometry(platform, icon, 10, 8, 28, 28)) {
            return false;
        }

        name = make_label(platform, row, entry->name, GUI_THEME_FONT_BODY,
                          GUI_COLOR_ON_SURFACE);
        if (name.id == 0) {
            return false;
        }
        if (!set_position(platform, name, 48, 12)) {
            return false;
        }

        kind = make_label(platform, row, entry->is_dir ? "Directory" : "File",
                          GUI_THEME_FONT_LABEL, GUI_COLOR_ON_SURFACE_VARIANT);
        if (kind.id == 0) {
            return false;
        }
        if (!set_alignment(platform, kind, GUI_ALIGN_MID_RIGHT, -12, 0)) {
            return false;
        }

        /* 行的上下文：外壳 + 目标路径副本。 */
        ctx->shell = shell;
        ctx->version = 0u;
        if (!join_path(state->files_cwd, entry->name, ctx->path,
                       sizeof(ctx->path))) {
            ctx->path[0] = '\0';
        }
        if (gui_widget_set_event_handler(platform, row, on_files_row_activate,
                                         ctx) != 0) {
            return false;
        }

        /* 目录行也是键盘焦点项：Tab 选中后回车即可进入。 */
        if (gui_focus_register(desktop_shell_input(shell), row, GUI_FOCUS_STRONG) != 0) {
            return false;
        }
    }

    return true;
}

void desktop_files_enter_path(desktop_shell_t *shell, const char *absolute_path)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);

    if (state == NULL || absolute_path == NULL || absolute_path[0] == '\0') {
        return;
    }
    if (strlen(absolute_path) + 1 > sizeof(state->files_cwd)) {
        return;
    }

    /* 只读浏览：只允许进入目录。 */
    DIR *dir = opendir(absolute_path);
    if (dir == NULL) {
        return;
    }
    closedir(dir);

    snprintf(state->files_cwd, sizeof(state->files_cwd), "%s", absolute_path);
    (void)files_refresh(shell);
}

void desktop_files_enter(desktop_shell_t *shell, const char *name)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    char path[DESKTOP_FILES_PATH_MAX];

    if (state == NULL || name == NULL || name[0] == '\0') {
        return;
    }
    if (!join_path(state->files_cwd, name, path, sizeof(path))) {
        return;
    }
    desktop_files_enter_path(shell, path);
}

bool desktop_files_go_up(desktop_shell_t *shell)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    const char *cwd;
    size_t len;
    char parent[DESKTOP_FILES_PATH_MAX];

    if (state == NULL) {
        return false;
    }
    cwd = state->files_cwd;
    if (cwd[0] == '\0' || strcmp(cwd, "/") == 0) {
        return false;
    }

    /* 去掉最后一段路径分量。 */
    len = strlen(cwd);
    while (len > 1 && cwd[len - 1] != '/') {
        len--;
    }
    if (len <= 1) {
        snprintf(parent, sizeof(parent), "%s", "/");
    } else {
        memcpy(parent, cwd, len - 1);
        parent[len - 1] = '\0';
    }

    snprintf(state->files_cwd, sizeof(state->files_cwd), "%s", parent);
    (void)files_refresh(shell);
    return true;
}

static bool build_files_app(gui_platform_t *platform, gui_widget_t root,
                            desktop_shell_t *shell)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    int32_t w = (int32_t)gui_platform_get_width(platform);
    int32_t h = (int32_t)gui_platform_get_height(platform);
    gui_widget_t list;
    gui_widget_t note;

    if (state == NULL) {
        return false;
    }

    list = make_pane(platform, root);
    if (list.id == 0) {
        return false;
    }
    if (!set_layout_column(platform, list,
                           (int32_t)gui_theme_space(GUI_THEME_SPACE_SM),
                           0, 0, 0, 0)) {
        return false;
    }
    if (!set_geometry(platform, list, APP_MARGIN,
                      DESKTOP_APP_BAR_HEIGHT + APP_MARGIN,
                      w - 2 * APP_MARGIN,
                      h - DESKTOP_APP_BAR_HEIGHT - 3 * APP_MARGIN)) {
        return false;
    }

    state->files_list = list;
    if (!files_refresh(shell)) {
        return false;
    }

    /* 边界说明：只读浏览器的限制必须写在界面上。 */
    note = make_label(platform, root, "Read-only: create, rename and delete are unavailable",
                      GUI_THEME_FONT_LABEL, GUI_COLOR_ON_SURFACE_VARIANT);
    if (note.id == 0) {
        return false;
    }
    if (!set_alignment(platform, note, GUI_ALIGN_BOTTOM_LEFT, APP_MARGIN, -10)) {
        return false;
    }

    return true;
}

/* ── 终端应用 ────────────────────────────────────────────────────── */

static void term_append(desktop_app_state_t *state, const char *text)
{
    size_t len = strlen(state->term_output);
    size_t room = sizeof(state->term_output) - len - 1;
    if (room == 0) {
        return;
    }
    snprintf(state->term_output + len, room + 1, "%s", text);
}

/* 执行一条内建命令。不 fork 子进程。 */
static void term_exec_builtin(desktop_app_state_t *state, const char *cmd,
                              const char *arg)
{
    if (strcmp(cmd, "help") == 0) {
        term_append(state, "Built-in commands: help ls cat uname date clear\n");
    } else if (strcmp(cmd, "uname") == 0) {
        term_append(state, "OS01 " APP_ARCH " (single-process desktop shell)\n");
    } else if (strcmp(cmd, "date") == 0) {
        time_t now = time(NULL);
        struct tm tm_buf;
        struct tm *tm = localtime_r(&now, &tm_buf);
        char buf[64];
        if (tm != NULL && strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm) > 0) {
            term_append(state, buf);
            term_append(state, "\n");
        } else {
            term_append(state, "date unavailable\n");
        }
    } else if (strcmp(cmd, "clear") == 0) {
        state->term_output[0] = '\0';
    } else if (strcmp(cmd, "ls") == 0) {
        const char *path = (arg != NULL && arg[0] != '\0') ? arg : "/";
        DIR *dir = opendir(path);
        if (dir == NULL) {
            term_append(state, "ls: cannot open directory\n");
            return;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            term_append(state, ent->d_name);
            term_append(state, "\n");
        }
        closedir(dir);
    } else if (strcmp(cmd, "cat") == 0) {
        if (arg == NULL || arg[0] == '\0') {
            term_append(state, "cat: missing file name\n");
            return;
        }
        FILE *f = fopen(arg, "r");
        if (f == NULL) {
            term_append(state, "cat: cannot open file\n");
            return;
        }
        /* cat 按原始字节输出：用 fread，宿主 libc 与 OS01 libc 都有它。 */
        char buf[256];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
            buf[got] = '\0';
            term_append(state, buf);
        }
        fclose(f);
    } else {
        term_append(state, "command not found: ");
        term_append(state, cmd);
        term_append(state, "\n");
    }
}

static void term_exec(desktop_app_state_t *state, const char *cmd)
{
    char work[DESKTOP_TERM_LINE_MAX];
    snprintf(work, sizeof(work), "%s", cmd);

    /* 拆出命令与参数（只支持一个参数）。 */
    char *arg = strchr(work, ' ');
    if (arg != NULL) {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    }

    term_append(state, TERM_PROMPT);
    term_append(state, cmd);
    term_append(state, "\n");
    term_exec_builtin(state, work, arg);
}

static void term_push_history(desktop_app_state_t *state, const char *cmd)
{
    if (cmd[0] == '\0') {
        return;
    }
    if (state->term_history_count >= DESKTOP_TERM_HISTORY_MAX) {
        for (int i = 1; i < DESKTOP_TERM_HISTORY_MAX; i++) {
            memcpy(state->term_history[i - 1], state->term_history[i],
                   DESKTOP_TERM_LINE_MAX);
        }
        state->term_history_count = DESKTOP_TERM_HISTORY_MAX - 1;
    }
    snprintf(state->term_history[state->term_history_count], DESKTOP_TERM_LINE_MAX,
             "%s", cmd);
    state->term_history_count++;
    state->term_history_cursor = -1;
    state->term_history_browsing = false;
}

static bool build_terminal_app(gui_platform_t *platform, gui_widget_t root,
                               desktop_shell_t *shell)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    int32_t w = (int32_t)gui_platform_get_width(platform);
    int32_t h = (int32_t)gui_platform_get_height(platform);
    gui_widget_t box;
    gui_widget_t out;
    gui_widget_t in;

    if (state == NULL) {
        return false;
    }

    /* 输出区：可滚动的透明容器。 */
    box = make_pane(platform, root);
    if (box.id == 0) {
        return false;
    }
    if (!set_geometry(platform, box, APP_MARGIN,
                      DESKTOP_APP_BAR_HEIGHT + APP_MARGIN,
                      w - 2 * APP_MARGIN,
                      h - DESKTOP_APP_BAR_HEIGHT - 3 * APP_MARGIN)) {
        return false;
    }

    out = make_label(platform, box, state->term_output, GUI_THEME_FONT_BODY,
                     GUI_COLOR_ON_SURFACE);
    if (out.id == 0) {
        return false;
    }
    if (!set_label_scroll(platform, out, GUI_TEXT_MODE_WRAP, GUI_TEXT_ALIGN_LEFT)) {
        return false;
    }
    if (!set_geometry_width(platform, out, 0, 0, w - 2 * APP_MARGIN)) {
        return false;
    }

    /* 输入行：提示符 + 当前命令行。 */
    in = make_label(platform, root, TERM_PROMPT, GUI_THEME_FONT_BODY,
                    GUI_COLOR_PRIMARY);
    if (in.id == 0) {
        return false;
    }
    if (!set_label_scroll(platform, in, GUI_TEXT_MODE_WRAP, GUI_TEXT_ALIGN_LEFT)) {
        return false;
    }
    if (!set_geometry_width(platform, in, APP_MARGIN, h - 2 * APP_MARGIN,
                            w - 2 * APP_MARGIN)) {
        return false;
    }

    state->term_output_label = out;
    state->term_input_label = in;
    /* 最近一次提交的文本：第一次刷新之前它与控件里的内容一致。 */
    snprintf(state->term_output_shown, sizeof(state->term_output_shown), "%s",
             state->term_output);
    snprintf(state->term_input_shown, sizeof(state->term_input_shown), "%s",
             TERM_PROMPT);
    return true;
}

/* ── 系统应用 ────────────────────────────────────────────────────── */

static const char *color_format_name(gui_pixel_format_t cf)
{
    switch (cf) {
    case GUI_PIXEL_FORMAT_RGB565:   return "RGB565";
    case GUI_PIXEL_FORMAT_XRGB8888: return "XRGB8888";
    case GUI_PIXEL_FORMAT_ARGB8888: return "ARGB8888";
    case GUI_PIXEL_FORMAT_I8:       return "I8";
    case GUI_PIXEL_FORMAT_UNKNOWN:
    default:                        return "unknown";
    }
}

/* 输入设备的情况：键盘是必需的（建输入对象失败即无键盘），鼠标可降级。 */
static const char *input_devices_label(const desktop_shell_t *shell)
{
    const gui_input_t *input = desktop_shell_input(shell);
    if (input == NULL) {
        return "not initialised";
    }
    return gui_input_has_mouse(input) ? "keyboard + mouse" : "keyboard only";
}

static bool build_system_app(gui_platform_t *platform, gui_widget_t root,
                              desktop_shell_t *shell)
{
    int32_t w = (int32_t)gui_platform_get_width(platform);
    gui_platform_stats_t stats;
    gui_widget_t label;
    char text[1024];
    size_t used = 0;

    memset(&stats, 0, sizeof(stats));
    if (gui_platform_get_stats(platform, &stats) != 0) {
        memset(&stats, 0, sizeof(stats));
    }

    snprintf(text + used, sizeof(text) - used, "Kernel: %s\n", APP_ARCH);
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Build profile: %s\n",
             OS01_BUILD_PROFILE);
    used = strlen(text);

    time_t started = desktop_shell_start_time(shell);
    struct tm start_buf;
    struct tm *start = localtime_r(&started, &start_buf);
    char start_text[32];
    if (start != NULL &&
        strftime(start_text, sizeof(start_text), "%Y-%m-%d %H:%M:%S", start) > 0) {
        snprintf(text + used, sizeof(text) - used, "Boot time: %s\n", start_text);
    } else {
        snprintf(text + used, sizeof(text) - used, "Boot time: unknown\n");
    }
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Display mode: %s\n",
             (gui_platform_get_mode(platform) == GUI_MODE_OFFSCREEN)
                 ? "offscreen" : "screen");
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Resolution: %ux%u\n",
             stats.width, stats.height);
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Pixel format: %s\n",
             color_format_name(stats.format));
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Widgets: %u\n", stats.widget_count);
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Input devices: %s\n",
             input_devices_label(shell));
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Focus capacity: %u\n",
             (unsigned)DESKTOP_FOCUS_MAX);
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Renderer: %s %u.%u.%u\n",
             stats.renderer_name,
             (unsigned)stats.renderer_version_major,
             (unsigned)stats.renderer_version_minor,
             (unsigned)stats.renderer_version_patch);
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Render pool: %llu KB / %llu KB (peak %llu KB)\n",
             (unsigned long long)(stats.pool_used / 1024u),
             (unsigned long long)(stats.pool_total / 1024u),
             (unsigned long long)(stats.pool_max_used / 1024u));
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Widget registry: %llu bytes\n",
             (unsigned long long)stats.registry_bytes);
    used = strlen(text);

    label = make_label(platform, root, text, GUI_THEME_FONT_BODY,
                       GUI_COLOR_ON_SURFACE);
    if (label.id == 0) {
        return false;
    }
    if (!set_label_scroll(platform, label, GUI_TEXT_MODE_WRAP, GUI_TEXT_ALIGN_LEFT)) {
        return false;
    }
    if (!set_geometry_width(platform, label, APP_MARGIN,
                            DESKTOP_APP_BAR_HEIGHT + APP_MARGIN,
                            w - 2 * APP_MARGIN)) {
        return false;
    }

    return true;
}

/* ── 应用构造入口 ────────────────────────────────────────────────── */

gui_widget_t desktop_app_build(gui_platform_t *platform, desktop_app_id_t id,
                               desktop_shell_t *shell, gui_widget_t root)
{
    const desktop_app_info_t *info;
    desktop_app_state_t *state;
    const char *subtitle = NULL;

    if (platform == NULL || shell == NULL || root.id == 0) {
        return GUI_WIDGET_NONE;
    }

    info = desktop_app_info(id);
    if (info == NULL) {
        return GUI_WIDGET_NONE;
    }

    state = desktop_shell_app_state(shell);
    if (state != NULL) {
        state->files_list = GUI_WIDGET_NONE;
        state->term_output_label = GUI_WIDGET_NONE;
        state->term_input_label = GUI_WIDGET_NONE;
    }

    if (id == DESKTOP_APP_FILES && state != NULL) {
        subtitle = state->files_cwd;
    }

    if (!build_app_bar(platform, root, shell, info->name, subtitle)) {
        return GUI_WIDGET_NONE;
    }

    switch (id) {
    case DESKTOP_APP_FILES:
        if (!build_files_app(platform, root, shell)) {
            return GUI_WIDGET_NONE;
        }
        break;
    case DESKTOP_APP_TERMINAL:
        if (!build_terminal_app(platform, root, shell)) {
            return GUI_WIDGET_NONE;
        }
        break;
    case DESKTOP_APP_SYSTEM:
        if (!build_system_app(platform, root, shell)) {
            return GUI_WIDGET_NONE;
        }
        break;
    case DESKTOP_APP_SETTINGS:
        if (desktop_shell_build_settings_body(shell, root).id == 0) {
            return GUI_WIDGET_NONE;
        }
        break;
    default:
        break;
    }

    return root;
}

/* ── 按键处理 ────────────────────────────────────────────────────── */

bool desktop_app_handle_key(desktop_shell_t *shell, const gui_key_event_t *ev)
{
    if (shell == NULL || ev == NULL || ev->action != GUI_KEY_DOWN) {
        return false;
    }
    if (desktop_shell_state(shell) != DESKTOP_STATE_APP) {
        return false;
    }
    /* 只有终端消费字符键；ESC 不消费：它要回到主屏。 */
    if (desktop_shell_current_app_id(shell) != DESKTOP_APP_TERMINAL) {
        return false;
    }

    desktop_app_state_t *state = desktop_shell_app_state(shell);
    if (state == NULL) {
        return false;
    }

    size_t len = strlen(state->term_input);

    switch (ev->physical_key) {
    case KEY_ENTER:
        term_push_history(state, state->term_input);
        term_exec(state, state->term_input);
        state->term_input[0] = '\0';
        return true;
    case KEY_BACKSPACE:
        if (len > 0) {
            state->term_input[len - 1] = '\0';
        }
        return true;
    case KEY_UP:
    case KEY_DOWN: {
        if (state->term_history_count == 0) {
            return true;
        }
        if (!state->term_history_browsing) {
            if (ev->physical_key != KEY_UP) {
                return true;
            }
            state->term_history_browsing = true;
            state->term_history_cursor = state->term_history_count - 1;
        } else {
            if (ev->physical_key == KEY_UP) {
                if (state->term_history_cursor > 0) {
                    state->term_history_cursor--;
                }
            } else {
                if (state->term_history_cursor < state->term_history_count - 1) {
                    state->term_history_cursor++;
                } else {
                    state->term_history_browsing = false;
                    state->term_history_cursor = -1;
                    state->term_input[0] = '\0';
                    return true;
                }
            }
        }
        snprintf(state->term_input, sizeof(state->term_input), "%s",
                 state->term_history[state->term_history_cursor]);
        return true;
    }
    default:
        break;
    }

    /* 可打印 ASCII。 */
    if (ev->codepoint >= 0x20u && ev->codepoint <= 0x7Eu) {
        if (len + 1 < sizeof(state->term_input)) {
            state->term_input[len] = (char)ev->codepoint;
            state->term_input[len + 1] = '\0';
        }
        return true;
    }

    return false;
}

/* ── 界面刷新 ────────────────────────────────────────────────────── */

void desktop_app_refresh(desktop_shell_t *shell)
{
    if (shell == NULL) {
        return;
    }
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    if (state == NULL) {
        return;
    }
    if (desktop_shell_state(shell) != DESKTOP_STATE_APP ||
        desktop_shell_current_app_id(shell) != DESKTOP_APP_TERMINAL) {
        return;
    }

    /* 只在文本真的变了才重设控件：tick() 每帧都会进来。
     * 去重靠**最近一次提交的文本副本**，不回读控件。 */
    if (state->term_output_label.id != 0) {
        if (strcmp(state->term_output_shown, state->term_output) != 0) {
            if (gui_widget_set_text(desktop_shell_platform(shell),
                                    state->term_output_label,
                                    state->term_output) == 0) {
                snprintf(state->term_output_shown,
                         sizeof(state->term_output_shown), "%s",
                         state->term_output);
            }
        }
    }
    if (state->term_input_label.id != 0) {
        char buf[DESKTOP_TERM_LINE_MAX + 32];
        snprintf(buf, sizeof(buf), "%s%s", TERM_PROMPT, state->term_input);
        if (strcmp(state->term_input_shown, buf) != 0) {
            if (gui_widget_set_text(desktop_shell_platform(shell),
                                    state->term_input_label, buf) == 0) {
                snprintf(state->term_input_shown,
                         sizeof(state->term_input_shown), "%s", buf);
            }
        }
    }
}
