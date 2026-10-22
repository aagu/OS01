/*
 * test_gui_input_lvgl.c — GUI-2 Task 7/8：keypad、重复、快捷键、焦点注册，
 *                         以及指针（光标、拖动、悬停滚轮）
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
 *
 *   指针（Task 8）
 *     - 同一步里的左键按下 / 释放只产生一次 click；
 *     - dx/dy 在 int16 极值下夹边正确（不回绕、不溢出）；
 *     - slider 拖动释放后继续移动不再改变滑块值；
 *     - 右 / 中键只报告原生 changed_buttons，不产生 click、不夺焦点；
 *     - 光标是 12×18 的程序绘制箭头，热点 (0,0)，挂在 system layer 上，
 *       像素可采样且不拦截 hit-test；
 *     - 指针静止时长按定时器仍然推进；
 *     - 悬停滚轮：悬停即滚（每刻度 32 px），内层到边界把余量交给父层，
 *       无目标不动作，slider 不被改值，拖动中忽略滚轮，移动指针不夺键盘焦点。
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

/* 消费按下，并记录应用看到的每一个事件（含 REPEAT）。
 * 用来钉住“重复只对真正到达 LVGL 的按键武装”。 */
typedef struct {
    int  downs;
    int  repeats;
    int  ups;
    bool consume_down;
} consume_log_t;

static bool consume_and_log(const gui_key_event_t *event, void *userdata)
{
    consume_log_t *log = (consume_log_t *)userdata;

    switch (event->action) {
    case GUI_KEY_DOWN:
        log->downs++;
        if (log->consume_down) {
            return true;   /* 消费：按键不进入 LVGL */
        }
        break;
    case GUI_KEY_REPEAT:
        log->repeats++;
        break;
    case GUI_KEY_UP:
        log->ups++;
        break;
    default:
        break;
    }
    return false;
}

/* 记录应用看到的每一个事件（含 REPEAT），但**不消费**：按键仍然进入控件路径。
 *
 * 用来钉住"被取消的按键不得产生重复"：应用流与控件流都必须没有 REPEAT。 */
typedef struct {
    int downs;
    int repeats;
    int ups;
} app_log_t;

static bool app_log(const gui_key_event_t *event, void *userdata)
{
    app_log_t *log = (app_log_t *)userdata;

    switch (event->action) {
    case GUI_KEY_DOWN:
        log->downs++;
        break;
    case GUI_KEY_REPEAT:
        log->repeats++;
        break;
    case GUI_KEY_UP:
        log->ups++;
        break;
    default:
        break;
    }
    return false;   /* 不消费：事件继续进入控件路径 */
}

/* ── 按钮探针 ──────────────────────────────────────────────────── */

typedef struct {
    gui_input_t *input;
    lv_obj_t    *focus_target;   /* 按下时把焦点移到这个控件 */
    lv_obj_t    *delete_self;    /* 按下时删除这个控件 */
    int clicks;
    int pressed;
    int released;
    int keys;                    /* 收到 LV_EVENT_KEY 的次数 */
    int cancels;                 /* 收到 LV_EVENT_CANCEL 的次数 */
    int long_presses;            /* 收到 LV_EVENT_LONG_PRESSED 的次数 */
} button_probe_t;

static void probe_button_cb(lv_event_t *e)
{
    button_probe_t *probe = (button_probe_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        probe->clicks++;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        probe->released++;
        return;
    }
    if (code == LV_EVENT_CANCEL) {
        probe->cancels++;
        return;
    }
    if (code == LV_EVENT_KEY) {
        probe->keys++;
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED) {
        probe->long_presses++;
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

/*
 * 交付中的按键回调：把焦点移到另一个控件。
 *
 * 焦点变化会标记取消协议（focus_group_cb → gui_input_lvgl_mark_cancel），
 * 取消协议在 deliver() 返回前执行 —— 于是这一次按键在交付途中就被取消。
 */
typedef struct {
    gui_input_t *input;
    lv_obj_t    *target;
} focus_move_ctx_t;

static void move_focus_on_key(lv_event_t *e)
{
    focus_move_ctx_t *ctx = (focus_move_ctx_t *)lv_event_get_user_data(e);

    if (!ctx || !ctx->input || !ctx->target) {
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }
    (void)gui_input_focus_set(ctx->input, ctx->target);
}

/*
 * 交付中的按键回调：删除焦点控件自己。
 *
 * 控件消失 → gui_input_focus_lost() → 标记取消协议 → 取消在 deliver() 内部
 * 执行（spec §7.2："当前焦点控件被删后，旧按键不能继续作用于自动选中的新
 * 控件"）。
 */
typedef struct {
    gui_input_t *input;
    lv_obj_t    *target;
} delete_ctx_t;

static void delete_self_on_key(lv_event_t *e)
{
    delete_ctx_t *ctx = (delete_ctx_t *)lv_event_get_user_data(e);

    if (!ctx || !ctx->target) {
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_KEY) {
        return;
    }
    lv_obj_delete(ctx->target);
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

/*
 * 焦点控件被隐藏 / 禁用时，焦点落到**它之后**的第一个可用项，而不是跳回
 * 第一个候选（spec §7.2："if it is the current focus, select the first
 * available item **after** it"）。
 *
 * 被隐藏 / 禁用的控件仍然登记在册，所以"它之后"是有定义的。
 *
 * 隐藏的情形用**祖先**被隐藏来构造：LVGL 只在"被隐藏的对象自己就是组焦点"
 * 时才把组焦点让给下一个可见项（lv_obj_add_flag → lv_group_focus_next），
 * 隐藏祖先时组焦点不动，因此这一步真正落在适配层的 focus_sync 上。
 */
TEST_FUNC(test_hidden_focus_moves_to_the_next_candidate_not_the_first)
{
    fixture_t fx;
    lv_obj_t *b1, *box, *b2, *b3;
    const uint8_t tab[] = { KEY_TAB_DOWN, KEY_TAB_UP };

    fixture_setup(&fx);
    b1 = make_button(&fx, 20, 20);
    box = lv_obj_create(fx.screen);
    assert_not_null(box);
    b2 = lv_button_create(box);
    assert_not_null(b2);
    b3 = make_button(&fx, 180, 20);

    assert_eq(0, gui_input_focus_add(fx.input, b1));
    assert_eq(0, gui_input_focus_add(fx.input, b2));
    assert_eq(0, gui_input_focus_add(fx.input, b3));

    /* 焦点停在中间那个（b2）。 */
    assert_true(gui_input_focus_current(fx.input) == b1);
    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b2);

    /* 隐藏它的祖先：b2 不再是候选，但它仍在册。 */
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    step(&fx);
    /* 焦点落到它之后的 b3，不是第一个 b1。 */
    assert_true(gui_input_focus_current(fx.input) != b1);
    assert_true(gui_input_focus_current(fx.input) == b3);

    fixture_teardown(&fx);
}

TEST_FUNC(test_disabled_focus_moves_to_the_next_candidate_not_the_first)
{
    fixture_t fx;
    lv_obj_t *b1, *b2, *b3;
    const uint8_t tab[] = { KEY_TAB_DOWN, KEY_TAB_UP };

    fixture_setup(&fx);
    b1 = make_button(&fx, 20, 20);
    b2 = make_button(&fx, 100, 20);
    b3 = make_button(&fx, 180, 20);

    assert_eq(0, gui_input_focus_add(fx.input, b1));
    assert_eq(0, gui_input_focus_add(fx.input, b2));
    assert_eq(0, gui_input_focus_add(fx.input, b3));

    assert_true(gui_input_focus_current(fx.input) == b1);
    feed(&fx, tab, sizeof(tab));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == b2);

    /* 禁用它：它不再是候选，但仍在册。 */
    lv_obj_add_state(b2, LV_STATE_DISABLED);
    step(&fx);
    /* 焦点落到它之后的 b3，不是第一个 b1。 */
    assert_true(gui_input_focus_current(fx.input) != b1);
    assert_true(gui_input_focus_current(fx.input) == b3);

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
    button_probe_t ta_probe;

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 160, 20);

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    probe.focus_target = ta;
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_PRESSED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_RELEASED, &probe);

    /* 新焦点上的探针：LVGL 的 keypad 把 RELEASED / CLICKED 发给**当前焦点**
     * 对象，所以幽灵 click 会落在 textarea 上而不是旧按钮上。 */
    memset(&ta_probe, 0, sizeof(ta_probe));
    ta_probe.input = fx.input;
    lv_obj_add_event_cb(ta, probe_button_cb, LV_EVENT_CLICKED, &ta_probe);
    lv_obj_add_event_cb(ta, probe_button_cb, LV_EVENT_RELEASED, &ta_probe);

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
    assert_eq(0, probe.released);
    assert_eq(0, ta_probe.clicks);
    assert_eq(0, ta_probe.released);
    /* 旧控件的 PRESSED 视觉必须被清除（取消协议的一步）。 */
    assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));

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
    button_probe_t ta_probe;
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
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_RELEASED, &probe);

    /* 新焦点上的探针：幽灵 RELEASED / CLICKED 会落在这里。 */
    memset(&ta_probe, 0, sizeof(ta_probe));
    ta_probe.input = fx.input;
    lv_obj_add_event_cb(ta, probe_button_cb, LV_EVENT_CLICKED, &ta_probe);
    lv_obj_add_event_cb(ta, probe_button_cb, LV_EVENT_RELEASED, &ta_probe);

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
    assert_eq(0, probe.released);
    assert_eq(0, ta_probe.clicks);
    assert_eq(0, ta_probe.released);

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

