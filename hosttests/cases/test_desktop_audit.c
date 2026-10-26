/*
 * test_desktop_audit.c — 桌面源码静态审计（GUI-3 Task 5）
 *
 * 宿主契约测试只能验证「行为」，而设计系统的「统一」与「不假装有窗口
 * 系统」这两条是**代码形状**上的约束，必须静态审计才钉得住：
 *
 *   ① 不得出现颜色字面量（颜色只能来自 gui_theme_colors()）
 *   ② 不得出现手绘 primitive（gfx_fill_rect / gfx_pixel / ...）——桌面是
 *      LVGL 界面，不再自己画像素
 *   ③ 不得直接打开输入设备（键鼠一律走 GUI-2 的输入通道）
 *   ④ 不得包含 libgui 的私有头（业务应用只能用公共 API）
 *   ⑤ 不得出现窗口装饰标识（minimize / maximize / window_decoration /
 *      title_bar）——GUI-3 不实现窗口系统，因此不得画窗口装饰
 *   ⑥ 不得出现字面量字号（字号只能经由 gui_theme_font_size() 取得）
 *
 * 审计对象：user/desktop/ 下的业务源码。
 */

#include "test_framework.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GUI_DESKTOP_SRC
#error "GUI_DESKTOP_SRC must be defined to the desktop source directory"
#endif

/* 被审计的源文件 */
static const char *const g_files[] = {
    "desktop.c",
    "desktop_shell.c",
    "desktop_shell.h",
    "desktop_apps.c",
    "desktop_apps.h",
    "desktop_registry.c",
    "desktop_registry.h",
    "desktop_state.c",
    "desktop_state.h",
    "app.mk",
};

#define FILE_COUNT (sizeof(g_files) / sizeof(g_files[0]))

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    size_t cap = 1 << 16;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (nb == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) {
            break;
        }
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/*
 * 把注释内容替换成等长空格（保留偏移，便于报错定位）。
 *
 * 审计要看的是**代码**，不是散文：注释里写「不再 open("/dev/mouse")」
 * 是在陈述约定，不是在违规。字符串字面量必须保留 —— 设备路径正是以
 * 字符串形式出现的。
 */
static char *strip_comments(char *text, bool is_make)
{
    size_t len = strlen(text);
    size_t i = 0;
    bool in_block = false;
    bool in_line = false;
    bool in_str = false;
    bool in_chr = false;

    while (i < len) {
        char c = text[i];
        char n = (i + 1 < len) ? text[i + 1] : '\0';

        if (in_block) {
            if (c == '*' && n == '/') {
                text[i] = ' ';
                text[i + 1] = ' ';
                i += 2;
                in_block = false;
                continue;
            }
            if (c != '\n') {
                text[i] = ' ';
            }
            i++;
            continue;
        }
        if (in_line) {
            if (c == '\n') {
                in_line = false;
            } else {
                text[i] = ' ';
            }
            i++;
            continue;
        }
        if (in_str) {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '"') {
                in_str = false;
            }
            i++;
            continue;
        }
        if (in_chr) {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '\'') {
                in_chr = false;
            }
            i++;
            continue;
        }

        if (is_make && c == '#') {
            in_line = true;
            text[i] = ' ';
            i++;
            continue;
        }
        if (!is_make && c == '/' && n == '*') {
            in_block = true;
            text[i] = ' ';
            text[i + 1] = ' ';
            i += 2;
            continue;
        }
        if (!is_make && c == '/' && n == '/') {
            in_line = true;
            text[i] = ' ';
            i++;
            continue;
        }
        if (c == '"') {
            in_str = true;
            i++;
            continue;
        }
        if (c == '\'') {
            in_chr = true;
            i++;
            continue;
        }
        i++;
    }
    return text;
}

/* 把源文件读进来；读不到直接失败（审计必须真的看见源码）。 */
static char *load(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", GUI_DESKTOP_SRC, name);
    char *text = slurp(path);
    assert_not_null(text);
    bool is_make = (strstr(name, ".mk") != NULL);
    return strip_comments(text, is_make);
}

/* 在 text 里找 needle；找到返回其偏移，否则返回 -1。 */
static long find(const char *text, const char *needle)
{
    const char *hit = strstr(text, needle);
    return hit ? (long)(hit - text) : -1;
}

