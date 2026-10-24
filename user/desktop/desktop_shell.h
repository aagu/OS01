#ifndef _DESKTOP_SHELL_H
#define _DESKTOP_SHELL_H

/*
 * desktop_shell.h — 桌面外壳（GUI-3）
 *
 * 外壳同时只有一个可见表面，所有表面都占满整个显示区（全屏），彼此互斥。
 * 迁移一律是**整屏替换**：旧表面的控件树被销毁，新表面从零构建。
 *
 * 语义约束（设计规范 §5 / §6）：
 *   - 没有窗口系统：不存在窗口位置 / 尺寸 / 边框 / 阴影 / Z-order，
 *     因此外壳**不绘制任何窗口装饰**（应用顶栏上的「返回」是应用界面
 *     的一部分，不是窗口标题栏）。
 *   - UI 回调只**投递意图**（desktop_shell_request），迁移由主循环调用
 *     desktop_shell_apply_pending 串行执行 —— LVGL 回调期间销毁控件树
 *     是未定义行为。
 */

#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <lvgl/lvgl.h>

#include <time.h>

#include "desktop_apps.h"
#include "desktop_registry.h"
#include "desktop_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 应用标识串的最大长度（含结尾 NUL）。 */
#define DESKTOP_APP_ID_MAX 16

/**
 * @brief 应用顶栏的高度（像素）
 *
 * 顶栏是**应用界面的一部分**（返回 + 标题），不是窗口装饰：GUI-3 不实现
 * 窗口系统，因此没有最小化 / 最大化 / 关闭按钮，也没有窗口边框。
 */
#define DESKTOP_APP_BAR_HEIGHT 40

/**
 * @brief 单个表面的可聚焦项容量上限
 */
#define DESKTOP_FOCUS_MAX 32

/**
 * @brief 布局区域
 *
 * 供宿主像素契约测试取样：每个区域对应一个应当呈现某个令牌色的位置。
 */
typedef enum {
    DESKTOP_REGION_WALLPAPER = 0, /* 状态栏与卡片之间的壁纸空白 */
    DESKTOP_REGION_STATUS_BAR,    /* 状态栏背景 */
    DESKTOP_REGION_CARD,          /* 欢迎卡片 */
    DESKTOP_REGION_LAUNCHER,      /* 启动器卡片 */
    DESKTOP_REGION_DRAWER,        /* 抽屉面板 */
    DESKTOP_REGION_SETTINGS,      /* 设置面板 */
    DESKTOP_REGION_COUNT
} desktop_region_t;

/**
 * @brief 壁纸配色
 */
typedef enum {
    DESKTOP_WALLPAPER_SURFACE_DIM = 0, /* #D8D2E4，最深 */
    DESKTOP_WALLPAPER_SURFACE,          /* #FDFCFF，最浅 */
    DESKTOP_WALLPAPER_CONTAINER_LOW,    /* #F7F2FA */
    DESKTOP_WALLPAPER_CONTAINER_HIGH,   /* #ECE6F0 */
    DESKTOP_WALLPAPER_CONTAINER_HIGHEST /* #E6E0E9 */
} desktop_wallpaper_t;

/**
 * @brief 卡片密度
 */
typedef enum {
    DESKTOP_DENSITY_COMFORTABLE = 0,
    DESKTOP_DENSITY_COMPACT
} desktop_density_t;

/**
 * @brief 可在设置表面调整的项
 *
 * 设置项的变更同样走「投递 → 主循环执行」：改壁纸 / 密度要重建当前表面，
 * 而在 LVGL 回调期间销毁控件树是未定义行为。
 */
typedef enum {
    DESKTOP_SETTING_NONE = 0,
    DESKTOP_SETTING_CLOCK_FORMAT, /* 12 / 24 小时制 */
    DESKTOP_SETTING_WALLPAPER,   /* 壁纸配色 */
    DESKTOP_SETTING_DENSITY      /* 卡片密度 */
} desktop_setting_t;

/**
 * @brief 创建外壳并构建主屏
 *
 * 平台拥有 LVGL display；外壳通过 lv_display_get_default() 借用它
 * （libgui 的公共 API 刻意不暴露 LVGL 类型）。
 *
 * @param platform 平台句柄（外壳只借用，不销毁）
 * @return 外壳；分配失败返回 NULL
 */
desktop_shell_t *desktop_shell_create(gui_platform_t *platform);

/**
 * @brief 销毁外壳（销毁当前表面与时钟定时器）
 */
void desktop_shell_destroy(desktop_shell_t *shell);

/**
 * @brief 当前表面状态
 */
desktop_state_t desktop_shell_state(const desktop_shell_t *shell);

/**
 * @brief 当前应用标识串（仅在 APP 状态下有意义）
 */
const char *desktop_shell_current_app(const desktop_shell_t *shell);

/**
 * @brief 取得当前表面的根对象
 *
 * 应用表面恒为全屏：根对象覆盖整个显示区。
 */
lv_obj_t *desktop_shell_surface_root(const desktop_shell_t *shell);

/**
 * @brief 取得外壳创建时刻（系统应用用来显示启动时间）
 */
time_t desktop_shell_start_time(const desktop_shell_t *shell);