/*
 * 隐藏当前焦点必须取消按住的键（spec §7.2）。
 *
 * 焦点转移到别的控件后，旧键不得继续作用：软件重复不能注入到新焦点控件上，
 * 迟到的真实 UP 也不能变成新控件的幽灵 click。
 */
TEST_FUNC(test_hiding_the_focused_widget_cancels_the_held_key)
{
    fixture_t fx;
    lv_obj_t *ta, *btn;
    button_probe_t probe;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t a_up[] = { KEY_A_UP };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 200, 120);

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_KEY, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_RELEASED, &probe);

    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_set(fx.input, ta));

    /* 按住 'a'：文本里有一个字符。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 隐藏当前焦点：焦点转移到按钮。 */
    lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == btn);

    /* 旧键已被取消：软件重复不再产生（新焦点控件收不到重复的键）。 */
    gui_test_set_now(1000);
    step(&fx);
    assert_eq(0, probe.keys);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 迟到的真实 UP 不产生 click。 */
    feed(&fx, a_up, sizeof(a_up));
    step(&fx);
    assert_eq(0, probe.clicks);
    assert_eq(0, probe.released);

    fixture_teardown(&fx);
}

/*
 * 唯一的焦点被隐藏时，按住的键同样必须被取消。
 *
 * 这一条与上一条的区别：隐藏之后**没有**可用候选，焦点变成空。此时不能
 * 靠"焦点转移"顺带取消（旧键仍然按着，重复还会继续跑）。
 */
TEST_FUNC(test_hiding_the_only_focused_widget_cancels_the_held_key)
{
    fixture_t fx;
    lv_obj_t *ta;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t a_up[] = { KEY_A_UP };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    /* 按住 'a'。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 唯一的焦点被隐藏：没有可用候选，焦点为空。 */
    lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
    step(&fx);
    assert_null(gui_input_focus_current(fx.input));

    /* 按住的键必须被取消：重复不再产生，活动键失效。 */
    assert_false(fx.input->repeat_armed);
    assert_eq(0, fx.input->lvgl.active_key);

    /* 迟到的真实 UP 不产生任何效果。 */
    feed(&fx, a_up, sizeof(a_up));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

/*
 * 禁用当前焦点同样必须取消按住的键（spec §7.2：hidden / disabled / 删除）。
 *
 * 与隐藏的区别：禁用不改变 LVGL 自己的组焦点，是适配层在 step 检查里发现
 * 焦点控件不再可用，于是把焦点让给下一个候选并取消旧键。
 */
TEST_FUNC(test_disabling_the_focused_widget_cancels_the_held_key)
{
    fixture_t fx;
    lv_obj_t *ta, *btn;
    button_probe_t probe;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t a_up[] = { KEY_A_UP };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    btn = make_button(&fx, 200, 120);

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_KEY, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_RELEASED, &probe);

    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_set(fx.input, ta));

    /* 按住 'a'。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 禁用当前焦点：焦点让给下一个可用项，按住的键必须被取消。 */
    lv_obj_add_state(ta, LV_STATE_DISABLED);
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == btn);

    /* 取消之后：软件重复不再产生（新焦点控件收不到重复的键）。 */
    gui_test_set_now(1000);
    step(&fx);
    assert_eq(0, probe.keys);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 迟到的真实 UP 不产生 click。 */
    feed(&fx, a_up, sizeof(a_up));
    step(&fx);
    assert_eq(0, probe.clicks);
    assert_eq(0, probe.released);

    fixture_teardown(&fx);
}

/*
 * 与当前焦点无关的成员变化**不得**取消按住的键。
 *
 * 登记一个新控件、隐藏 / 显示一个非焦点控件，都会引起焦点组的成员集合
 * 重建，但焦点没有变：spec §7.2 只要求焦点控件被隐藏 / 禁用 / 删除时取消
 * 旧键。误取消会让"按住 a 时登记控件"变成按键丢失。
 */
TEST_FUNC(test_unrelated_focus_changes_do_not_cancel_the_held_key)
{
    fixture_t fx;
    lv_obj_t *ta, *other;
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_set(fx.input, ta));

    /* 按住 'a'。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* 登记一个与焦点无关的控件：焦点不变，按住的键必须继续有效。 */
    other = make_button(&fx, 20, 120);
    assert_eq(0, gui_input_focus_add(fx.input, other));
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == ta);

    /* 隐藏 / 显示一个非焦点控件：同样不得取消。 */
    lv_obj_add_flag(other, LV_OBJ_FLAG_HIDDEN);
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == ta);
    lv_obj_remove_flag(other, LV_OBJ_FLAG_HIDDEN);
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == ta);

    /* 按住的键仍然有效：软件重复照常发生。 */
    gui_test_set_now(500);
    step(&fx);
    assert_str_eq("aa", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}


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
    assert_eq(2, g_gui_test_keypad_reads);   /* cancel, then the retained a DOWN */
    assert_eq(keys_before + 1, probe.keys);
    assert_eq(0, probe.clicks);
    assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));

    /* 管线恢复：保留的 a 已送达，后续 b 也正常送达（a 仍按住）。 */
    feed(&fx, b_down, sizeof(b_down));
    step(&fx);
    assert_eq(keys_before + 2, probe.keys);

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

/*
 * 重复只对**真正到达 LVGL** 的按键武装。
 *
 * 应用消费了 DOWN 时，那个按键从未进入 LVGL：应用不得随后收到它的
 * REPEAT（否则应用会执行一个自己没有交给控件的动作）。
 */
