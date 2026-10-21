/*
 * input_lvgl.c — LVGL 控件适配（pointer / keypad / 焦点组）
 *
 * 本文件实现 Task 6 固定的跨文件 hook，并补齐 Task 7 的键盘行为：
 *
 *   gui_input_lvgl_create   建立并绑定本 display 的 keypad / pointer
 *                           indev、read 回调与私有默认焦点组；
 *   gui_input_lvgl_destroy  取消未完成手势并删除全部对象；
 *   gui_input_lvgl_cancel   按 spec 的 wait-release 重新就绪协议取消
 *                           当前键（不合成 click）；
 *   gui_input_lvgl_deliver  把原生按键事件投递为 LVGL 键；
 *   gui_input_lvgl_pointer  把原生指针事件投递为位置 / 左键。
 *
 * keypad 使用 EVENT 模式：每个逻辑边沿显式调用 lv_indev_read()，不依赖
 * continue_reading 自动排空。pointer 保留 TIMER 模式（长按 / 拖动定时器
 * 需要它），新事件通过显式 read 及时送达。
 *
 * 焦点组是**私有**的：keypad 显式绑定它，不使用 LVGL 全局默认组。焦点成员
 * 的登记、Tab 导航与焦点视觉由 input_focus.c 拥有。
 *
 * 取消协议（spec §6.2.2）严格按顺序执行：
 *   停重复 → 清未投递事件 → wait_release → 显式 RELEASED read →
 *   清旧控件视觉 → 之后才投递新键的 PRESSED。
 * 协议若发生在 LVGL 回调内部，只标记 pending，真实的 read 放到外层
 * lv_indev_read() 返回之后（回调里递归读 indev 是不允许的）。
 */

#include <gui/gui_input.h>
#include <gui_input_private.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>

/*
 * lv_draw_rect() 属于 LVGL 的**公开**绘制头（不是 _private.h）：光标必须
 * 用程序绘制，不能引入图片 / 字体资源。
 */
#include <lvgl/src/draw/lv_draw_rect.h>

#include <errno.h>
#include <string.h>

/* ── Set 1 物理键身份 ─────────────────────────────────────────── */

#define PS2_KEY_ESC        0x001u
#define PS2_KEY_BACKSPACE  0x00Eu
#define PS2_KEY_TAB        0x00Fu
#define PS2_KEY_ENTER      0x01Cu
#define PS2_KEY_LSHIFT     0x02Au
#define PS2_KEY_RSHIFT     0x036u
#define PS2_KEY_LCTRL      0x01Du
#define PS2_KEY_RCTRL      0x11Du
#define PS2_KEY_LALT       0x038u
#define PS2_KEY_RALT       0x138u
#define PS2_KEY_CAPS       0x03Au
#define PS2_KEY_DELETE     0x053u
#define PS2_KEY_UP         0x148u   /* E0 48 */
#define PS2_KEY_DOWN       0x150u   /* E0 50 */
#define PS2_KEY_RIGHT      0x14Du   /* E0 4D */
#define PS2_KEY_LEFT       0x14Bu   /* E0 4B */
#define PS2_KEY_HOME       0x147u   /* E0 47 */
#define PS2_KEY_END        0x14Fu   /* E0 4F */
#define PS2_KEY_KP_ENTER   0x11Cu   /* E0 1C */

/*
 * 物理键 → LVGL 特殊键。返回 0 表示该键没有 LVGL 键义（修饰键、
 * 功能键、keypad 数字区等只报告物理身份）。
 */
static uint32_t lvgl_special_key(uint16_t physical)
{
    switch (physical) {
    case PS2_KEY_ESC:       return LV_KEY_ESC;
    case PS2_KEY_BACKSPACE: return LV_KEY_BACKSPACE;
    case PS2_KEY_TAB:       return LV_KEY_NEXT;
    case PS2_KEY_ENTER:     return LV_KEY_ENTER;
    case PS2_KEY_KP_ENTER:  return LV_KEY_ENTER;
    case PS2_KEY_DELETE:    return LV_KEY_DEL;
    case PS2_KEY_UP:        return LV_KEY_UP;
    case PS2_KEY_DOWN:      return LV_KEY_DOWN;
    case PS2_KEY_RIGHT:     return LV_KEY_RIGHT;
    case PS2_KEY_LEFT:      return LV_KEY_LEFT;
    case PS2_KEY_HOME:      return LV_KEY_HOME;
    case PS2_KEY_END:       return LV_KEY_END;
    default:                return 0;
    }
}

