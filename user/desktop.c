/* user/desktop.c — GUI Desktop for OS01
 *
 * Demonstrates:
 *   - Framebuffer metadata query via /dev/fb + FBIOSURRENDER
 *   - 2D accelerated presentation via libgfx (/dev/gfx0)
 *   - Classic Windows 98 desktop:
 *       * Neutral grey desktop background (0x00808080)
 *       * Desktop icons (My Computer, Network, Recycle Bin, Internet, Readme)
 *       * Bottom taskbar with 3D bevels, active window button, system tray & clock
 *       * Windows 98 4-color Start button & pop-up Start Menu with OS01 98 banner
 *       * Welcome window with gradient title bar, 3D controls & system info
 *       * Interactive mouse cursor & keyboard control (toggle Start menu, exit)
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

/* ── Classic Windows 98 Palette ─────────────────────────────── */
#define CLR_DESKTOP        0x00808080u   /* Classic neutral grey */
#define CLR_FACE           0x00C0C0C0u   /* Standard 3D button/window face */
#define CLR_LIGHT          0x00FFFFFFu   /* 3D highlight (white) */
#define CLR_LIGHT_SHADOW   0x00DFDFDFu   /* 3D soft light */
#define CLR_DARK_SHADOW    0x00808080u   /* 3D shadow (dark grey) */
#define CLR_BLACK          0x00000000u   /* 3D outer dark shadow / border */
#define CLR_TITLE_ACTIVE_L 0x00000080u   /* Active title bar gradient left (dark blue) */
#define CLR_TITLE_ACTIVE_R 0x001084D0u   /* Active title bar gradient right (cyan blue) */
#define CLR_TEXT           0x00000000u   /* Text black */
#define CLR_TEXT_WHITE     0x00FFFFFFu   /* Text white */
#define CLR_MENU_SEL       0x00000080u   /* Menu item selection blue */
#define CLR_WINDOW_BG      0x00FFFFFFu   /* Window client area background (white) */

/* Windows Logo / Accent Colors */
#define CLR_WIN_RED        0x00EE3322u
#define CLR_WIN_GREEN      0x0000B050u
#define CLR_WIN_BLUE       0x000070C0u
#define CLR_WIN_YELLOW     0x00FFC000u

/* ── Global State ───────────────────────────────────────────── */
static gfx_handle_t *gfx = NULL;
static uint32_t screen_w = 0;
static uint32_t screen_h = 0;

static bool start_menu_open = false;
static bool window_open = true;
static int mouse_x = 100;
static int mouse_y = 100;
static bool has_mouse = false;

/* ── 3D Bevel Drawing Helpers ───────────────────────────────── */

/* Raised 3D box (Button, Window border, Menu border) */
static void draw_raised_box(int32_t x, int32_t y, uint32_t w, uint32_t h, bool fill)
{
    if (fill) gfx_fill_rect(gfx, x, y, w, h, CLR_FACE);

    /* Outer bevel: Top & Left = White, Bottom & Right = Black */
    gfx_hline(gfx, x, y, w - 1, CLR_LIGHT);
    gfx_vline(gfx, x, y, h - 1, CLR_LIGHT);
    gfx_hline(gfx, x, y + (int32_t)h - 1, w, CLR_BLACK);
    gfx_vline(gfx, x + (int32_t)w - 1, y, h, CLR_BLACK);

    /* Inner bevel: Top & Left = Light grey, Bottom & Right = Dark grey */
    if (w > 2 && h > 2) {
        gfx_hline(gfx, x + 1, y + 1, w - 3, CLR_LIGHT_SHADOW);
        gfx_vline(gfx, x + 1, y + 1, h - 3, CLR_LIGHT_SHADOW);
        gfx_hline(gfx, x + 1, y + (int32_t)h - 2, w - 2, CLR_DARK_SHADOW);
        gfx_vline(gfx, x + (int32_t)w - 2, y + 1, h - 2, CLR_DARK_SHADOW);
    }
}

