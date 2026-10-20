/*
 * test_gui_input_lvgl.c — GUI-2 Task 7：keypad、重复、快捷键与焦点注册
 *
 * 本用例 host-compile **真实**的 libgui 输入单元（libgui/src/input.c +
 * input_lvgl.c + input_focus.c）、真实平台（libgui/src/platform.c）与真实
 * PS/2 解码器（libgui/src/input_ps2.c），并链接 hosttests/mock 下的记录
 * 运行时与真实 LVGL 归档。
 *
 * 被测部分（keypad indev、焦点组、事件处理）**没有**被 mock 掉：它们就是
 * 生产代码本身。合成的 Set 1 字节通过私有注入桥接
 * （gui_input_test_feed_keybytes）进入**同一条**解码器 / FIFO / 适配层
 * 路径；该桥接只在宿主构建（-DOS01_HOST_TEST）里存在，因此永远不会进入
 * 公共 sysroot。
 *
 * 被断言的契约（spec §6.2 / §7.2）：
 *
 *   焦点
 *     - 登记顺序就是 Tab 顺序；Tab / Shift-Tab 在可聚焦项之间前进 / 后退
 *       并循环；隐藏 / 禁用（含祖先）的控件不占导航位置，恢复后回到原位；
 *     - 首个登记的可用控件建立初始焦点；空焦点组不崩溃；
 *     - 焦点轮廓（LV_STATE_FOCUS_KEY 下的 outline）可以被像素断言采样；
 *     - 点击空白保持既有键盘焦点。
 *
 *   取消协议
 *     - Enter 按下后焦点变化或旧对象被删除：不得产生额外的按钮 click，
 *       旧控件的 PRESSED 状态被清除，下一个普通键仍然被输入。
 *
 *   重复策略
 *     - 首次 400 ms、之后 40 ms，每步至多一个，不补发；真正的 UP 之后
 *       不再重复；Ctrl/Alt 的 DOWN 取消旧键且释放后不恢复；Shift/Caps 的
 *       变化不改变缓存字符，但 REPEAT 的 modifiers 是当前已投递快照；
 *       空格在 textarea 里重复、在按钮上只激活一次；Tab / Enter / Esc
 *       不重复。
 *
 *   快捷键
 *     - 被消费的快捷键不得插入字符；被消费的释放仍然必须释放 LVGL 侧的
 *       活动键，且不得释放别的键。
 */

#include "test_framework.h"
#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include <gui_input_private.h>
#include <gui_lvgl_private.h>
#include "gui_test_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <lvgl/lvgl.h>

/* platform.c 的 OS01_HOST_TEST 断言注入点：本用例不篡改帧头，但符号必须存在。 */
bool g_gui_test_tamper_header = false;

/* ── LVGL 投递观察点（包装器只观察，不改变行为） ───────────────── */

/*
 * 适配层发起的 keypad indev 读取次数。
 *
 * 包装器立刻转发给真实的 lv_indev_read，因此产品路径的行为完全不变；只用
 * 公开 API（lv_indev_get_type）区分 keypad 与 pointer。keypad 是 EVENT 模式
 * （读定时器已暂停），所以计数只包含适配层的显式投递。
 */
static uint32_t g_gui_test_keypad_reads;

extern void __real_lv_indev_read(lv_indev_t *indev);
void __wrap_lv_indev_read(lv_indev_t *indev)
{
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
        g_gui_test_keypad_reads++;
    }
    __real_lv_indev_read(indev);
}

extern int __real_gui_input_lvgl_create(gui_input_t *input);
int __wrap_gui_input_lvgl_create(gui_input_t *input)
{
    g_gui_test_counts.lvgl_create++;
    return __real_gui_input_lvgl_create(input);
}

/* ── Set 1 字节 ────────────────────────────────────────────────── */

#define KEY_A_DOWN      0x1Eu
#define KEY_A_UP        0x9Eu
#define KEY_B_DOWN      0x30u
#define KEY_B_UP        0xB0u
#define KEY_C_DOWN      0x2Eu
#define KEY_C_UP        0xAEu
#define KEY_Q_DOWN      0x10u
#define KEY_Q_UP        0x90u
#define KEY_ENTER_DOWN  0x1Cu
#define KEY_ENTER_UP    0x9Cu
#define KEY_TAB_DOWN    0x0Fu
#define KEY_TAB_UP      0x8Fu
#define KEY_LSHIFT_DOWN 0x2Au
#define KEY_LSHIFT_UP   0xAAu
#define KEY_LCTRL_DOWN  0x1Du
#define KEY_LCTRL_UP    0x9Du
#define KEY_SPACE_DOWN  0x39u
#define KEY_SPACE_UP    0xB9u
#define KEY_ESC_DOWN    0x01u
#define KEY_ESC_UP      0x81u