/* 修饰键（Shift / Ctrl / Alt / Caps）永远不成为 keypad 的活动键。 */
static bool ps2_is_modifier(uint16_t physical)
{
    switch (physical) {
    case PS2_KEY_LSHIFT:
    case PS2_KEY_RSHIFT:
    case PS2_KEY_LCTRL:
    case PS2_KEY_RCTRL:
    case PS2_KEY_LALT:
    case PS2_KEY_RALT:
    case PS2_KEY_CAPS:
        return true;
    default:
        return false;
    }
}

/*
 * 原生按键事件 → LVGL 键值。返回 0 表示没有 LVGL 键义。
 *
 * 空格是唯一的上下文相关键：文本输入里它是可打印字符（可以重复），
 * 其他控件上它是激活键（映射成 Enter，只激活一次）。原生 physical_key
 * 不受影响：应用看到的始终是真实物理身份。
 */
static uint32_t lvgl_map_key(gui_input_t *input, const gui_key_event_t *event)
{
    if (event->codepoint != 0) {
        if (event->codepoint == (uint32_t)' ' && !gui_input_focus_is_text_input(input)) {
            return LV_KEY_ENTER;
        }
        return event->codepoint;   /* ASCII 字符 */
    }
    return lvgl_special_key(event->physical_key);
}



/* ── read 回调 ────────────────────────────────────────────────── */

/*
 * LVGL 在每次 lv_indev_read() 时回调本函数。
 *
 * keypad：只投递**显式排入**的样本（pending_valid）；没有待投递样本时
 * 报告 RELEASED，避免自动排空造成重复边沿。
 * pointer：报告当前绝对位置与左键状态。
 */
static void gui_input_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (!indev || !data) {
        return;
    }

    gui_input_t *input = (gui_input_t *)lv_indev_get_user_data(indev);
    if (!input) {
        return;
    }

    gui_input_lvgl_t *lv = &input->lvgl;

    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
        if (lv->pending_valid) {
            data->key = lv->pending_key;
            data->state = lv->pending_state;
        } else {
            data->key = 0;
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }

    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        data->point.x = lv->cursor_x;
        data->point.y = lv->cursor_y;
        data->state = lv->pointer_pressed ? LV_INDEV_STATE_PRESSED
                                          : LV_INDEV_STATE_RELEASED;
        /*
         * 本版不用 enc_diff 实现滚轮：LVGL 的 indev_proc_pointer_diff() 只在
         * last_pressed 存在时处理它，悬停（没有按下）时它什么也不做。因此
         * 这里恒为 0：没有新样本时只报告当前状态，不会反复返回旧滚轮值。
         */
        data->enc_diff = 0;
        /* 手动排空必须有界：不允许 LVGL 自动继续读取。 */
        data->continue_reading = 0;
    }
}

/* ── 投递一个 LVGL 样本 ───────────────────────────────────────── */

/*
 * 排入一个 keypad 样本并显式读一次。
 *
 * EVENT 模式下每个逻辑边沿都需要一次显式 read：read 回调只会报告显式排入
 * 的样本（pending_valid），没有待投递样本时报告 RELEASED，避免自动排空
 * 造成重复边沿。
 */
static void lvgl_emit(gui_input_t *input, uint32_t key, lv_indev_state_t state)
{
    gui_input_lvgl_t *lv = &input->lvgl;

    lv->pending_key = key;
    lv->pending_state = state;
    lv->pending_valid = true;
    lv_indev_read(lv->keypad);
    lv->pending_valid = false;
}

/* ── 光标（程序绘制的箭头） ────────────────────────────────────── */

/*
 * 画一个 12×18 的箭头。
 *
 * 形状：上 8 行是逐行加宽的三角（尖端在左上角 = 热点 (0,0)），下面 10 行是
 * 2 px 宽的竖杆。整个箭头是**程序**用矩形画出来的：不引入图片 / 字体资源。
 *
 * 坐标是**绝对**坐标：lv_draw_rect() 与 LVGL 自己的控件绘制一样使用屏幕
 * 坐标，因此这里把控件坐标加上局部偏移。
 */
