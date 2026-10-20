/*
 * input_focus.c — 私有焦点组：注册记录、Tab 导航与焦点视觉
 *
 * 焦点组是**私有**的（keypad 显式绑定它，不使用 LVGL 全局默认组）。本文件
 * 拥有焦点成员的生命周期记录与导航规则：
 *
 *   - 注册顺序 = Tab 顺序。记录本身就是"存活证明"：条目在对象收到
 *     LV_EVENT_DELETE 时被摘掉，因此记录里的指针一定是活对象。摘除只比较
 *     指针值，从不解引用已经失效的指针。
 *   - 组内成员 = 当前**可聚焦**项（自身与祖先都可见、可用）。成员顺序始终
 *     等于注册顺序：隐藏 / 禁用期间不占导航位置，恢复后回到原位。
 *   - 焦点视觉是 LV_STATE_FOCUS_KEY 下的 outline，像素断言可以直接采样。
 *
 * 导航与焦点变更只做"标记"：真正的取消 / 重新就绪协议必须在外层
 * lv_indev_read() 返回之后执行（见 input_lvgl.c）。
 */

#include <gui/gui_input.h>
#include <gui_input_private.h>

#include <errno.h>
#include <string.h>

/* ── 记录操作 ──────────────────────────────────────────────────── */

/* 在记录里查找控件：只比较指针值，从不解引用。 */
static int focus_index(gui_input_t *input, lv_obj_t *obj)
{
    unsigned i;

    for (i = 0; i < input->focus.count; i++) {
        if (input->focus.entries[i] == obj) {
            return (int)i;
        }
    }
    return -1;
}

bool gui_input_focus_alive(gui_input_t *input, lv_obj_t *obj)
{
    if (!input || !obj) {
        return false;
    }
    return focus_index(input, obj) >= 0;
}

/*
 * 自身或祖先被隐藏 / 禁用 → 不是导航候选。
 *
 * LVGL 只在对象自身上检查 DISABLED（禁用父对象不会禁用子对象），隐藏则沿
 * 祖先链判断；这里两条链都查，隐藏 / 禁用的祖先同样排除后代。
 */