/* Sunken 3D box (Taskbar Tray, Text box, Pressed button) */
static void draw_sunken_box(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            uint32_t fill_color, bool fill)
{
    if (fill) gfx_fill_rect(gfx, x, y, w, h, fill_color);

    /* Outer bevel: Top & Left = Dark grey, Bottom & Right = White */
    gfx_hline(gfx, x, y, w - 1, CLR_DARK_SHADOW);
    gfx_vline(gfx, x, y, h - 1, CLR_DARK_SHADOW);
    gfx_hline(gfx, x, y + (int32_t)h - 1, w, CLR_LIGHT);
    gfx_vline(gfx, x + (int32_t)w - 1, y, h, CLR_LIGHT);

    /* Inner bevel: Top & Left = Black, Bottom & Right = Light grey */
    if (w > 2 && h > 2) {
        gfx_hline(gfx, x + 1, y + 1, w - 3, CLR_BLACK);
        gfx_vline(gfx, x + 1, y + 1, h - 3, CLR_BLACK);
        gfx_hline(gfx, x + 1, y + (int32_t)h - 2, w - 2, CLR_LIGHT_SHADOW);
        gfx_vline(gfx, x + (int32_t)w - 2, y + 1, h - 2, CLR_LIGHT_SHADOW);
    }
}

/* Title bar color gradient */
static void draw_gradient_titlebar(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return;
    for (uint32_t i = 0; i < w; i++) {
        uint32_t r = 0 + (16 * i) / w;
        uint32_t g = 0 + (132 * i) / w;
        uint32_t b = 128 + ((208 - 128) * i) / w;
        uint32_t color = (r << 16) | (g << 8) | b;
        gfx_vline(gfx, x + (int32_t)i, y, h, color);
    }
}

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

/* ── Windows 98 4-Color Flag Logo ───────────────────────────── */
static void draw_windows_flag(int32_t x, int32_t y)
{
    /* 4 colored squares separated by 1px spacing */
    gfx_fill_rect(gfx, x,     y,     6, 6, CLR_WIN_RED);
    gfx_fill_rect(gfx, x + 7, y,     6, 6, CLR_WIN_GREEN);
    gfx_fill_rect(gfx, x,     y + 7, 6, 6, CLR_WIN_BLUE);
    gfx_fill_rect(gfx, x + 7, y + 7, 6, 6, CLR_WIN_YELLOW);
}

/* ── Desktop Icons ──────────────────────────────────────────── */

/* "My Computer" icon */
static void draw_icon_my_computer(int32_t x, int32_t y)
{
    /* Monitor frame */
    gfx_fill_rect(gfx, x + 2, y, 28, 20, 0x00E0E0E0u);
    gfx_rect(gfx, x + 2, y, 28, 20, CLR_BLACK);
    /* Screen */
    gfx_fill_rect(gfx, x + 5, y + 3, 22, 14, 0x00008080u);
    /* Mini window on screen */
    gfx_fill_rect(gfx, x + 7, y + 5, 10, 2, CLR_LIGHT);
    gfx_fill_rect(gfx, x + 7, y + 7, 10, 6, CLR_FACE);
    /* Monitor stand */
    gfx_fill_rect(gfx, x + 13, y + 20, 6, 4, 0x00B0B0B0u);
    gfx_rect(gfx, x + 13, y + 20, 6, 4, CLR_BLACK);
    /* Monitor base */
    gfx_fill_rect(gfx, x + 8, y + 24, 16, 3, 0x00E0E0E0u);
    gfx_rect(gfx, x + 8, y + 24, 16, 3, CLR_BLACK);
}

/* "Network" icon */
static void draw_icon_network(int32_t x, int32_t y)
{
    /* Left PC */
    gfx_fill_rect(gfx, x, y + 2, 16, 12, 0x00D0D0D0u);
    gfx_rect(gfx, x, y + 2, 16, 12, CLR_BLACK);
    gfx_fill_rect(gfx, x + 2, y + 4, 12, 8, 0x00000080u);

    /* Right PC */
    gfx_fill_rect(gfx, x + 14, y + 10, 16, 12, 0x00D0D0D0u);
    gfx_rect(gfx, x + 14, y + 10, 16, 12, CLR_BLACK);
    gfx_fill_rect(gfx, x + 16, y + 12, 12, 8, 0x00000080u);

    /* Connecting cable */
    gfx_hline(gfx, x + 8, y + 25, 16, CLR_BLACK);
    gfx_vline(gfx, x + 8, y + 14, 11, CLR_BLACK);
    gfx_vline(gfx, x + 22, y + 22, 3, CLR_BLACK);
}

