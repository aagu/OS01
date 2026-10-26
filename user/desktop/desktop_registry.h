#ifndef _DESKTOP_REGISTRY_H
#define _DESKTOP_REGISTRY_H

/*
 * desktop_registry.h — 桌面应用注册表（GUI-3）
 *
 * 注册表是**纯数据**：应用的身份、显示名、一行说明、图标字与强调色。
 * 它不含任何 LVGL 头，也不含任何颜色字面量——强调色用**角色**表示，
 * 具体颜色由 gui_theme_colors() 在运行时给出，因此令牌表仍是唯一的
 * 颜色真源。
 *
 * 启动器是注册表的**子集**（in_launcher 为真的那些），应用抽屉是全集。
 */

#include <gui/gui_layout.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用标识
 */
typedef enum {
    DESKTOP_APP_FILES = 0,   /* 文件：只读浏览根文件系统 */
    DESKTOP_APP_TERMINAL,    /* 终端：壳内命令台 */
    DESKTOP_APP_SYSTEM,      /* 系统：内核与运行时信息 */
    DESKTOP_APP_SETTINGS,    /* 设置：时钟、壁纸与密度 */
    DESKTOP_APP_COUNT
} desktop_app_id_t;

/**
 * @brief 图标强调色角色
 *
 * 用角色而不是颜色值，是为了让注册表保持「无颜色字面量」：具体颜色
 * 由 desktop_app_accent_color() 从令牌表取。
 */
typedef enum {
    DESKTOP_ACCENT_PRIMARY = 0,
    DESKTOP_ACCENT_SECONDARY,
    DESKTOP_ACCENT_TERTIARY,
    DESKTOP_ACCENT_SUCCESS,
    DESKTOP_ACCENT_ERROR
} desktop_accent_t;

/**
 * @brief 一个应用的注册信息
 */
typedef struct {
    desktop_app_id_t id;
    const char      *name;        /* 显示名 */
    const char      *desc;        /* 一行说明 */
    const char      *glyph;       /* 图标字（首字或符号） */
    desktop_accent_t accent;      /* 图标底色角色 */
    bool             in_launcher; /* 是否出现在启动器 */
} desktop_app_info_t;

/**
 * @brief 取得注册表条目
 * @param id 应用标识
 * @return 指向静态常量的指针；越界返回 NULL
 */
const desktop_app_info_t *desktop_app_info(desktop_app_id_t id);

/**
 * @brief 注册表条目总数
 */
uint32_t desktop_app_count(void);

/**
 * @brief 取得应用的强调色（从令牌表取）
 * @param id 应用标识
 * @return 0x00RRGGBB 颜色；越界返回 0
 */
uint32_t desktop_app_accent_color(desktop_app_id_t id);

/**
 * @brief 取得应用的强调色**角色**
 *
 * GUI-2b：桌面只用公共 API 表达外观，公共 API 接受的是**色彩角色**而不是
 * 颜色值，因此注册表直接给出角色。角色与强调色一一对应，颜色仍只来自
 * 令牌表。
 *
 * @param id 应用标识
 * @return 色彩角色；越界返回 GUI_COLOR_COUNT（非法哨兵）
 */
gui_color_role_t desktop_app_accent_role(desktop_app_id_t id);

/**
 * @brief 取得应用的稳定标识串（用于状态机迁移的 app_id 参数）
 * @param id 应用标识
 * @return 静态字符串（如 "files"）；越界返回 NULL
 */
const char *desktop_app_id_name(desktop_app_id_t id);

/**
 * @brief 由标识串反查应用
 * @param name 标识串（如 "files"）
 * @param out  输出应用标识
 * @return 找到返回 true；未找到返回 false 且不修改 out
 */
bool desktop_app_id_from_name(const char *name, desktop_app_id_t *out);

/**
 * @brief 取得启动器中的应用（注册表的 in_launcher 子集）
 * @param index 序号（0 起）
 * @return 指向注册表条目的指针；越界返回 NULL
 */
const desktop_app_info_t *desktop_app_launcher_entry(uint32_t index);

/**
 * @brief 启动器中的应用数量
 */
uint32_t desktop_app_launcher_count(void);

#ifdef __cplusplus
}
#endif

#endif /* _DESKTOP_REGISTRY_H */