TEST_FUNC(test_a_consumed_down_is_never_repeated)
{
    fixture_t fx;
    consume_log_t log;
    lv_obj_t *ta;
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    memset(&log, 0, sizeof(log));
    log.consume_down = true;
    gui_input_set_key_handler(fx.input, consume_and_log, &log);

    /* 应用消费了 DOWN：按键没有进入 LVGL，文本框里没有字符。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_eq(1, log.downs);
    assert_str_eq("", lv_textarea_get_text(ta));

    /* 400 ms 之后：不得出现这个按键的 REPEAT。 */
    gui_test_set_now(400);
    step(&fx);
    gui_test_set_now(440);
    step(&fx);
    gui_test_set_now(1000);
    step(&fx);
    assert_eq(1, log.downs);
    assert_eq(0, log.repeats);
    assert_false(fx.input->repeat_armed);
    assert_str_eq("", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

/*
 * 应用可见 / 控件可见的分歧（公共头文件 gui_input.h 有记载）：应用先看到
 * 事件，控件路径可能永远看不到它——没有 LVGL 键义的键（功能键、keypad
 * 数字区）只到应用为止。
 */
TEST_FUNC(test_a_key_without_an_lvgl_meaning_reaches_only_the_app)
{
    fixture_t fx;
    consume_log_t log;
    lv_obj_t *ta;
    const uint8_t f1_down[] = { 0x3B };    /* F1: no LVGL key meaning */

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(fx.input, consume_and_log, &log);

    feed(&fx, f1_down, sizeof(f1_down));
    step(&fx);

    /* 应用看到了它 ... */
    assert_eq(1, log.downs);
    /* ... 控件路径没有：它没有 LVGL 键义。 */
    assert_str_eq("", lv_textarea_get_text(ta));

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

/*
 * 交付途中被取消的按键：不得重复。
 *
 * 取消协议有两种触发方式：
 *   - 焦点在**两次重复之间**转移（已有的
 *     test_focus_change_mid_repeat_stops_the_repeat_without_dropping_keys）；
 *   - 焦点在**交付这一次按键的途中**转移（回调在 LV_EVENT_KEY 里移走焦点），
 *     取消协议在 deliver() 内部执行。
 *
 * 第二种是本用例覆盖的：deliver() 内部的取消清掉了重复的武装，如果武装
 * 发生在 deliver **之后**，取消就被撤销 —— 旧键于是重复到自动选中的新控件
 * 上（spec §6.2："按键切换、焦点变化…都取消重复"；spec §7.2："当前焦点控件
 * 被删后，旧按键不能继续作用于自动选中的新控件"）。
 */
TEST_FUNC(test_a_key_cancelled_by_its_own_callback_never_repeats)
{
    fixture_t fx;
    lv_obj_t *ta1, *ta2;
    app_log_t log;
    focus_move_ctx_t ctx;
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    ta1 = make_textarea(&fx, 20, 20);
    ta2 = make_textarea(&fx, 20, 60);
    assert_eq(0, gui_input_focus_add(fx.input, ta1));
    assert_eq(0, gui_input_focus_add(fx.input, ta2));

    /* ta1 收到按键时把焦点移到 ta2：这一次按键在交付途中被取消。 */
    ctx.input = fx.input;
    ctx.target = ta2;
    lv_obj_add_event_cb(ta1, move_focus_on_key, LV_EVENT_KEY, &ctx);

    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(fx.input, app_log, &log);

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);

    /* 这一次 DOWN 真的进了控件路径：ta1 收到了字符。 */
    assert_str_eq("a", lv_textarea_get_text(ta1));
    assert_eq(1, log.downs);
    /* 回调把焦点移走了：焦点现在在 ta2。 */
    assert_eq(ta2, gui_input_focus_current(fx.input));

    /* 被取消的按键不得武装重复，也不得产生 REPEAT。 */
    assert_eq(0, log.repeats);
    assert_true(!fx.input->repeat_armed);

    /* 过了重复期限也不得补发：旧键不得作用到新控件上。 */
    gui_test_set_now(600);
    step(&fx);
    assert_eq(0, log.repeats);
    assert_true(!fx.input->repeat_armed);
    assert_str_eq("a", lv_textarea_get_text(ta1));
    assert_str_eq("", lv_textarea_get_text(ta2));

    fixture_teardown(&fx);
}

/*
 * 焦点控件在交付途中被删除：旧键同样不得重复。
 *
 * spec §7.2："当前焦点控件被删后，旧按键不能继续作用于自动选中的新控件"。
 * 删除同样标记取消协议，取消在 deliver() 内部执行。
 */
TEST_FUNC(test_a_key_whose_focus_is_deleted_in_its_callback_never_repeats)
{
    fixture_t fx;
    lv_obj_t *ta1, *ta2;
    app_log_t log;
    delete_ctx_t ctx;
    const uint8_t a_down[] = { KEY_A_DOWN };

    fixture_setup(&fx);
    ta1 = make_textarea(&fx, 20, 20);
    ta2 = make_textarea(&fx, 20, 60);
    assert_eq(0, gui_input_focus_add(fx.input, ta1));
    assert_eq(0, gui_input_focus_add(fx.input, ta2));

    /* ta1 收到按键时删除自己：焦点控件在交付途中消失。 */
    ctx.input = fx.input;
    ctx.target = ta1;
    lv_obj_add_event_cb(ta1, delete_self_on_key, LV_EVENT_KEY, &ctx);

    memset(&log, 0, sizeof(log));
    gui_input_set_key_handler(fx.input, app_log, &log);

    feed(&fx, a_down, sizeof(a_down));
    step(&fx);

    assert_eq(1, log.downs);
    /* 焦点控件被删 → 旧键被取消 → 不得重复。 */
    assert_eq(0, log.repeats);
    assert_true(!fx.input->repeat_armed);

    /* 过了重复期限也不得补发到自动选中的新控件上。 */
    gui_test_set_now(600);
    step(&fx);
    assert_eq(0, log.repeats);
    assert_true(!fx.input->repeat_armed);
    assert_str_eq("", lv_textarea_get_text(ta2));

    fixture_teardown(&fx);
}

/*
 * 编辑键与方向键：路由给焦点控件，并且参与软件重复。
 *
 * Set 1 的方向键是 E0 前缀序列（E0 4B = Left，E0 47 = Home，E0 4F = End）。
 */
TEST_FUNC(test_editing_keys_repeat_and_route_to_the_textarea)
{
    fixture_t fx;
    lv_obj_t *ta;
    const uint8_t left[] = { 0xE0u, 0x4Bu };
    const uint8_t left_up[] = { 0xE0u, 0x9Bu };
    const uint8_t right[] = { 0xE0u, 0x4Du };
    const uint8_t right_up[] = { 0xE0u, 0x9Du };
    const uint8_t home[] = { 0xE0u, 0x47u };
    const uint8_t home_up[] = { 0xE0u, 0xC7u };
    const uint8_t end[] = { 0xE0u, 0x4Fu };
    const uint8_t end_up[] = { 0xE0u, 0xCFu };
    const uint8_t backspace[] = { 0x0Eu };
    const uint8_t del[] = { 0xE0u, 0x53u };
    const uint8_t del_up[] = { 0xE0u, 0xD3u };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    lv_textarea_set_text(ta, "abcd");
    lv_textarea_set_cursor_pos(ta, 4);

    /* 方向键：光标左移。 */
    feed(&fx, left, sizeof(left));
    step(&fx);
    assert_eq(3, lv_textarea_get_cursor_pos(ta));
    feed(&fx, left_up, sizeof(left_up));
    step(&fx);

    /* Home：回到开头；End：到末尾。 */
    feed(&fx, home, sizeof(home));
    step(&fx);
    assert_eq(0, lv_textarea_get_cursor_pos(ta));
    feed(&fx, home_up, sizeof(home_up));
    step(&fx);

    feed(&fx, end, sizeof(end));
    step(&fx);
    assert_eq(4, lv_textarea_get_cursor_pos(ta));
    feed(&fx, end_up, sizeof(end_up));
    step(&fx);

    /* 末尾再按右键不再移动。 */
    feed(&fx, right, sizeof(right));
    step(&fx);
    assert_eq(4, lv_textarea_get_cursor_pos(ta));
    feed(&fx, right_up, sizeof(right_up));
    step(&fx);

    /* Delete：删除光标处的字符（末尾无字符可删，光标前移也没有作用）。 */
    feed(&fx, del, sizeof(del));
    step(&fx);
    assert_str_eq("abcd", lv_textarea_get_text(ta));
    feed(&fx, del_up, sizeof(del_up));
    step(&fx);

    feed(&fx, home, sizeof(home));
    step(&fx);
    feed(&fx, home_up, sizeof(home_up));
    step(&fx);

    feed(&fx, del, sizeof(del));
    step(&fx);
    assert_str_eq("bcd", lv_textarea_get_text(ta));
    feed(&fx, del_up, sizeof(del_up));
    step(&fx);

    /* Backspace：删除光标前的字符，并且参与软件重复。 */
    lv_textarea_set_text(ta, "abcd");
    lv_textarea_set_cursor_pos(ta, 4);
    feed(&fx, backspace, sizeof(backspace));
    step(&fx);
    assert_str_eq("abc", lv_textarea_get_text(ta));

    gui_test_set_now(400);
    step(&fx);
    assert_str_eq("ab", lv_textarea_get_text(ta));

    gui_test_set_now(440);
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    fixture_teardown(&fx);
}

/* Esc：交给焦点控件的取消动作，不结束进程。 */
TEST_FUNC(test_esc_is_delivered_as_cancel_to_the_focused_widget)
{
    fixture_t fx;
    lv_obj_t *ta;
    button_probe_t probe;
    const uint8_t esc_down[] = { KEY_ESC_DOWN };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    lv_obj_add_event_cb(ta, probe_button_cb, LV_EVENT_CANCEL, &probe);
    lv_obj_add_event_cb(ta, probe_button_cb, LV_EVENT_KEY, &probe);

    assert_eq(0, gui_input_focus_add(fx.input, ta));
    assert_eq(0, gui_input_focus_set(fx.input, ta));

    feed(&fx, esc_down, sizeof(esc_down));
    step(&fx);

    /* 焦点控件收到 Esc 键与取消动作。 */
    assert_eq(1, probe.keys);
    assert_eq(1, probe.cancels);

    fixture_teardown(&fx);
}

/* 左键点击可聚焦控件：它成为焦点（LVGL 的 indev_click_focus 路径）。 */
TEST_FUNC(test_left_click_focuses_a_registered_widget)
{
    fixture_t fx;
    lv_obj_t *btn1, *btn2;
    mouse_event_t move;
    mouse_event_t press;
    mouse_event_t release;

    fixture_setup(&fx);
    btn1 = make_button(&fx, 20, 20);
    btn2 = make_button(&fx, 200, 120);
    assert_eq(0, gui_input_focus_add(fx.input, btn1));
    assert_eq(0, gui_input_focus_add(fx.input, btn2));
    assert_true(gui_input_focus_current(fx.input) == btn1);

    /* 光标初始在视口中心：移动到第二个按钮上再点击。 */
    memset(&move, 0, sizeof(move));
    memset(&press, 0, sizeof(press));
    memset(&release, 0, sizeof(release));
    move.dx = 70;
    move.dy = 35;
    press.buttons = 0x01u;
    gui_input_lvgl_pointer(fx.input, &move);
    gui_input_lvgl_pointer(fx.input, &press);
    step(&fx);

    /* 被点击的注册控件成为焦点。 */
    assert_true(gui_input_focus_current(fx.input) == btn2);

    gui_input_lvgl_pointer(fx.input, &release);
    step(&fx);
    assert_true(gui_input_focus_current(fx.input) == btn2);

    fixture_teardown(&fx);
}

/*
 * Ctrl/Alt 路径上的取消也必须用同一份重新就绪协议。
 *
 * read 被阻塞时旧的那份协议会把 wait_until_release 留在 LVGL 里，下一次
 * PRESSED 会被 LVGL 静默吞掉：按键就丢了。现在两条路径共用
 * gui_input_lvgl_cancel_key()，被阻塞时保留 pending 并重试。
 */
TEST_FUNC(test_blocked_read_on_the_ctrl_path_does_not_lose_the_next_key)
{
    fixture_t fx;
    lv_obj_t *ta;
    lv_indev_t *keypad;
    const uint8_t a_down[] = { KEY_A_DOWN };
    const uint8_t ctrl_down[] = { KEY_LCTRL_DOWN };
    const uint8_t ctrl_up[] = { KEY_LCTRL_UP };
    const uint8_t c_down[] = { KEY_C_DOWN };

    fixture_setup(&fx);
    ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));

    keypad = find_keypad();
    assert_not_null(keypad);

    /* 按住 'a'。 */
    feed(&fx, a_down, sizeof(a_down));
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));

    /* indev 被暂停：Ctrl 的取消协议读不到取消样本。 */
    lv_indev_enable(keypad, false);
    feed(&fx, ctrl_down, sizeof(ctrl_down));
    step(&fx);
    feed(&fx, ctrl_up, sizeof(ctrl_up));
    step(&fx);

    /* 读取恢复：待处理的取消协议被补做。 */
    lv_indev_enable(keypad, true);
    step(&fx);

    /* 管线恢复：下一个键正常送达（没有被 LVGL 的 wait_until_release 吞掉）。 */
    feed(&fx, c_down, sizeof(c_down));
    step(&fx);
    assert_str_eq("ac", lv_textarea_get_text(ta));

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

