#ifndef _DESKTOP_SHELL_H
#define _DESKTOP_SHELL_H

/*
 * desktop_shell.h — 桌面外壳（GUI-3 / GUI-2b Task 10）
 *
 * 外壳同时只有一个可见表面，所有表面都占满整个显示区（全屏），彼此互斥。
 * 迁移一律是**整屏替换**：旧表面的控件树被销毁，新表面从零构建。
 *
 * 语义约束（设计规范 §5 / §6）：
 *   - 没有窗口系统：不存在窗口位置 / 尺寸 / 边框 / 阴影 / Z-order，
 *     因此外壳**不绘制任何窗口装饰**（应用顶栏上的「返回」是应用界面
 *     的一部分，不是窗口标题栏）。
 *   - UI 回调只**投递意图**（desktop_shell_request），迁移由主循环调用
 *     desktop_shell_apply_pending 串行执行 —— 回调期间销毁控件树
 *     是未定义行为。
 *
 * GUI-2b：桌面**只依赖 libgui 的公共 API**（原生控件注册表 + 唯一焦点
 * 引擎）。本文件不再包含任何后端（LVGL）头：控件身份是 gui_widget_t，
 * 焦点由 libgui 的焦点引擎持有，桌面不再维护第二套焦点序列。
 */

#include <gui/gui_event.h>
#include <gui/gui_focus.h>
#include <gui/gui_input.h>
#include <gui/gui_layout.h>
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <gui/gui_widget.h>

#include <stdint.h>
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
 *
 * libgui 的焦点登记容量是 128（原生登记与诊断登记共用一张表）。桌面把
 * 整个容量当作自己的上限：文件应用的 96 行加返回按钮共 97 项必须全部
 * 可达。
 */
#define DESKTOP_FOCUS_MAX 128

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
 * 而在回调期间销毁控件树是未定义行为。
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
 * 平台与输入都必须有效且**关联**（输入属于同一个平台）：焦点引擎是
 * 输入对象的一部分，输入与平台不匹配时桌面无法登记任何可聚焦控件。
 *
 * 外壳在创建时把焦点引擎的初始策略设为 MANUAL：主屏建好之后**没有**
 * 焦点，第一次按 Tab 才落到第一个可聚焦项。
 *
 * @param platform 平台句柄（外壳只借用，不销毁）
 * @param input    输入句柄（外壳只借用，不销毁）
 * @return 外壳；参数无效或不匹配时返回 NULL
 */
desktop_shell_t *desktop_shell_create(gui_platform_t *platform, gui_input_t *input);

/**
 * @brief 销毁外壳（销毁当前表面）
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
 * @brief 取得当前表面的根控件
 *
 * 应用表面恒为全屏：根控件覆盖整个显示区。
 */
gui_widget_t desktop_shell_surface_root(const desktop_shell_t *shell);

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
 *
 * 导航是分期的：先读快照，再做不可逆的提交。提交阶段失败（控件创建或
 * 焦点登记失败）时返回 -1：旧树已经被删除，桌面**不**假装回滚，主入口
 * 应当清理并退出。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int desktop_shell_apply_pending(desktop_shell_t *shell);

/**
 * @brief 投递一次文件导航意图（只复制意图）
 *
 * 意图按**值**复制：不借用调用方的路径缓冲，也不延长任何控件或上下文的
 * 寿命。同一轮里第一个导航意图占槽，后到的请求被忽略（不覆盖）。
 *
 * 既有或后来的表面切换（ESC / 返回）**优先**于导航意图，并清掉导航槽。
 * 真正执行前，外层还要验证「仍是同一个文件表面、且版本未变」。
 *
 * @param shell         外壳
 * @param absolute_path 目标目录的绝对路径
 * @param version       发起方看到的目录版本
 * @return 0 接受（或忽略后到的请求）；-1 校验失败（errno = EINVAL）
 */
int desktop_shell_request_file_nav(desktop_shell_t *shell,
                                   const char *absolute_path, uint64_t version);

/**
 * @brief 当前的目录版本
 *
 * 每提交一次新的目录列表就单调增加一。导航请求必须携带发起方看到的
 * 版本，外层据此拒绝**陈旧**的行。
 */
uint64_t desktop_shell_dir_version(const desktop_shell_t *shell);

/**
 * @brief 当前的目录路径（文件浏览器的 cwd）
 *
 * @param shell 外壳
 * @param buf   输出缓冲
 * @param len   缓冲长度
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int desktop_shell_files_cwd(const desktop_shell_t *shell, char *buf, size_t len);

/**
 * @brief 刷新时钟（主循环调用）
 */
void desktop_shell_tick(desktop_shell_t *shell);

/**
 * @brief 处理原生按键
 * @return true 表示已消费（不再进入控件路径）
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
 * @brief 取得平台句柄（外壳只借用）
 */
gui_platform_t *desktop_shell_platform(const desktop_shell_t *shell);

/**
 * @brief 取得输入句柄（外壳只借用）
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
 * @return 填充好的容器；失败返回 GUI_WIDGET_NONE
 */
gui_widget_t desktop_shell_build_settings_body(desktop_shell_t *shell,
                                                gui_widget_t parent);

/**
 * @brief 取得跨应用存活的应用状态
 *
 * 终端的命令行与历史、文件浏览器的当前目录都放在这里：退出应用后这些
 * 状态要保留，因此它们不能寄居在会被销毁的控件树里。
 */
desktop_app_state_t *desktop_shell_app_state(desktop_shell_t *shell);

/**
 * @brief 取得当前设置（供设置表面与宿主测试读取）
 */
void desktop_shell_get_settings(const desktop_shell_t *shell,
                                desktop_wallpaper_t *wallpaper,
                                desktop_density_t *density,
                                bool *clock_24h);

/**
 * @brief 诊断模式：只读业务状态快照（供 /bin/desktop probe 发布）
 */
typedef struct {
    char surface[DESKTOP_APP_ID_MAX];
    char app_id[DESKTOP_APP_ID_MAX];
    char cwd[DESKTOP_FILES_PATH_MAX];
    char subtitle[DESKTOP_FILES_PATH_MAX];
    char focus_tag[DESKTOP_FILES_PATH_MAX];
    uint32_t item_count;
} desktop_probe_state_t;

/**
 * @brief 取得当前只读业务状态快照（外层 step 结束时调用）
 *
 * @param shell 外壳
 * @param out   输出结构体指针
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int desktop_shell_probe_get_state(desktop_shell_t *shell, desktop_probe_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _DESKTOP_SHELL_H */
