/*
 * theme.c — Material 设计令牌表实现（GUI-3）
 *
 * 本文件**只**包含令牌表的取值，不含 LVGL 代码：LVGL 主题落地在
 * theme_apply.c。这样宿主契约测试可以只编译本文件就验证令牌表，不必
 * 链接 LVGL。
 *
 * 令牌值的唯一真源是设计规范 docs/superpowers/specs/
 * 2026-10-24-gui-3-material-desktop-design.md §4。
 */

#include <gui/gui_theme.h>

#include <stddef.h>

/* ── 色彩角色（0x00RRGGBB） ─────────────────────────────────────── */

static const gui_theme_colors_t g_colors = {
    .primary                  = 0x000B57D0u,  /* #0B57D0 */
    .on_primary               = 0x00FFFFFFu,  /* #FFFFFF */
    .primary_container        = 0x00D3E3FDu,  /* #D3E3FD */
    .on_primary_container     = 0x00041E49u,  /* #041E49 */
    .secondary                = 0x0000639Bu,  /* #00639B */
    .on_secondary             = 0x00FFFFFFu,  /* #FFFFFF */
    .secondary_container      = 0x00CDE5FFu,  /* #CDE5FF */
    .on_secondary_container   = 0x00001D33u,  /* #001D33 */
    .tertiary                 = 0x0008B5000u,  /* #8B5000 */
    .on_tertiary              = 0x00FFFFFFu,  /* #FFFFFF */
    .tertiary_container       = 0x00FFDDB6u,  /* #FFDDB6 */
    .on_tertiary_container    = 0x002C1600u,  /* #2C1600 */
    .error                    = 0x00BA1A1Au,  /* #BA1A1A */
    .on_error                 = 0x00FFFFFFu,  /* #FFFFFF */
    .error_container          = 0x00FFDAD6u,  /* #FFDAD6 */
    .on_error_container       = 0x00410002u,  /* #410002 */
    .success                  = 0x00146C2Eu,  /* #146C2E */
    .on_success               = 0x00FFFFFFu,  /* #FFFFFF */
    .success_container        = 0x00C4EED0u,  /* #C4EED0 */
    .on_success_container     = 0x0000210Bu,  /* #00210B */
    .surface                  = 0x00FDFCFFu,  /* #FDFCFF */
    .surface_dim              = 0x00D8D2E4u,  /* #D8D2E4 */
    .on_surface               = 0x001A1C1Eu,  /* #1A1C1E */
    .surface_variant          = 0x00E0E2ECu,  /* #E0E2EC */
    .on_surface_variant       = 0x0044474Fu,  /* #44474F */
    .surface_container_low    = 0x00F7F2FAu,  /* #F7F2FA */
    .surface_container        = 0x00F3EDF7u,  /* #F3EDF7 */
    .surface_container_high   = 0x00ECE6F0u,  /* #ECE6F0 */
    .surface_container_highest= 0x00E6E0E9u,  /* #E6E0E9 */
    .outline                  = 0x0074777Fu,  /* #74777F */
    .outline_variant          = 0x00C4C6D0u,  /* #C4C6D0 */
    .inverse_surface          = 0x002F3033u,  /* #2F3033 */
    .inverse_on_surface       = 0x00F1F0F4u,  /* #F1F0F4 */
    .scrim                    = 0x00000000u,  /* #000000 */
    .shadow                   = 0x00000000u,  /* #000000 */
};

/* ── 字号层级 ──────────────────────────────────────────────────── */

static const int g_font_sizes[] = { 24, 20, 16, 14, 12 };

/* ── 间距刻度 ──────────────────────────────────────────────────── */

static const int g_spaces[] = { 4, 8, 16, 24, 32 };

/* ── 圆角刻度 ──────────────────────────────────────────────────── */

static const int g_radii[] = { 4, 8, 12, 16, 28 };

/* ── elevation 刻度 ────────────────────────────────────────────── */

static const int g_elev_shadow_width[]   = { 0, 1, 2, 4, 6, 8 };
static const int g_elev_shadow_offset_y[] = { 0, 1, 2, 4, 6, 8 };
static const int g_elev_shadow_opa[]      = { 0, 20, 31, 41, 51, 61 };

/*
 * elevation 的表面色：0 → 页面底，1..5 → 由低到高的容器表面。
 * 4 与 5 同色（M3 的最高两层容器色一致），靠阴影区分。
 */
static const uint32_t g_elev_surfaces[] = {
    0x00FDFCFFu, /* ELEVATION_0  surface */
    0x00F7F2FAu, /* ELEVATION_1  surface_container_low */
    0x00F3EDF7u, /* ELEVATION_2  surface_container */
    0x00ECE6F0u, /* ELEVATION_3  surface_container_high */
    0x00E6E0E9u, /* ELEVATION_4  surface_container_highest */
    0x00E6E0E9u, /* ELEVATION_5  surface_container_highest */
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

const gui_theme_colors_t *gui_theme_colors(void)
{
    return &g_colors;
}

int gui_theme_font_size(gui_theme_font_t role)
{
    if ((size_t)role >= ARRAY_LEN(g_font_sizes)) {
        return -1;
    }
    return g_font_sizes[role];
}

int gui_theme_space(gui_theme_space_t role)
{
    if ((size_t)role >= ARRAY_LEN(g_spaces)) {
        return -1;
    }
    return g_spaces[role];
}

int gui_theme_radius(gui_theme_radius_t role)
{
    if ((size_t)role >= ARRAY_LEN(g_radii)) {
        return -1;
    }
    return g_radii[role];
}

int gui_theme_elevation_shadow_width(gui_theme_elevation_t role)
{
    if ((size_t)role >= ARRAY_LEN(g_elev_shadow_width)) {
        return -1;
    }
    return g_elev_shadow_width[role];
}

int gui_theme_elevation_shadow_offset_y(gui_theme_elevation_t role)
{
    if ((size_t)role >= ARRAY_LEN(g_elev_shadow_offset_y)) {
        return -1;
    }
    return g_elev_shadow_offset_y[role];
}

int gui_theme_elevation_shadow_opa(gui_theme_elevation_t role)
{
    if ((size_t)role >= ARRAY_LEN(g_elev_shadow_opa)) {
        return -1;
    }
    return g_elev_shadow_opa[role];
}

uint32_t gui_theme_elevation_surface(gui_theme_elevation_t role)
{
    if ((size_t)role >= ARRAY_LEN(g_elev_surfaces)) {
        return 0xFFFFFFFFu;
    }
    return g_elev_surfaces[role];
}
