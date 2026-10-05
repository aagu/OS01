/* user/desktop.c — Material Design Graphical Desktop for OS01 (High-Clarity M3)
 *
 * Demonstrates:
 *   - Framebuffer metadata query via /dev/fb + FBIOSURRENDER
 *   - 2D accelerated presentation via libgfx (/dev/gfx0)
 *   - Google Material Design aesthetics with pixel-perfect clarity:
 *       * High-contrast Slate canvas with Google 4-color accent top strip
 *       * Elevated Material Cards with crisp 1px structural outline & clean 2px drop shadow
 *       * Rounded squircle app icons with sharp contours & high-contrast vector glyphs
 *       * Material Bottom Navigation Bar with crisp divider & pill launcher
 *       * Google 4-color identity dots with dark borders
 *       * Material Card Welcome Window with primary header, feature tiles & action buttons
 *       * Elevated App Drawer with search bar & Material app list
 *       * Sleek status tray with battery/speaker indicators & dynamic clock
 *       * Smooth mouse cursor & keyboard navigation ('S' drawer, 'Q' exit)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <uapi/mouse.h>
#include <gfx.h>
#include "font8x16.h"

#define FBIOSURRENDER 0x00004601

struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

/* ── Material Design Color Palette (M3 High-Clarity Edition) ── */
#define CLR_WALLPAPER          0x00CBD5E1u  /* Slate 300 — high-contrast clean canvas */
#define CLR_CARD_SURFACE       0x00FFFFFFu  /* Pure White Surface */
#define CLR_CARD_BORDER        0x00334155u  /* Slate 700 — crisp 1px structural outline */
#define CLR_CARD_BORDER_SUBTLE 0x0094A3B8u  /* Slate 400 — clean secondary stroke */
#define CLR_SHADOW_CRISP       0x00475569u  /* Slate 600 — clean 2px solid elevation shadow */

#define CLR_PRIMARY            0x001A73E8u  /* Google Blue 600 */
#define CLR_PRIMARY_DARK       0x001557B0u  /* Google Blue 800 */
#define CLR_PRIMARY_CONTAINER  0x00E8F0FEu  /* Selected tab / container bg */
#define CLR_SURFACE_VARIANT    0x00F1F5F9u  /* Slate 100 — high-legibility chip / card bg */

#define CLR_TEXT_HIGH          0x00000000u  /* 100% Solid Black for maximum bitmap clarity */
#define CLR_TEXT_MED           0x00334155u  /* Slate 700 (sharp secondary text) */
#define CLR_TEXT_WHITE         0x00FFFFFFu  /* Pure White on dark/primary */
#define CLR_DIVIDER            0x00CBD5E1u  /* Crisp divider rule */

/* Google 4-Color Identity */
#define CLR_G_BLUE             0x001A73E8u
#define CLR_G_RED              0x00EA4335u
#define CLR_G_YELLOW           0x00FBBC05u
#define CLR_G_GREEN            0x0034A853u

/* App Squircle Colors */
#define CLR_APP_FILES          0x00F59E0Bu  /* Amber 500 */
#define CLR_APP_FILES_DARK     0x0078350Fu  /* Amber 900 border */
#define CLR_APP_NET            0x000284C7u  /* Sky Blue 600 */
#define CLR_APP_NET_DARK       0x00075985u  /* Sky Blue 800 border */
#define CLR_APP_TERM           0x001E293Bu  /* Slate 800 */
#define CLR_APP_TERM_DARK      0x000F172Au  /* Slate 900 border */
#define CLR_APP_SETTINGS       0x007C3AEDu  /* Violet 600 */
#define CLR_APP_SETTINGS_DARK  0x004C1D95u  /* Violet 900 border */
#define CLR_APP_TRASH          0x00DC2626u  /* Red 600 */
#define CLR_APP_TRASH_DARK     0x00991B1Bu  /* Red 800 border */

/* ── Global State ───────────────────────────────────────────── */
static gfx_handle_t *gfx = NULL;
static uint32_t screen_w = 0;
static uint32_t screen_h = 0;

static bool drawer_open = false;
static bool window_open = true;
static int mouse_x = 100;
static int mouse_y = 100;
static bool has_mouse = false;

/* ── Text Drawing Helper ────────────────────────────────────── */
static void draw_string(int32_t x, int32_t y, const char *str,
                        uint32_t fg, uint32_t bg, bool bg_opaque)
{
    while (*str) {
        uint8_t c = (uint8_t)*str++;
        if (c < 128) {
            gfx_draw_glyph(gfx, x, y, font_8x16[c], 1, 8, 16, fg, bg, bg_opaque);
        }
        x += 8;
    }
}

/* ── Material Elevation & Card Helpers ──────────────────────── */