/* "Recycle Bin" icon */
static void draw_icon_recycle_bin(int32_t x, int32_t y)
{
    /* Wastebasket rim */
    gfx_fill_rect(gfx, x + 4, y + 2, 24, 4, 0x00E0E0E0u);
    gfx_rect(gfx, x + 4, y + 2, 24, 4, CLR_BLACK);
    /* Bin body */
    gfx_fill_rect(gfx, x + 6, y + 6, 20, 20, 0x00D0D0D0u);
    gfx_rect(gfx, x + 6, y + 6, 20, 20, CLR_BLACK);
    /* Vertical slats / ribs */
    gfx_vline(gfx, x + 10, y + 7, 18, 0x00808080u);
    gfx_vline(gfx, x + 15, y + 7, 18, 0x00808080u);
    gfx_vline(gfx, x + 20, y + 7, 18, 0x00808080u);
    /* Green recycle symbol dot */
    gfx_fill_rect(gfx, x + 13, y + 13, 6, 6, CLR_WIN_GREEN);
}

/* "Internet Explorer" icon */
static void draw_icon_internet(int32_t x, int32_t y)
{
    /* Blue 'e' */
    gfx_fill_rect(gfx, x + 6, y + 4, 20, 20, 0x000066CCu);
    gfx_fill_rect(gfx, x + 10, y + 8, 12, 4, CLR_LIGHT);
    gfx_fill_rect(gfx, x + 10, y + 16, 16, 4, CLR_DESKTOP);
    /* Gold orbital swoosh */
    gfx_line(gfx, x + 2, y + 20, x + 28, y + 6, CLR_WIN_YELLOW);
    gfx_line(gfx, x + 2, y + 21, x + 28, y + 7, CLR_WIN_YELLOW);
}

/* "Readme" text document icon */
static void draw_icon_readme(int32_t x, int32_t y)
{
    /* White sheet */
    gfx_fill_rect(gfx, x + 6, y + 2, 20, 26, CLR_LIGHT);
    gfx_rect(gfx, x + 6, y + 2, 20, 26, CLR_BLACK);
    /* Folded corner */
    gfx_fill_rect(gfx, x + 20, y + 2, 6, 6, CLR_FACE);
    gfx_line(gfx, x + 20, y + 2, x + 26, y + 8, CLR_BLACK);
    /* Text lines */
    gfx_hline(gfx, x + 9,  y + 10, 11, 0x00000080u);
    gfx_hline(gfx, x + 9,  y + 14, 14, 0x00808080u);
    gfx_hline(gfx, x + 9,  y + 18, 14, 0x00808080u);
    gfx_hline(gfx, x + 9,  y + 22, 10, 0x00808080u);
}

static void draw_desktop_icons(void)
{
    int32_t start_x = 20;
    int32_t start_y = 20;
    int32_t spacing_y = 68;

    /* 1. My Computer */
    draw_icon_my_computer(start_x + 12, start_y);
    draw_string(start_x, start_y + 32, "My Computer", CLR_TEXT_WHITE, 0, false);

    /* 2. Network Neighborhood */
    draw_icon_network(start_x + 12, start_y + spacing_y);
    draw_string(start_x + 12, start_y + spacing_y + 32, "Network", CLR_TEXT_WHITE, 0, false);

    /* 3. Recycle Bin */
    draw_icon_recycle_bin(start_x + 12, start_y + spacing_y * 2);
    draw_string(start_x + 4, start_y + spacing_y * 2 + 32, "Recycle Bin", CLR_TEXT_WHITE, 0, false);

    /* 4. Internet Explorer */
    draw_icon_internet(start_x + 12, start_y + spacing_y * 3);
    draw_string(start_x + 8, start_y + spacing_y * 3 + 32, "Internet", CLR_TEXT_WHITE, 0, false);

    /* 5. Readme.txt */
    draw_icon_readme(start_x + 12, start_y + spacing_y * 4);
    draw_string(start_x + 4, start_y + spacing_y * 4 + 32, "Readme.txt", CLR_TEXT_WHITE, 0, false);
}

