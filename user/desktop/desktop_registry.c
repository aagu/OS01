/*
 * desktop_registry.c — 桌面应用注册表（GUI-3）
 *
 * 纯 C 数据实现：不包含 LVGL 头，颜色一律通过角色映射到令牌表。
 */

#include "desktop_registry.h"

#include <gui/gui_theme.h>

#include <stddef.h>
#include <string.h>

static const desktop_app_info_t g_apps[DESKTOP_APP_COUNT] = {
    {
        .id = DESKTOP_APP_FILES,
        .name = "文件",
        .desc = "只读浏览根文件系统",
        .glyph = "文",
        .accent = DESKTOP_ACCENT_PRIMARY,
        .in_launcher = true,
    },
    {
        .id = DESKTOP_APP_TERMINAL,
        .name = "终端",
        .desc = "壳内命令台",
        .glyph = ">",
        .accent = DESKTOP_ACCENT_SECONDARY,
        .in_launcher = true,
    },
    {
        .id = DESKTOP_APP_SYSTEM,
        .name = "系统",
        .desc = "内核与运行时信息",
        .glyph = "系",
        .accent = DESKTOP_ACCENT_TERTIARY,
        .in_launcher = false,
    },
    {
        .id = DESKTOP_APP_SETTINGS,
        .name = "设置",
        .desc = "时钟、壁纸与密度",
        .glyph = "设",
        .accent = DESKTOP_ACCENT_SUCCESS,
        .in_launcher = true,
    },
};

static const char *const g_id_names[DESKTOP_APP_COUNT] = {
    "files",
    "terminal",
    "system",
    "settings",
};

const desktop_app_info_t *desktop_app_info(desktop_app_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)DESKTOP_APP_COUNT) {
        return NULL;
    }
    return &g_apps[id];
}

uint32_t desktop_app_count(void)
{
    return (uint32_t)DESKTOP_APP_COUNT;
}

uint32_t desktop_app_accent_color(desktop_app_id_t id)
{
    const desktop_app_info_t *info = desktop_app_info(id);
    if (info == NULL) {
        return 0u;
    }

    const gui_theme_colors_t *c = gui_theme_colors();
    switch (info->accent) {
    case DESKTOP_ACCENT_PRIMARY:   return c->primary;
    case DESKTOP_ACCENT_SECONDARY: return c->secondary;
    case DESKTOP_ACCENT_TERTIARY:  return c->tertiary;
    case DESKTOP_ACCENT_SUCCESS:   return c->success;
    case DESKTOP_ACCENT_ERROR:     return c->error;
    default:                       return c->primary;
    }
}

const char *desktop_app_id_name(desktop_app_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)DESKTOP_APP_COUNT) {
        return NULL;
    }
    return g_id_names[id];
}

bool desktop_app_id_from_name(const char *name, desktop_app_id_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < (int)DESKTOP_APP_COUNT; i++) {
        if (strcmp(name, g_id_names[i]) == 0) {
            *out = (desktop_app_id_t)i;
            return true;
        }
    }
    return false;
}

const desktop_app_info_t *desktop_app_launcher_entry(uint32_t index)
{
    uint32_t seen = 0;
    for (int i = 0; i < (int)DESKTOP_APP_COUNT; i++) {
        if (!g_apps[i].in_launcher) {
            continue;
        }
        if (seen == index) {
            return &g_apps[i];
        }
        seen++;
    }
    return NULL;
}

uint32_t desktop_app_launcher_count(void)
{
    uint32_t count = 0;
    for (int i = 0; i < (int)DESKTOP_APP_COUNT; i++) {
        if (g_apps[i].in_launcher) {
            count++;
        }
    }
    return count;
}