/* 忽略大小写地找 needle。 */
static long find_ci(const char *text, const char *needle)
{
    size_t nlen = strlen(needle);
    for (const char *p = text; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return (long)(p - text);
        }
    }
    return -1;
}

/* ── 审计 ①：不得出现颜色字面量 ─────────────────────────────────── */

TEST_FUNC(test_no_color_literals_in_desktop_sources)
{
    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);

        /* 颜色字面量：0x 后面跟 6~8 位十六进制。 */
        const char *p = text;
        while ((p = strstr(p, "0x")) != NULL) {
            const char *q = p + 2;
            size_t digits = 0;
            while (digits < 8 && q[digits] != '\0' &&
                   ((q[digits] >= '0' && q[digits] <= '9') ||
                    (q[digits] >= 'a' && q[digits] <= 'f') ||
                    (q[digits] >= 'A' && q[digits] <= 'F'))) {
                digits++;
            }
            assert_true(digits < 6 || digits > 8);
            p = q;
        }
        free(text);
    }
}

/* ── 审计 ②：不得出现手绘 primitive ─────────────────────────────── */

TEST_FUNC(test_no_hand_drawn_primitives)
{
    static const char *const banned[] = {
        "gfx_fill_rect", "gfx_pixel", "gfx_rect", "gfx_hline",
        "gfx_vline", "gfx_line", "gfx_sprite", "font8x16",
    };

    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);
        for (size_t j = 0; j < sizeof(banned) / sizeof(banned[0]); j++) {
            assert_true(find(text, banned[j]) < 0);
        }
        free(text);
    }
}

/* ── 审计 ③：不得直接打开输入设备 ───────────────────────────────── */

TEST_FUNC(test_no_direct_input_device_access)
{
    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);
        assert_true(find(text, "/dev/keyboard") < 0);
        assert_true(find(text, "/dev/mouse") < 0);
        assert_true(find(text, "/dev/input") < 0);
        free(text);
    }
}

/* ── 审计 ④：不得包含 libgui 的私有头 ───────────────────────────── */

static bool has_private_header(const char *text)
{
    static const char *const banned[] = {
        "gui_lvgl_private.h",
        "gui_input_private.h",
        "gui_platform_private.h",
        "gui_tick.h",
        "gui_widget_private.h",
        "gui_cursor_icon.h",
        "gui_input_ps2.h",
    };
    for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
        if (find(text, banned[i]) >= 0) {
            return true;
        }
    }
    return false;
}

TEST_FUNC(test_no_private_libgui_headers)
{
    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);
        assert_false(has_private_header(text));
        free(text);
    }
}

/* ── 审计 ⑤：不得出现窗口装饰标识 ───────────────────────────────── */

TEST_FUNC(test_no_window_decoration_identifiers)
{
    static const char *const banned[] = {
        "minimize", "maximize", "window_decoration", "title_bar",
        "window_title", "close_button", "minimize_button", "maximize_button",
    };

    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);
        for (size_t j = 0; j < sizeof(banned) / sizeof(banned[0]); j++) {
            assert_true(find_ci(text, banned[j]) < 0);
        }
        free(text);
    }
}

/* ── 审计 ⑥：不得出现字面量字号 ─────────────────────────────────── */

TEST_FUNC(test_no_literal_font_sizes)
{
    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);

        /* lv_obj_set_style_text_font(obj, <字号>, ...) 的字号位置必须是
         * 一个表达式（来自 gui_theme_font_size / desktop_shell_font），
         * 不能是字面量数字。 */
        const char *p = text;
        while ((p = strstr(p, "text_font")) != NULL) {
            const char *comma = strchr(p, ',');
            if (comma != NULL) {
                const char *q = comma + 1;
                while (*q == ' ' || *q == '\t') {
                    q++;
                }
                assert_false(*q >= '0' && *q <= '9');
            }
            p += 9;
        }
        free(text);
    }
}

/* ── 审计 ⑦：不得包含后端 include 或 lv_* 符号 ─────────────────── */

static bool has_lv_symbol(const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "lv_")) != NULL) {
        if (p == text || (!isalnum((unsigned char)*(p - 1)) && *(p - 1) != '_')) {
            if (isalnum((unsigned char)*(p + 3))) {
                return true;
            }
        }
        p += 3;
    }
    return false;
}

