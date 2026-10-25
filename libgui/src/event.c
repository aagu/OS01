/*
 * event.c — 原生事件桥接（GUI-2b Task 6）
 *
 * 本文件是"后端原生事件"与"公共事件"之间的接缝。原生控件被真正点中时，
 * 桥接把**一次** ACTIVATE 交给业务回调。
 *
 * 桥接的几条硬规矩：
 *
 *   1. **只有按钮**：ACTIVATE 只属于按钮。标签与装饰容器即使装了桥接也
 *      不投递——它们不拦截命中，被点中的永远是按钮自己。
 *   2. **一次**：投递由按钮自己那一次原生回调负责。原生事件会向父级冒泡，
 *      冒泡上来的重复事件被丢弃（目标不是当前对象就不投递）。
 *   3. **不占后端 user data**：槽下标放在**事件回调自己的** user data 里，
 *      后端对象的 user data 仍然归业务自己用。
 *   4. **回调深度是计数**：进入业务回调前自增、返回后自减（必须对称）。
 *      计数而不是单个槽指针：嵌套回调不会把外层的记录覆盖掉。
 */

#include <gui/gui_event.h>
#include <gui/gui_layout.h>
#include <gui/gui_widget.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>
#include <gui_widget_private.h>

#include <errno.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

/**
 * @brief 原生点击的桥接回调
 *
 * 它挂在每个新控件上（创建时、发布前装一次），但只有按钮会真的投递。
 */
static void gui_widget_on_native_click(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    lv_obj_t *current = lv_event_get_current_target(event);

    /* 冒泡：原生事件会向父级冒泡。目标不是当前对象时，这是子对象冒泡上
     * 来的重复事件，不投递——投递责任只属于被点中的那一个控件。 */
    if (!target || target != current) {
        return;
    }

    /* 槽下标 + 1 存在事件回调的 user data 里：下标 0 也必须与"没有
     * user data"区分开。后端对象的 user data 不被占用。 */
    intptr_t index = (intptr_t)lv_event_get_user_data(event) - 1;
    if (index < 0 || index >= (intptr_t)GUI_WIDGET_REGISTRY_CAPACITY) {
        return;
    }

    lv_display_t *disp = lv_obj_get_display(target);
    gui_platform_t *platform =
        disp ? (gui_platform_t *)lv_display_get_user_data(disp) : NULL;
    if (!platform || !platform->registry) {
        return;
    }

    gui_widget_slot_t *slot = &platform->registry->slots[index];
    if (slot->state != GUI_WIDGET_SLOT_LIVE || slot->obj != target) {
        return;
    }
    /* ACTIVATE 只属于按钮。 */
    if (slot->kind != GUI_WIDGET_BUTTON) {
        return;
    }
    if (!slot->handler) {
        return;
    }

    gui_event_t ev;
    ev.kind = GUI_EVENT_ACTIVATE;
    ev.target.id = slot->id;

    /*
     * 回调深度用**计数**：进入业务回调前自增、返回后自减。它必须对称，
     * 否则守卫（create/destroy/clear/layout/嵌套 step 的 EBUSY）就会在
     * 第一次回调之后永远生效。
     */
    platform->registry->callback_depth++;
    slot->handler(&ev, slot->userdata);
    platform->registry->callback_depth--;

    platform->stat_events_delivered++;
}

/**
 * @brief 给一个新控件装上事件桥接
 *
 * 每个新对象**发布前**装一次。桥接不安装成功就当创建失败回滚：控件要么
 * 完整可用，要么根本不存在。
 *
 * @return 0 成功；-1 失败（errno = ENOMEM）
 */
int gui_widget_bridge_install(lv_obj_t *obj, uint32_t slot_index)
{
    if (!obj) {
        errno = EINVAL;
        return -1;
    }
    lv_event_dsc_t *dsc = lv_obj_add_event_cb(obj, gui_widget_on_native_click, LV_EVENT_CLICKED,
                                              (void *)(intptr_t)(slot_index + 1));
    if (!dsc) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

/* ── 公共设置器 ───────────────────────────────────────────────────── */

int gui_widget_set_event_handler(gui_platform_t *platform, gui_widget_t widget,
                                 gui_event_handler_t handler, void *userdata)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = gui_widget_slot_find(platform, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    /* ACTIVATE 只属于按钮。 */
    if (slot->kind != GUI_WIDGET_BUTTON) {
        errno = EOPNOTSUPP;
        return -1;
    }

    /* 挂上、替换或注销，都是同一个槽上的**唯一**一份业务回调。 */
    slot->handler = handler;
    slot->userdata = handler ? userdata : NULL;
    return 0;
}
