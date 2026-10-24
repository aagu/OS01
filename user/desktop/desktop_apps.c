/*
 * desktop_apps.c — 桌面内置应用实现（GUI-3）
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

static lv_color_t tok(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static lv_obj_t *app_label(lv_obj_t *parent, const char *text,
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

/* 透明容器：应用内容区的滚动容器都长这样。 */
static lv_obj_t *app_pane(lv_obj_t *parent)
{
    lv_obj_t *pane = lv_obj_create(parent);
    if (pane == NULL) {
        return NULL;
    }
    lv_obj_set_layout(pane, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_opa(pane, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pane, 0, 0);
    lv_obj_set_style_pad_all(pane, 0, 0);
    lv_obj_set_style_radius(pane, 0, 0);
    lv_obj_set_style_shadow_width(pane, 0, 0);
    lv_obj_add_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    return pane;
}

static void app_card(lv_obj_t *obj, gui_theme_elevation_t elevation)
{
    const gui_theme_colors_t *c = gui_theme_colors();

    lv_obj_set_style_bg_color(obj, tok(gui_theme_elevation_surface(elevation)), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, gui_theme_radius(GUI_THEME_RADIUS_LG), 0);
    lv_obj_set_style_border_color(obj, tok(c->outline_variant), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_all(obj, gui_theme_space(GUI_THEME_SPACE_MD), 0);
    lv_obj_set_style_shadow_color(obj, tok(c->shadow), 0);
    lv_obj_set_style_shadow_width(obj, gui_theme_elevation_shadow_width(elevation), 0);
    lv_obj_set_style_shadow_offset_y(obj, gui_theme_elevation_shadow_offset_y(elevation), 0);
    lv_obj_set_style_shadow_opa(obj, gui_theme_elevation_shadow_opa(elevation), 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
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
static void app_back_cb(lv_event_t *event)
{
    desktop_shell_t *shell = (desktop_shell_t *)lv_event_get_user_data(event);
    if (shell == NULL) {
        return;
    }

    /* 当前应用标识由外壳持有：顶栏属于当前表面，标识自然也是当前应用。 */
    desktop_app_id_t id = desktop_shell_current_app_id(shell);

    /* 文件应用的返回先回父目录，父目录是根才回主屏。 */
    if (id == DESKTOP_APP_FILES && desktop_files_go_up(shell)) {
        return;
    }

    desktop_app_request_back(shell);
}

static lv_obj_t *build_app_bar(lv_obj_t *root, lv_display_t *disp,
                               desktop_shell_t *shell, const char *title,
                               const char *subtitle)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    int32_t w = lv_display_get_horizontal_resolution(disp);

    lv_obj_t *bar = lv_obj_create(root);
    if (bar == NULL) {
        return NULL;
    }
    lv_obj_set_layout(bar, LV_LAYOUT_NONE);
    lv_obj_set_size(bar, w, DESKTOP_APP_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, tok(gui_theme_elevation_surface(GUI_THEME_ELEVATION_1)), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 返回 */
    lv_obj_t *back = lv_button_create(bar);
    if (back != NULL) {
        lv_obj_set_size(back, 88, 28);
        lv_obj_set_pos(back, APP_MARGIN, 6);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        lv_obj_set_style_radius(back, gui_theme_radius(GUI_THEME_RADIUS_MD), 0);
        lv_obj_add_event_cb(back, app_back_cb, LV_EVENT_CLICKED, shell);
        lv_obj_t *label = app_label(back, "< Back", GUI_THEME_FONT_BODY, c->primary);
        if (label != NULL) {
            lv_obj_center(label);
        }
        /* 返回按钮是应用表面的第一个焦点项。 */
        desktop_shell_focus_add(shell, back);
    }

    lv_obj_t *name = app_label(bar, title, GUI_THEME_FONT_TITLE, c->on_surface);
    if (name != NULL) {
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 10);
    }

    if (subtitle != NULL) {
        lv_obj_t *sub = app_label(bar, subtitle, GUI_THEME_FONT_LABEL, c->on_surface_variant);
        if (sub != NULL) {
            lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -APP_MARGIN, 13);
        }
    }

    return bar;
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

/* 行的 user data 存「序号 + 1」：0 留给「没有 user data」。 */
static void set_row_index(lv_obj_t *obj, uint32_t index)
{
    lv_obj_set_user_data(obj, (void *)(intptr_t)(index + 1));
}

static int row_index(lv_obj_t *obj)
{
    intptr_t tag = (intptr_t)lv_obj_get_user_data(obj);
    return tag > 0 ? (int)(tag - 1) : -1;
}

static void files_row_cb(lv_event_t *event)
{
    desktop_shell_t *shell = (desktop_shell_t *)lv_event_get_user_data(event);
    if (shell == NULL) {
        return;
    }
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    if (state == NULL) {
        return;
    }
    int index = row_index(lv_event_get_target(event));
    if (index < 0 || (uint32_t)index >= state->files_entry_count) {
        return;
    }
    desktop_files_enter(shell, state->files_entries[index].name);
}

/* 重新读取当前目录并重建条目列表。 */
static void files_refresh(desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    if (state == NULL || state->files_list == NULL) {
        return;
    }

    lv_obj_t *list = state->files_list;
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());

    /* 旧的行对象即将被删除：先从焦点序列里摘掉，避免留下野指针。 */
    desktop_shell_focus_remove_children(shell, list);

    /* 清空旧条目。 */
    lv_obj_clean(list);
    state->files_entry_count = 0;

    DIR *dir = opendir(state->files_cwd);
    if (dir == NULL) {
        lv_obj_t *err = app_label(list, "Cannot open directory", GUI_THEME_FONT_BODY, c->error);
        (void)err;
        return;
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

        lv_obj_t *row = lv_obj_create(list);
        if (row == NULL) {
            break;
        }
        app_card(row, GUI_THEME_ELEVATION_1);
        lv_obj_set_style_bg_color(row, tok(c->surface_container_low), 0);
        lv_obj_set_width(row, w - 2 * APP_MARGIN);
        lv_obj_set_height(row, 44);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon = lv_obj_create(row);
        if (icon != NULL) {
            lv_obj_set_size(icon, 28, 28);
            lv_obj_set_pos(icon, 10, 8);
            lv_obj_set_style_bg_color(icon,
                                     tok(entry->is_dir ? c->primary : c->outline), 0);
            lv_obj_set_style_radius(icon, gui_theme_radius(GUI_THEME_RADIUS_FULL), 0);
            lv_obj_set_style_border_width(icon, 0, 0);
            lv_obj_set_style_shadow_width(icon, 0, 0);
            lv_obj_set_style_pad_all(icon, 0, 0);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_obj_t *name = app_label(row, entry->name, GUI_THEME_FONT_BODY, c->on_surface);
        if (name != NULL) {
            lv_obj_set_pos(name, 48, 12);
        }
        lv_obj_t *kind = app_label(row, entry->is_dir ? "Directory" : "File",
                                   GUI_THEME_FONT_LABEL, c->on_surface_variant);
        if (kind != NULL) {
            lv_obj_align(kind, LV_ALIGN_RIGHT_MID, -12, 0);
        }

        set_row_index(row, i);
        lv_obj_add_event_cb(row, files_row_cb, LV_EVENT_CLICKED, shell);

        /* 目录行也是键盘焦点项：Tab 选中后回车即可进入。 */
        desktop_shell_focus_add(shell, row);
    }
}

void desktop_files_enter(desktop_shell_t *shell, const char *name)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    if (state == NULL || name == NULL || name[0] == '\0') {
        return;
    }

    char path[DESKTOP_FILES_PATH_MAX];
    if (strcmp(state->files_cwd, "/") == 0) {
        snprintf(path, sizeof(path), "/%s", name);
    } else {
        snprintf(path, sizeof(path), "%s/%s", state->files_cwd, name);
    }

    /* 只读浏览：只允许进入目录。 */
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return;
    }
    closedir(dir);

    snprintf(state->files_cwd, sizeof(state->files_cwd), "%s", path);
    files_refresh(shell);
}

bool desktop_files_go_up(desktop_shell_t *shell)
{
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    if (state == NULL) {
        return false;
    }
    const char *cwd = state->files_cwd;
    if (cwd[0] == '\0' || strcmp(cwd, "/") == 0) {
        return false;
    }

    /* 去掉最后一段路径分量。 */
    size_t len = strlen(cwd);
    while (len > 1 && cwd[len - 1] != '/') {
        len--;
    }
    char parent[DESKTOP_FILES_PATH_MAX];
    if (len <= 1) {
        snprintf(parent, sizeof(parent), "%s", "/");
    } else {
        memcpy(parent, cwd, len - 1);
        parent[len - 1] = '\0';
    }

    snprintf(state->files_cwd, sizeof(state->files_cwd), "%s", parent);
    files_refresh(shell);
    return true;
}

static lv_obj_t *build_files_app(lv_obj_t *root, lv_display_t *disp,
                                 desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    if (state == NULL) {
        return NULL;
    }

    lv_obj_t *list = app_pane(root);
    if (list == NULL) {
        return NULL;
    }
    lv_obj_set_size(list, w - 2 * APP_MARGIN,
                    h - DESKTOP_APP_BAR_HEIGHT - 3 * APP_MARGIN);
    lv_obj_set_pos(list, APP_MARGIN, DESKTOP_APP_BAR_HEIGHT + APP_MARGIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, gui_theme_space(GUI_THEME_SPACE_SM), 0);

    state->files_list = list;
    files_refresh(shell);

    /* 边界说明：只读浏览器的限制必须写在界面上。 */
    lv_obj_t *note = app_label(root, "Read-only: create, rename and delete are unavailable",
                                GUI_THEME_FONT_LABEL, c->on_surface_variant);
    if (note != NULL) {
        lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, APP_MARGIN, -10);
    }

    return list;
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

static lv_obj_t *build_terminal_app(lv_obj_t *root, lv_display_t *disp,
                                    desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    desktop_app_state_t *state = desktop_shell_app_state(shell);
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    if (state == NULL) {
        return NULL;
    }

    /* 输出区：可滚动的透明容器。 */
    lv_obj_t *box = app_pane(root);
    if (box == NULL) {
        return NULL;
    }
    lv_obj_set_size(box, w - 2 * APP_MARGIN,
                    h - DESKTOP_APP_BAR_HEIGHT - 3 * APP_MARGIN);
    lv_obj_set_pos(box, APP_MARGIN, DESKTOP_APP_BAR_HEIGHT + APP_MARGIN);

    lv_obj_t *out = app_label(box, state->term_output, GUI_THEME_FONT_BODY, c->on_surface);
    if (out != NULL) {
        lv_obj_set_width(out, w - 2 * APP_MARGIN);
        lv_label_set_long_mode(out, LV_LABEL_LONG_WRAP);
    }

    /* 输入行：提示符 + 当前命令行。 */
    lv_obj_t *in = app_label(root, TERM_PROMPT, GUI_THEME_FONT_BODY, c->primary);
    if (in != NULL) {
        lv_obj_set_pos(in, APP_MARGIN, h - 2 * APP_MARGIN);
        lv_obj_set_width(in, w - 2 * APP_MARGIN);
    }

    state->term_output_label = out;
    state->term_input_label = in;
    return out;
}

/* ── 系统应用 ────────────────────────────────────────────────────── */

static const char *color_format_name(lv_color_format_t cf)
{
    switch (cf) {
    case LV_COLOR_FORMAT_RGB565:   return "RGB565";
    case LV_COLOR_FORMAT_RGB888:   return "RGB888";
    case LV_COLOR_FORMAT_ARGB8888: return "ARGB8888";
    case LV_COLOR_FORMAT_XRGB8888: return "XRGB8888";
    case LV_COLOR_FORMAT_I1:       return "I1";
    case LV_COLOR_FORMAT_I8:       return "I8";
    case LV_COLOR_FORMAT_L8:       return "L8";
    case LV_COLOR_FORMAT_A8:       return "A8";
    default:                       return "other";
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

static lv_obj_t *build_system_app(lv_obj_t *root, lv_display_t *disp,
                                   desktop_shell_t *shell)
{
    const gui_theme_colors_t *c = gui_theme_colors();
    gui_platform_t *platform = desktop_shell_platform(shell);
    int32_t w = lv_display_get_horizontal_resolution(disp);

    char text[1024];
    size_t used = 0;

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
             (platform != NULL &&
              gui_platform_get_mode(platform) == GUI_MODE_OFFSCREEN)
                 ? "offscreen" : "screen");
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Resolution: %ux%u\n",
             (platform != NULL) ? gui_platform_get_width(platform) : 0u,
             (platform != NULL) ? gui_platform_get_height(platform) : 0u);
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Pixel format: %s\n",
             color_format_name(lv_display_get_color_format(disp)));
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Input devices: %s\n",
             input_devices_label(shell));
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "Focus capacity: %u\n",
             (unsigned)DESKTOP_FOCUS_MAX);
    used = strlen(text);

    snprintf(text + used, sizeof(text) - used, "LVGL: %d.%d.%d\n",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    used = strlen(text);

    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    snprintf(text + used, sizeof(text) - used, "LVGL heap: %u KB / %u KB (%u%%)\n",
             (unsigned)(mem.used_cnt / 1024u), (unsigned)(mem.total_size / 1024u),
             (unsigned)mem.used_pct);
    used = strlen(text);

    lv_obj_t *label = app_label(root, text, GUI_THEME_FONT_BODY, c->on_surface);
    if (label != NULL) {
        lv_obj_set_pos(label, APP_MARGIN, DESKTOP_APP_BAR_HEIGHT + APP_MARGIN);
        lv_obj_set_width(label, w - 2 * APP_MARGIN);
    }

    return label;
}