/* Draw an elevated Material card with crisp 1px border & 2px elevation shadow */
static void draw_elevated_card(int32_t x, int32_t y, uint32_t w, uint32_t h,
                               uint32_t bg_color, bool outline)
{
    /* Clean 2px solid elevation shadow (bottom & right) */
    gfx_fill_rect(gfx, x + 2, y + (int32_t)h, w, 2, CLR_SHADOW_CRISP);
    gfx_fill_rect(gfx, x + (int32_t)w, y + 2, 2, h, CLR_SHADOW_CRISP);

    /* Card background */
    gfx_fill_rect(gfx, x, y, w, h, bg_color);

    /* Crisp structural outline */
    if (outline) {
        gfx_rect(gfx, x, y, w, h, CLR_CARD_BORDER);
    }
}

/* Draw a pill / rounded button */
static void draw_pill_button(int32_t x, int32_t y, uint32_t w, uint32_t h,
                             uint32_t bg_color, uint32_t border_color)
{
    if (w < 4 || h < 4) return;

    /* Main rectangular core */
    gfx_fill_rect(gfx, x + 2, y, w - 4, h, bg_color);
    gfx_fill_rect(gfx, x, y + 2, w, h - 4, bg_color);

    /* Corner fill pixels */
    gfx_pixel(gfx, x + 1, y + 1, bg_color);
    gfx_pixel(gfx, x + (int32_t)w - 2, y + 1, bg_color);
    gfx_pixel(gfx, x + 1, y + (int32_t)h - 2, bg_color);
    gfx_pixel(gfx, x + (int32_t)w - 2, y + (int32_t)h - 2, bg_color);

    /* Border outline if different */
    if (border_color != bg_color) {
        gfx_hline(gfx, x + 2, y, w - 4, border_color);
        gfx_hline(gfx, x + 2, y + (int32_t)h - 1, w - 4, border_color);
        gfx_vline(gfx, x, y + 2, h - 4, border_color);
        gfx_vline(gfx, x + (int32_t)w - 1, y + 2, h - 4, border_color);
        gfx_pixel(gfx, x + 1, y + 1, border_color);
        gfx_pixel(gfx, x + (int32_t)w - 2, y + 1, border_color);
        gfx_pixel(gfx, x + 1, y + (int32_t)h - 2, border_color);
        gfx_pixel(gfx, x + (int32_t)w - 2, y + (int32_t)h - 2, border_color);
    }
}

/* ── Google 4-Color Dots Indicator ──────────────────────────── */
static void draw_google_dots(int32_t x, int32_t y)
{
    /* 4 neat circular/square dots in Google identity colors */
    gfx_fill_rect(gfx, x,      y, 5, 5, CLR_G_BLUE);
    gfx_rect(gfx,      x,      y, 5, 5, 0x000F172Au);
    gfx_fill_rect(gfx, x + 7,  y, 5, 5, CLR_G_RED);
    gfx_rect(gfx,      x + 7,  y, 5, 5, 0x000F172Au);
    gfx_fill_rect(gfx, x + 14, y, 5, 5, CLR_G_YELLOW);
    gfx_rect(gfx,      x + 14, y, 5, 5, 0x000F172Au);
    gfx_fill_rect(gfx, x + 21, y, 5, 5, CLR_G_GREEN);
    gfx_rect(gfx,      x + 21, y, 5, 5, 0x000F172Au);
}

/* ── Material Squircle App Icons ────────────────────────────── */

/* Base squircle tile (36x36) with crisp outline & elevation */
static void draw_app_squircle(int32_t x, int32_t y, uint32_t bg_color, uint32_t border_color)
{
    /* 1px crisp solid drop shadow (bottom & right) */
    gfx_fill_rect(gfx, x + 3, y + 36, 31, 2, CLR_SHADOW_CRISP);
    gfx_fill_rect(gfx, x + 36, y + 3, 2, 31, CLR_SHADOW_CRISP);

    /* Main body fill */
    gfx_fill_rect(gfx, x + 2, y + 1, 32, 34, bg_color);
    gfx_fill_rect(gfx, x + 1, y + 2, 34, 32, bg_color);

    /* Crisp 1px perimeter border */
    gfx_hline(gfx, x + 3, y, 30, border_color);
    gfx_hline(gfx, x + 3, y + 35, 30, border_color);
    gfx_vline(gfx, x, y + 3, 30, border_color);
    gfx_vline(gfx, x + 35, y + 3, 30, border_color);

    /* Corner bevels for clean roundness without jagged corners */
    gfx_pixel(gfx, x + 1, y + 1, border_color);
    gfx_pixel(gfx, x + 2, y, border_color);
    gfx_pixel(gfx, x, y + 2, border_color);

    gfx_pixel(gfx, x + 34, y + 1, border_color);
    gfx_pixel(gfx, x + 33, y, border_color);
    gfx_pixel(gfx, x + 35, y + 2, border_color);

    gfx_pixel(gfx, x + 1, y + 34, border_color);
    gfx_pixel(gfx, x + 2, y + 35, border_color);
    gfx_pixel(gfx, x, y + 33, border_color);

    gfx_pixel(gfx, x + 34, y + 34, border_color);
    gfx_pixel(gfx, x + 33, y + 35, border_color);
    gfx_pixel(gfx, x + 35, y + 33, border_color);
}