/* ── Welcome Window ─────────────────────────────────────────── */
static void draw_welcome_window(int32_t wx, int32_t wy, uint32_t ww, uint32_t wh)
{
    /* Window frame */
    draw_raised_box(wx, wy, ww, wh, true);

    /* Title bar */
    int32_t tx = wx + 3;
    int32_t ty = wy + 3;
    uint32_t tw = ww - 6;
    uint32_t th = 18;
    draw_gradient_titlebar(tx, ty, tw, th);

    /* Title bar icon & text */
    draw_windows_flag(tx + 3, ty + 3);
    draw_string(tx + 22, ty + 1, "Welcome to OS01", CLR_TEXT_WHITE, 0, false);

    /* Title bar control buttons: [_] [口] [X] */
    int32_t btn_y = ty + 2;
    int32_t btn_w = 16;
    int32_t btn_h = 14;

    /* Close button [X] */
    int32_t cx = tx + (int32_t)tw - 18;
    draw_raised_box(cx, btn_y, btn_w, btn_h, true);
    draw_string(cx + 4, btn_y - 1, "x", CLR_TEXT, 0, false);

    /* Maximize button [口] */
    int32_t mx = cx - 18;
    draw_raised_box(mx, btn_y, btn_w, btn_h, true);
    gfx_rect(gfx, mx + 3, btn_y + 3, 9, 8, CLR_BLACK);
    gfx_hline(gfx, mx + 3, btn_y + 4, 9, CLR_BLACK);

    /* Minimize button [_] */
    int32_t lx = mx - 18;
    draw_raised_box(lx, btn_y, btn_w, btn_h, true);
    gfx_hline(gfx, lx + 4, btn_y + 9, 7, CLR_BLACK);
    gfx_hline(gfx, lx + 4, btn_y + 10, 7, CLR_BLACK);

    /* Menu bar: File  Edit  View  Help */
    int32_t my = wy + 23;
    gfx_fill_rect(gfx, wx + 3, my, ww - 6, 18, CLR_FACE);
    draw_string(wx + 8,  my + 1, "File", CLR_TEXT, 0, false);
    draw_string(wx + 52, my + 1, "Edit", CLR_TEXT, 0, false);
    draw_string(wx + 96, my + 1, "View", CLR_TEXT, 0, false);
    draw_string(wx + 140, my + 1, "Help", CLR_TEXT, 0, false);

    /* Client area (sunken white box) */
    int32_t cw_x = wx + 6;
    int32_t cw_y = wy + 43;
    uint32_t cw_w = ww - 12;
    uint32_t cw_h = wh - 72;
    draw_sunken_box(cw_x, cw_y, cw_w, cw_h, CLR_WINDOW_BG, true);

    /* Client content */
    int32_t px = cw_x + 16;
    int32_t py = cw_y + 12;

    /* Header banner */
    draw_windows_flag(px, py + 2);
    draw_string(px + 20, py, "Microsoft Windows 98", CLR_TITLE_ACTIVE_L, 0, false);
    draw_string(px + 20, py + 16, "for OS01 Multicore Operating System", CLR_DARK_SHADOW, 0, false);

    /* Horizontal divider */
    gfx_hline(gfx, px, py + 36, cw_w - 32, CLR_DARK_SHADOW);
    gfx_hline(gfx, px, py + 37, cw_w - 32, CLR_LIGHT);

    /* System Features list */
    int32_t ly = py + 46;
    draw_string(px, ly,      "> OS Kernel: x86_64 Higher-Half SMP (smp=2)", CLR_TEXT, 0, false);
    draw_string(px, ly + 18, "> Graphics : UEFI GOP -> /dev/gfx0 2D Engine", CLR_TEXT, 0, false);
    draw_string(px, ly + 36, "> Memory   : 4KB / 2MB Huge Paging + EEVDF", CLR_TEXT, 0, false);
    draw_string(px, ly + 54, "> Userland : BusyBox 1.36.1 & PTY Terminal", CLR_TEXT, 0, false);

    /* Hint box */
    draw_sunken_box(px, ly + 78, cw_w - 32, 28, 0x00F8F8F8u, true);
    draw_string(px + 6, ly + 84, "Press 'S' for Start Menu, 'Q' or ESC to exit", 0x00800000u, 0, false);

    /* Bottom window status / buttons */
    /* Checkbox: [X] Show on startup */
    int32_t bty = wy + (int32_t)wh - 25;
    draw_sunken_box(wx + 10, bty + 2, 12, 12, CLR_LIGHT, true);
    draw_string(wx + 12, bty - 1, "x", CLR_TEXT, 0, false);
    draw_string(wx + 26, bty, "Show this screen at startup", CLR_TEXT, 0, false);

    /* [ Close ] button */
    int32_t cl_btn_x = wx + (int32_t)ww - 80;
    int32_t cl_btn_y = bty - 2;
    draw_raised_box(cl_btn_x, cl_btn_y, 70, 22, true);
    draw_string(cl_btn_x + 14, cl_btn_y + 3, "Close", CLR_TEXT, 0, false);
}