static void cursor_draw_cb(lv_event_t *e)
{
    lv_obj_t *cursor = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t dsc;
    lv_area_t base;
    int32_t row;

    lv_obj_get_coords(cursor, &base);

    lv_draw_rect_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.bg_color = lv_color_hex(GUI_INPUT_CURSOR_COLOR);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    dsc.border_opa = LV_OPA_TRANSP;
    dsc.outline_opa = LV_OPA_TRANSP;
    dsc.shadow_opa = LV_OPA_TRANSP;

    /* 三角头：第 row 行的宽度是 row + 1。 */
    for (row = 0; row < 8; row++) {
        lv_area_t area;

        area.x1 = base.x1;
        area.y1 = base.y1 + row;
        area.x2 = base.x1 + row;
        area.y2 = base.y1 + row;
        lv_draw_rect(layer, &dsc, &area);
    }

    /* 竖杆。 */
    {
        lv_area_t area;

        area.x1 = base.x1;
        area.y1 = base.y1 + 8;
        area.x2 = base.x1 + 1;
        area.y2 = base.y1 + GUI_INPUT_CURSOR_HEIGHT - 1;
        lv_draw_rect(layer, &dsc, &area);
    }
}

/*
 * 建立光标对象。
 *
 * 光标**属于 input**（不是 platform）：platform 只是显示，input 才是输入
 * 的所有者。光标固定 12×18，热点在左上角 (0,0)，挂在该 display 的 system
 * layer 上，并且不是 CLICKABLE：它永远不截获 hit-test。
 */
static lv_obj_t *cursor_create(lv_display_t *disp, int32_t x, int32_t y)
{
    lv_obj_t *cursor;

    if (!disp) {
        return NULL;
    }
    cursor = lv_obj_create(lv_display_get_layer_sys(disp));
    if (!cursor) {
        return NULL;
    }

    /*
     * 先清样式再设尺寸 / 位置：lv_obj_set_size() / lv_obj_set_pos() 写的是
     * **样式属性**，先清后设才不会把它们一起清掉。
     */
    lv_obj_remove_style_all(cursor);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cursor, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cursor, 0, LV_PART_MAIN);
    lv_obj_set_size(cursor, GUI_INPUT_CURSOR_WIDTH, GUI_INPUT_CURSOR_HEIGHT);
    lv_obj_set_pos(cursor, x, y);

    /* 不参与命中、不参与布局、不参与滚动。 */
    lv_obj_remove_flag(cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cursor, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cursor, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);

    lv_obj_add_event_cb(cursor, cursor_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return cursor;
}

/* ── 悬停滚轮 ──────────────────────────────────────────────────── */

/*
 * 当前指针位置命中的对象。
 *
 * 搜索顺序与 LVGL 自己的 pointer_search_obj() 一致：system layer → top
 * layer → 当前屏幕 → bottom layer。cursor 在 system layer 上，而且不是
 * CLICKABLE，因此不会截获命中；这里仍然显式排除它（双保险）。
 */
static lv_obj_t *wheel_hit_object(gui_input_t *input)
{
    lv_display_t *disp = gui_platform_get_lv_display(input->platform);
    lv_point_t point;
    lv_obj_t *hit;

    if (!disp) {
        return NULL;
    }
    point.x = input->lvgl.cursor_x;
    point.y = input->lvgl.cursor_y;

    hit = lv_indev_search_obj(lv_display_get_layer_sys(disp), &point);
    if (!hit) {
        hit = lv_indev_search_obj(lv_display_get_layer_top(disp), &point);
    }
    if (!hit) {
        hit = lv_indev_search_obj(lv_display_get_screen_active(disp), &point);
    }
    if (!hit) {
        hit = lv_indev_search_obj(lv_display_get_layer_bottom(disp), &point);
    }
    if (!hit) {
        return NULL;
    }
    if (hit == input->lvgl.cursor) {
        return NULL;   /* cursor 永远不是滚轮目标 */
    }
    /* 隐藏 / 禁用（含祖先）不是滚轮目标：与焦点候选共用同一条判定。 */
    if (!gui_input_focus_object_usable(hit)) {
        return NULL;
    }
    return hit;
}

/*
 * 最近的**纵向可滚动**祖先（自身也算）。
 *
 * "可滚动" = 有 SCROLLABLE 标志，并且当前确实有纵向余量（内容比视口高，
 * 或者已经向下滚过一段）。
 */
static lv_obj_t *wheel_scroller(lv_obj_t *obj)
{
    lv_obj_t *node;

    for (node = obj; node; node = lv_obj_get_parent(node)) {
        if (!lv_obj_has_flag(node, LV_OBJ_FLAG_SCROLLABLE)) {
            continue;
        }
        /* 余量查询用的是子控件的坐标：布局必须先算出来。 */
        lv_obj_update_layout(node);
        if (lv_obj_get_scroll_bottom(node) > 0 || lv_obj_get_scroll_top(node) > 0) {
            return node;
        }
    }
    return NULL;
}