#define SCREEN_W 320
#define SCREEN_H 200

/* ── 夹具 ──────────────────────────────────────────────────────── */

typedef struct {
    gui_platform_t *platform;
    gui_input_t    *input;
    lv_display_t   *disp;
    lv_obj_t       *screen;
} fixture_t;

static void fixture_setup(fixture_t *fx)
{
    gui_config_t cfg = {
        .mode = GUI_MODE_OFFSCREEN,
        .width = SCREEN_W,
        .height = SCREEN_H,
        .buffer_lines = 16
    };
    gui_input_config_t icfg = {
        .mode = GUI_INPUT_SYNTHETIC,
        .require_mouse = false
    };

    gui_test_reset();
    /*
     * LVGL 的单调 tick 从 0 开始：软件重复的期限（首次 400 ms、之后 40 ms）
     * 以 0 为基准，断言可以直接写成 399 / 400 / 439 / 440。
     */
    gui_test_set_now(0);

    fx->platform = gui_platform_create(&cfg);
    assert_not_null(fx->platform);
    fx->disp = gui_platform_get_lv_display(fx->platform);
    assert_not_null(fx->disp);
    fx->screen = lv_display_get_screen_active(fx->disp);
    assert_not_null(fx->screen);
    /*
     * 夹具里没有屏幕切换动画：屏幕切换期间 LVGL 会阻塞 indev 读取
     * （lv_indev_read 直接返回），主路径必须是在读取不被阻塞时验证的。
     * 阻塞路径由 test_blocked_read_does_not_leak_the_old_enter 单独覆盖。
     */
    assert_null(lv_display_get_screen_prev(fx->disp));

    fx->input = gui_input_create(fx->platform, &icfg);
    assert_not_null(fx->input);
}

static void fixture_teardown(fixture_t *fx)
{
    if (fx->input) {
        gui_input_destroy(fx->input);
        fx->input = NULL;
    }
    if (fx->platform) {
        gui_platform_destroy(fx->platform);
        fx->platform = NULL;
    }
}

/* 合成字节进入同一条解码器 / FIFO 路径。 */
static void feed(fixture_t *fx, const uint8_t *bytes, size_t len)
{
    assert_eq(0, gui_input_test_feed_keybytes(fx->input, bytes, len));
}

/* 走一次平台步进：FIFO 投递 + LVGL 定时器 + 渲染。 */
static void step(fixture_t *fx)
{
    uint32_t delay = 0;
    assert_eq(GUI_STATUS_OK, gui_platform_step(fx->platform, &delay));
}

/*
 * 把单调时钟推到指定时刻再步进。
 *
 * LVGL 的刷新定时器按 tick 到期：时钟不动时屏幕不会重绘，像素断言必须
 * 显式推进时钟才能看到新的画面。
 */
static void step_at(fixture_t *fx, uint64_t now_ms)
{
    gui_test_set_now(now_ms);
    step(fx);
}

/* 找到本 display 的 keypad indev（断言 LVGL 侧的按键状态用）。 */
static lv_indev_t *find_keypad(void)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);

    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            return indev;
        }
        indev = lv_indev_get_next(indev);
    }
    return NULL;
}

/* ── 原生按键日志 ──────────────────────────────────────────────── */

#define MAX_LOGGED 32

typedef struct {
    gui_key_event_t events[MAX_LOGGED];
    int count;
} key_log_t;

static bool log_key(const gui_key_event_t *event, void *userdata)
{
    key_log_t *log = (key_log_t *)userdata;

    if (log->count < MAX_LOGGED) {
        log->events[log->count] = *event;
    }
    log->count++;
    return false;   /* 不消费：事件继续进入控件路径 */
}

/* 消费指定物理键的按下 / 释放（模拟应用自己处理快捷键）。
 * physical 是**归一化**的物理身份（break 位已被解码器去掉）。 */
typedef struct {
    uint16_t physical;
    bool consume_down;
    bool consume_up;
    int down_count;
    int up_count;
} shortcut_log_t;