/* ── Taskbar and Start Button ───────────────────────────────── */
static void draw_taskbar(void)
{
    int32_t ty = (int32_t)screen_h - 28;

    /* Taskbar background */
    gfx_fill_rect(gfx, 0, ty, screen_w, 28, CLR_FACE);

    /* Top 3D highlight */
    gfx_hline(gfx, 0, ty, screen_w, CLR_LIGHT);
    gfx_hline(gfx, 0, ty + 1, screen_w, CLR_LIGHT_SHADOW);

    /* Start Button: x=2, y=ty+2, w=58, h=22 */
    int32_t sb_x = 2;
    int32_t sb_y = ty + 3;
    uint32_t sb_w = 60;
    uint32_t sb_h = 22;

    if (start_menu_open) {
        draw_sunken_box(sb_x, sb_y, sb_w, sb_h, CLR_FACE, true);
        draw_windows_flag(sb_x + 6, sb_y + 5);
        draw_string(sb_x + 22, sb_y + 4, "Start", CLR_TEXT, 0, false);
    } else {
        draw_raised_box(sb_x, sb_y, sb_w, sb_h, true);
        draw_windows_flag(sb_x + 5, sb_y + 4);
        draw_string(sb_x + 21, sb_y + 3, "Start", CLR_TEXT, 0, false);
    }

    /* Vertical divider after Start button */
    gfx_vline(gfx, sb_x + (int32_t)sb_w + 4, ty + 4, 20, CLR_DARK_SHADOW);
    gfx_vline(gfx, sb_x + (int32_t)sb_w + 5, ty + 4, 20, CLR_LIGHT);

    /* Window task on taskbar */
    if (window_open) {
        int32_t tb_win_x = sb_x + (int32_t)sb_w + 10;
        int32_t tb_win_w = 160;
        draw_sunken_box(tb_win_x, sb_y, tb_win_w, sb_h, CLR_LIGHT_SHADOW, true);
        draw_windows_flag(tb_win_x + 6, sb_y + 5);
        draw_string(tb_win_x + 22, sb_y + 4, "Welcome to OS01", CLR_TEXT, 0, false);
    }

    /* System Tray (clock + speaker icon at bottom right) */
    uint32_t tray_w = 80;
    uint32_t tray_h = 22;
    int32_t tray_x = (int32_t)screen_w - (int32_t)tray_w - 4;
    int32_t tray_y = ty + 3;
    draw_sunken_box(tray_x, tray_y, tray_w, tray_h, CLR_FACE, true);

    /* Small speaker icon */
    gfx_fill_rect(gfx, tray_x + 6, tray_y + 6, 3, 8, CLR_BLACK);
    gfx_line(gfx, tray_x + 9, tray_y + 6, tray_x + 13, tray_y + 2, CLR_BLACK);
    gfx_line(gfx, tray_x + 9, tray_y + 13, tray_x + 13, tray_y + 17, CLR_BLACK);
    gfx_vline(gfx, tray_x + 13, tray_y + 2, 16, CLR_BLACK);

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
    draw_string(tray_x + 26, tray_y + 3, time_str, CLR_TEXT, 0, false);
}