/* ═══════════════════════════════════════════════════════════════
 *  指针：光标、拖动与悬停滚轮（Task 8）
 *  ═══════════════════════════════════════════════════════════════ */

/* ── 指针注入 ──────────────────────────────────────────────────── */

/*
 * 合成鼠标事件进入**同一条** FIFO / 适配层路径。
 *
 * 私有注入桥接只在宿主构建（-DOS01_HOST_TEST）里存在，不进入公共 sysroot，
 * 也不出现在任何公共头文件里。
 */
static void feed_mouse(fixture_t *fx, const mouse_event_t *events, size_t count)
{
    assert_eq(0, gui_input_test_feed_mouse(fx->input, events, count));
}

static void feed_mouse_one(fixture_t *fx, uint8_t buttons, int16_t dx, int16_t dy,
                           int8_t wheel)
{
    mouse_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.buttons = buttons;
    ev.dx = dx;
    ev.dy = dy;
    ev.wheel = wheel;
    feed_mouse(fx, &ev, 1);
}

/*
 * 把指针移到控件中心（相对当前绝对位置的位移）。
 *
 * 必须先跑一次布局：控件刚建好时坐标还没有算出来（lv_obj_get_coords() 会
 * 返回无效区域）。
 */
static void move_pointer_to(fixture_t *fx, lv_obj_t *obj)
{
    lv_area_t area;

    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &area);
    feed_mouse_one(fx, 0,
                   (int16_t)((area.x1 + area.x2) / 2 - fx->input->lvgl.cursor_x),
                   (int16_t)((area.y1 + area.y2) / 2 - fx->input->lvgl.cursor_y),
                   0);
}

/* ── 原生指针日志 ──────────────────────────────────────────────── */

#define MAX_POINTER_LOGGED 32

typedef struct {
    gui_pointer_event_t events[MAX_POINTER_LOGGED];
    int count;
} pointer_log_t;

static void log_pointer(const gui_pointer_event_t *event, void *userdata)
{
    pointer_log_t *log = (pointer_log_t *)userdata;

    if (log->count < MAX_POINTER_LOGGED) {
        log->events[log->count] = *event;
    }
    log->count++;
}

/* ── 资源计数 ──────────────────────────────────────────────────── */

static int count_indevs(void)
{
    int n = 0;
    lv_indev_t *indev = lv_indev_get_next(NULL);

    while (indev) {
        n++;
        indev = lv_indev_get_next(indev);
    }
    return n;
}

static int count_timers(void)
{
    int n = 0;
    lv_timer_t *timer = lv_timer_get_next(NULL);

    while (timer) {
        n++;
        timer = lv_timer_get_next(timer);
    }
    return n;
}

/* ── 控件工厂 ──────────────────────────────────────────────────── */

static lv_obj_t *make_slider(fixture_t *fx, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *slider = lv_slider_create(fx->screen);

    assert_not_null(slider);
    lv_obj_set_pos(slider, x, y);
    lv_obj_set_size(slider, w, h);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    return slider;
}

/* 纯内容块：不可滚动，也没有内边距 / 边框，保证几何可预测。 */
static lv_obj_t *make_plain_box(lv_obj_t *parent, int32_t x, int32_t y,
                                int32_t w, int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);

    assert_not_null(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    return box;
}

/* 把指针移到绝对位置 (x, y)（用相对位移）。 */
static void move_pointer_to_abs(fixture_t *fx, int32_t x, int32_t y)
{
    feed_mouse_one(fx, 0,
                   (int16_t)(x - fx->input->lvgl.cursor_x),
                   (int16_t)(y - fx->input->lvgl.cursor_y),
                   0);
}

/*
 * 一个自带 100 px 纵向余量的滚动容器（内容比视口高 100 px）。
 *
 * 用例用它当"如果不被排除就会被滚动"的目标：只有真正可滚动的容器才能
 * 证明排除规则生效。
 */
static lv_obj_t *make_scrollable_box(lv_obj_t *parent, int32_t x, int32_t y,
                                     int32_t w, int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);

    assert_not_null(box);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    /* 内容比视口高 100 px → 向下余量 100 px。 */
    (void)make_plain_box(box, 0, 0, w, h + 100);
    return box;
}

/*
 * 嵌套滚动容器夹具。
 *
 *   outer 200×200，内容高 300 → 向下余量 100 px
 *   ├─ tall 100×300（outer 的滚动内容，命中它会滚 outer）
 *   └─ inner 60×60，内容高 92 → 向下余量 32 px（正好一个刻度）
 *
 * 几何刻意做成整数：断言可以直接写出期望的 scroll_y。
 */
typedef struct {
    lv_obj_t *outer;
    lv_obj_t *tall;
    lv_obj_t *inner;
    lv_obj_t *inner_content;
} scroll_fixture_t;

