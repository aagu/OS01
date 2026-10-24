#ifndef _GUI_THEME_PRIVATE_H
#define _GUI_THEME_PRIVATE_H

/*
 * gui_theme_private.h — 令牌表到 LVGL 主题的内部接缝
 *
 * 「把 Material 令牌表落地成 LVGL 主题」这一步需要 LVGL 类型，而
 * sysroot 的已发布头文件**不得出现任何 LVGL 符号**（见
 * qemutests/gui_build_audit.py 的发布头卫生检查）。因此这一步整体留在
 * libgui 内部：gui_platform_create() 建好 display 之后自动套用主题，
 * 业务应用不需要、也不允许自己调用它。
 */

#include <gui/gui_theme.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建该 display 的 Material 主题，并给当前屏幕套用
 *
 * 主题**不以 LVGL 默认主题为父主题**：默认主题的圆角 / 间距 / 状态色与
 * Material 令牌不一致，继承会让「统一」落空。因此这里自建主题，按控件类
 * 套用令牌样式，并对 hover / pressed / focus / disabled / checked 五类
 * 状态分别给出样式。
 *
 * 屏幕必须显式套用主题：LVGL 的 lv_display_set_theme() 只在 display 有
 * 4 个 screen 时才自动重套，而 libgui 的 display 只有一个 screen，
 * 不显式套用的话壁纸不会生效。
 *
 * **生命周期**：主题的样式缓冲在 LVGL 内存池里，必须在 gui_theme_destroy()
 * 释放，且 destroy 必须发生在 lv_deinit() 之前。因此主题必须与平台同生
 * 共死，由 gui_platform_create() / gui_platform_destroy() 持有。
 *
 * @param display 目标 LVGL display；NULL 时返回 NULL
 * @return 新建的主题；分配失败时返回 NULL
 */
lv_theme_t *gui_theme_create(lv_display_t *display);

/**
 * @brief 释放主题及其样式缓冲
 *
 * 必须在 lv_deinit() 之前调用。
 *
 * @param theme 主题；NULL 时什么都不做
 */
void gui_theme_destroy(lv_theme_t *theme);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_THEME_PRIVATE_H */