/* ── Start Menu ─────────────────────────────────────────────── */
static void draw_start_menu(void)
{
    if (!start_menu_open) return;

    int32_t menu_w = 168;
    int32_t menu_h = 220;
    int32_t menu_x = 2;
    int32_t menu_y = (int32_t)screen_h - 28 - menu_h;

    /* Outer raised frame */
    draw_raised_box(menu_x, menu_y, menu_w, menu_h, true);

    /* Left banner: vertical blue gradient */
    int32_t ban_x = menu_x + 3;
    int32_t ban_y = menu_y + 3;
    uint32_t ban_w = 22;
    uint32_t ban_h = menu_h - 6;

    for (uint32_t i = 0; i < ban_h; i++) {
        uint32_t r = 0;
        uint32_t g = (128 * i) / ban_h;
        uint32_t b = 128 + ((220 - 128) * i) / ban_h;
        uint32_t c = (r << 16) | (g << 8) | b;
        gfx_hline(gfx, ban_x, ban_y + (int32_t)i, ban_w, c);
    }

    /* Vertical text "OS01 98" in the left banner */
    const char *vtext = "OS01 98";
    int32_t vy = ban_y + (int32_t)ban_h - 18;
    for (int i = (int)strlen(vtext) - 1; i >= 0; i--) {
        char s[2] = { vtext[i], '\0' };
        draw_string(ban_x + 7, vy, s, CLR_TEXT_WHITE, 0, false);
        vy -= 14;
    }

    /* Menu items list */
    struct {
        const char *name;
        bool has_arrow;
        bool is_sep;
    } items[] = {
        { "Programs",       true,  false },
        { "Favorites",      true,  false },
        { "Documents",      true,  false },
        { "Settings",       true,  false },
        { "Find",           true,  false },
        { "Help",           false, false },
        { "Run...",         false, false },
        { NULL,             false, true  },
        { "Log Off OS01...",false, false },
        { "Shut Down...",   false, false },
    };

    int32_t ix = ban_x + (int32_t)ban_w + 6;
    int32_t iy = menu_y + 6;
    int item_count = (int)(sizeof(items) / sizeof(items[0]));

    for (int i = 0; i < item_count; i++) {
        if (items[i].is_sep) {
            gfx_hline(gfx, ix, iy + 4, menu_w - (ix - menu_x) - 6, CLR_DARK_SHADOW);
            gfx_hline(gfx, ix, iy + 5, menu_w - (ix - menu_x) - 6, CLR_LIGHT);
            iy += 8;
            continue;
        }

        /* Draw item icon (small colored square or flag) */
        if (i == 0) draw_windows_flag(ix, iy + 2);
        else if (i == 9) gfx_fill_rect(gfx, ix, iy + 2, 12, 12, CLR_WIN_RED);
        else gfx_fill_rect(gfx, ix, iy + 2, 12, 12, CLR_WIN_BLUE);

        /* Draw item text */
        draw_string(ix + 18, iy, items[i].name, CLR_TEXT, 0, false);

        /* Submenu arrow '>' */
        if (items[i].has_arrow) {
            draw_string(menu_x + menu_w - 18, iy, ">", CLR_DARK_SHADOW, 0, false);
        }

        iy += 20;
    }
}

/* ── Mouse Cursor Drawing ───────────────────────────────────── */
static void draw_mouse_cursor(int32_t mx, int32_t my)
{
    /* Classic Windows 98 12x19 arrow cursor bitmap:
     * 'X' = black border, 'W' = white fill, '.' = transparent */
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
                gfx_pixel(gfx, mx + c, my + r, CLR_BLACK);
            } else if (p == 'W') {
                gfx_pixel(gfx, mx + c, my + r, CLR_LIGHT);
            }
        }
    }
}

/* ── Full Desktop Redraw ────────────────────────────────────── */
static void render_desktop(void)
{
    /* 1. Desktop background: Windows 98 Grey */
    gfx_fill_rect(gfx, 0, 0, screen_w, screen_h, CLR_DESKTOP);

    /* 2. Desktop icons */
    draw_desktop_icons();

    /* 3. Welcome Window (centered) */
    if (window_open) {
        uint32_t ww = 440;
        uint32_t wh = 280;
        if (ww > screen_w - 40) ww = screen_w - 40;
        if (wh > screen_h - 60) wh = screen_h - 60;
        int32_t wx = (int32_t)(screen_w - ww) / 2;
        int32_t wy = (int32_t)(screen_h - wh - 28) / 2;
        draw_welcome_window(wx, wy, ww, wh);
    }

    /* 4. Taskbar & Start Button */
    draw_taskbar();

    /* 5. Start Menu (if open) */
    draw_start_menu();

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
                        /* Toggle Start Menu */
                        start_menu_open = !start_menu_open;
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
                        int32_t ty = (int32_t)screen_h - 28;
                        /* Check Start Button: x=2..62, y=ty+2..ty+24 */
                        if (mouse_x >= 2 && mouse_x <= 62 &&
                            mouse_y >= ty + 2 && mouse_y <= ty + 24) {
                            start_menu_open = !start_menu_open;
                        } else if (start_menu_open) {
                            /* Clicked outside Start menu: close it */
                            start_menu_open = false;
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