static bool consume_shortcut(const gui_key_event_t *event, void *userdata)
{
    shortcut_log_t *log = (shortcut_log_t *)userdata;

    if (event->physical_key != log->physical) {
        return false;
    }
    if (event->action == GUI_KEY_DOWN && log->consume_down) {
        log->down_count++;
        return true;   /* 消费 */
    }
    if (event->action == GUI_KEY_UP && log->consume_up) {
        log->up_count++;
        return true;   /* 消费 */
    }
    return false;
}

/* ── 按钮探针 ──────────────────────────────────────────────────── */

typedef struct {
    gui_input_t *input;
    lv_obj_t    *focus_target;   /* 按下时把焦点移到这个控件 */
    lv_obj_t    *delete_self;    /* 按下时删除这个控件 */
    int clicks;
    int pressed;
    int keys;                    /* 收到 LV_EVENT_KEY 的次数 */
} button_probe_t;

static void probe_button_cb(lv_event_t *e)
{
    button_probe_t *probe = (button_probe_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        probe->clicks++;
        return;
    }
    if (code == LV_EVENT_KEY) {
        probe->keys++;
        return;
    }
    if (code != LV_EVENT_PRESSED) {
        return;
    }
    probe->pressed++;

    /* 应用行为：按下时改变焦点 / 删除自己（发生在 LVGL 回调内部）。 */
    if (probe->focus_target) {
        (void)gui_input_focus_set(probe->input, probe->focus_target);
    }
    if (probe->delete_self) {
        lv_obj_delete(probe->delete_self);
    }
}

/* ── 控件工厂 ──────────────────────────────────────────────────── */

static lv_obj_t *make_textarea(fixture_t *fx, int32_t x, int32_t y)
{
    lv_obj_t *ta = lv_textarea_create(fx->screen);

    assert_not_null(ta);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, 120, 24);
    return ta;
}

static lv_obj_t *make_button(fixture_t *fx, int32_t x, int32_t y)
{
    lv_obj_t *btn = lv_button_create(fx->screen);

    assert_not_null(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 60, 30);
    return btn;
}

/* ═══════════════════════════════════════════════════════════════
 *  焦点
 * ═══════════════════════════════════════════════════════════════ */

TEST_FUNC(test_focus_add_rejects_foreign_display_and_duplicates)
{
    fixture_t fx;
    lv_obj_t *btn;
    lv_display_t *other;
    lv_obj_t *foreign;

    fixture_setup(&fx);
    btn = make_button(&fx, 20, 20);

    /* 重复登记：成功但不添加第二份。 */
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_true(gui_input_focus_current(fx.input) == btn);

    /* 别的 display 上的控件：EINVAL。 */
    other = lv_display_create(64, 64);
    assert_not_null(other);
    foreign = lv_button_create(lv_display_get_screen_active(other));
    assert_not_null(foreign);
    errno = 0;
    assert_eq(-1, gui_input_focus_add(fx.input, foreign));
    assert_eq(EINVAL, errno);
    lv_display_delete(other);

    /* 未登记的控件不能成为焦点：EINVAL。 */
    errno = 0;
    assert_eq(-1, gui_input_focus_set(fx.input, foreign));
    assert_eq(EINVAL, errno);

    fixture_teardown(&fx);
}

TEST_FUNC(test_tab_cycles_in_registration_order)
{
    fixture_t fx;
    lv_obj_t *b1, *b2, *b3;
    const uint8_t tab[] = { KEY_TAB_DOWN, KEY_TAB_UP };
    const uint8_t shift_tab[] = { KEY_LSHIFT_DOWN, KEY_TAB_DOWN, KEY_TAB_UP };

    fixture_setup(&fx);
    b1 = make_button(&fx, 20, 20);
    b2 = make_button(&fx, 100, 20);
    b3 = make_button(&fx, 180, 20);

    assert_eq(0, gui_input_focus_add(fx.input, b1));
    assert_eq(0, gui_input_focus_add(fx.input, b2));
    assert_eq(0, gui_input_focus_add(fx.input, b3));

    /* 首个登记的可用控件建立初始焦点。 */
    assert_true(gui_input_focus_current(fx.input) == b1);

    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b2);

    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b3);

    /* 循环。 */
    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b1);

    /* Shift-Tab 往回走。 */
    feed(&fx, shift_tab, sizeof(shift_tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b3);

    fixture_teardown(&fx);
}

