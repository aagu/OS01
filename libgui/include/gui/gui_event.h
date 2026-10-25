#ifndef _GUI_EVENT_H
#define _GUI_EVENT_H

/*
 * gui_event.h — 公共事件类型（GUI-2b）
 *
 * 事件头**引用**控件头；handler 的声明只在这里，控件头不反向包含事件头，
 * 因此两个公共头之间没有循环依赖。
 *
 * 事件由库**桥接**：原生控件被真正点中时，桥接把一次 ACTIVATE 交给业务
 * 回调。业务回调只拿到值句柄与业务上下文地址，看不到后端对象。
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

/**
 * @brief 给控件挂上（或摘掉）业务事件回调
 *
 * ACTIVATE 只属于**按钮**：给容器或标签挂回调被拒绝（EOPNOTSUPP）。
 *
 * `handler` 为 NULL 表示注销：回调与上下文一起被摘掉，之后不再投递。
 * 换一个 handler 就是**替换**：同一时刻一个控件只有一个业务回调。
 *
 * `userdata` 是调用方自己的**上下文地址**（业务 context 的指针）。它原样
 * 传给回调，库不解释它、不复制它，也不用它当身份：值句柄才是身份。
 *
 * 回调期间禁止改动控件树：`gui_widget_create` / `gui_widget_destroy` /
 * `gui_widget_clear` / `gui_widget_set_layout` 与嵌套的 `gui_platform_step`
 * 一律返回 EBUSY（嵌套 step 返回 GUI_STATUS_ERROR），并且不发生部分修改。
 *
 * @param platform 平台上下文句柄
 * @param widget   控件句柄
 * @param handler  业务回调；NULL 表示注销
 * @param userdata 业务上下文地址；可为 NULL
 * @return 0 成功；-1 失败（errno：EINVAL 身份无效，EOPNOTSUPP 不是按钮）
 */
int gui_widget_set_event_handler(gui_platform_t *platform, gui_widget_t widget,
                                 gui_event_handler_t handler, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_EVENT_H */