/**
 * @brief 取得当前应用标识（仅在 APP 状态下有意义）
 *
 * @return 应用标识；不在应用里返回 DESKTOP_APP_COUNT
 */
desktop_app_id_t desktop_shell_current_app_id(const desktop_shell_t *shell);

/**
 * @brief 投递一个迁移意图
 *
 * 意图由主循环执行；回调里不得直接调用 desktop_shell_apply_pending。
 */
void desktop_shell_request(desktop_shell_t *shell, desktop_intent_t intent,
                            const char *app_id);

/**
 * @brief 投递一个设置项变更
 *
 * 变更在 desktop_shell_apply_pending() 里执行，并立即重建当前表面。
 */
void desktop_shell_request_setting(desktop_shell_t *shell, desktop_setting_t setting);

/**
 * @brief 执行挂起的意图（主循环调用）
 *
 * 非法迁移被忽略并记诊断；合法迁移销毁旧表面并重建新表面。
 */
void desktop_shell_apply_pending(desktop_shell_t *shell);

/**
 * @brief 刷新时钟（主循环调用）
 */
void desktop_shell_tick(desktop_shell_t *shell);

/**
 * @brief 处理原生按键
 * @return true 表示已消费（不再进入 LVGL 控件路径）
 */
bool desktop_shell_handle_key(desktop_shell_t *shell, const gui_key_event_t *ev);

/**
 * @brief 取得某布局区域的取样坐标
 *
 * 几何与外壳构建共用同一套常量，因此测试取样点始终落在对应区域内。
 *
 * @return 成功返回 true；区域非法返回 false
 */
bool desktop_shell_probe_point(const desktop_shell_t *shell, desktop_region_t region,
                                uint32_t *x, uint32_t *y);

/**
 * @brief 取得某个字号层级对应的 LVGL 字体
 *
 * 与 libgui 内部的令牌→字体映射保持一致；宿主测试会比对两者一致。
 */
const lv_font_t *desktop_shell_font(gui_theme_font_t role);

/**
 * @brief 取得平台句柄（外壳只借用）
 */
gui_platform_t *desktop_shell_platform(const desktop_shell_t *shell);

/**
 * @brief 挂接输入对象（外壳只借用）
 *
 * 系统应用用它报告键鼠是否存在；输入对象的生命周期归调用方（通常是
 * 桌面主入口），外壳既不创建也不销毁它。
 */
void desktop_shell_attach_input(desktop_shell_t *shell, gui_input_t *input);

/**
 * @brief 取得输入对象（未挂接时返回 NULL）
 */
gui_input_t *desktop_shell_input(const desktop_shell_t *shell);

/**
 * @brief 在给定容器里填充设置项
 *
 * 设置**表面**与设置**应用**共用同一份实现（规范 §7.4）：两者都读同一份
 * shell 设置状态，变更也走同一条投递通道。
 *
 * @param shell 外壳
 * @param parent 承载设置的容器（应用表面用）
 * @return 填充好的容器；失败返回 NULL
 */
lv_obj_t *desktop_shell_build_settings_body(desktop_shell_t *shell, lv_obj_t *parent);

/**
 * @brief 取得跨应用存活的应用状态
 *
 * 终端的命令行与历史、文件浏览器的当前目录都放在这里：退出应用后这些
 * 状态要保留，因此它们不能寄居在会被销毁的控件树里。
 */
desktop_app_state_t *desktop_shell_app_state(desktop_shell_t *shell);

/**
 * @brief 把一个控件登记为当前表面的可聚焦项
 *
 * 桌面**自己**维护焦点序列：libgui 的焦点组是私有的，公共 API 不暴露
 * 「登记可聚焦控件」的入口（业务应用只能用公共 API），因此外壳用
 * LVGL 的 LV_STATE_FOCUSED 状态实现同一语义：Tab / Shift+Tab 在序列里
 * 前进 / 后退，Enter 激活当前项，焦点环由 Material 主题给出。
 *
 * 登记的控件随当前表面一起销毁：外壳在重建表面时先清空序列，因此不会
 * 残留已销毁对象。
 *
 * @return 登记成功返回 true；超出容量返回 false
 */
bool desktop_shell_focus_add(desktop_shell_t *shell, lv_obj_t *obj);

/**
 * @brief 把某个容器下的所有焦点项移出序列
 *
 * 容器即将被销毁时必须先调用它：序列里不能留下已销毁对象的指针。
 */
void desktop_shell_focus_remove_children(desktop_shell_t *shell, lv_obj_t *parent);

/**
 * @brief 当前表面的可聚焦项数量
 */
uint32_t desktop_shell_focus_count(const desktop_shell_t *shell);

/**
 * @brief 当前焦点所在的可聚焦项序号；没有焦点时返回 -1
 */
int32_t desktop_shell_focus_index(const desktop_shell_t *shell);

/**
 * @brief 取得当前设置（供设置表面与宿主测试读取）
 */
void desktop_shell_get_settings(const desktop_shell_t *shell,
                                desktop_wallpaper_t *wallpaper,
                                desktop_density_t *density,
                                bool *clock_24h);

#ifdef __cplusplus
}
#endif

#endif /* _DESKTOP_SHELL_H */