/* Icon 1: Files / Storage */
static void draw_icon_files(int32_t x, int32_t y)
{
    draw_app_squircle(x, y, CLR_APP_FILES, CLR_APP_FILES_DARK);

    /* Back flap & tab */
    gfx_fill_rect(gfx, x + 8, y + 9, 8, 4, CLR_CARD_SURFACE);
    gfx_rect(gfx, x + 8, y + 9, 8, 4, 0x001E293Bu);

    /* White document paper inside */
    gfx_fill_rect(gfx, x + 11, y + 10, 14, 4, CLR_CARD_SURFACE);
    gfx_rect(gfx, x + 11, y + 10, 14, 4, 0x001E293Bu);

    /* Front folder body */
    gfx_fill_rect(gfx, x + 7, y + 13, 22, 13, 0x00FFFBEBu);
    gfx_rect(gfx, x + 7, y + 13, 22, 13, 0x001E293Bu);

    /* Folder crease line */
    gfx_hline(gfx, x + 9, y + 16, 18, 0x00F59E0Bu);
}

/* Icon 2: Network / Cloud */
static void draw_icon_network(int32_t x, int32_t y)
{
    draw_app_squircle(x, y, CLR_APP_NET, CLR_APP_NET_DARK);

    /* Crisp cloud graphic */
    gfx_fill_rect(gfx, x + 11, y + 10, 14, 15, CLR_CARD_SURFACE);
    gfx_fill_rect(gfx, x + 7,  y + 14, 22, 11, CLR_CARD_SURFACE);
    gfx_rect(gfx,      x + 7,  y + 14, 22, 11, 0x001E293Bu);
    gfx_rect(gfx,      x + 11, y + 10, 14, 15, 0x001E293Bu);

    /* Inner signal wave */
    gfx_fill_rect(gfx, x + 16, y + 17, 4, 4, 0x000284C7u);
    gfx_hline(gfx, x + 10, y + 20, 16, 0x000284C7u);
}

/* Icon 3: Terminal */
static void draw_icon_terminal(int32_t x, int32_t y)
{
    draw_app_squircle(x, y, CLR_APP_TERM, CLR_APP_TERM_DARK);

    /* Mini terminal frame */
    gfx_fill_rect(gfx, x + 6, y + 8, 24, 20, 0x00000000u);
    gfx_rect(gfx,      x + 6, y + 8, 24, 20, 0x0064748Bu);

    /* Title bar with dots */
    gfx_fill_rect(gfx, x + 7, y + 9, 22, 3, 0x00334155u);
    gfx_pixel(gfx, x + 8,  y + 10, CLR_G_RED);
    gfx_pixel(gfx, x + 11, y + 10, CLR_G_YELLOW);
    gfx_pixel(gfx, x + 14, y + 10, CLR_G_GREEN);

    /* Bold green prompt `>` */
    gfx_line(gfx, x + 9,  y + 15, x + 13, y + 18, CLR_G_GREEN);
    gfx_line(gfx, x + 9,  y + 16, x + 13, y + 19, CLR_G_GREEN);
    gfx_line(gfx, x + 13, y + 18, x + 9,  y + 21, CLR_G_GREEN);
    gfx_line(gfx, x + 13, y + 19, x + 9,  y + 22, CLR_G_GREEN);

    /* White cursor `_` */
    gfx_fill_rect(gfx, x + 16, y + 21, 6, 2, CLR_CARD_SURFACE);
}

/* Icon 4: Settings */
static void draw_icon_settings(int32_t x, int32_t y)
{
    draw_app_squircle(x, y, CLR_APP_SETTINGS, CLR_APP_SETTINGS_DARK);

    /* Slider 1 track & knob */
    gfx_fill_rect(gfx, x + 8,  y + 13, 20, 2, 0x00312E81u);
    gfx_fill_rect(gfx, x + 11, y + 10, 6,  8, CLR_CARD_SURFACE);
    gfx_rect(gfx,      x + 11, y + 10, 6,  8, 0x001E293Bu);
    gfx_vline(gfx,     x + 13, y + 12, 4, 0x007C3AEDu);

    /* Slider 2 track & knob */
    gfx_fill_rect(gfx, x + 8,  y + 22, 20, 2, 0x00312E81u);
    gfx_fill_rect(gfx, x + 19, y + 19, 6,  8, CLR_CARD_SURFACE);
    gfx_rect(gfx,      x + 19, y + 19, 6,  8, 0x001E293Bu);
    gfx_vline(gfx,     x + 21, y + 21, 4, 0x007C3AEDu);
}