TEST_FUNC(test_tab_skips_hidden_and_disabled_ancestors_and_restores_order)
{
    fixture_t fx;
    lv_obj_t *b1, *b2, *box_hidden, *btn_hidden, *box_disabled, *btn_disabled;
    const uint8_t tab[] = { KEY_TAB_DOWN, KEY_TAB_UP };

    fixture_setup(&fx);
    b1 = make_button(&fx, 20, 20);
    box_hidden = lv_obj_create(fx.screen);
    assert_not_null(box_hidden);
    btn_hidden = lv_button_create(box_hidden);
    assert_not_null(btn_hidden);
    box_disabled = lv_obj_create(fx.screen);
    assert_not_null(box_disabled);
    btn_disabled = lv_button_create(box_disabled);
    assert_not_null(btn_disabled);
    b2 = make_button(&fx, 100, 20);

    lv_obj_add_flag(box_hidden, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(box_disabled, LV_STATE_DISABLED);

    assert_eq(0, gui_input_focus_add(fx.input, b1));
    assert_eq(0, gui_input_focus_add(fx.input, btn_hidden));
    assert_eq(0, gui_input_focus_add(fx.input, btn_disabled));
    assert_eq(0, gui_input_focus_add(fx.input, b2));

    assert_true(gui_input_focus_current(fx.input) == b1);

    /* 隐藏 / 禁用的后代不占导航位置。 */
    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b2);

    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b1);

    /* 恢复隐藏的祖先：它回到原来的登记位置。 */
    lv_obj_remove_flag(box_hidden, LV_OBJ_FLAG_HIDDEN);
    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == btn_hidden);

    fixture_teardown(&fx);
}

TEST_FUNC(test_empty_focus_group_survives_tab)
{
    fixture_t fx;
    key_log_t log;
    const uint8_t tab[] = { KEY_TAB_DOWN, KEY_TAB_UP };
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(fx.input, log_key, &log);

    /* 空组：Tab 不崩溃，也不产生焦点。 */
    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_null(gui_input_focus_current(fx.input));

    /* 键仍然到达应用（没有被丢弃）。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_eq(3, log.count);
    assert_eq(GUI_KEY_DOWN, log.events[2].action);

    fixture_teardown(&fx);
}

/*
 * 统计控件矩形外侧一圈里的“焦点轮廓色”像素数量。
 *
 * 轮廓环的确切几何（离控件多远、多宽）属于 LVGL 的实现细节，不作为契约；
 * 契约只要求：聚焦之后，控件附近必须出现可被像素断言采样的轮廓。
 */
#define FOCUS_OUTLINE_COLOR 0x0000FF00u

static uint32_t count_outline_pixels(gui_platform_t *platform, const lv_area_t *area)
{
    uint32_t count = 0;
    int32_t y;

    for (y = area->y1 - 8; y <= area->y2 + 8; y++) {
        int32_t x;

        for (x = area->x1 - 8; x <= area->x2 + 8; x++) {
            if (gui_platform_get_pixel(platform, (uint32_t)x, (uint32_t)y) == FOCUS_OUTLINE_COLOR) {
                count++;
            }
        }
    }

    return count;
}

TEST_FUNC(test_focus_outline_is_pixel_observable)
{
    fixture_t fx;
    lv_obj_t *btn1, *btn2;
    lv_area_t area1, area2;

    fixture_setup(&fx);
    /* 两个相距很远的按钮：轮廓扫描区域互不重叠。 */
    btn1 = make_button(&fx, 20, 20);
    btn2 = make_button(&fx, 200, 120);

    /* 首个登记的可用控件建立初始焦点。 */
    assert_eq(0, gui_input_focus_add(fx.input, btn1));
    assert_eq(0, gui_input_focus_add(fx.input, btn2));
    step_at(&fx, 0);
    assert_true(gui_input_focus_current(fx.input) == btn1);

    lv_obj_get_coords(btn1, &area1);
    lv_obj_get_coords(btn2, &area2);

    /* 有焦点的控件：轮廓可以被像素断言采样。 */
    assert_true(count_outline_pixels(fx.platform, &area1) > 0);
    /* 没有焦点的控件：没有轮廓。 */
    assert_eq(0, count_outline_pixels(fx.platform, &area2));

    /* 焦点转移：轮廓跟着走。 */
    assert_eq(0, gui_input_focus_set(fx.input, btn2));
    step_at(&fx, 50);
    assert_eq(0, count_outline_pixels(fx.platform, &area1));
    assert_true(count_outline_pixels(fx.platform, &area2) > 0);

    fixture_teardown(&fx);
}

TEST_FUNC(test_clicking_blank_space_keeps_the_keyboard_focus)
{
    fixture_t fx;
    lv_obj_t *ta, *btn;
    mouse_event_t move;
    mouse_event_t press;

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 200, 150);
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_set(fx.input, ta));

    /* 光标初始在视口中心：那里是空白。 */
    memset(&move, 0, sizeof(move));
    memset(&press, 0, sizeof(press));
    press.buttons = 0x01u;   /* 左键按下 */
    gui_input_lvgl_pointer(fx.input, &move);
    gui_input_lvgl_pointer(fx.input, &press);
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == ta);

    fixture_teardown(&fx);
}

