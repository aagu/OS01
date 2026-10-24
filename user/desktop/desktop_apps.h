#ifndef _DESKTOP_APPS_H
#define _DESKTOP_APPS_H

/*
 * desktop_apps.h — 桌面内置应用（GUI-3）
 *
 * 全部是**壳内应用**：应用界面由外壳自己创建与销毁，占满整个显示区。
 * 应用不 fork 子进程、不接管显示所有权，退出应用即销毁界面并回到主屏。
 *
 * 应用状态（终端的命令行与历史、文件浏览器的当前目录）保存在
 * desktop_app_state_t 里，由**外壳**持有：退出应用后这些状态要保留，
 * 所以它们不能寄居在会被销毁的控件树里。
 */

#include <gui/gui_input.h>
#include <gui/gui_theme.h>
#include <lvgl/lvgl.h>

#include "desktop_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct desktop_shell desktop_shell_t;

/** 终端命令历史的条数上限。 */
#define DESKTOP_TERM_HISTORY_MAX 32
/** 终端单条命令的长度上限（含结尾 NUL）。 */
#define DESKTOP_TERM_LINE_MAX 128
/** 终端输出区的字节上限。 */
#define DESKTOP_TERM_OUTPUT_MAX 4096
/** 文件浏览器当前目录路径的长度上限（含结尾 NUL）。 */
#define DESKTOP_FILES_PATH_MAX 256
/** 文件浏览器单页展示的条目数上限。 */
#define DESKTOP_FILES_MAX_ENTRIES 96
/** 文件浏览器单个条目名的长度上限（含结尾 NUL）。 */
#define DESKTOP_FILES_NAME_MAX 64

/**
 * @brief 文件浏览器的一个条目
 */
typedef struct {
    char name[DESKTOP_FILES_NAME_MAX];
    bool is_dir;
} desktop_files_entry_t;

/**
 * @brief 跨应用存活的应用状态
 */
typedef struct {
    /* 终端 */
    char term_input[DESKTOP_TERM_LINE_MAX];
    char term_output[DESKTOP_TERM_OUTPUT_MAX];
    char term_history[DESKTOP_TERM_HISTORY_MAX][DESKTOP_TERM_LINE_MAX];
    int  term_history_count;
    int  term_history_cursor;   /* -1 表示不在历史里 */
    bool term_history_browsing;

    /* 文件浏览器 */
    char files_cwd[DESKTOP_FILES_PATH_MAX];
    desktop_files_entry_t files_entries[DESKTOP_FILES_MAX_ENTRIES];
    uint32_t files_entry_count;

    /* 界面句柄：随表面创建与销毁，状态本身跨应用存活。 */
    lv_obj_t *files_list;
    lv_obj_t *term_output_label;
    lv_obj_t *term_input_label;
} desktop_app_state_t;

/**
 * @brief 创建应用状态
 */
desktop_app_state_t *desktop_app_state_create(void);

/**
 * @brief 释放应用状态
 */
void desktop_app_state_destroy(desktop_app_state_t *state);

/**
 * @brief 构造某个应用的全屏表面
 *
 * 表面结构：顶部是应用自己的顶栏（返回 + 标题），下面是应用内容。
 * 顶栏上的「返回」是**应用界面的一部分**，不是窗口装饰 —— GUI-3 不
 * 实现窗口系统，因此没有最小化 / 最大化 / 关闭按钮，也没有窗口边框。
 *
 * @param disp  LVGL display
 * @param id    应用标识
 * @param shell 外壳（用于取应用状态、投递返回意图）
 * @return 表面根对象；失败返回 NULL
 */
lv_obj_t *desktop_app_build(lv_display_t *disp, desktop_app_id_t id,
                            desktop_shell_t *shell);

/**
 * @brief 文件浏览器：进入一个子目录
 *
 * 只读：只进入目录，不提供写入 / 删除 / 新建。
 */
void desktop_files_enter(desktop_shell_t *shell, const char *name);

/**
 * @brief 文件浏览器：回到父目录
 * @return true 表示已经回到父目录；false 表示已在根目录
 */
bool desktop_files_go_up(desktop_shell_t *shell);

/**
 * @brief 刷新当前应用的界面（主循环调用）
 *
 * 终端的输入行与输出区由按键回调改状态、由这里改界面：LVGL 回调期间
 * 不做界面重建。
 */
void desktop_app_refresh(desktop_shell_t *shell);

/**
 * @brief 应用内部请求返回主屏
 */
void desktop_app_request_back(desktop_shell_t *shell);

/**
 * @brief 交给应用处理原生按键
 * @return true 表示已消费（不再进入 LVGL 控件路径）
 */
bool desktop_app_handle_key(desktop_shell_t *shell, const gui_key_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif /* _DESKTOP_APPS_H */