/* ── 应用构造入口 ────────────────────────────────────────────────── */

lv_obj_t *desktop_app_build(lv_display_t *disp, desktop_app_id_t id,
                            desktop_shell_t *shell)
{
    if (disp == NULL || shell == NULL) {
        return NULL;
    }

    const desktop_app_info_t *info = desktop_app_info(id);
    if (info == NULL) {
        return NULL;
    }

    desktop_app_state_t *state = desktop_shell_app_state(shell);
    if (state != NULL) {
        state->files_list = NULL;
        state->term_output_label = NULL;
        state->term_input_label = NULL;
    }

    /* 表面根对象：透明全屏容器，壁纸由屏幕承载。 */
    lv_obj_t *root = lv_obj_create(lv_display_get_screen_active(disp));
    if (root == NULL) {
        return NULL;
    }
    lv_obj_set_layout(root, LV_LAYOUT_NONE);
    lv_obj_set_size(root, lv_display_get_horizontal_resolution(disp),
                    lv_display_get_vertical_resolution(disp));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_shadow_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    const char *subtitle = NULL;
    if (id == DESKTOP_APP_FILES && state != NULL) {
        subtitle = state->files_cwd;
    }

    build_app_bar(root, disp, shell, info->name, subtitle);

    switch (id) {
    case DESKTOP_APP_FILES:
        build_files_app(root, disp, shell);
        break;
    case DESKTOP_APP_TERMINAL:
        build_terminal_app(root, disp, shell);
        break;
    case DESKTOP_APP_SYSTEM:
        build_system_app(root, disp, shell);
        break;
    case DESKTOP_APP_SETTINGS:
        desktop_shell_build_settings_body(shell, root);
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

    /* 只在文本真的变了才重设 label：tick() 每帧都会进来。 */
    if (state->term_output_label != NULL) {
        const char *shown = lv_label_get_text(state->term_output_label);
        if (shown == NULL || strcmp(shown, state->term_output) != 0) {
            lv_label_set_text(state->term_output_label, state->term_output);
        }
    }
    if (state->term_input_label != NULL) {
        char buf[DESKTOP_TERM_LINE_MAX + 32];
        snprintf(buf, sizeof(buf), "%s%s", TERM_PROMPT, state->term_input);
        const char *shown = lv_label_get_text(state->term_input_label);
        if (shown == NULL || strcmp(shown, buf) != 0) {
            lv_label_set_text(state->term_input_label, buf);
        }
    }
}