/* ═══════════════════════════════════════════════════════════════
 *  取消 / 重新就绪协议
 * ═══════════════════════════════════════════════════════════════ */

TEST_FUNC(test_enter_press_with_focus_change_has_no_ghost_click)
{
    fixture_t fx;
    lv_obj_t *ta, *btn;
    button_probe_t probe;
    const uint8_t enter_down[] = { KEY_ENTER_DOWN };
    const uint8_t enter_up[] = { KEY_ENTER_UP };
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 160, 20);

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    probe.focus_target = ta;
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_PRESSED, &probe);

    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_set(fx.input, btn));

    feed(&fx, enter_down, sizeof(enter_down));
    step(&fx);
    assert_eq(1, probe.pressed);
    /* 应用在回调里把焦点移走。 */
    assert_true(gui_input_focus_current(fx.input) == ta);

    /* 释放不得产生任何 click（既不在旧控件上，也不在新焦点上）。 */
    feed(&fx, enter_up, sizeof(enter_up));
    step(&fx);
    assert_eq(0, probe.clicks);

    /* 下一个普通键仍然被输入。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

TEST_FUNC(test_enter_press_with_deletion_inside_callback_has_no_ghost_click)
{
    fixture_t fx;
    lv_obj_t *ta, *btn;
    button_probe_t probe;
    const uint8_t enter_down[] = { KEY_ENTER_DOWN };
    const uint8_t enter_up[] = { KEY_ENTER_UP };
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 160, 20);

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    probe.delete_self = btn;
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_PRESSED, &probe);

    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_set(fx.input, btn));

    /* 按下时旧控件被删除：不得产生幽灵 click，也不得触碰已释放的对象。 */
    feed(&fx, enter_down, sizeof(enter_down));
    step(&fx);
    assert_eq(1, probe.pressed);
    assert_eq(0, probe.clicks);
    assert_true(gui_input_focus_current(fx.input) == ta);

    /* 迟到的释放被拒绝：新的焦点不产生 click。 */
    feed(&fx, enter_up, sizeof(enter_up));
    step(&fx);
    assert_eq(0, probe.clicks);

    /* 下一个普通键仍然被输入（没有被吞掉）。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

TEST_FUNC(test_all_hidden_widgets_do_not_drop_the_next_key)
{
    fixture_t fx;
    lv_obj_t *ta, *box;
    key_log_t log;
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(fx.input, log_key, &log);

    box = lv_obj_create(fx.screen);
    assert_not_null(box);
    ta = lv_textarea_create(box);
    assert_not_null(ta);
    lv_textarea_set_one_line(ta, true);
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_true(gui_input_focus_current(fx.input) == ta);

    /* 全部隐藏：没有可用候选。 */
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    step(&fx);
    assert_null(gui_input_focus_current(fx.input));

    /* 下一个键不得被丢弃：应用仍然收到它。 */
    g_gui_test_keypad_reads = 0;
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_eq(1, log.count);
    assert_eq(GUI_KEY_DOWN, log.events[0].action);
    assert_eq('a', log.events[0].codepoint);
    /* 键也确实被投递给 LVGL（一次显式 PRESSED 读取）。 */
    assert_eq(1, g_gui_test_keypad_reads);

    fixture_teardown(&fx);
}

/* ═══════════════════════════════════════════════════════════════
 *  软件重复
 * ═══════════════════════════════════════════════════════════════ */