static void make_scroll_fixture(fixture_t *fx, scroll_fixture_t *sf)
{
    sf->outer = lv_obj_create(fx->screen);
    assert_not_null(sf->outer);
    lv_obj_set_style_pad_all(sf->outer, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sf->outer, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(sf->outer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(sf->outer, 0, 0);
    lv_obj_set_size(sf->outer, 200, 200);

    /* outer 的内容比视口高 100 px。 */
    sf->tall = make_plain_box(sf->outer, 0, 0, 100, 300);

    sf->inner = lv_obj_create(sf->outer);
    assert_not_null(sf->inner);
    lv_obj_set_style_pad_all(sf->inner, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sf->inner, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(sf->inner, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(sf->inner, 20, 20);
    lv_obj_set_size(sf->inner, 60, 60);

    sf->inner_content = make_plain_box(sf->inner, 0, 0, 40, 92);

    /* 布局必须先算出来：滚动余量查询用的是子控件的坐标。 */
    lv_obj_update_layout(fx->screen);
}

/* ═══════════════════════════════════════════════════════════════
 *  指针：click / 位移 / 拖动 / 按钮
 *  ═══════════════════════════════════════════════════════════════ */

TEST_FUNC(test_left_down_and_up_in_one_step_make_one_click)
{
    fixture_t fx;
    lv_obj_t *btn;
    button_probe_t probe;

    fixture_setup(&fx);
    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    btn = make_button(&fx, 20, 20);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_PRESSED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_RELEASED, &probe);

    /* 先把指针移到按钮上。 */
    move_pointer_to(&fx, btn);
    step(&fx);

    /* 同一步里投递按下与释放。 */
    feed_mouse_one(&fx, 0x01u, 0, 0, 0);
    feed_mouse_one(&fx, 0x00u, 0, 0, 0);
    step(&fx);

    assert_eq(1, probe.pressed);
    assert_eq(1, probe.released);
    assert_eq(1, probe.clicks);

    fixture_teardown(&fx);
}

TEST_FUNC(test_pointer_delta_extremes_clamp_without_wraparound)
{
    fixture_t fx;

    fixture_setup(&fx);

    /* int16 最大值：夹到右 / 下边界。 */
    feed_mouse_one(&fx, 0, INT16_MAX, INT16_MAX, 0);
    step(&fx);
    assert_eq((int32_t)SCREEN_W - 1, fx.input->lvgl.cursor_x);
    assert_eq((int32_t)SCREEN_H - 1, fx.input->lvgl.cursor_y);

    /* int16 最小值：夹回 0，不回绕成大正数。 */
    feed_mouse_one(&fx, 0, INT16_MIN, INT16_MIN, 0);
    step(&fx);
    assert_eq(0, fx.input->lvgl.cursor_x);
    assert_eq(0, fx.input->lvgl.cursor_y);

    /* 位置始终在视口内。 */
    assert_true(fx.input->lvgl.cursor_x >= 0);
    assert_true(fx.input->lvgl.cursor_x < (int32_t)SCREEN_W);
    assert_true(fx.input->lvgl.cursor_y >= 0);
    assert_true(fx.input->lvgl.cursor_y < (int32_t)SCREEN_H);

    fixture_teardown(&fx);
}

TEST_FUNC(test_slider_stops_dragging_after_the_release)
{
    fixture_t fx;
    lv_obj_t *slider;
    int32_t value_after_drag;

    fixture_setup(&fx);
    slider = make_slider(&fx, 20, 20, 200, 20);

    /* 移到滑块上并按下。 */
    move_pointer_to(&fx, slider);
    step(&fx);
    feed_mouse_one(&fx, 0x01u, 0, 0, 0);
    step(&fx);

    /* 拖动：位移必须超过 LVGL 的 scroll_limit(10) 才会真正开始拖动。 */
    feed_mouse_one(&fx, 0x01u, 60, 0, 0);
    step(&fx);
    value_after_drag = lv_slider_get_value(slider);
    assert_true(value_after_drag > 0);

    /* 释放。 */
    feed_mouse_one(&fx, 0x00u, 0, 0, 0);
    step(&fx);

    /* 释放后继续移动：滑块不再跟随。 */
    feed_mouse_one(&fx, 0x00u, 60, 0, 0);
    step(&fx);
    assert_eq(value_after_drag, lv_slider_get_value(slider));

    fixture_teardown(&fx);
}

TEST_FUNC(test_right_and_middle_buttons_only_report_native_changes)
{
    fixture_t fx;
    lv_obj_t *btn;
    button_probe_t probe;
    pointer_log_t log;

    fixture_setup(&fx);
    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    memset(&log, 0, sizeof(log));
    gui_input_set_pointer_handler(fx.input, log_pointer, &log);

    btn = make_button(&fx, 20, 20);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_CLICKED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_PRESSED, &probe);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_RELEASED, &probe);

    /* 移到按钮上（不按左键）。 */
    move_pointer_to(&fx, btn);
    step(&fx);

    /* 右键：只报告原生变化，不产生 click / press。 */
    feed_mouse_one(&fx, 0x02u, 0, 0, 0);
    step(&fx);
    assert_eq(0, probe.pressed);
    assert_eq(0, probe.released);
    assert_eq(0, probe.clicks);
    assert_true(log.count >= 1);
    assert_eq(0x02u, log.events[log.count - 1].buttons);
    assert_eq(0x02u, log.events[log.count - 1].changed_buttons);

    /* 中键：changed_buttons 只含中键（右键仍然按住）。 */
    feed_mouse_one(&fx, 0x06u, 0, 0, 0);
    step(&fx);
    assert_eq(0, probe.clicks);
    assert_true(log.count >= 2);
    assert_eq(0x06u, log.events[log.count - 1].buttons);
    assert_eq(0x04u, log.events[log.count - 1].changed_buttons);

    /* 全部松开：仍然没有 click。 */
    feed_mouse_one(&fx, 0x00u, 0, 0, 0);
    step(&fx);
    assert_eq(0, probe.pressed);
    assert_eq(0, probe.released);
    assert_eq(0, probe.clicks);

    fixture_teardown(&fx);
}

/* ═══════════════════════════════════════════════════════════════
 *  光标
 *  ═══════════════════════════════════════════════════════════════ */

TEST_FUNC(test_cursor_is_a_12x18_program_drawn_arrow_with_hotspot_at_zero)
{
    fixture_t fx;
    lv_obj_t *cursor;
    lv_area_t area;
    lv_point_t point;
    lv_obj_t *hit;

    fixture_setup(&fx);

    /* 光标属于 input，并挂在该 display 的 system layer 上。 */
    cursor = fx.input->lvgl.cursor;
    assert_not_null(cursor);
    if (!cursor) {
        fixture_teardown(&fx);
        return;
    }
    assert_true(lv_obj_get_parent(cursor) == lv_display_get_layer_sys(fx.disp));

    /*
     * 没有鼠标源时（keyboard-only 降级）光标是隐藏的：键盘与焦点仍然可用。
     */
    assert_true(lv_obj_has_flag(cursor, LV_OBJ_FLAG_HIDDEN));

    /*
     * 移动指针：这一步同时让布局跑起来（控件的尺寸由布局写进坐标），
     * 并且第一个鼠标事件证明存在鼠标源 → 光标显示出来。
     */
    feed_mouse_one(&fx, 0, 10, 20, 0);
    step_at(&fx, 50);
    assert_false(lv_obj_has_flag(cursor, LV_OBJ_FLAG_HIDDEN));

    /*
     * 规格固定 12×18。
     *
     * 这里断言**字面量** 12 / 18，而不是 GUI_INPUT_CURSOR_WIDTH /
     * GUI_INPUT_CURSOR_HEIGHT：后者与实现用的是同一个宏，断言会退化成
     * "实现和自己比"，把光标改成 16 px 或 40×40 都不会被发现。
     */
    assert_eq(12, lv_obj_get_width(cursor));
    assert_eq(18, lv_obj_get_height(cursor));

    /* 热点 (0,0)：对象左上角就是指针位置。 */
    lv_obj_get_coords(cursor, &area);
    assert_eq(fx.input->lvgl.cursor_x, area.x1);
    assert_eq(fx.input->lvgl.cursor_y, area.y1);

    /* 命中透明：光标不拦截 hit-test。 */
    assert_false(lv_obj_has_flag(cursor, LV_OBJ_FLAG_CLICKABLE));
    point.x = fx.input->lvgl.cursor_x;
    point.y = fx.input->lvgl.cursor_y;
    hit = lv_indev_search_obj(lv_display_get_layer_sys(fx.disp), &point);
    assert_true(hit != cursor);

    /* 程序绘制的箭头真的画出来了：热点处是光标颜色。 */
    assert_eq(GUI_INPUT_CURSOR_COLOR,
              gui_platform_get_pixel(fx.platform, (uint32_t)area.x1, (uint32_t)area.y1));

    fixture_teardown(&fx);
}

TEST_FUNC(test_stationary_pointer_still_advances_the_long_press_timer)
{
    fixture_t fx;
    lv_obj_t *btn;
    button_probe_t probe;

    fixture_setup(&fx);
    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    btn = make_button(&fx, 20, 20);
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_LONG_PRESSED, &probe);

    /*
     * pointer indev 必须是 TIMER 模式（brief："pointer 保留 TIMER 模式"）。
     *
     * 单靠"长按最终触发"钉不住这一点：LVGL 自己在按下期间会恢复被暂停的
     * 读定时器（thirdpart/lvgl/src/indev/lv_indev.c 的 indev_proc_press()），
     * 所以 EVENT 模式下长按同样会触发。这里显式断言模式本身。
     */
    assert_not_null(fx.input->lvgl.pointer);
    assert_eq(LV_INDEV_MODE_TIMER, lv_indev_get_mode(fx.input->lvgl.pointer));

    /* 移到按钮并按下：之后指针不再移动。 */
    move_pointer_to(&fx, btn);
    step(&fx);
    feed_mouse_one(&fx, 0x01u, 0, 0, 0);
    step_at(&fx, 0);
    assert_eq(0, probe.long_presses);

    /* 指针静止，但定时器继续推进：长按仍然触发。 */
    step_at(&fx, 500);
    assert_true(probe.long_presses >= 1);

    fixture_teardown(&fx);
}

/* ═══════════════════════════════════════════════════════════════
 *  悬停滚轮
 *  ═══════════════════════════════════════════════════════════════ */

TEST_FUNC(test_hover_wheel_scrolls_the_hovered_container_by_one_detent)
{
    fixture_t fx;
    scroll_fixture_t sf;

    fixture_setup(&fx);
    make_scroll_fixture(&fx, &sf);

    /* 夹具几何：inner 正好 32 px 余量，outer 有 100 px。 */
    assert_eq(32, lv_obj_get_scroll_bottom(sf.inner));
    assert_eq(100, lv_obj_get_scroll_bottom(sf.outer));

    /* 悬停到 inner 上（不点击）。 */
    move_pointer_to(&fx, sf.inner);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(sf.inner));
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    /* 一个刻度：内容向下滚 32 px。 */
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(32, lv_obj_get_scroll_y(sf.inner));
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    /* 反向刻度：滚回来。 */
    feed_mouse_one(&fx, 0, 0, 0, -1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(sf.inner));
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    fixture_teardown(&fx);
}