/*
 * 把 obj 纵向滚动 amount 像素，返回真正滚动的像素数（带符号）。
 *
 * amount > 0：内容向下滚（scroll_y 增大）；amount < 0：内容向上滚。
 * 夹紧到当前可用范围：向下用 scroll_bottom，向上用 scroll_top。
 *
 * lv_obj_scroll_by() 的 dy 与 scroll_y 方向相反（它写的是内部 scroll.y），
 * 因此向下滚要传负的 dy。
 */
static int32_t wheel_scroll_by(lv_obj_t *obj, int32_t amount)
{
    int32_t room;
    int32_t moved;

    if (amount == 0) {
        return 0;
    }

    lv_obj_update_layout(obj);

    if (amount > 0) {
        room = lv_obj_get_scroll_bottom(obj);
        if (room < 0) {
            room = 0;
        }
        moved = (amount < room) ? amount : room;
        if (moved > 0) {
            lv_obj_scroll_by(obj, 0, -moved, LV_ANIM_OFF);
        }
        return moved;
    }

    room = lv_obj_get_scroll_top(obj);
    if (room < 0) {
        room = 0;
    }
    /*
     * moved = min(-amount, room)：向上滚的请求量与可用余量取小的那个。
     *
     * 注意不能写成 (amount < -room) ? -amount : room——那是反向比较：请求量
     * 比余量**小**时反而返回整个余量（一次刻度把剩余范围全部退光），请求量
     * 比余量**大**时返回整个请求量（越过上边界，scroll_y 变成负数）。
     * lv_obj_scroll_by() 本身不夹紧（夹紧在 lv_obj_scroll_by_bounded() 里，
     * 本实现没有调用它），所以这一行是唯一的保护。
     */
    moved = (-amount < room) ? -amount : room;
    if (moved > 0) {
        lv_obj_scroll_by(obj, 0, moved, LV_ANIM_OFF);
    }
    return -moved;
}

/*
 * 悬停滚轮：把滚轮刻度换算成像素，滚动命中对象最近的纵向可滚动祖先。
 *
 * 本版**不**使用 enc_diff：LVGL 的 indev_proc_pointer_diff() 只在
 * last_pressed 存在时才处理 enc_diff，悬停（没有按下）时它什么也不做。
 * 目标选择、范围夹紧与余量传播都在这里自己完成。
 *
 * 到了边界，剩余的量交给**父**滚动容器；没有可用目标则完全不动作
 * （不得改成 slider 的值，也不得改键盘焦点）。
 */
static void wheel_scroll(gui_input_t *input, int32_t detents)
{
    lv_obj_t *hit;
    lv_obj_t *scroller;
    int32_t remaining;

    if (detents == 0) {
        return;
    }

    hit = wheel_hit_object(input);
    if (!hit) {
        return;   /* 无目标：不动作 */
    }
    scroller = wheel_scroller(hit);
    if (!scroller) {
        return;   /* 没有可纵向滚动的祖先：不动作 */
    }

    remaining = detents * GUI_INPUT_WHEEL_STEP_PX;
    while (scroller && remaining != 0) {
        int32_t moved = wheel_scroll_by(scroller, remaining);

        remaining -= moved;
        /* 下一个候选是当前滚动容器的父容器（余量传播）。 */
        scroller = wheel_scroller(lv_obj_get_parent(scroller));
    }
}

/* ── 生命周期 hook ────────────────────────────────────────────── */