/*
 * read 被阻塞时不得泄漏旧 Enter，也不得投递新键。
 *
 * LVGL 在屏幕切换动画期间会直接跳过 indev 读取（lv_indev_read 提前返回），
 * 取消样本因此没有被消费：适配层必须保留 pending、拒绝投递新键，等读取
 * 恢复后再补做取消协议。
 */
TEST_FUNC(test_blocked_read_does_not_leak_the_old_enter)
{
    fixture_t fx;
    lv_obj_t *ta, *btn;
    button_probe_t probe;
    lv_indev_t *keypad;
    const uint8_t enter_down[] = { KEY_ENTER_DOWN };
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t b_down[] = { KEY_B_DOWN };
    int keys_before;

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 160, 20);

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_PRESSED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_KEY, &probe);

    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_set(fx.input, btn));

    keypad = find_keypad();
    assert_not_null(keypad);

    /* Enter 按下：按钮进入 PRESSED。 */
    feed(&fx, enter_down, sizeof(enter_down));
    step(&fx);
    assert_eq(1, probe.pressed);
    assert_eq(0, probe.clicks);
    keys_before = probe.keys;

    /* indev 被暂停：读取不再消费取消样本。 */
    lv_indev_enable(keypad, false);

    g_gui_test_keypad_reads = 0;
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);

    /* 旧 Enter 没有泄漏成 click，新键也没有被投递。 */
    assert_eq(0, probe.clicks);
    assert_eq(keys_before, probe.keys);
    /*
     * 取消协议尚未完成：旧控件的 PRESSED 视觉不得提前清除
     * （清除必须发生在显式 RELEASED read 之后，spec §6.2.2）。
     */
    assert_true(lv_obj_has_state(btn, LV_STATE_PRESSED));

    /* 读取恢复：待处理的取消协议被补做（一次显式 RELEASED read）。 */
    g_gui_test_keypad_reads = 0;
    lv_indev_enable(keypad, true);
    step(&fx);
    assert_eq(1, g_gui_test_keypad_reads);   /* pending 的取消协议被补做 */
    assert_eq(0, probe.clicks);
    assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));

    /* 管线恢复：下一个键正常送达焦点控件（换一个键：'a' 仍被解码器视为按住）。 */
    feed(&fx, b_down, sizeof(b_down));
    step(&fx);
    assert_eq(keys_before + 1, probe.keys);

    fixture_teardown(&fx);
}

