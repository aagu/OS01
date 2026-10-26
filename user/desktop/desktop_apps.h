#ifndef _DESKTOP_APPS_H
#define _DESKTOP_APPS_H

/*
 * desktop_apps.h — 桌面内置应用（GUI-3 / GUI-2b Task 10）
 *
 * 全部是**壳内应用**：应用界面由外壳自己创建与销毁，占满整个显示区。
 * 应用不 fork 子进程、不接管显示所有权，退出应用即销毁界面并回到主屏。
 *
 * 应用状态（终端的命令行与历史、文件浏览器的当前目录）保存在
 * desktop_app_state_t 里，由**外壳**持有：退出应用后这些状态要保留，
 * 所以它们不能寄居在会被销毁的控件树里。
 *
 * GUI-2b：应用界面同样只用 libgui 的公共 API —— 控件身份是 gui_widget_t，
 * 不出现任何后端类型。
 */

#include <stdint.h>

#include <gui/gui_input.h>
#include <gui/gui_layout.h>
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <gui/gui_widget.h>

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
 * @brief 应用顶栏「返回」按钮的业务上下文
 *
 * 返回按钮只借外壳：它不关心当前应用是谁，返回的语义由外壳与文件应用
 * 共同决定（文件应用先回父目录，父目录是根才回主屏）。
 */
typedef struct {
    desktop_shell_t *shell;
} desktop_back_ctx_t;

/**
 * @brief 文件列表的**快照**：目标目录的条目，纯 C 的暂存
 *
 * 导航是**分期**的：先把目标目录读进快照（不碰控件、上下文与焦点），
 * 打不开就完整保留旧视图；只有快照到手之后才进入不可逆的提交阶段。
 */
typedef struct {
    char path[DESKTOP_FILES_PATH_MAX];                 /* 目标目录的绝对路径 */
    uint32_t count;                                    /* 条目数 */
    desktop_files_entry_t entries[DESKTOP_FILES_MAX_ENTRIES];
} desktop_files_snapshot_t;

/**
 * @brief 文件列表一行的业务上下文
 *
 * 行的回调只拿业务数据：外壳、**创建时**的目录版本与目标路径副本。
 *
 * 上下文数组由应用状态持有（外壳的生命周期），因此地址稳定；行的控件被
 * 销毁之后，对应的槽位才可以被下一轮的行复用。
 */
typedef struct {
    desktop_shell_t *shell;                  /* 外壳 */
    uint64_t version;                        /* 创建时的目录版本 */
    char path[DESKTOP_FILES_PATH_MAX];       /* 目标路径副本 */
    bool is_dir;                            /* 只有目录可导航 */
} desktop_files_ctx_t;

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
    /* 最近一次提交到控件的文本副本：界面刷新靠它去重，不再回读控件。 */
    char term_output_shown[DESKTOP_TERM_OUTPUT_MAX];
    char term_input_shown[DESKTOP_TERM_LINE_MAX + 32];

    /* 文件浏览器 */
    char files_cwd[DESKTOP_FILES_PATH_MAX];
    desktop_files_entry_t files_entries[DESKTOP_FILES_MAX_ENTRIES];
    uint32_t files_entry_count;
    desktop_files_ctx_t files_ctx[DESKTOP_FILES_MAX_ENTRIES];
    /* 目录版本：每提交一次新的目录列表就单调增加一。 */
    uint64_t dir_version;

    /* 界面句柄：随表面创建与销毁，状态本身跨应用存活。 */
    gui_widget_t files_list;
    gui_widget_t term_output_label;
    gui_widget_t term_input_label;

    /* 应用顶栏返回按钮的上下文（只借外壳）。 */
    desktop_back_ctx_t back_ctx;
    /* 应用顶栏的副标题（文件应用显示当前目录）。 */
    gui_widget_t app_subtitle;
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
 * 表面根容器由**外壳**创建（外壳负责它的壁纸与几何），应用只往里填内容。
 *
 * @param platform 平台句柄（应用界面建在它的活动屏幕上）
 * @param id       应用标识
 * @param shell    外壳（用于取应用状态、投递返回意图）
 * @param root     外壳给定的表面根容器
 * @return 表面根控件；失败返回 GUI_WIDGET_NONE
 */
gui_widget_t desktop_app_build(gui_platform_t *platform, desktop_app_id_t id,
                               desktop_shell_t *shell, gui_widget_t root);

/**
 * @brief 读取一个目录的条目到**快照**（纯 C，不碰任何控件）
 *
 * 只读：只进入目录，不提供写入 / 删除 / 新建。目录优先、同目录内按名字
 * 排序，最多 {@link DESKTOP_FILES_MAX_ENTRIES} 条。
 *
 * @param path 目标目录的绝对路径
 * @param out  输出快照
 * @return 0 成功；-1 失败（errno 已设置：路径非法或打不开）
 */
int desktop_files_read_entries(const char *path, desktop_files_snapshot_t *out);

/**
 * @brief 提交一次新的文件列表（**不可逆**阶段）
 *
 * 旧列表子树先被注销 / 销毁，旧行上下文随之释放；随后按 new_version
 * 创建新行并登记焦点。旧行**不会**与新行同时在册。
 *
 * 创建或登记失败时按契约清理未完成的树并返回错误：旧树已经被删除，
 * 因此不假装回滚。
 *
 * @param shell      外壳
 * @param snapshot   目标目录的快照
 * @param new_version 新目录的版本（单调增加）
 * @return 0 成功；-1 失败
 */
int desktop_files_replace_list(desktop_shell_t *shell,
                               const desktop_files_snapshot_t *snapshot,
                               uint64_t new_version);

/**
 * @brief 刷新当前应用的界面（主循环调用）
 *
 * 终端的输入行与输出区由按键回调改状态、由这里改界面：回调期间
 * 不做界面重建。
 */
void desktop_app_refresh(desktop_shell_t *shell);

/**
 * @brief 应用内部请求返回主屏
 */
void desktop_app_request_back(desktop_shell_t *shell);

/**
 * @brief 交给应用处理原生按键
 * @return true 表示已消费（不再进入控件路径）
 */
bool desktop_app_handle_key(desktop_shell_t *shell, const gui_key_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif /* _DESKTOP_APPS_H */