/* Icon 5: Trash */
static void draw_icon_trash(int32_t x, int32_t y)
{
    draw_app_squircle(x, y, CLR_APP_TRASH, CLR_APP_TRASH_DARK);

    /* Lid handle */
    gfx_fill_rect(gfx, x + 16, y + 8, 4, 2, CLR_CARD_SURFACE);
    gfx_rect(gfx,      x + 16, y + 8, 4, 2, 0x001E293Bu);

    /* Lid */
    gfx_fill_rect(gfx, x + 9,  y + 10, 18, 3, CLR_CARD_SURFACE);
    gfx_rect(gfx,      x + 9,  y + 10, 18, 3, 0x001E293Bu);

    /* Bin body */
    gfx_fill_rect(gfx, x + 11, y + 13, 14, 14, CLR_CARD_SURFACE);
    gfx_rect(gfx,      x + 11, y + 13, 14, 14, 0x001E293Bu);

    /* Slats */
    gfx_fill_rect(gfx, x + 14, y + 16, 2, 8, 0x00DC2626u);
    gfx_fill_rect(gfx, x + 20, y + 16, 2, 8, 0x00DC2626u);
}

static void draw_desktop_icons(void)
{
    int32_t start_x = 24;
    int32_t start_y = 28;
    int32_t spacing_y = 74;

    /* 1. Files */
    draw_icon_files(start_x, start_y);
    draw_string(start_x - 2, start_y + 42, "Files", CLR_TEXT_HIGH, 0, false);

    /* 2. Network */
    draw_icon_network(start_x, start_y + spacing_y);
    draw_string(start_x - 10, start_y + spacing_y + 42, "Network", CLR_TEXT_HIGH, 0, false);

    /* 3. Terminal */
    draw_icon_terminal(start_x, start_y + spacing_y * 2);
    draw_string(start_x - 14, start_y + spacing_y * 2 + 42, "Terminal", CLR_TEXT_HIGH, 0, false);

    /* 4. Settings */
    draw_icon_settings(start_x, start_y + spacing_y * 3);
    draw_string(start_x - 14, start_y + spacing_y * 3 + 42, "Settings", CLR_TEXT_HIGH, 0, false);

    /* 5. Trash */
    draw_icon_trash(start_x, start_y + spacing_y * 4);
    draw_string(start_x - 2, start_y + spacing_y * 4 + 42, "Trash", CLR_TEXT_HIGH, 0, false);
}

/* ── Material Wallpaper Artwork ─────────────────────────────── */
static void draw_wallpaper(void)
{
    /* Clean base canvas in modern Slate 300 — high-contrast & zero smudges */
    gfx_fill_rect(gfx, 0, 0, screen_w, screen_h, CLR_WALLPAPER);

    /* Clean Google Material 4-Color Accent Strip at the very top (3px) */
    if (screen_w > 0) {
        uint32_t seg = screen_w / 4;
        gfx_fill_rect(gfx, 0,           0, seg,                3, CLR_G_BLUE);
        gfx_fill_rect(gfx, seg,         0, seg,                3, CLR_G_RED);
        gfx_fill_rect(gfx, seg * 2,     0, seg,                3, CLR_G_YELLOW);
        gfx_fill_rect(gfx, seg * 3,     0, screen_w - seg * 3, 3, CLR_G_GREEN);
    }
}