TEST_FUNC(test_repeat_is_400ms_then_40ms)
{
    fixture_t fx;
    lv_obj_t *ta;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t a_up[] = { KEY_A_UP };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    gui_test_set_now(399);
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    gui_test_set_now(400);
    step(&fx);
    assert_str_eq("aa", lv_textarea_get_text(ta));

    gui_test_set_now(439);
    step(&fx);
    assert_str_eq("aa", lv_textarea_get_text(ta));

    gui_test_set_now(440);
    step(&fx);
    assert_str_eq("aaa", lv_textarea_get_text(ta));

    /* 真正的 UP 之后保持稳定。 */
    feed(&fx, a_up, sizeof(a_up));
    step(&fx);
    gui_test_set_now(1000);
    step(&fx);
    assert_str_eq("aaa", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

TEST_FUNC(test_repeat_does_not_burst_after_a_stall)
{
    fixture_t fx;
    lv_obj_t *ta;
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 停顿 5 秒后一步：只产生一个重复，不补发整串。 */
    gui_test_set_now(5000);
    step(&fx);
    assert_str_eq("aa", lv_textarea_get_text(ta));

    gui_test_set_now(5040);
    step(&fx);
    assert_str_eq("aaa", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

TEST_FUNC(test_ctrl_down_cancels_and_release_does_not_restore)
{
    fixture_t fx;
    lv_obj_t *ta;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t a_up[] = { KEY_A_UP };
    const uint8_t ctrl_down[] = { KEY_LCTRL_DOWN };
    const uint8_t ctrl_up[] = { KEY_LCTRL_UP };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* Ctrl 从无到有：取消旧活动键与它的重复。 */
    feed(&fx, ctrl_down, sizeof(ctrl_down));
    step(&fx);
    feed(&fx, a_up, sizeof(a_up));
    step(&fx);

    /* Ctrl 释放后旧键不恢复：文本保持不变。 */
    feed(&fx, ctrl_up, sizeof(ctrl_up));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 必须重新按下才继续输入。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("aa", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

TEST_FUNC(test_shift_change_keeps_the_cached_char_of_a_repeat)
{
    fixture_t fx;
    lv_obj_t *ta;
    key_log_t log;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t shift_down[] = { KEY_LSHIFT_DOWN };

    fixture_setup(&fx);
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(fx.input, log_key, &log);

    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* Shift 不改变已按下键的缓存字符。 */
    feed(&fx, shift_down, sizeof(shift_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 但 REPEAT 的 modifiers 是当前已投递快照。 */
    gui_test_set_now(400);
    step(&fx);
    assert_str_eq("aa", lv_textarea_get_text(ta));
    assert_eq(3, log.count);
    assert_eq(GUI_KEY_REPEAT, log.events[2].action);
    assert_eq('a', log.events[2].codepoint);
    assert_true((log.events[2].modifiers & GUI_MOD_LSHIFT) != 0);

    fixture_teardown(&fx);
}

TEST_FUNC(test_space_repeats_in_a_textarea_but_activates_a_button_once)
{
    fixture_t fx;
    lv_obj_t *ta, *btn;
    button_probe_t probe;
    const uint8_t space_down[] = { KEY_SPACE_DOWN };
    const uint8_t space_up[] = { KEY_SPACE_UP };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 160, 20);

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_PRESSED, &probe);

    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_add(fx.input, btn));

    /* textarea 里空格是可打印字符：可以重复。 */
    assert_eq(0, gui_input_focus_set(fx.input, ta));
    feed(&fx, space_down, sizeof(space_down));
    step(&fx);
    assert_str_eq(" ", lv_textarea_get_text(ta));
    gui_test_set_now(400);
    step(&fx);
    assert_str_eq("  ", lv_textarea_get_text(ta));
    feed(&fx, space_up, sizeof(space_up));
    step(&fx);

    /* 按钮上空格是激活键：只激活一次，不重复。 */
    assert_eq(0, gui_input_focus_set(fx.input, btn));
    feed(&fx, space_down, sizeof(space_down));
    step(&fx);
    assert_eq(1, probe.pressed);
    gui_test_set_now(800);
    step(&fx);
    assert_eq(1, probe.pressed);
    assert_eq(0, probe.clicks);

    fixture_teardown(&fx);
}

TEST_FUNC(test_tab_enter_and_esc_do_not_repeat)
{
    fixture_t fx;
    lv_obj_t *ta;
    key_log_t log;
    const uint8_t enter_down[] = { KEY_ENTER_DOWN };
    const uint8_t tab[] = { KEY_TAB_DOWN, KEY_TAB_UP };
    const uint8_t esc_down[] = { KEY_ESC_DOWN };
    int i;

    fixture_setup(&fx);
    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(fx.input, log_key, &log);

    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    feed(&fx, enter_down, sizeof(enter_down));
    step(&fx);
    feed(&fx, tab, sizeof(tab));
    step(&fx);
    feed(&fx, esc_down, sizeof(esc_down));
    step(&fx);

    gui_test_set_now(1000);
    step(&fx);

    /* 没有任何 REPEAT 事件。 */
    assert_eq(4, log.count);
    for (i = 0; i < log.count; i++) {
        assert_true(log.events[i].action != GUI_KEY_REPEAT);
    }

    fixture_teardown(&fx);
}

TEST_FUNC(test_focus_change_mid_repeat_stops_the_repeat_without_dropping_keys)
{
    fixture_t fx;
    lv_obj_t *ta1, *ta2;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t a_up[] = { KEY_A_UP };

    fixture_setup(&fx);
    ta1 = make_textarea(&fx, 20, 20);
    ta2 = make_textarea(&fx, 20, 60);
    assert_eq(0, gui_input_focus_add(fx.input, ta1));
    assert_eq(0, gui_input_focus_add(fx.input, ta2));

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta1));

    /* 重复进行中：焦点转移 → 重复停止，旧键不再作用于新控件。 */
    gui_test_set_now(200);
    assert_eq(0, gui_input_focus_set(fx.input, ta2));
    step(&fx);

    gui_test_set_now(600);
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta1));
    assert_str_eq("", lv_textarea_get_text(ta2));

    /* 下一个键不得被丢弃。 */
    feed(&fx, a_up, sizeof(a_up));
    step(&fx);
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta2));

    fixture_teardown(&fx);
}