static bool has_backend_include_or_symbol(const char *text)
{
    if (find_ci(text, "lvgl.h") >= 0 || find_ci(text, "lvgl/") >= 0 ||
        find_ci(text, "thirdpart/lvgl") >= 0 || find_ci(text, "lv_conf.h") >= 0) {
        return true;
    }
    if (has_lv_symbol(text)) {
        return true;
    }
    return false;
}

TEST_FUNC(test_no_backend_includes_or_symbols)
{
    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);
        assert_false(has_backend_include_or_symbol(text));
        free(text);
    }
}

/* ── 审计 ⑧：不得出现旧 desktop_shell_focus_* 符号 ─────────────── */

static bool has_legacy_focus(const char *text)
{
    /* 必须匹配 desktop_shell_focus_*，不能使用会误伤 gui_focus_move 的裸 substring focus_move */
    return find(text, "desktop_shell_focus_") >= 0;
}

TEST_FUNC(test_no_legacy_desktop_shell_focus)
{
    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);
        assert_false(has_legacy_focus(text));
        free(text);
    }
}

/* ── 审计 ⑨：不得出现手工聚焦投递或后端地址转换 ─────────────────── */

static bool has_manual_focus_or_backend_conversion(const char *text)
{
    static const char *const banned[] = {
        "lv_group",
        "LV_EVENT_FOCUSED",
        "LV_EVENT_DEFOCUSED",
        "LV_EVENT_CLICKED",
        "lv_obj_t",
        "lv_group_t",
        "(lv_obj_t *)",
        "(lv_group_t *)",
    };
    for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
        if (find(text, banned[i]) >= 0) {
            return true;
        }
    }
    return false;
}

TEST_FUNC(test_no_manual_focus_or_backend_conversion)
{
    for (size_t i = 0; i < FILE_COUNT; i++) {
        char *text = load(g_files[i]);
        assert_not_null(text);
        assert_false(has_manual_focus_or_backend_conversion(text));
        free(text);
    }
}

/* ── 审计负 fixture：各项规则确实能捕获违规 ─────────────────────── */

TEST_FUNC(test_desktop_audit_negative_fixtures)
{
    /* ① 桌面偷偷引入后端 include */
    char bad_inc[] = "#include <lvgl.h>\nint x = 0;\n";
    assert_true(has_backend_include_or_symbol(bad_inc));

    /* ② 桌面偷偷使用后端 lv_ 符号 */
    char bad_sym[] = "void foo(void) { lv_obj_create(0); }\n";
    assert_true(has_backend_include_or_symbol(bad_sym));

    /* ③ 桌面引入私有头 */
    char bad_priv[] = "#include <gui_widget_private.h>\n";
    assert_true(has_private_header(bad_priv));

    /* ④ 旧 desktop_shell_focus_* 调用 */
    char bad_legacy[] = "desktop_shell_focus_add(shell, btn);\n";
    assert_true(has_legacy_focus(bad_legacy));

    /* ⑤ 手工聚焦投递或后端地址转换 */
    char bad_manual[] = "lv_event_send(obj, LV_EVENT_FOCUSED, NULL);\n";
    assert_true(has_manual_focus_or_backend_conversion(bad_manual));

    char bad_cast[] = "lv_obj_t *target = (lv_obj_t *)handle;\n";
    assert_true(has_manual_focus_or_backend_conversion(bad_cast));

    /* ⑥ 正向控制：公共 API gui_focus_move 不被误伤 */
    char good_focus[] = "int rc = gui_focus_move(input, GUI_FOCUS_FORWARD);\n";
    assert_false(has_backend_include_or_symbol(good_focus));
    assert_false(has_legacy_focus(good_focus));
    assert_false(has_manual_focus_or_backend_conversion(good_focus));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_no_color_literals_in_desktop_sources),
    TEST_ENTRY(test_no_hand_drawn_primitives),
    TEST_ENTRY(test_no_direct_input_device_access),
    TEST_ENTRY(test_no_private_libgui_headers),
    TEST_ENTRY(test_no_window_decoration_identifiers),
    TEST_ENTRY(test_no_literal_font_sizes),
    TEST_ENTRY(test_no_backend_includes_or_symbols),
    TEST_ENTRY(test_no_legacy_desktop_shell_focus),
    TEST_ENTRY(test_no_manual_focus_or_backend_conversion),
    TEST_ENTRY(test_desktop_audit_negative_fixtures),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