/* ── Welcome Window (Material Card Design) ───────────────────── */
static void draw_welcome_window(int32_t wx, int32_t wy, uint32_t ww, uint32_t wh)
{
    /* Elevated surface card with clean 1px border and crisp 2px elevation shadow */
    draw_elevated_card(wx, wy, ww, wh, CLR_CARD_SURFACE, true);

    /* 1. App Bar Header: Google Blue */
    uint32_t bar_h = 38;
    gfx_fill_rect(gfx, wx + 1, wy + 1, ww - 2, bar_h - 1, CLR_PRIMARY);
    gfx_hline(gfx, wx, wy + bar_h, ww, CLR_CARD_BORDER);

    /* Google 4-color dots logo + Title */
    draw_google_dots(wx + 12, wy + 17);
    draw_string(wx + 44, wy + 11, "Welcome to OS01", CLR_TEXT_WHITE, 0, false);

    /* Header Window Controls: [ — ] [ □ ] [ ✕ ] */
    int32_t cx = wx + (int32_t)ww - 28;
    int32_t cy = wy + 11;

    /* Close (✕) */
    draw_string(cx, cy, "x", CLR_TEXT_WHITE, 0, false);

    /* Maximize (□) */
    int32_t mx = cx - 24;
    gfx_rect(gfx, mx, cy + 3, 10, 10, CLR_TEXT_WHITE);
    gfx_hline(gfx, mx, cy + 4, 10, CLR_TEXT_WHITE);

    /* Minimize (—) */
    int32_t lx = mx - 24;
    gfx_fill_rect(gfx, lx, cy + 8, 10, 2, CLR_TEXT_WHITE);

    /* 2. Card Content Area */
    int32_t cx_body = wx + 20;
    int32_t cy_body = wy + 52;

    /* Material Chip: [ MATERIAL DESIGN ] */
    draw_pill_button(cx_body, cy_body, 148, 22, CLR_PRIMARY_CONTAINER, CLR_PRIMARY);
    draw_string(cx_body + 10, cy_body + 3, "MATERIAL DESIGN", CLR_PRIMARY, 0, false);

    /* Title & subtitle */
    draw_string(cx_body, cy_body + 30, "Next-Gen 2D Desktop for OS01", CLR_TEXT_HIGH, 0, false);
    draw_string(cx_body, cy_body + 48, "Clean elevation, cards & responsive blitter", CLR_TEXT_MED, 0, false);

    /* Divider */
    gfx_hline(gfx, cx_body, cy_body + 68, ww - 40, CLR_DIVIDER);

    /* 3. System Specs Tiles (Material Sub-Cards) */
    int32_t tile_y = cy_body + 78;
    int32_t tile_w = (int32_t)ww - 40;

    /* Feature Row 1: Kernel (Blue) */
    draw_pill_button(cx_body, tile_y, tile_w, 24, CLR_SURFACE_VARIANT, CLR_CARD_BORDER_SUBTLE);
    gfx_fill_rect(gfx, cx_body + 4, tile_y + 4, 4, 16, CLR_G_BLUE);
    draw_string(cx_body + 14, tile_y + 4, "Kernel   : x86_64 Multi-Core SMP (smp=2)", CLR_TEXT_HIGH, 0, false);

    /* Feature Row 2: Display (Green) */
    tile_y += 28;
    draw_pill_button(cx_body, tile_y, tile_w, 24, CLR_SURFACE_VARIANT, CLR_CARD_BORDER_SUBTLE);
    gfx_fill_rect(gfx, cx_body + 4, tile_y + 4, 4, 16, CLR_G_GREEN);
    draw_string(cx_body + 14, tile_y + 4, "Graphics : UEFI GOP -> /dev/gfx0 2D Engine", CLR_TEXT_HIGH, 0, false);

    /* Feature Row 3: Memory (Yellow/Amber) */
    tile_y += 28;
    draw_pill_button(cx_body, tile_y, tile_w, 24, CLR_SURFACE_VARIANT, CLR_CARD_BORDER_SUBTLE);
    gfx_fill_rect(gfx, cx_body + 4, tile_y + 4, 4, 16, CLR_G_YELLOW);
    draw_string(cx_body + 14, tile_y + 4, "Memory   : 4KB / 2MB Huge Paging + EEVDF", CLR_TEXT_HIGH, 0, false);

    /* Feature Row 4: Userland (Red) */
    tile_y += 28;
    draw_pill_button(cx_body, tile_y, tile_w, 24, CLR_SURFACE_VARIANT, CLR_CARD_BORDER_SUBTLE);
    gfx_fill_rect(gfx, cx_body + 4, tile_y + 4, 4, 16, CLR_G_RED);
    draw_string(cx_body + 14, tile_y + 4, "Userland : BusyBox 1.36.1 & PTY Terminal", CLR_TEXT_HIGH, 0, false);

    /* 4. Action Row (Hint + Filled Primary Button) */
    int32_t act_y = wy + (int32_t)wh - 38;
    draw_string(cx_body, act_y + 5, "Press 'S' for Apps, 'Q' or ESC to exit", CLR_TEXT_MED, 0, false);

    /* Filled Primary Button: [ CLOSE ] */
    int32_t btn_w = 84;
    int32_t btn_h = 28;
    int32_t btn_x = wx + (int32_t)ww - btn_w - 20;
    int32_t btn_y = act_y;
    draw_pill_button(btn_x, btn_y, btn_w, btn_h, CLR_PRIMARY, CLR_PRIMARY_DARK);
    draw_string(btn_x + 22, btn_y + 6, "CLOSE", CLR_TEXT_WHITE, 0, false);
}