static bool focus_candidate(gui_input_t *input, lv_obj_t *obj)
{
    lv_obj_t *node = obj;

    (void)input;
    while (node) {
        if (lv_obj_has_flag(node, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
        if (lv_obj_get_state(node) & LV_STATE_DISABLED) {
            return false;
        }
        node = lv_obj_get_parent(node);
    }
    return true;
}

/* ── 焦点视觉 ──────────────────────────────────────────────────── */

/* 焦点轮廓：LV_STATE_FOCUS_KEY 生效时才被绘制。 */
static void focus_visual_set(lv_obj_t *obj, bool on)
{
    if (!obj) {
        return;
    }
    if (on) {
        lv_obj_set_style_outline_width(obj, GUI_INPUT_FOCUS_OUTLINE_WIDTH,
                                       LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_color(obj, lv_color_hex(GUI_INPUT_FOCUS_OUTLINE_COLOR),
                                        LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_opa(obj, LV_OPA_COVER,
                                     LV_PART_MAIN | LV_STATE_FOCUS_KEY);
        lv_obj_add_state(obj, LV_STATE_FOCUS_KEY);
    } else {
        lv_obj_remove_state(obj, LV_STATE_FOCUS_KEY);
    }
}

/* ── LVGL 回调 ─────────────────────────────────────────────────── */

/*
 * 组焦点变化（无论来自适配层还是应用直接调用 lv_group_focus_obj）。
 * 这里只更新记录与视觉，并**标记**需要取消当前键；真正的取消协议不能在
 * 回调里执行（那会递归读 indev）。
 */
static void focus_group_cb(lv_group_t *group)
{
    gui_input_t *input = (gui_input_t *)lv_group_get_user_data(group);
    lv_obj_t *now;
    lv_obj_t *old;

    if (!input) {
        return;
    }

    now = lv_group_get_focused(group);
    old = input->focus.focus;
    if (now == old) {
        return;
    }

    /* 旧焦点可能已被删除：记录里查不到就不去碰它。 */
    if (old && gui_input_focus_alive(input, old)) {
        focus_visual_set(old, false);
    }
    input->focus.focus = now;
    if (now) {
        focus_visual_set(now, true);
    }

    /* 焦点变化 → 取消当前键与它的重复。 */
    gui_input_lvgl_mark_cancel(input);
}

/*
 * 控件被删除：从记录里摘掉（只比较指针值），并通知适配层。
 */
static void focus_obj_event_cb(lv_event_t *e)
{
    gui_input_t *input = (gui_input_t *)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target_obj(e);
    int idx;
    unsigned i;

    if (!input || !obj) {
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    idx = focus_index(input, obj);
    if (idx >= 0) {
        /* 顺序保持：后面的项整体前移一位。 */
        for (i = (unsigned)idx; i + 1u < input->focus.count; i++) {
            input->focus.entries[i] = input->focus.entries[i + 1u];
        }
        input->focus.count--;
    }
    if (input->focus.focus == obj) {
        /* 焦点指针失效：清掉，之后不再解引用它。 */
        input->focus.focus = NULL;
    }

    /* LVGL 随后会把对象移出焦点组；旧键不能继续作用于自动选中的新控件。 */
    gui_input_focus_lost(input, obj);
}

/* ── 成员同步与导航 ────────────────────────────────────────────── */

/* 组内成员是否已经等于"当前可聚焦项集合"。 */
static bool focus_members_match(gui_input_t *input)
{
    unsigned i;

    for (i = 0; i < input->focus.count; i++) {
        lv_obj_t *obj = input->focus.entries[i];
        bool want = focus_candidate(input, obj);
        bool have = (lv_obj_get_group(obj) == input->lvgl.group);

        if (want != have) {
            return false;
        }
    }
    return true;
}

/* 把焦点落到目标控件（NULL = 无焦点）。 */
static void focus_apply(gui_input_t *input, lv_obj_t *obj)
{
    lv_obj_t *old = input->focus.focus;

    if (old == obj) {
        return;
    }

    if (old && gui_input_focus_alive(input, old)) {
        focus_visual_set(old, false);
    }
    input->focus.focus = NULL;

    if (obj) {
        lv_group_focus_obj(obj);
        if (lv_group_get_focused(input->lvgl.group) != obj) {
            /* 没有生效（例如对象不在组内）：保持无焦点。 */
            return;
        }
        input->focus.focus = obj;
        focus_visual_set(obj, true);
    }
}

/*
 * 让组内成员 = 当前可聚焦项（顺序 = 注册顺序），并保持焦点有效。
 * 成员集合变化时整体重建：被移出组的对象回到原位只需要一次重建。
 */
static void focus_sync(gui_input_t *input)
{
    lv_group_t *group = input->lvgl.group;
    lv_obj_t *old;
    lv_obj_t *target = NULL;
    bool old_ok;
    unsigned i;

    if (focus_members_match(input)) {
        return;
    }

    old = input->focus.focus;
    old_ok = old && gui_input_focus_alive(input, old) && focus_candidate(input, old);

    if (old && gui_input_focus_alive(input, old)) {
        focus_visual_set(old, false);
    }
    input->focus.focus = NULL;

    /* 冻结焦点：避免 lv_group_add_obj() 自动把焦点抢到第一个成员上。 */
    lv_group_focus_freeze(group, true);
    lv_group_remove_all_objs(group);
    for (i = 0; i < input->focus.count; i++) {
        if (focus_candidate(input, input->focus.entries[i])) {
            lv_group_add_obj(group, input->focus.entries[i]);
        }
    }
    lv_group_focus_freeze(group, false);

    if (old_ok) {
        target = old;
    } else {
        for (i = 0; i < input->focus.count; i++) {
            if (focus_candidate(input, input->focus.entries[i])) {
                target = input->focus.entries[i];
                break;
            }
        }
    }
    if (target) {
        /*
         * 先把记录里的焦点清空再交给 LVGL：组回调只在"焦点真的变了"时
         * 标记取消（focus_apply 也是这么做的）。否则隐藏 / 禁用引起的焦点
         * 转移会被当成"没变"，旧键不会被取消（spec §7.2）。
         */
        input->focus.focus = NULL;
        lv_group_focus_obj(target);
        if (lv_group_get_focused(group) != target) {
            /* 没有生效（例如对象不在组内）：保持无焦点。 */
            input->focus.focus = NULL;
        }
    }
}

/* 按注册顺序找出 from 之后的下一个候选（循环）。 */
static lv_obj_t *focus_next_candidate(gui_input_t *input, lv_obj_t *from, bool forward)
{
    unsigned n = input->focus.count;
    unsigned i;
    int cur = -1;

    for (i = 0; i < n; i++) {
        if (input->focus.entries[i] == from) {
            cur = (int)i;
            break;
        }
    }

    for (i = 1; i <= n; i++) {
        unsigned idx;

        if (cur < 0) {
            idx = forward ? (i - 1u) : (n - i);
        } else if (forward) {
            idx = ((unsigned)cur + i) % n;
        } else {
            idx = ((unsigned)cur + n - (i % n)) % n;
        }

        lv_obj_t *cand = input->focus.entries[idx];
        if (gui_input_focus_alive(input, cand) && focus_candidate(input, cand)) {
            return cand;
        }
    }
    return NULL;
}

/*
 * 建立本 display 的私有焦点组。
 *
 * 组是私有的：keypad 显式绑定它，不使用 LVGL 全局默认组。
 */
int gui_input_focus_create(gui_input_t *input)
{
    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (input->lvgl.group) {
        return 0;   /* 已经建立 */
    }

    input->lvgl.group = lv_group_create();
    if (!input->lvgl.group) {
        errno = ENOMEM;
        return -1;
    }
    lv_group_set_user_data(input->lvgl.group, input);
    lv_group_set_focus_cb(input->lvgl.group, focus_group_cb);
    return 0;
}

/* 删除焦点组并清空记录。 */
void gui_input_focus_destroy(gui_input_t *input)
{
    unsigned i;

    if (!input) {
        return;
    }

    /*
     * 先摘掉每个控件上的 LV_EVENT_DELETE 回调。
     *
     * 控件比 input 活得久：gui_input_destroy() 释放 input 之后，平台/屏幕
     * 才被销毁，控件随之收到 LV_EVENT_DELETE。回调里会解引用 input
     * （记录与适配层状态），因此必须在这里注销，否则就是释放后使用。
     */
    for (i = 0; i < input->focus.count; i++) {
        lv_obj_t *obj = input->focus.entries[i];

        if (obj) {
            lv_obj_remove_event_cb(obj, focus_obj_event_cb);
        }
    }

    if (input->lvgl.group) {
        lv_group_delete(input->lvgl.group);
        input->lvgl.group = NULL;
    }
    memset(&input->focus, 0, sizeof(input->focus));
}

int gui_input_focus_add(gui_input_t *input, lv_obj_t *obj)
{
    lv_display_t *disp;

    if (!input || !obj || !input->lvgl.group || !input->lvgl.keypad) {
        errno = EINVAL;
        return -1;
    }

    /* 属于别的 display 的控件不是本组的成员。 */
    disp = lv_indev_get_display(input->lvgl.keypad);
    if (disp && lv_obj_get_display(obj) != disp) {
        errno = EINVAL;
        return -1;
    }

    /* 重复登记：返回成功但不添加第二份。 */
    if (focus_index(input, obj) >= 0) {
        return 0;
    }

    if (input->focus.count >= GUI_INPUT_FOCUS_CAPACITY) {
        errno = ENOSPC;
        return -1;
    }

    input->focus.entries[input->focus.count++] = obj;
    lv_obj_add_event_cb(obj, focus_obj_event_cb, LV_EVENT_DELETE, input);

    focus_sync(input);

    /* 第一个登记的可用控件建立初始焦点。 */
    if (!input->focus.focus) {
        lv_obj_t *target = focus_next_candidate(input, NULL, true);
        if (target) {
            focus_apply(input, target);
        }
    }
    return 0;
}

void gui_input_focus_remove(gui_input_t *input, lv_obj_t *obj)
{
    int idx;
    unsigned i;

    if (!input || !obj) {
        return;
    }

    idx = focus_index(input, obj);
    if (idx < 0) {
        /* 未登记：不碰它（它可能属于别的焦点组）。 */
        return;
    }

    for (i = (unsigned)idx; i + 1u < input->focus.count; i++) {
        input->focus.entries[i] = input->focus.entries[i + 1u];
    }
    input->focus.count--;

    lv_obj_remove_event_cb(obj, focus_obj_event_cb);
    if (lv_obj_get_group(obj) == input->lvgl.group) {
        lv_group_remove_obj(obj);
    }
    if (input->focus.focus == obj) {
        input->focus.focus = NULL;
    }
    focus_sync(input);
}

int gui_input_focus_set(gui_input_t *input, lv_obj_t *obj)
{
    if (!input || !obj || !input->lvgl.group) {
        errno = EINVAL;
        return -1;
    }
    /* 未登记的控件不能成为焦点。 */
    if (!gui_input_focus_alive(input, obj)) {
        errno = EINVAL;
        return -1;
    }

    focus_sync(input);
    focus_apply(input, obj);
    return 0;
}

lv_obj_t *gui_input_focus_current(gui_input_t *input)
{
    if (!input) {
        return NULL;
    }
    return input->focus.focus;
}

bool gui_input_focus_navigate(gui_input_t *input, bool forward)
{
    lv_obj_t *target;

    if (!input || !input->lvgl.group) {
        return false;
    }

    target = focus_next_candidate(input, input->focus.focus, forward);
    focus_sync(input);

    if (!target || !gui_input_focus_alive(input, target) ||
        !focus_candidate(input, target)) {
        /* 没有可用项：无焦点。 */
        focus_apply(input, NULL);
        return false;
    }
    focus_apply(input, target);
    return true;
}

bool gui_input_focus_is_text_input(gui_input_t *input)
{
    lv_obj_t *focus;

    if (!input) {
        return false;
    }
    focus = input->focus.focus;
    if (!focus || !gui_input_focus_alive(input, focus)) {
        return false;
    }
#if LV_USE_TEXTAREA
    return lv_obj_has_class(focus, &lv_textarea_class);
#else
    return false;
#endif
}

/*
 * 每步一次的焦点检查：成员集合与焦点都必须仍然有效。
 * 返回 true 表示焦点发生了变化（适配层据此取消当前键）。
 */
bool gui_input_focus_refresh(gui_input_t *input)
{
    lv_obj_t *before;
    lv_obj_t *target;

    if (!input || !input->lvgl.group) {
        return false;
    }

    before = input->focus.focus;
    focus_sync(input);

    if (input->focus.focus && gui_input_focus_alive(input, input->focus.focus) &&
        focus_candidate(input, input->focus.focus)) {
        return before != input->focus.focus;
    }

    target = focus_next_candidate(input, input->focus.focus, true);
    focus_apply(input, target);
    return before != input->focus.focus;
}
