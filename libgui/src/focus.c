/*
 * focus.c — 公共焦点 API（GUI-2b Task 7）
 *
 * 本文件是"业务能做什么"与"唯一焦点引擎"之间的接缝。它只做四件事：
 *
 *   1. **参数校验**：无效枚举、无效身份先被拒绝，失败不留半状态；
 *   2. **回调守卫**：业务回调深度 > 0 时禁止焦点 API 变更（只读查询允许）；
 *   3. **模式与归属**：登记模式锁定、身份解析、display / platform 归属；
 *   4. **一次提交**：真正改变选择的动作全部交给唯一焦点引擎。
 *
 * 它不持有任何选择状态：选择只有一份，就在焦点引擎里。
 */

#include <gui/gui_focus.h>
#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include <gui/gui_widget.h>
#include <gui_input_private.h>
#include <gui_platform_private.h>
#include <gui_widget_private.h>

#include <errno.h>

#include <lvgl/lvgl.h>

/*
 * 业务回调守卫。
 *
 * 回调深度是**业务**回调深度（不是删除事务深度）：回调期间树结构必须稳定，
 * 焦点选择也不许被偷换。只读查询不在守卫范围内。
 */
static bool focus_in_callback(gui_input_t *input)
{
    gui_platform_t *platform;

    if (!input) {
        return false;
    }
    platform = input->platform;
    if (!platform || !platform->registry) {
        return false;
    }
    return platform->registry->callback_depth > 0u;
}

/*
 * 解析原生身份。
 *
 * 身份必须属于 input 自己的平台：外平台身份、失效身份一律 EINVAL。
 */
static int focus_resolve(gui_input_t *input, gui_widget_t widget, gui_widget_slot_t **out_slot)
{
    gui_widget_slot_t *slot;

    if (!input || !input->platform || !input->platform->registry) {
        errno = EINVAL;
        return -1;
    }
    slot = gui_widget_slot_find(input->platform, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    if (out_slot) {
        *out_slot = slot;
    }
    return 0;
}

/*
 * 归属检查：对象必须在本平台自己的 display 上。
 *
 * 解析（身份在不在本平台的注册表里）与归属（对象在不在本平台的 display
 * 上）是两层独立的拒绝：任何一层不通过都是 EINVAL。
 */
static int focus_check_ownership(gui_input_t *input, gui_widget_slot_t *slot)
{
    if (!slot->obj) {
        errno = EINVAL;
        return -1;
    }
    if (input->platform->disp && lv_obj_get_display(slot->obj) != input->platform->disp) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int gui_focus_register(gui_input_t *input, gui_widget_t widget, gui_focus_policy_t policy)
{
    gui_widget_slot_t *slot = NULL;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned int)policy > (unsigned int)GUI_FOCUS_STRONG) {
        errno = EINVAL;
        return -1;
    }
    if (focus_in_callback(input)) {
        errno = EBUSY;
        return -1;
    }
    /* 混用两种登记被拒绝：诊断(raw)模式下原生登记 EBUSY。 */
    if (gui_input_focus_mode(input) == (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        errno = EBUSY;
        return -1;
    }
    if (widget.id == 0) {
        errno = EINVAL;
        return -1;
    }
    if (focus_resolve(input, widget, &slot) != 0) {
        return -1;
    }
    /* 登记仅限按钮。 */
    if (slot->kind != GUI_WIDGET_BUTTON) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (focus_check_ownership(input, slot) != 0) {
        return -1;
    }
    return gui_input_focus_register_native(input, slot->obj, widget.id, policy);
}

int gui_focus_unregister(gui_input_t *input, gui_widget_t widget)
{
    uint8_t mode;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (focus_in_callback(input)) {
        errno = EBUSY;
        return -1;
    }
    mode = gui_input_focus_mode(input);
    if (mode == (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (mode == (uint8_t)GUI_FOCUS_MODE_UNSET) {
        errno = EINVAL;
        return -1;
    }
    if (widget.id == 0) {
        errno = EINVAL;
        return -1;
    }
    return gui_input_focus_unregister_native(input, widget.id);
}

int gui_focus_set(gui_input_t *input, gui_widget_t widget)
{
    uint8_t mode;
    gui_widget_slot_t *slot = NULL;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (focus_in_callback(input)) {
        errno = EBUSY;
        return -1;
    }
    mode = gui_input_focus_mode(input);
    if (mode == (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (mode == (uint8_t)GUI_FOCUS_MODE_UNSET) {
        /* UNSET：get 是 NONE、move 成功、set(NONE) 成功，set 其他 EINVAL。 */
        if (widget.id != 0) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }

    if (widget.id != 0) {
        if (focus_resolve(input, widget, &slot) != 0) {
            return -1;
        }
        if (slot->kind != GUI_WIDGET_BUTTON) {
            errno = EOPNOTSUPP;
            return -1;
        }
        if (focus_check_ownership(input, slot) != 0) {
            return -1;
        }
    }
    return gui_input_focus_set_native(input, widget.id);
}

int gui_focus_get(gui_input_t *input, gui_widget_t *out)
{
    /* 先校验输出指针，再清输出：失败的调用永远不留下上一次的结果。 */
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    out->id = 0;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (gui_input_focus_mode(input) == (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        /* 诊断(raw)模式：公共焦点 API 不支持，输出保持 NONE。 */
        errno = EOPNOTSUPP;
        return -1;
    }
    /* 只读查询在业务回调里允许：返回最后已提交的状态。 */
    out->id = gui_input_focus_current_native_id(input);
    return 0;
}

int gui_focus_move(gui_input_t *input, gui_focus_direction_t direction)
{
    uint8_t mode;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned int)direction > (unsigned int)GUI_FOCUS_BACKWARD) {
        errno = EINVAL;
        return -1;
    }
    if (focus_in_callback(input)) {
        errno = EBUSY;
        return -1;
    }
    mode = gui_input_focus_mode(input);
    if (mode == (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (mode == (uint8_t)GUI_FOCUS_MODE_UNSET) {
        /* 没有登记：移动成功，焦点仍然为空。 */
        return 0;
    }
    (void)gui_input_focus_navigate(input, direction == GUI_FOCUS_FORWARD);
    return 0;
}

int gui_focus_set_initial_policy(gui_input_t *input, gui_focus_initial_t initial)
{
    uint8_t mode;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned int)initial > (unsigned int)GUI_FOCUS_INITIAL_MANUAL) {
        errno = EINVAL;
        return -1;
    }
    if (focus_in_callback(input)) {
        errno = EBUSY;
        return -1;
    }
    mode = gui_input_focus_mode(input);
    if (mode == (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        errno = EOPNOTSUPP;
        return -1;
    }
    /* 初始策略只控制"自动建立首次焦点"，不移动已经存在的选择。 */
    gui_input_focus_set_initial(input, initial);
    return 0;
}