/* ── Material Bottom Navigation Bar / Dock ──────────────────── */
static void draw_taskbar(void)
{
    uint32_t bar_h = 42;
    int32_t ty = (int32_t)screen_h - (int32_t)bar_h;

    /* Crisp top structural border */
    gfx_hline(gfx, 0, ty - 1, screen_w, CLR_CARD_BORDER);

    /* Pure White Dock Surface */
    gfx_fill_rect(gfx, 0, ty, screen_w, bar_h, CLR_CARD_SURFACE);

    /* 1. Material App Drawer / Launcher Button (Pill FAB) */
    int32_t btn_x = 12;
    int32_t btn_y = ty + 6;
    uint32_t btn_w = 96;
    uint32_t btn_h = 30;

    if (drawer_open) {
        /* Open state: filled primary */
        draw_pill_button(btn_x, btn_y, btn_w, btn_h, CLR_PRIMARY, CLR_PRIMARY_DARK);
        draw_google_dots(btn_x + 8, btn_y + 12);
        draw_string(btn_x + 40, btn_y + 7, "Apps", CLR_TEXT_WHITE, 0, false);
    } else {
        /* Idle state: crisp surface container */
        draw_pill_button(btn_x, btn_y, btn_w, btn_h, CLR_SURFACE_VARIANT, CLR_CARD_BORDER_SUBTLE);
        draw_google_dots(btn_x + 8, btn_y + 12);
        draw_string(btn_x + 40, btn_y + 7, "Apps", CLR_TEXT_HIGH, 0, false);
    }

    /* 2. Active Window Chip on Dock */
    if (window_open) {
        int32_t chip_x = btn_x + (int32_t)btn_w + 12;
        uint32_t chip_w = 180;
        draw_pill_button(chip_x, btn_y, chip_w, btn_h, CLR_PRIMARY_CONTAINER, CLR_PRIMARY);

        /* Blue indicator dot + Window title */
        gfx_fill_rect(gfx, chip_x + 10, btn_y + 12, 6, 6, CLR_PRIMARY);
        gfx_rect(gfx, chip_x + 10, btn_y + 12, 6, 6, CLR_PRIMARY_DARK);
        draw_string(chip_x + 22, btn_y + 7, "Welcome to OS01", CLR_PRIMARY, 0, false);

        /* Bottom active underline indicator */
        gfx_fill_rect(gfx, chip_x + 16, btn_y + (int32_t)btn_h - 3, chip_w - 32, 2, CLR_PRIMARY);
    }

    /* 3. System Status Tray (Pill Chip at Bottom Right) */
    uint32_t tray_w = 114;
    uint32_t tray_h = 30;
    int32_t tray_x = (int32_t)screen_w - (int32_t)tray_w - 12;
    int32_t tray_y = ty + 6;

    draw_pill_button(tray_x, tray_y, tray_w, tray_h, CLR_SURFACE_VARIANT, CLR_CARD_BORDER_SUBTLE);

    /* Speaker Icon */
    gfx_fill_rect(gfx, tray_x + 10, tray_y + 10, 3, 10, CLR_TEXT_HIGH);
    gfx_line(gfx, tray_x + 13, tray_y + 10, tray_x + 17, tray_y + 6, CLR_TEXT_HIGH);
    gfx_line(gfx, tray_x + 13, tray_y + 19, tray_x + 17, tray_y + 23, CLR_TEXT_HIGH);
    gfx_vline(gfx, tray_x + 17, tray_y + 6, 18, CLR_TEXT_HIGH);

    /* Battery / Power bar */
    gfx_rect(gfx, tray_x + 24, tray_y + 10, 14, 10, CLR_TEXT_HIGH);
    gfx_fill_rect(gfx, tray_x + 26, tray_y + 12, 8, 6, CLR_G_GREEN);
    gfx_vline(gfx, tray_x + 38, tray_y + 12, 6, CLR_TEXT_HIGH);

    /* Clock text (HH:MM) */
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char time_str[16];
    if (tm_now) {
        snprintf(time_str, sizeof(time_str), "%02d:%02d",
                 tm_now->tm_hour, tm_now->tm_min);
    } else {
        snprintf(time_str, sizeof(time_str), "12:00");
    }
    draw_string(tray_x + 50, tray_y + 7, time_str, CLR_TEXT_HIGH, 0, false);
}