TEST_FUNC(test_hover_wheel_negative_detent_scrolls_back_within_range)
{
    fixture_t fx;
    scroll_fixture_t sf;

    fixture_setup(&fx);
    make_scroll_fixture(&fx, &sf);

    /* outer 的向下余量是 100 px（> 一个刻度），这样负向才有夹边可言。 */
    assert_eq(100, lv_obj_get_scroll_bottom(sf.outer));

    /*
     * 悬停到 outer 的滚动内容上（tall 在 inner 之外，命中 tall 的最近可
     * 滚动祖先就是 outer，因此滚的是 outer 而不是 inner）。
     */
    move_pointer_to(&fx, sf.tall);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    /* 向下两刻度：64 px。 */
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(32, lv_obj_get_scroll_y(sf.outer));
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(64, lv_obj_get_scroll_y(sf.outer));

    /*
     * 负向刻度必须只退**一个**刻度。
     *
     * 请求 32 px、余量 64 px → 移动 32 px，剩 32 px。夹紧写反的实现会在
     * 这里把整个余量（64 px）一次退光，scroll_y 直接变成 0。
     */
    feed_mouse_one(&fx, 0, 0, 0, -1);
    step(&fx);
    assert_eq(32, lv_obj_get_scroll_y(sf.outer));

    /* 再退一个刻度：正好回到起点。 */
    feed_mouse_one(&fx, 0, 0, 0, -1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    /*
     * 已经在上边界：负向刻度必须**完全不动**。
     *
     * 夹紧写反的实现在这里会把内容滚到边界之上（scroll_y 变成 -32），那是
     * 滚动模型不承认的状态。
     */
    feed_mouse_one(&fx, 0, 0, 0, -1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    fixture_teardown(&fx);
}

TEST_FUNC(test_hover_wheel_ignores_disabled_and_hidden_targets)
{
    fixture_t fx;
    scroll_fixture_t sf;
    lv_obj_t *disabled_box;
    lv_obj_t *disabled_ancestor;
    lv_obj_t *disabled_ancestor_child;
    lv_obj_t *hidden_ancestor;

    fixture_setup(&fx);
    make_scroll_fixture(&fx, &sf);

    /*
     * 三个目标都放在屏幕右侧的空白区（x >= 205），避免指针落在 outer 上：
     * 这样"不动"只能来自排除规则，而不是碰巧没有可滚动的祖先。
     *
     * 目标一：滚动容器**自身**被禁用。
     *
     * LVGL 的 hit-test 不看 DISABLED（只看隐藏），所以禁用对象照样会被命中：
     * 排除禁用对象完全靠适配层的判定。
     */
    disabled_box = make_scrollable_box(fx.screen, 205, 5, 60, 60);
    lv_obj_add_state(disabled_box, LV_STATE_DISABLED);

    /* 目标二：滚动容器在一个**被禁用的祖先**下面。 */
    disabled_ancestor = lv_obj_create(fx.screen);
    assert_not_null(disabled_ancestor);
    lv_obj_set_style_pad_all(disabled_ancestor, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(disabled_ancestor, 0, LV_PART_MAIN);
    lv_obj_set_pos(disabled_ancestor, 205, 70);
    lv_obj_set_size(disabled_ancestor, 100, 70);
    disabled_ancestor_child = make_scrollable_box(disabled_ancestor, 0, 0, 80, 70);
    lv_obj_add_state(disabled_ancestor, LV_STATE_DISABLED);

    /* 目标三：滚动容器在一个**被隐藏的祖先**下面。 */
    hidden_ancestor = lv_obj_create(fx.screen);
    assert_not_null(hidden_ancestor);
    lv_obj_set_style_pad_all(hidden_ancestor, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(hidden_ancestor, 0, LV_PART_MAIN);
    lv_obj_set_pos(hidden_ancestor, 205, 145);
    lv_obj_set_size(hidden_ancestor, 100, 50);
    (void)make_scrollable_box(hidden_ancestor, 0, 0, 80, 50);
    lv_obj_add_flag(hidden_ancestor, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(fx.screen);

    /* 三个目标都不可用：适配层的"可用"判定必须覆盖它们（含祖先链）。 */
    assert_false(gui_input_focus_object_usable(disabled_box));
    assert_false(gui_input_focus_object_usable(disabled_ancestor));
    assert_false(gui_input_focus_object_usable(disabled_ancestor_child));
    assert_false(gui_input_focus_object_usable(hidden_ancestor));

    /* 悬停到被禁用的容器上：滚轮不得滚动它，也不得滚到它的祖先。 */
    move_pointer_to_abs(&fx, 235, 35);
    step(&fx);
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(disabled_box));
    assert_eq(0, lv_obj_get_scroll_y(fx.screen));

    /* 悬停到被禁用祖先下的容器上：同样不动。 */
    move_pointer_to_abs(&fx, 255, 105);
    step(&fx);
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(disabled_ancestor));
    assert_eq(0, lv_obj_get_scroll_y(disabled_ancestor_child));
    assert_eq(0, lv_obj_get_scroll_y(fx.screen));

    /* 悬停到被隐藏祖先下的容器上：隐藏的子树根本不参与命中，同样不动。 */
    move_pointer_to_abs(&fx, 255, 170);
    step(&fx);
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(hidden_ancestor));
    assert_eq(0, lv_obj_get_scroll_y(fx.screen));

    /* 对照：可用的容器仍然照常滚动（证明上面的"不动"来自排除规则）。 */
    move_pointer_to(&fx, sf.tall);
    step(&fx);
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(32, lv_obj_get_scroll_y(sf.outer));

    fixture_teardown(&fx);
}

TEST_FUNC(test_hover_wheel_propagates_the_remainder_to_the_parent)
{
    fixture_t fx;
    scroll_fixture_t sf;

    fixture_setup(&fx);
    make_scroll_fixture(&fx, &sf);

    move_pointer_to(&fx, sf.inner);
    step(&fx);

    /* 第一刻度：inner 自己滚到底（它的余量正好 32 px）。 */
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(32, lv_obj_get_scroll_y(sf.inner));
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    /* 第二刻度：inner 已到边界，剩余的 32 px 交给父滚动容器。 */
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(32, lv_obj_get_scroll_y(sf.inner));
    assert_eq(32, lv_obj_get_scroll_y(sf.outer));

    fixture_teardown(&fx);
}

TEST_FUNC(test_hover_wheel_without_a_target_does_nothing)
{
    fixture_t fx;
    scroll_fixture_t sf;

    fixture_setup(&fx);
    make_scroll_fixture(&fx, &sf);

    /* 悬停到 outer 之外的空白：命中的只有屏幕，屏幕没有纵向余量。 */
    feed_mouse_one(&fx, 0, 140, 80, 0);
    step(&fx);
    assert_true(fx.input->lvgl.cursor_x >= 200);

    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(sf.inner));
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));
    assert_eq(0, lv_obj_get_scroll_y(fx.screen));

    fixture_teardown(&fx);
}

TEST_FUNC(test_hover_wheel_does_not_change_a_slider_value)
{
    fixture_t fx;
    lv_obj_t *slider;
    int32_t value_before;

    fixture_setup(&fx);
    slider = make_slider(&fx, 20, 20, 200, 20);
    value_before = lv_slider_get_value(slider);

    move_pointer_to(&fx, slider);
    step(&fx);

    /* 悬停滚轮不得变成"修改 slider 值"。 */
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(value_before, lv_slider_get_value(slider));
    assert_eq(0, lv_obj_get_scroll_y(fx.screen));

    fixture_teardown(&fx);
}

TEST_FUNC(test_hover_wheel_is_ignored_while_dragging)
{
    fixture_t fx;
    scroll_fixture_t sf;

    fixture_setup(&fx);
    make_scroll_fixture(&fx, &sf);

    move_pointer_to(&fx, sf.inner);
    step(&fx);

    /* 左键按住：进入拖动状态。 */
    feed_mouse_one(&fx, 0x01u, 0, 0, 0);
    step(&fx);

    /* 拖动中滚轮被忽略，避免手势冲突。 */
    feed_mouse_one(&fx, 0x01u, 0, 0, 1);
    step(&fx);
    assert_eq(0, lv_obj_get_scroll_y(sf.inner));
    assert_eq(0, lv_obj_get_scroll_y(sf.outer));

    /* 释放后滚轮重新生效。 */
    feed_mouse_one(&fx, 0x00u, 0, 0, 0);
    step(&fx);
    feed_mouse_one(&fx, 0, 0, 0, 1);
    step(&fx);
    assert_eq(32, lv_obj_get_scroll_y(sf.inner));

    fixture_teardown(&fx);
}