int gui_input_lvgl_create(gui_input_t *input)
{
    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (input->lvgl.keypad) {
        return 0;   /* 已经建立 */
    }

    lv_display_t *disp = gui_platform_get_lv_display(input->platform);
    if (!disp) {
        errno = EINVAL;
        return -1;
    }

    gui_input_lvgl_t *lv = &input->lvgl;
    lv->width = gui_platform_get_width(input->platform);
    lv->height = gui_platform_get_height(input->platform);
    /* 初始光标位于视口中心。 */
    lv->cursor_x = (int32_t)(lv->width / 2u);
    lv->cursor_y = (int32_t)(lv->height / 2u);

    /* 私有默认焦点组：keypad 显式绑定它。 */
    if (gui_input_focus_create(input) != 0) {
        errno = ENOMEM;
        return -1;
    }


    lv->keypad = lv_indev_create();
    if (!lv->keypad) {
        errno = ENOMEM;
        goto fail;
    }
    lv_indev_set_type(lv->keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(lv->keypad, disp);
    lv_indev_set_mode(lv->keypad, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(lv->keypad, gui_input_read_cb);
    lv_indev_set_user_data(lv->keypad, input);
    lv_indev_set_group(lv->keypad, lv->group);

    lv->pointer = lv_indev_create();
    if (!lv->pointer) {
        errno = ENOMEM;
        goto fail;
    }
    lv_indev_set_type(lv->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(lv->pointer, disp);
    /* pointer 保留 TIMER 模式：长按 / 拖动定时器需要它继续推进。 */
    lv_indev_set_mode(lv->pointer, LV_INDEV_MODE_TIMER);
    lv_indev_set_read_cb(lv->pointer, gui_input_read_cb);
    lv_indev_set_user_data(lv->pointer, input);

    /*
     * 光标：属于 input，不属于 platform。
     *
     * lv_indev_set_cursor() 负责把它挂到该 display 的 system layer、清掉
     * CLICKABLE，并在指针移动时自动跟随（热点 (0,0) = 左上角）。
     */
    lv->cursor = cursor_create(disp, lv->cursor_x, lv->cursor_y);
    if (!lv->cursor) {
        errno = ENOMEM;
        goto fail;
    }
    lv_indev_set_cursor(lv->pointer, lv->cursor);
    /* lv_indev_set_cursor() 把光标放到 act_point（初始 (0,0)）：重新定位。 */
    lv_obj_set_pos(lv->cursor, lv->cursor_x, lv->cursor_y);

    /*
     * 没有鼠标源时隐藏光标（keyboard-only 降级），键盘与焦点仍然可用。
     * 合成输入的鼠标源是私有注入桥接：它到达时再把光标显示出来。
     */
    lv->cursor_shown = gui_input_has_mouse(input);
    if (!lv->cursor_shown) {
        lv_obj_add_flag(lv->cursor, LV_OBJ_FLAG_HIDDEN);
    }

    return 0;

fail:
    gui_input_lvgl_destroy(input);
    return -1;
}

void gui_input_lvgl_destroy(gui_input_t *input)
{
    if (!input) {
        return;
    }

    gui_input_lvgl_t *lv = &input->lvgl;
    if (!lv->keypad && !lv->pointer && !lv->group && !lv->cursor) {
        return;
    }

    /* 取消未完成的手势：不合成 click。 */
    gui_input_lvgl_cancel(input);

    if (lv->keypad) {
        lv_indev_set_group(lv->keypad, NULL);
        lv_indev_delete(lv->keypad);
        lv->keypad = NULL;
    }
    if (lv->pointer) {
        lv_indev_delete(lv->pointer);
        lv->pointer = NULL;
    }
    /* 光标属于 input：indev 删掉之后单独删除光标对象。 */
    if (lv->cursor) {
        lv_obj_delete(lv->cursor);
        lv->cursor = NULL;
    }
    gui_input_focus_destroy(input);
    lv->pending_valid = false;
    lv->active_key = 0;
    lv->active_physical = 0;
    lv->pressed_obj = NULL;
    lv->cancel_pending = false;
    lv->pointer_pressed = false;
    lv->cursor_shown = false;
}

void gui_input_lvgl_cancel(gui_input_t *input)
{
    gui_input_lvgl_t *lv;

    if (!input) {
        return;
    }
    lv = &input->lvgl;

    /*
     * 键盘：与"新键取消旧键"共用**同一份**重新就绪协议
     * （gui_input_lvgl_cancel_key）。两份协议各自演化会漂移：旧的那份不
     * 检查 read 是否真的消费了取消样本，被阻塞时 LVGL 会吞掉下一次
     * PRESSED（wait_until_release 仍然有效），按键就被静默丢掉了。
     */
    (void)gui_input_lvgl_cancel_key(input);

    if (lv->pointer) {
        /* pointer 用 reset 清理手势状态（不补 click）。 */
        lv_indev_reset(lv->pointer, NULL);
        lv->pointer_pressed = false;
    }
}

/* ── 取消 / 重新就绪协议 ───────────────────────────────────────── */

/*
 * 取消当前活动键并让 keypad 重新就绪（spec §6.2.2）。
 *
 * 顺序：停重复 → 清未投递事件 → wait_release → 显式 RELEASED read →
 * 清旧控件视觉。调用方必须在这一步完成之后才投递新的 PRESSED。
 *
 * 返回 false 表示重新就绪尚未完成：read 被屏幕动画 / indev 暂停阻塞，
 * 此时旧键已经失效（不会再泄漏），但不允许投递新键，调用方稍后重试。
 */
bool gui_input_lvgl_cancel_key(gui_input_t *input)
{
    gui_input_lvgl_t *lv;

    if (!input) {
        return true;
    }
    lv = &input->lvgl;

    /* 1. 停止旧键的重复。 */
    input->repeat_armed = false;

    /* 2. 清空尚未投递的旧键事件。 */
    lv->pending_valid = false;

    /* 旧活动键立即失效：迟到的释放被拒绝，旧键不会作用到新控件上。 */
    lv->active_key = 0;
    lv->active_physical = 0;

    if (!lv->keypad) {
        lv->cancel_pending = false;
        return true;
    }

    /* 3. wait_release：让下一次 read 提供一次 RELEASED。 */
    lv_indev_wait_release(lv->keypad);

    /* 4. 显式读一次，消费掉那次 RELEASED。
     *    该合成样本不产生原生 UP 回调，也不合成 click。 */
    lv->pending_key = 0;
    lv->pending_state = LV_INDEV_STATE_RELEASED;
    lv->pending_valid = true;
    lv_indev_read(lv->keypad);
    lv->pending_valid = false;

    if (lv_indev_get_state(lv->keypad) != LV_INDEV_STATE_RELEASED) {
        /* read 没有真正消费取消样本：保留 pending，稍后重试。 */
        lv->cancel_pending = true;
        return false;
    }

    /* 5. 清旧控件的视觉按下状态（对象仍存活时）。 */
    if (lv->pressed_obj && gui_input_focus_alive(input, lv->pressed_obj)) {
        lv_obj_remove_state(lv->pressed_obj, LV_STATE_PRESSED);
    }
    lv->pressed_obj = NULL;
    lv->cancel_pending = false;
    return true;
}

/*
 * 该键是否参与软件重复。
 *
 * 只有可打印字符、Backspace / Delete、方向键 / Home / End 重复；Tab、
 * Enter、Esc 与快捷键（Ctrl / Alt 组合）不重复执行动作。空格在文本输入里
 * 是可打印字符（重复），在按钮上是激活键（不重复）。
 */
bool gui_input_lvgl_repeatable(gui_input_t *input, const gui_key_event_t *event)
{
    uint32_t key;

    if (!input || !event || event->action != GUI_KEY_DOWN) {
        return false;
    }

    /* 快捷键不重复。 */
    if (event->modifiers & (uint16_t)(GUI_MOD_LCTRL | GUI_MOD_RCTRL |
                                      GUI_MOD_LALT | GUI_MOD_RALT)) {
        return false;
    }
    /* Tab 由适配层导航，不进入 keypad。 */
    if (event->physical_key == PS2_KEY_TAB) {
        return false;
    }

    key = lvgl_map_key(input, event);
    if (key == 0) {
        return false;
    }
    if (key == LV_KEY_NEXT || key == LV_KEY_PREV ||
        key == LV_KEY_ENTER || key == LV_KEY_ESC) {
        return false;
    }

    /* 可打印字符。 */
    if (key >= 0x20u && key < 0x7Fu) {
        return true;
    }
    /* 编辑键与方向键。 */
    switch (key) {
    case LV_KEY_BACKSPACE:
    case LV_KEY_DEL:
    case LV_KEY_UP:
    case LV_KEY_DOWN:
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
    case LV_KEY_HOME:
    case LV_KEY_END:
        return true;
    default:
        return false;
    }
}

/*
 * 新的按键按下：取消旧活动键，并维护软件重复策略。
 *
 * 由 input.c 在**报告原生 handler 之前**调用：即使新键随后被应用消费，
 * 旧键也必须先被取消（spec §6.2.1）。
 *
 * 修饰键（Shift / Ctrl / Alt / Caps）既不成为活动键，也不改变已按下键的
 * 缓存字符：Shift / Caps 的变化不会让正在重复的键换字符。
 */
void gui_input_lvgl_key_down(gui_input_t *input, const gui_key_event_t *event,
                             uint64_t now_ms)
{
    gui_input_lvgl_t *lv;

    if (!input || !event) {
        return;
    }
    lv = &input->lvgl;

    if (ps2_is_modifier(event->physical_key)) {
        return;
    }

    /* 新的非修饰键 DOWN：先取消旧活动键。 */
    if (lv->active_physical != 0) {
        (void)gui_input_lvgl_cancel_key(input);
    }

    /*
     * 武装软件重复：首次 400 ms、之后 40 ms。字符取该键 DOWN 时解码器缓存
     * 的值（Shift / Caps 的后续变化不改变它）。
     */
    if (gui_input_lvgl_repeatable(input, event)) {
        input->repeat_armed = true;
        input->repeat_physical = event->physical_key;
        input->repeat_char = input->ps2.cached[gui_ps2_key_slot(event->physical_key)];
        input->repeat_deadline_ms = now_ms + GUI_INPUT_REPEAT_FIRST_MS;
    } else {
        input->repeat_armed = false;
    }
}


/*
 * 被消费的释放仍然必须释放 LVGL 侧的活动键。
 *
 * 否则 LVGL 会一直认为那个键处于按下状态（LVGL 的 keypad 是单键模型，
 * 下一次按下别的键会被当成错配的释放）。这里复用取消协议：LVGL 侧的按键
 * 状态被复位，但**不**产生原生 UP 回调，也不合成 click。
 */
void gui_input_lvgl_force_release(gui_input_t *input, uint16_t physical_key)
{
    gui_input_lvgl_t *lv;

    if (!input) {
        return;
    }
    lv = &input->lvgl;
    if (lv->active_physical != physical_key || lv->active_key == 0) {
        /* 不是当前活动键：不得释放别的键。 */
        return;
    }

    (void)gui_input_lvgl_cancel_key(input);
}

/*
 * 标记"需要执行取消协议"。
 *
 * 只能在回调之外真正执行：回调里递归读 indev 是不允许的。
 */
void gui_input_lvgl_mark_cancel(gui_input_t *input)
{
    gui_input_lvgl_t *lv;

    if (!input) {
        return;
    }
    lv = &input->lvgl;
    if (lv->active_physical == 0) {
        return;   /* 没有活动键：无需取消 */
    }
    lv->cancel_pending = true;
}

/*
 * 每步一次的适配层维护：焦点检查 + 重试待处理的取消协议。
 * 返回 false 表示取消协议尚未完成（read 被阻塞），此时不能投递新键。
 */
bool gui_input_lvgl_pump(gui_input_t *input)
{
    if (!input) {
        return true;
    }

    /*
     * 焦点检查：隐藏 / 禁用 / 删除的控件不再占导航位置。
     *
     * 焦点因此发生转移时，旧键与它的重复必须取消（spec §7.2）：否则软件
     * 重复会被注入到新焦点控件上，随后的真实 UP 还会变成新控件的幽灵
     * click。focus_sync() 里的组回调也会标记，但这里显式接住返回值，
     * 两条路径都覆盖。
     */
    if (gui_input_focus_refresh(input)) {
        gui_input_lvgl_mark_cancel(input);
    }

    if (input->lvgl.cancel_pending) {
        return gui_input_lvgl_cancel_key(input);
    }
    return true;
}

/* 焦点对象消失：旧键不能继续作用于自动选中的新控件。 */
void gui_input_focus_lost(gui_input_t *input, lv_obj_t *obj)
{
    gui_input_lvgl_t *lv;

    if (!input) {
        return;
    }
    lv = &input->lvgl;

    if (lv->pressed_obj == obj) {
        lv->pressed_obj = NULL;   /* 对象已消失：不再去碰它 */
    }
    gui_input_lvgl_mark_cancel(input);
}


/* ── 投递 hook ────────────────────────────────────────────────── */

int gui_input_lvgl_deliver(gui_input_t *input, const gui_key_event_t *event)
{
    gui_input_lvgl_t *lv;
    uint32_t key;

    if (!input || !event) {
        return 0;
    }

    lv = &input->lvgl;
    if (!lv->keypad) {
        return 0;
    }

    /* 待处理的取消协议必须先完成：否则旧键还处于按下状态。 */
    if (lv->cancel_pending && !gui_input_lvgl_cancel_key(input)) {
        return 0;
    }

    if (event->action == GUI_KEY_REPEAT) {
        /*
         * 软件重复：注入完整的 RELEASED→PRESSED 对。最后一个 PRESSED 保持
         * 按下，直到真正的 UP 到达。
         */
        if (lv->active_key == 0) {
            return 0;
        }
        key = (event->codepoint != 0) ? event->codepoint
                                      : lvgl_special_key(event->physical_key);
        if (key == 0) {
            return 0;
        }
        lvgl_emit(input, key, LV_INDEV_STATE_RELEASED);
        lvgl_emit(input, key, LV_INDEV_STATE_PRESSED);
        return 1;
    }

    if (event->action == GUI_KEY_UP) {
        /*
         * UP 沿用对应 DOWN 的键值（UP 的 codepoint 恒为 0），但**必须**
         * 与当前按住键的物理身份一致：LVGL 的 keypad indev 是单键模型，
         * 错配的释放会把别的键释放掉（并让真正的那次释放被丢弃）。
         * 例如 press 'a' → press 'b' → release 'a' → release 'b'：
         * release 'a' 与当前活动键不符 → 拒绝投递；release 'b' 才是
         * 真正的释放。键被取消（cancel）后 active_physical 为 0，
         * 迟到的释放同样被拒绝。
         */
        if (event->physical_key != lv->active_physical) {
            return 0;
        }
        key = lv->active_key;
        if (key == 0) {
            return 0;
        }
        lvgl_emit(input, key, LV_INDEV_STATE_RELEASED);
        lv->active_key = 0;
        lv->active_physical = 0;
        lv->pressed_obj = NULL;
        input->repeat_armed = false;
        return 1;
    }

    /* DOWN */
    if (event->physical_key == PS2_KEY_TAB) {
        /*
         * Tab 由适配层导航：它不进入 keypad（否则会被 textarea 当字符
         * 插入）。Shift-Tab 往回走。
         */
        bool shift = (event->modifiers &
                      (uint16_t)(GUI_MOD_LSHIFT | GUI_MOD_RSHIFT)) != 0;
        (void)gui_input_focus_navigate(input, !shift);
        return 1;
    }

    key = lvgl_map_key(input, event);
    if (key == 0) {
        /* 没有 LVGL 键义（修饰键、功能键、keypad 数字区……）。 */
        return 0;
    }

    lv->active_key = key;
    lv->active_physical = event->physical_key;
    lv->pressed_obj = gui_input_focus_current(input);
    lvgl_emit(input, key, LV_INDEV_STATE_PRESSED);

    /*
     * 回调里可能发生焦点变化或对象删除：此时执行取消协议（旧键不得继续
     * 作用到自动选中的新控件上）。
     */
    if (lv->cancel_pending) {
        (void)gui_input_lvgl_cancel_key(input);
    }
    return 1;
}


int gui_input_lvgl_pointer(gui_input_t *input, const mouse_event_t *event)
{
    gui_input_lvgl_t *lv = &input->lvgl;
    int32_t x;
    int32_t y;

    if (!input || !event) {
        return 0;
    }

    lv = &input->lvgl;
    if (!lv->pointer) {
        return 0;
    }

    /*
     * 光标可见性：没有鼠标源时（keyboard-only 降级）光标是隐藏的。收到第一
     * 个鼠标事件说明存在鼠标源（设备鼠标或合成注入），此时显示光标。
     */
    if (!lv->cursor_shown && lv->cursor) {
        lv->cursor_shown = true;
        lv_obj_remove_flag(lv->cursor, LV_OBJ_FLAG_HIDDEN);
    }

    /* 绝对位置：相对位移累加后夹到视口内（1 计数 = 1 像素）。 */
    x = lv->cursor_x + (int32_t)event->dx;
    y = lv->cursor_y + (int32_t)event->dy;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (lv->width > 0 && (uint32_t)x >= lv->width) {
        x = (int32_t)lv->width - 1;
    }
    if (lv->height > 0 && (uint32_t)y >= lv->height) {
        y = (int32_t)lv->height - 1;
    }
    lv->cursor_x = x;
    lv->cursor_y = y;

    /* 左键对应 pointer 的按下 / 释放。右 / 中键只报告给原生回调。 */
    lv->pointer_pressed = (event->buttons & 0x01u) != 0;

    /*
     * 悬停滚轮：左键按住（拖动中）时忽略，避免手势冲突。
     *
     * 滚轮值按 mouse_event 原值使用：正值 = 内容向下滚（scroll_y 增大），
     * 每刻度 32 px。
     */
    if (event->wheel != 0 && !lv->pointer_pressed) {
        wheel_scroll(input, (int32_t)event->wheel);
    }

    lv_indev_read(lv->pointer);
    return 1;
}