/* ── Material App Drawer (Elevated Floating Card) ────────────── */
static void draw_app_drawer(void)
{
    if (!drawer_open) return;

    uint32_t dw_w = 230;
    uint32_t dw_h = 270;
    int32_t dw_x = 12;
    int32_t dw_y = (int32_t)screen_h - 42 - (int32_t)dw_h - 8;

    /* Elevated surface card with clean 1px border and crisp 2px elevation shadow */
    draw_elevated_card(dw_x, dw_y, dw_w, dw_h, CLR_CARD_SURFACE, true);

    /* 1. Search Bar / Header */
    int32_t sx = dw_x + 12;
    int32_t sy = dw_y + 12;
    uint32_t sw = dw_w - 24;
    draw_pill_button(sx, sy, sw, 30, CLR_SURFACE_VARIANT, CLR_CARD_BORDER_SUBTLE);
    draw_string(sx + 12, sy + 7, "Search apps...", CLR_TEXT_MED, 0, false);

    /* 2. App Items */
    struct {
        const char *name;
        uint32_t color;
        bool is_sep;
    } apps[] = {
        { "Files & Storage", CLR_APP_FILES,    false },
        { "Terminal",        CLR_APP_TERM,     false },
        { "Network Manager", CLR_APP_NET,      false },
        { "System Settings", CLR_APP_SETTINGS, false },
        { NULL,              0,                true  },
        { "Power Off...",    CLR_G_RED,        false },
    };

    int32_t item_y = sy + 40;
    int count = (int)(sizeof(apps) / sizeof(apps[0]));

    for (int i = 0; i < count; i++) {
        if (apps[i].is_sep) {
            gfx_hline(gfx, sx, item_y + 4, sw, CLR_DIVIDER);
            item_y += 10;
            continue;
        }

        /* App icon dot with crisp outline */
        gfx_fill_rect(gfx, sx + 4, item_y + 3, 10, 10, apps[i].color);
        gfx_rect(gfx,      sx + 4, item_y + 3, 10, 10, 0x001E293Bu);

        /* App name in solid black */
        draw_string(sx + 24, item_y, apps[i].name, CLR_TEXT_HIGH, 0, false);
        item_y += 24;
    }

    /* 3. Bottom Brand Footer */
    draw_google_dots(dw_x + 14, dw_y + (int32_t)dw_h - 18);
    draw_string(dw_x + 44, dw_y + (int32_t)dw_h - 23, "OS01 Material 3", CLR_TEXT_MED, 0, false);
}

/* ── Minimalist Material Mouse Cursor ───────────────────────── */
static void draw_mouse_cursor(int32_t mx, int32_t my)
{
    /* Clean, modern Material cursor bitmap (12x19) */
    static const char *cursor[19] = {
        "X...........",
        "XX..........",
        "XWX.........",
        "XWWX........",
        "XWWWX.......",
        "XWWWWX......",
        "XWWWWWX.....",
        "XWWWWWWX....",
        "XWWWWWWWX...",
        "XWWWWWWWWX..",
        "XWWWWXXXXX..",
        "XWWXWWX.....",
        "XWX..XWWX...",
        "XX...XWWX...",
        "X.....XWWX..",
        "......XWWX..",
        ".......XX...",
        "............",
        "............"
    };

    for (int r = 0; r < 19; r++) {
        for (int c = 0; c < 12; c++) {
            char p = cursor[r][c];
            if (p == 'X') {
                gfx_pixel(gfx, mx + c, my + r, CLR_TEXT_HIGH);
            } else if (p == 'W') {
                gfx_pixel(gfx, mx + c, my + r, CLR_CARD_SURFACE);
            }
        }
    }
}

/* ── Full Desktop Redraw ────────────────────────────────────── */
static void render_desktop(void)
{
    /* 1. Wallpaper */
    draw_wallpaper();

    /* 2. Desktop icons */
    draw_desktop_icons();

    /* 3. Welcome Window (Material Card) */
    if (window_open) {
        uint32_t ww = 460;
        uint32_t wh = 290;
        if (ww > screen_w - 40) ww = screen_w - 40;
        if (wh > screen_h - 60) wh = screen_h - 60;
        int32_t wx = (int32_t)(screen_w - ww) / 2;
        int32_t wy = (int32_t)(screen_h - wh - 42) / 2;
        draw_welcome_window(wx, wy, ww, wh);
    }

    /* 4. Material Bottom Bar / Dock */
    draw_taskbar();

    /* 5. App Drawer (if open) */
    draw_app_drawer();

    /* 6. Mouse Cursor */
    if (has_mouse) {
        draw_mouse_cursor(mouse_x, mouse_y);
    }
}