TEST_FUNC(test_moving_the_pointer_does_not_take_the_keyboard_focus)
{
    fixture_t fx;
    lv_obj_t *btn;
    lv_obj_t *other;

    fixture_setup(&fx);
    btn = make_button(&fx, 20, 20);
    other = make_button(&fx, 200, 120);

    /*
     * 被悬停的控件**也**要登记进焦点组。
     *
     * 否则这个用例看不见焦点被抢：gui_input_focus_set() 拒绝未登记的对象，
     * 一个"移动指针就抢焦点"的实现不会改动焦点记录，用例照样通过。登记
     * 之后，焦点一旦被抢到 other 上就能被观察到。
     */
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_add(fx.input, other));
    assert_true(gui_input_focus_current(fx.input) == btn);

    /* 悬停到另一个按钮上（不点击）：键盘焦点不变。 */
    move_pointer_to(&fx, other);
    step(&fx);
    step_at(&fx, 50);
    assert_true(gui_input_focus_current(fx.input) == btn);

    /* 悬停本身不得把焦点移到 other 上。 */
    assert_true(gui_input_focus_current(fx.input) != other);

    fixture_teardown(&fx);
}

TEST_FUNC(test_mouse_injection_bridge_respects_the_dispatch_budget)
{
    fixture_t fx;
    mouse_event_t events[65];
    size_t i;

    fixture_setup(&fx);
    for (i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        memset(&events[i], 0, sizeof(events[i]));
    }

    /*
     * 一次 dispatch 最多 64 个鼠标事件（与产品路径 gui_input_read_mouse()
     * 同一预算）：夹具不能注入产品根本送不出来的批量。
     *
     * 这里用**字面量** 64 / 65，而不是 GUI_INPUT_MOUSE_READ_BUDGET，避免
     * 断言退化成"实现和自己比"。
     */
    errno = 0;
    assert_eq(-1, gui_input_test_feed_mouse(fx.input, events, 65));
    assert_eq(EINVAL, errno);

    /* 预算之内可以正常注入。 */
    assert_eq(0, gui_input_test_feed_mouse(fx.input, events, 64));

    fixture_teardown(&fx);
}

TEST_FUNC(test_destroy_leaves_no_pointer_resources_behind)
{
    fixture_t fx;
    lv_obj_t *btn;
    int timers_before;

    fixture_setup(&fx);
    btn = make_button(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_not_null(fx.input->lvgl.cursor);
    assert_eq(2, count_indevs());
    timers_before = count_timers();

    gui_input_destroy(fx.input);
    fx.input = NULL;

    /* indev 与它们的读定时器都被释放。 */
    assert_eq(0, count_indevs());
    assert_eq(timers_before - 2, count_timers());

    /* 光标对象被删除：system layer 上不再有子对象。 */
    assert_eq(0, lv_obj_get_child_count(lv_display_get_layer_sys(fx.disp)));

    /* 私有焦点组被删除：登记的控件不再属于任何组。 */
    assert_null(lv_obj_get_group(btn));

    gui_platform_destroy(fx.platform);
    fx.platform = NULL;
}


/* The non-extended keypad decimal is not the dedicated E0 Delete key. */
TEST_FUNC(test_keypad_decimal_does_not_delete_text)
{
    fixture_t fx;
    fixture_setup(&fx);
    lv_obj_t *ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    lv_textarea_set_text(ta, "abc");
    lv_textarea_set_cursor_pos(ta, 0);
    const uint8_t decimal[] = { 0x53, 0xD3 };
    feed(&fx, decimal, sizeof(decimal));
    step(&fx);
    assert_str_eq("abc", lv_textarea_get_text(ta));
    fixture_teardown(&fx);
}

/* Invalidation between steps must precede the queued Enter release. */
TEST_FUNC(test_invalid_focus_is_cancelled_before_queued_release)
{
    for (unsigned variant = 0; variant < 4; variant++) {
        fixture_t fx;
        fixture_setup(&fx);
        lv_obj_t *btn = make_button(&fx, 20, 20);
        lv_obj_t *ta = make_textarea(&fx, 20, 90);
        lv_obj_t *target = btn;
        if (variant >= 2) {
            target = lv_obj_create(fx.screen);
            lv_obj_set_parent(btn, target);
        }
        assert_eq(0, gui_input_focus_add(fx.input, btn));
        assert_eq(0, gui_input_focus_add(fx.input, ta));
        button_probe_t probe = { .input = fx.input };
        lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_ALL, &probe);
        const uint8_t down[] = { KEY_ENTER_DOWN };
        const uint8_t rest[] = { KEY_ENTER_UP, KEY_A_DOWN, KEY_A_UP };
        feed(&fx, down, sizeof(down));
        step(&fx);
        if (variant & 1) lv_obj_add_state(target, LV_STATE_DISABLED);
        else lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
        feed(&fx, rest, sizeof(rest));
        step(&fx);
        assert_eq(0, probe.clicks);
        assert_eq(0, probe.released);
        assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));
        assert_str_eq("a", lv_textarea_get_text(ta));
        fixture_teardown(&fx);
    }
}

static void hide_parent_on_press(lv_event_t *event)
{
    lv_obj_t *parent = lv_event_get_user_data(event);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_HIDDEN);
}

TEST_FUNC(test_callback_invalidates_focus_before_same_batch_release)
{
    fixture_t fx;
    fixture_setup(&fx);
    lv_obj_t *parent = lv_obj_create(fx.screen);
    lv_obj_t *btn = make_button(&fx, 20, 20);
    lv_obj_set_parent(btn, parent);
    lv_obj_t *ta = make_textarea(&fx, 20, 90);
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    button_probe_t probe = { .input = fx.input };
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_ALL, &probe);
    lv_obj_add_event_cb(btn, hide_parent_on_press, LV_EVENT_PRESSED, parent);
    const uint8_t bytes[] = { KEY_ENTER_DOWN, KEY_ENTER_UP, KEY_A_DOWN, KEY_A_UP };
    feed(&fx, bytes, sizeof(bytes));
    step(&fx);
    assert_eq(0, probe.clicks);
    assert_eq(0, probe.released);
    assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));
    assert_str_eq("a", lv_textarea_get_text(ta));
    fixture_teardown(&fx);
}

TEST_FUNC(test_repeat_deadline_bounds_the_platform_wait)
{
    fixture_t fx;
    fixture_setup(&fx);
    lv_obj_t *ta = make_textarea(&fx, 20, 20);
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    const uint8_t down[] = { KEY_A_DOWN };
    feed(&fx, down, sizeof(down));
    step(&fx);
    gui_test_set_now(399);
    uint32_t delay;
    /* Drain LVGL's immediately due refresh before checking the steady wait. */
    for (unsigned i = 0; i < 3; i++) {
        assert_eq(GUI_STATUS_OK, gui_platform_step(fx.platform, &delay));
    }
    assert_eq(1, delay);
    step_at(&fx, 400);
    assert_str_eq("aa", lv_textarea_get_text(ta));
    step_at(&fx, 439);
    assert_eq(GUI_STATUS_OK, gui_platform_step(fx.platform, &delay));
    assert_eq(1, delay);
    step_at(&fx, 440);
    assert_str_eq("aaa", lv_textarea_get_text(ta));
    fixture_teardown(&fx);
}

TEST_FUNC(test_unrelated_deletion_preserves_repeat_and_enter_click)
{
    for (unsigned enter = 0; enter < 2; enter++) {
        fixture_t fx;
        fixture_setup(&fx);
        lv_obj_t *focus = enter ? make_button(&fx, 20, 20) : make_textarea(&fx, 20, 20);
        lv_obj_t *other = make_button(&fx, 200, 120);
        assert_eq(0, gui_input_focus_add(fx.input, focus));
        assert_eq(0, gui_input_focus_add(fx.input, other));
        button_probe_t probe = { .input = fx.input };
        lv_obj_add_event_cb(focus, probe_button_cb, LV_EVENT_CLICKED, &probe);
        uint8_t down[] = { enter ? KEY_ENTER_DOWN : KEY_A_DOWN };
        uint8_t up[] = { enter ? KEY_ENTER_UP : KEY_A_UP };
        feed(&fx, down, sizeof(down));
        step(&fx);
        lv_obj_delete(other);
        if (!enter) {
            step_at(&fx, 400);
            assert_str_eq("aa", lv_textarea_get_text(focus));
        }
        feed(&fx, up, sizeof(up));
        step(&fx);
        if (enter) assert_eq(1, probe.clicks);
        fixture_teardown(&fx);
    }
}


static bool disable_parent_on_enter_up(const gui_key_event_t *event, void *userdata)
{
    if (event->physical_key == 0x1C && event->action == GUI_KEY_UP) {
        lv_obj_add_state((lv_obj_t *)userdata, LV_STATE_DISABLED);
    }
    return false;
}

