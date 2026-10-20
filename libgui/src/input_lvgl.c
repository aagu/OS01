/*
 * input_lvgl.c — LVGL 控件适配（pointer / keypad / 焦点组）
 *
 * 本文件实现 Task 6 固定的跨文件 hook：
 *
 *   gui_input_lvgl_create   建立并绑定本 display 的 keypad / pointer
 *                           indev、read 回调与空的默认焦点组；
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
 * 焦点组是**私有**的：keypad 显式绑定它，不使用 LVGL 全局默认组。组内
 * 还没有注册控件时，键盘没有目标是合法的空组行为（Task 7 补焦点注册）。
 */

#include <gui/gui_input.h>
#include <gui_input_private.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>

#include <errno.h>
#include <string.h>

/* ── Set 1 物理键身份 ─────────────────────────────────────────── */

#define PS2_KEY_ESC        0x001u
#define PS2_KEY_BACKSPACE  0x00Eu
#define PS2_KEY_TAB        0x00Fu
#define PS2_KEY_ENTER      0x01Cu
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
    lv->group = lv_group_create();
    if (!lv->group) {
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
    if (!lv->keypad && !lv->pointer && !lv->group) {
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
    if (lv->group) {
        lv_group_delete(lv->group);
        lv->group = NULL;
    }
    lv->pending_valid = false;
    lv->active_key = 0;
    lv->active_physical = 0;
    lv->pointer_pressed = false;
}

void gui_input_lvgl_cancel(gui_input_t *input)
{
    if (!input) {
        return;
    }

    gui_input_lvgl_t *lv = &input->lvgl;

    /* 尚未投递的键事件被丢弃，当前活动键被清除（迟到的释放会被拒绝）。 */
    lv->pending_valid = false;
    lv->active_key = 0;
    lv->active_physical = 0;

    if (lv->keypad) {
        /*
         * wait-release 重新就绪协议：
         *   lv_indev_wait_release() 让下一次 read 提供一次 RELEASED；
         *   显式 lv_indev_read() 完成这一次读，之后才投递新的键。
         * 该合成样本不产生原生 UP 回调，也不合成 click。
         */
        lv_indev_wait_release(lv->keypad);
        lv->pending_key = 0;
        lv->pending_state = LV_INDEV_STATE_RELEASED;
        lv->pending_valid = true;
        lv_indev_read(lv->keypad);
        lv->pending_valid = false;
    }

    if (lv->pointer) {
        /* pointer 用 reset 清理手势状态（不补 click）。 */
        lv_indev_reset(lv->pointer, NULL);
        lv->pointer_pressed = false;
    }
}

/* ── 投递 hook ────────────────────────────────────────────────── */

int gui_input_lvgl_deliver(gui_input_t *input, const gui_key_event_t *event)
{
    if (!input || !event) {
        return 0;
    }

    gui_input_lvgl_t *lv = &input->lvgl;
    if (!lv->keypad) {
        return 0;
    }

    uint32_t key = 0;
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
        if (event->physical_key == lv->active_physical) {
            key = lv->active_key;
        }
    } else {
        if (event->codepoint != 0) {
            key = event->codepoint;   /* ASCII 字符 */
        } else {
            key = lvgl_special_key(event->physical_key);
        }
        if (key != 0) {
            lv->active_key = key;
            lv->active_physical = event->physical_key;
        }
    }

    if (key == 0) {
        /* 没有 LVGL 键义（修饰键、功能键、keypad 数字区……），或者这是一次
         * 与当前活动键不匹配的释放：都不向 LVGL 投递。 */
        return 0;
    }

    lv->pending_key = key;
    lv->pending_state = (event->action == GUI_KEY_UP) ? LV_INDEV_STATE_RELEASED
                                                      : LV_INDEV_STATE_PRESSED;
    lv->pending_valid = true;
    lv_indev_read(lv->keypad);
    lv->pending_valid = false;

    if (event->action == GUI_KEY_UP) {
        lv->active_key = 0;
        lv->active_physical = 0;
    }
    return 1;
}

int gui_input_lvgl_pointer(gui_input_t *input, const mouse_event_t *event)
{
    if (!input || !event) {
        return 0;
    }

    gui_input_lvgl_t *lv = &input->lvgl;
    if (!lv->pointer) {
        return 0;
    }

    /* 绝对位置：相对位移累加后夹到视口内（1 计数 = 1 像素）。 */
    int32_t x = lv->cursor_x + (int32_t)event->dx;
    int32_t y = lv->cursor_y + (int32_t)event->dy;
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

    /* 左键对应 pointer 的按下 / 释放。 */
    lv->pointer_pressed = (event->buttons & 0x01u) != 0;

    lv_indev_read(lv->pointer);
    return 1;
}
