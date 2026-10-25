#ifndef _GUI_EVENT_H
#define _GUI_EVENT_H

/*
 * gui_event.h — 公共事件类型（GUI-2b）
 *
 * 事件头**引用**控件头；handler 的声明只在这里，控件头不反向包含事件头，
 * 因此两个公共头之间没有循环依赖。
 *
 * 本阶段只落地事件**类型**：事件的投递行为由后续任务实现。
 */

#include <gui/gui_widget.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 公共事件种类
 */
typedef enum {
    GUI_EVENT_ACTIVATE = 0 /* 按钮被激活 */
} gui_event_kind_t;

/**
 * @brief 公共事件
 *
 * `target` 是事件的控件身份（值句柄），不是后端对象指针。
 */
typedef struct {
    gui_event_kind_t kind;
    gui_widget_t target;
} gui_event_t;

/**
 * @brief 公共事件回调
 *
 * @param event    事件（生命周期由 libgui 管理，回调内不得释放）
 * @param userdata 调用方上下文（业务 context 的地址）
 */
typedef void (*gui_event_handler_t)(const gui_event_t *event, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_EVENT_H */