TEST_FUNC(test_native_callback_invalidates_focus_before_release)
{
    fixture_t fx;
    fixture_setup(&fx);
    lv_obj_t *parent = lv_obj_create(fx.screen);
    lv_obj_t *btn = make_button(&fx, 20, 20);
    lv_obj_set_parent(btn, parent);
    lv_obj_t *ta = make_textarea(&fx, 20, 90);
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    button_probe_t probe = { .input = fx.input };
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_ALL, &probe);
    gui_input_set_key_handler(fx.input, disable_parent_on_enter_up, parent);
    const uint8_t bytes[] = { KEY_ENTER_DOWN, KEY_ENTER_UP, KEY_A_DOWN, KEY_A_UP };
    feed(&fx, bytes, sizeof(bytes));
    step(&fx);
    assert_eq(0, probe.clicks);
    assert_eq(0, probe.released);
    assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));
    assert_str_eq("a", lv_textarea_get_text(ta));
    fixture_teardown(&fx);
}

TEST_FUNC(test_blocked_focus_cancellation_retains_the_next_queued_key)
{
    fixture_t fx;
    fixture_setup(&fx);
    lv_obj_t *btn = make_button(&fx, 20, 20);
    lv_obj_t *ta = make_textarea(&fx, 20, 90);
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    button_probe_t probe = { .input = fx.input };
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_ALL, &probe);
    const uint8_t down[] = { KEY_ENTER_DOWN };
    const uint8_t rest[] = { KEY_ENTER_UP, KEY_A_DOWN, KEY_A_UP };
    feed(&fx, down, sizeof(down));
    step(&fx);
    lv_indev_enable(find_keypad(), false);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    feed(&fx, rest, sizeof(rest));
    step(&fx);
    assert_str_eq("", lv_textarea_get_text(ta));
    assert_eq(0, probe.clicks);
    lv_indev_enable(find_keypad(), true);
    step(&fx);
    assert_str_eq("a", lv_textarea_get_text(ta));
    assert_eq(0, probe.clicks);
    assert_eq(0, probe.released);
    assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));
    fixture_teardown(&fx);
}


typedef struct {
    gui_input_t *input;
    lv_obj_t *target;
    int releases;
} blocked_callback_t;

static bool block_and_move_on_release(const gui_key_event_t *event, void *userdata)
{
    blocked_callback_t *ctx = userdata;
    if (event->physical_key == 0x1C && event->action == GUI_KEY_UP) {
        ctx->releases++;
        lv_indev_enable(find_keypad(), false);
        assert_eq(0, gui_input_focus_set(ctx->input, ctx->target));
    }
    return false;
}

TEST_FUNC(test_blocked_native_callback_resumes_without_replaying_the_handler)
{
    fixture_t fx;
    fixture_setup(&fx);
    lv_obj_t *btn = make_button(&fx, 20, 20);
    lv_obj_t *ta = make_textarea(&fx, 20, 90);
    assert_eq(0, gui_input_focus_add(fx.input, btn));
    assert_eq(0, gui_input_focus_add(fx.input, ta));
    button_probe_t probe = { .input = fx.input };
    lv_obj_add_event_cb(btn, probe_button_cb, LV_EVENT_ALL, &probe);
    blocked_callback_t ctx = { .input = fx.input, .target = ta };
    gui_input_set_key_handler(fx.input, block_and_move_on_release, &ctx);
    const uint8_t bytes[] = { KEY_ENTER_DOWN, KEY_ENTER_UP, KEY_A_DOWN, KEY_A_UP };
    feed(&fx, bytes, sizeof(bytes));
    step(&fx);
    uint32_t delay = 0;
    for (unsigned i = 0; i < 3; i++) {
        assert_eq(GUI_STATUS_OK, gui_platform_step(fx.platform, &delay));
    }
    assert_true(delay > 0); /* blocked cancellation must not busy-poll the FIFO */
    assert_eq(1, ctx.releases);
    assert_str_eq("", lv_textarea_get_text(ta));
    lv_indev_enable(find_keypad(), true);
    step(&fx);
    assert_eq(1, ctx.releases);
    assert_str_eq("a", lv_textarea_get_text(ta));
    assert_eq(0, probe.clicks);
    assert_eq(0, probe.released);
    assert_false(lv_obj_has_state(btn, LV_STATE_PRESSED));
    fixture_teardown(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_blocked_native_callback_resumes_without_replaying_the_handler),
    TEST_ENTRY(test_native_callback_invalidates_focus_before_release),
    TEST_ENTRY(test_blocked_focus_cancellation_retains_the_next_queued_key),
    TEST_ENTRY(test_keypad_decimal_does_not_delete_text),
    TEST_ENTRY(test_invalid_focus_is_cancelled_before_queued_release),
    TEST_ENTRY(test_callback_invalidates_focus_before_same_batch_release),
    TEST_ENTRY(test_repeat_deadline_bounds_the_platform_wait),
    TEST_ENTRY(test_unrelated_deletion_preserves_repeat_and_enter_click),

    TEST_ENTRY(test_focus_add_rejects_foreign_display_and_duplicates),
    TEST_ENTRY(test_tab_cycles_in_registration_order),
    TEST_ENTRY(test_tab_skips_hidden_and_disabled_ancestors_and_restores_order),
    TEST_ENTRY(test_hidden_focus_moves_to_the_next_candidate_not_the_first),
    TEST_ENTRY(test_disabled_focus_moves_to_the_next_candidate_not_the_first),
    TEST_ENTRY(test_empty_focus_group_survives_tab),
    TEST_ENTRY(test_focus_outline_is_pixel_observable),
    TEST_ENTRY(test_clicking_blank_space_keeps_the_keyboard_focus),
    TEST_ENTRY(test_enter_press_with_focus_change_has_no_ghost_click),
    TEST_ENTRY(test_enter_press_with_deletion_inside_callback_has_no_ghost_click),
    TEST_ENTRY(test_all_hidden_widgets_do_not_drop_the_next_key),
    TEST_ENTRY(test_hiding_the_focused_widget_cancels_the_held_key),
    TEST_ENTRY(test_hiding_the_only_focused_widget_cancels_the_held_key),
    TEST_ENTRY(test_disabling_the_focused_widget_cancels_the_held_key),
    TEST_ENTRY(test_unrelated_focus_changes_do_not_cancel_the_held_key),
    TEST_ENTRY(test_blocked_read_does_not_leak_the_old_enter),
    TEST_ENTRY(test_blocked_read_on_the_ctrl_path_does_not_lose_the_next_key),
    TEST_ENTRY(test_repeat_is_400ms_then_40ms),
    TEST_ENTRY(test_a_consumed_down_is_never_repeated),
    TEST_ENTRY(test_a_key_without_an_lvgl_meaning_reaches_only_the_app),
    TEST_ENTRY(test_repeat_does_not_burst_after_a_stall),
    TEST_ENTRY(test_ctrl_down_cancels_and_release_does_not_restore),
    TEST_ENTRY(test_shift_change_keeps_the_cached_char_of_a_repeat),
    TEST_ENTRY(test_space_repeats_in_a_textarea_but_activates_a_button_once),
    TEST_ENTRY(test_tab_enter_and_esc_do_not_repeat),
    TEST_ENTRY(test_editing_keys_repeat_and_route_to_the_textarea),
    TEST_ENTRY(test_esc_is_delivered_as_cancel_to_the_focused_widget),
    TEST_ENTRY(test_left_click_focuses_a_registered_widget),
    TEST_ENTRY(test_focus_change_mid_repeat_stops_the_repeat_without_dropping_keys),
    TEST_ENTRY(test_a_key_cancelled_by_its_own_callback_never_repeats),
    TEST_ENTRY(test_a_key_whose_focus_is_deleted_in_its_callback_never_repeats),
    TEST_ENTRY(test_consumed_shortcut_is_not_inserted),
    TEST_ENTRY(test_consumed_release_still_releases_the_lvgl_key),
    TEST_ENTRY(test_left_down_and_up_in_one_step_make_one_click),
    TEST_ENTRY(test_pointer_delta_extremes_clamp_without_wraparound),
    TEST_ENTRY(test_slider_stops_dragging_after_the_release),
    TEST_ENTRY(test_right_and_middle_buttons_only_report_native_changes),
    TEST_ENTRY(test_cursor_is_a_12x18_program_drawn_arrow_with_hotspot_at_zero),
    TEST_ENTRY(test_stationary_pointer_still_advances_the_long_press_timer),
    TEST_ENTRY(test_hover_wheel_scrolls_the_hovered_container_by_one_detent),
    TEST_ENTRY(test_hover_wheel_negative_detent_scrolls_back_within_range),
    TEST_ENTRY(test_hover_wheel_ignores_disabled_and_hidden_targets),
    TEST_ENTRY(test_hover_wheel_propagates_the_remainder_to_the_parent),
    TEST_ENTRY(test_hover_wheel_without_a_target_does_nothing),
    TEST_ENTRY(test_hover_wheel_does_not_change_a_slider_value),
    TEST_ENTRY(test_hover_wheel_is_ignored_while_dragging),
    TEST_ENTRY(test_moving_the_pointer_does_not_take_the_keyboard_focus),
    TEST_ENTRY(test_mouse_injection_bridge_respects_the_dispatch_budget),
    TEST_ENTRY(test_destroy_leaves_no_pointer_resources_behind),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