/* ── Main Entry Point ───────────────────────────────────────── */
int main(int argc, char **argv)
{
    bool smoke = (argc > 1 && argv[1] && strcmp(argv[1], "smoke") == 0);

    /* 1. Query Framebuffer metadata */
    int fb_fd = open("/dev/fb", O_RDWR);
    if (fb_fd < 0) {
        perror("desktop: open /dev/fb");
        return 1;
    }

    struct fb_info info;
    ssize_t r = read(fb_fd, &info, sizeof(info));
    ioctl(fb_fd, FBIOSURRENDER, NULL);
    close(fb_fd);

    if (r != (ssize_t)sizeof(info) || info.width == 0 || info.height == 0) {
        fprintf(stderr, "desktop: invalid framebuffer metadata\n");
        return 1;
    }

    screen_w = info.width;
    screen_h = info.height;
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;

    /* 2. Open libgfx 2D view */
    gfx = gfx_open(0, 0, screen_w, screen_h);
    if (!gfx) {
        fprintf(stderr, "desktop: gfx_open failed (errno=%d)\n", errno);
        return 1;
    }

    /* 3. Smoke mode: paint once, present, and exit */
    if (smoke) {
        render_desktop();
        if (gfx_present(gfx) != 0) {
            gfx_close(gfx);
            return 1;
        }
        gfx_close(gfx);
        write(1, "[DESKTOP] SMOKE PASS\n", 21);
        return 0;
    }

    /* 4. Interactive mode setup */
    struct termios orig_term;
    bool has_term = (tcgetattr(STDIN_FILENO, &orig_term) == 0);
    if (has_term) {
        struct termios raw = orig_term;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    /* Switch to alt screen */
    write(1, "\x1b[?1049h\x1b[2J", 12);

    /* Wait out terminal's CMD_HOLD_MS (500ms) delayed flush window so
     * terminal.elf doesn't trample our initial frame with its alt-screen clear */
    poll(NULL, 0, 550);

    /* Non-blocking mouse device */
    int mouse_fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
    if (mouse_fd >= 0) {
        has_mouse = true;
    }

    /* Initial paint */
    render_desktop();
    gfx_present(gfx);

    /* 5. Event loop */
    struct pollfd fds[2];
    int nfds = 1;
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;

    if (mouse_fd >= 0) {
        fds[1].fd = mouse_fd;
        fds[1].events = POLLIN;
        nfds = 2;
    }

    bool running = true;
    time_t last_clock = time(NULL);
    int startup_frames = 0;

    while (running) {
        int ready = poll(fds, nfds, 100); /* 100ms tick for responsiveness */
        bool need_redraw = false;

        /* Actively redraw during the first second to guarantee desktop visibility */
        if (startup_frames < 10) {
            startup_frames++;
            need_redraw = true;
        }

        if (ready > 0) {
            /* Keyboard input */
            if (fds[0].revents & POLLIN) {
                char ch = 0;
                if (read(STDIN_FILENO, &ch, 1) == 1) {
                    if (ch == 'q' || ch == 'Q' || ch == 27 || ch == 3) {
                        /* Exit */
                        running = false;
                        break;
                    } else if (ch == 's' || ch == 'S' || ch == ' ') {
                        /* Toggle App Drawer */
                        drawer_open = !drawer_open;
                        need_redraw = true;
                    } else if (ch == 'w' || ch == 'W') {
                        /* Toggle Welcome Window */
                        window_open = !window_open;
                        need_redraw = true;
                    }
                }
            }

            /* Mouse input */
            if (nfds > 1 && (fds[1].revents & POLLIN)) {
                mouse_event_t mev;
                while (read(mouse_fd, &mev, sizeof(mev)) == (ssize_t)sizeof(mev)) {
                    mouse_x += mev.dx;
                    mouse_y += mev.dy;
                    if (mouse_x < 0) mouse_x = 0;
                    if (mouse_x >= (int)screen_w) mouse_x = (int)screen_w - 1;
                    if (mouse_y < 0) mouse_y = 0;
                    if (mouse_y >= (int)screen_h) mouse_y = (int)screen_h - 1;
                    need_redraw = true;

                    /* Left click */
                    if (mev.buttons & 1) {
                        int32_t ty = (int32_t)screen_h - 42;
                        /* Check Apps button: x=12..108, y=ty+6..ty+36 */
                        if (mouse_x >= 12 && mouse_x <= 108 &&
                            mouse_y >= ty + 6 && mouse_y <= ty + 36) {
                            drawer_open = !drawer_open;
                        } else if (drawer_open) {
                            /* Click outside drawer closes it */
                            drawer_open = false;
                        }
                    }
                }
            }
        }

        /* Clock update check */
        time_t cur_time = time(NULL);
        if (cur_time != last_clock) {
            last_clock = cur_time;
            need_redraw = true;
        }

        if (need_redraw) {
            render_desktop();
            gfx_present(gfx);
        }
    }

    /* 6. Cleanup */
    if (mouse_fd >= 0) close(mouse_fd);
    gfx_close(gfx);

    if (has_term) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    }
    write(1, "\x1b[?1049l", 8);

    return 0;
}
