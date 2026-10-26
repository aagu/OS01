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
        .name = "Files",
        .desc = "Read-only browser for the root filesystem",
        .glyph = "F",
        .accent = DESKTOP_ACCENT_PRIMARY,
        .in_launcher = true,
    },
    {
        .id = DESKTOP_APP_TERMINAL,
        .name = "Terminal",
        .desc = "Built-in command line",
        .glyph = ">",
        .accent = DESKTOP_ACCENT_SECONDARY,
        .in_launcher = true,
    },
    {
        .id = DESKTOP_APP_SYSTEM,
        .name = "System",
        .desc = "Kernel and runtime information",
        .glyph = "K",
        .accent = DESKTOP_ACCENT_TERTIARY,
        .in_launcher = false,
    },
    {
        .id = DESKTOP_APP_SETTINGS,
        .name = "Settings",
        .desc = "Clock, wallpaper and density",
        .glyph = "S",
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

gui_color_role_t desktop_app_accent_role(desktop_app_id_t id)
{
    const desktop_app_info_t *info = desktop_app_info(id);
    if (info == NULL) {
        return GUI_COLOR_COUNT;
    }

    switch (info->accent) {
    case DESKTOP_ACCENT_PRIMARY:   return GUI_COLOR_PRIMARY;
    case DESKTOP_ACCENT_SECONDARY: return GUI_COLOR_SECONDARY;
    case DESKTOP_ACCENT_TERTIARY:  return GUI_COLOR_TERTIARY;
    case DESKTOP_ACCENT_SUCCESS:   return GUI_COLOR_SUCCESS;
    case DESKTOP_ACCENT_ERROR:     return GUI_COLOR_ERROR;
    default:                       return GUI_COLOR_PRIMARY;
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