/* ═══════════════════════════════════════════════════════════════
 *  快捷键
 * ═══════════════════════════════════════════════════════════════ */

TEST_FUNC(test_consumed_shortcut_is_not_inserted)
{
    fixture_t fx;
    lv_obj_t *ta;
    shortcut_log_t log;
    const uint8_t ctrl_down[] = { KEY_LCTRL_DOWN };
    const uint8_t ctrl_up[] = { KEY_LCTRL_UP };
    const uint8_t q_down[] = { KEY_Q_DOWN };
    const uint8_t q_up[] = { KEY_Q_UP };
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    memset(&log, 0, sizeof(log));
    log.physical = KEY_Q_DOWN;
    log.consume_down = true;
    log.consume_up = true;
    gui_input_set_key_handler(fx.input, consume_shortcut, &log);

    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    feed(&fx, ctrl_down, sizeof(ctrl_down));
    step(&fx);
    feed(&fx, q_down, sizeof(q_down));
    step(&fx);
    feed(&fx, q_up, sizeof(q_up));
    step(&fx);
    feed(&fx, ctrl_up, sizeof(ctrl_up));
    step(&fx);

    /* 被消费的快捷键不得插入字符。 */
    assert_eq(1, log.down_count);
    assert_eq(1, log.up_count);
    assert_str_eq("", lv_textarea_get_text(ta));

    /* 下一个键仍然被输入。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

TEST_FUNC(test_consumed_release_still_releases_the_lvgl_key)
{
    fixture_t fx;
    lv_obj_t *ta;
    shortcut_log_t log;
    lv_indev_t *keypad;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t b_down[] = { KEY_B_DOWN };
    const uint8_t b_up[] = { KEY_B_UP };

    fixture_setup(&fx);
    memset(&log, 0, sizeof(log));
    log.physical = KEY_B_DOWN;
    log.consume_up = true;   /* 只消费释放：按下仍然进入控件路径 */
    gui_input_set_key_handler(fx.input, consume_shortcut, &log);

    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    feed(&fx, b_down, sizeof(b_down));
    step(&fx);
    assert_str_eq("ab", lv_textarea_get_text(ta));

    keypad = find_keypad();
    assert_not_null(keypad);
    assert_eq(LV_INDEV_STATE_PRESSED, lv_indev_get_state(keypad));

    /* 被消费的释放：LVGL 侧不得继续按住这个键，也不得释放别的键。 */
    feed(&fx, b_up, sizeof(b_up));
    step(&fx);
    assert_eq(1, log.up_count);
    assert_eq(LV_INDEV_STATE_RELEASED, lv_indev_get_state(keypad));

    fixture_teardown(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_focus_add_rejects_foreign_display_and_duplicates),
    TEST_ENTRY(test_tab_cycles_in_registration_order),
    TEST_ENTRY(test_tab_skips_hidden_and_disabled_ancestors_and_restores_order),
    TEST_ENTRY(test_empty_focus_group_survives_tab),
    TEST_ENTRY(test_focus_outline_is_pixel_observable),
    TEST_ENTRY(test_clicking_blank_space_keeps_the_keyboard_focus),
    TEST_ENTRY(test_enter_press_with_focus_change_has_no_ghost_click),
    TEST_ENTRY(test_enter_press_with_deletion_inside_callback_has_no_ghost_click),
    TEST_ENTRY(test_all_hidden_widgets_do_not_drop_the_next_key),
    TEST_ENTRY(test_blocked_read_does_not_leak_the_old_enter),
    TEST_ENTRY(test_repeat_is_400ms_then_40ms),
    TEST_ENTRY(test_repeat_does_not_burst_after_a_stall),
    TEST_ENTRY(test_ctrl_down_cancels_and_release_does_not_restore),
    TEST_ENTRY(test_shift_change_keeps_the_cached_char_of_a_repeat),
    TEST_ENTRY(test_space_repeats_in_a_textarea_but_activates_a_button_once),
    TEST_ENTRY(test_tab_enter_and_esc_do_not_repeat),
    TEST_ENTRY(test_focus_change_mid_repeat_stops_the_repeat_without_dropping_keys),
    TEST_ENTRY(test_consumed_shortcut_is_not_inserted),
    TEST_ENTRY(test_consumed_release_still_releases_the_lvgl_key),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
