#ifndef _GUI_LVGL_PRIVATE_H
#define _GUI_LVGL_PRIVATE_H

#include <gui/gui_platform.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取与平台关联的内部 lv_display_t 句柄
 *
 * 注意：此函数仅供内部单元测试及 GUI-1/GUI-2 过渡期间的 test_lvgl 使用。
 * 公共业务应用禁止直接使用。
 *
 * @param platform 平台上下文句柄
 * @return 关联的 lv_display_t 句柄；若 platform 为 NULL 则返回 NULL 并设置 errno 为 EINVAL
 */
lv_display_t *gui_platform_get_lv_display(gui_platform_t *platform);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_LVGL_PRIVATE_H */
