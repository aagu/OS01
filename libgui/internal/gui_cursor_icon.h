#ifndef _GUI_CURSOR_ICON_H
#define _GUI_CURSOR_ICON_H

/*
 * gui_cursor_icon.h — 桌面鼠标光标的图像资源（内部头，不进 sysroot）。
 *
 * 资源是一张 16x24 的 ARGB8888 图像，由 tools/gen_cursor_icon.py 从
 * libgui/assets/cursor-arrow.png 离线生成到 libgui/src/cursor_icon.c。
 * 热点在左上角 (0,0)。
 *
 * 它带 lv_image_dsc_t，因此要包含 LVGL 头 —— 内部头允许，发布头不允许。
 */

#include <lvgl/lvgl.h>

extern const lv_image_dsc_t gui_cursor_icon;

#endif
