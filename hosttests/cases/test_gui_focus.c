/*
 * test_gui_focus.c — 唯一焦点引擎的公共契约（GUI-2b Task 7/8/9）
 *
 * 本文件是**公共契约**测试：它只包含 gui 目录下的公共头、标准库、测试
 * 框架和宿主私有 fixture 头。断言全部通过公共 API 与值句柄表达；后端的
 * 真实状态只通过宿主私有 fixture 的观察桥读取。
 *
 * 三条主线：
 *   1. 登记模式：原生登记与诊断(raw)登记互相排斥；模式由第一次**成功**的
 *      登记锁定，失败不锁定，全注销不解锁；容量 128。
 *   2. 选择历史：MANUAL / AUTO_FIRST、显式清空、隐藏禁用后的修复、取消
 *      协议；任何选择变化只经同一条提交路径。
 *      主题 primary 焦点环跟着焦点走。
 */

#include "test_framework.h"

#include <gui/gui.h>
#include <gui/gui_event.h>
#include <gui/gui_focus.h>
#include <gui/gui_layout.h>
#include <gui/gui_platform.h>
#include <gui/gui_theme.h>
#include <gui/gui_widget.h>

#include "gui_native_fixture.h"
#include "gui_test_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The platform TU's host-only header tamper switch.  Every GUI host
 * fixture defines it; this fixture never flips it. */
bool g_gui_test_tamper_header = false;

/* ── 扫描码（Set 1） ────────────────────────────────────────────── */

#define KEY_A_DOWN      0x1Eu
#define KEY_A_UP        0x9Eu
#define KEY_ENTER_DOWN  0x1Cu
#define KEY_ENTER_UP    0x9Cu
#define KEY_TAB_DOWN    0x0Fu
#define KEY_TAB_UP      0x8Fu
#define KEY_LSHIFT_DOWN 0x2Au
#define KEY_LSHIFT_UP   0xAAu
#define KEY_SPACE_DOWN  0x39u
#define KEY_SPACE_UP    0xB9u

/* ── 控件构造 ────────────────────────────────────────────────────── */

static gui_widget_t make_root(gui_native_fixture_t *fx)
{
    gui_widget_t root;

    assert_eq(0, gui_widget_create(fx->platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    return root;
}

static gui_widget_t make_button(gui_native_fixture_t *fx, gui_widget_t parent, int32_t x,
                                int32_t y)
{
    gui_widget_t btn;
    gui_geometry_t g;

    assert_eq(0, gui_widget_create(fx->platform, parent, GUI_WIDGET_BUTTON, &btn));
    g.x = x;
    g.y = y;
    g.width = GUI_SIZE_PX(90);
    g.height = GUI_SIZE_PX(40);
    assert_eq(0, gui_widget_set_geometry(fx->platform, btn, &g));
    return btn;
}

static gui_widget_t make_label(gui_native_fixture_t *fx, gui_widget_t parent, int32_t x,
                               int32_t y)
{
    gui_widget_t label;
    gui_geometry_t g;

    assert_eq(0, gui_widget_create(fx->platform, parent, GUI_WIDGET_LABEL, &label));
    g.x = x;
    g.y = y;
    g.width = GUI_SIZE_PX(40);
    g.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx->platform, label, &g));
    return label;
}

/* ── 键盘与指针驱动 ──────────────────────────────────────────────── */

static void feed_keys(gui_native_fixture_t *fx, const uint8_t *bytes, size_t len)
{
    assert_eq(0, gui_native_fixture_keys(fx, bytes, len));
}

static void step(gui_native_fixture_t *fx, uint64_t advance_ms)
{
    assert_eq(0, gui_native_fixture_step(fx, advance_ms));
}

/* 按一次键（按下 + 释放 + 一步）。 */
static void press_key(gui_native_fixture_t *fx, uint8_t down, uint8_t up)
{
    const uint8_t bytes[2] = { down, up };

    feed_keys(fx, bytes, sizeof(bytes));
    step(fx, 20);
}

/* ── 业务回调探针 ────────────────────────────────────────────────── */

typedef struct {
    gui_input_t *input;
    gui_platform_t *platform;
    gui_widget_t other;
    int calls;

    int reg_rc;
    int reg_errno;
    int unreg_rc;
    int unreg_errno;
    int set_rc;
    int set_errno;
    int move_rc;
    int move_errno;
    int init_rc;
    int init_errno;

    int get_rc;
    int get_errno;
    gui_widget_t got;

    int destroy_rc;
    int destroy_errno;
} guard_probe_t;

static void guard_handler(const gui_event_t *event, void *userdata)
{
    guard_probe_t *probe = (guard_probe_t *)userdata;

    probe->calls++;

    /* 只读查询在业务回调里**允许**：返回最后已提交的选择。 */
    errno = 0;
    probe->get_rc = gui_focus_get(probe->input, &probe->got);
    probe->get_errno = errno;

    /* 变更一律 EBUSY。 */
    errno = 0;
    probe->reg_rc = gui_focus_register(probe->input, probe->other, GUI_FOCUS_CLICK);
    probe->reg_errno = errno;

    errno = 0;
    probe->unreg_rc = gui_focus_unregister(probe->input, probe->other);
    probe->unreg_errno = errno;

    errno = 0;
    probe->set_rc = gui_focus_set(probe->input, probe->other);
    probe->set_errno = errno;

    errno = 0;
    probe->move_rc = gui_focus_move(probe->input, GUI_FOCUS_FORWARD);
    probe->move_errno = errno;

    errno = 0;
    probe->init_rc = gui_focus_set_initial_policy(probe->input, GUI_FOCUS_INITIAL_MANUAL);
    probe->init_errno = errno;

    /* 业务回调里删除树同样被拒绝（两种守卫不得混淆）。 */
    errno = 0;
    probe->destroy_rc = gui_widget_destroy(probe->platform, probe->other);
    probe->destroy_errno = errno;
}

typedef struct {
    int calls;
    gui_widget_t last_target;
} activate_probe_t;

static void activate_handler(const gui_event_t *event, void *userdata)
{
    activate_probe_t *probe = (activate_probe_t *)userdata;

    probe->calls++;
    probe->last_target = event->target;
}

/* A business callback that tries to delete the tree: the callback guard rejects it. */
typedef struct {
    gui_platform_t *platform;
    gui_widget_t victim;
    int calls;
    int destroy_rc;
    int destroy_errno;
} destroy_guard_probe_t;

static void destroy_guard_handler(const gui_event_t *event, void *userdata)
{
    destroy_guard_probe_t *probe = (destroy_guard_probe_t *)userdata;

    (void)event;
    probe->calls++;
    errno = 0;
    probe->destroy_rc = gui_widget_destroy(probe->platform, probe->victim);
    probe->destroy_errno = errno;
}

/* 在 (x, y) 处点一下。 */
static void click_at(gui_native_fixture_t *fx, int32_t x, int32_t y)
{
    mouse_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.dx = INT16_MIN;
    ev.dy = INT16_MIN;
    assert_eq(0, gui_native_fixture_mouse(fx, &ev, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));

    memset(&ev, 0, sizeof(ev));
    ev.dx = (int16_t)x;
    ev.dy = (int16_t)y;
    assert_eq(0, gui_native_fixture_mouse(fx, &ev, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));

    memset(&ev, 0, sizeof(ev));
    ev.buttons = 0x01u;
    assert_eq(0, gui_native_fixture_mouse(fx, &ev, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));

    memset(&ev, 0, sizeof(ev));
    assert_eq(0, gui_native_fixture_mouse(fx, &ev, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));
}

static void click_widget(gui_native_fixture_t *fx, gui_widget_t widget)
{
    gui_rect_t r;

    assert_eq(0, gui_widget_get_bounds(fx->platform, widget, &r));
    click_at(fx, r.x + r.width / 2, r.y + r.height / 2);
}

/* 统计控件矩形内的"主题 primary 焦点环"像素数量。 */
/* ═══════════════════════════════════════════════════════════════════
 * Task 7：登记模式、策略与生命周期事务
 * ═══════════════════════════════════════════════════════════════════ */

TEST_FUNC(test_focus_registration_modes)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t button;
    gui_widget_t out;

    /* ── UNSET 行为 ────────────────────────────────────────────── */
    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    button = make_button(&fx, root, 10, 10);

    /* UNSET：get 是 NONE、move 成功、set(NONE) 成功、set 其他 EINVAL。 */
    assert_eq(0, gui_focus_get(fx.input, &out));
    assert_eq(0, out.id);
    assert_eq(0, gui_focus_move(fx.input, GUI_FOCUS_FORWARD));
    assert_eq(0, gui_focus_set(fx.input, GUI_WIDGET_NONE));
    errno = 0;
    assert_eq(-1, gui_focus_set(fx.input, button));
    assert_eq(EINVAL, errno);

    /* 失败登记不锁定模式：仍然可以成功登记。 */
    errno = 0;
    assert_eq(-1, gui_focus_register(fx.input, button, (gui_focus_policy_t)99));
    assert_eq(EINVAL, errno);
    assert_eq(0, gui_focus_register(fx.input, button, GUI_FOCUS_TAB));

    /* 全注销仍锁定：诊断(raw)登记被拒绝。 */
    assert_eq(0, gui_focus_unregister(fx.input, button));
    errno = 0;
    assert_eq(-1, gui_native_fixture_raw_focus_add_button(&fx));
    assert_eq(EBUSY, errno);

    gui_native_fixture_destroy(&fx);

    /* ── RAW 模式：公共焦点 API 不支持 ─────────────────────────── */
    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    button = make_button(&fx, root, 10, 10);

    assert_eq(0, gui_native_fixture_raw_focus_add_button(&fx));

    /* 混用：原生登记被拒绝。 */
    errno = 0;
    assert_eq(-1, gui_focus_register(fx.input, button, GUI_FOCUS_TAB));
    assert_eq(EBUSY, errno);

    /* 其他公共焦点 API：EOPNOTSUPP，输出保持 NONE。 */
    out.id = 0x1234u;
    errno = 0;
    assert_eq(-1, gui_focus_get(fx.input, &out));
    assert_eq(EOPNOTSUPP, errno);
    assert_eq(0, out.id);

    errno = 0;
    assert_eq(-1, gui_focus_set_initial_policy(fx.input, GUI_FOCUS_INITIAL_MANUAL));
    assert_eq(EOPNOTSUPP, errno);

    errno = 0;
    assert_eq(-1, gui_focus_set(fx.input, GUI_WIDGET_NONE));
    assert_eq(EOPNOTSUPP, errno);

    errno = 0;
    assert_eq(-1, gui_focus_move(fx.input, GUI_FOCUS_FORWARD));
    assert_eq(EOPNOTSUPP, errno);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_callback_focus_mutation_guard)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t button;
    gui_widget_t other;
    gui_widget_t got;
    guard_probe_t probe;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    button = make_button(&fx, root, 10, 10);
    other = make_button(&fx, root, 120, 10);

    assert_eq(0, gui_focus_register(fx.input, button, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, button));

    memset(&probe, 0, sizeof(probe));
    probe.input = fx.input;
    probe.platform = fx.platform;
    probe.other = other;
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, guard_handler, &probe));

    click_widget(&fx, button);
    assert_eq(1, probe.calls);

    /* 变更一律 EBUSY。 */
    assert_eq(-1, probe.reg_rc);
    assert_eq(EBUSY, probe.reg_errno);
    assert_eq(-1, probe.unreg_rc);
    assert_eq(EBUSY, probe.unreg_errno);
    assert_eq(-1, probe.set_rc);
    assert_eq(EBUSY, probe.set_errno);
    assert_eq(-1, probe.move_rc);
    assert_eq(EBUSY, probe.move_errno);
    assert_eq(-1, probe.init_rc);
    assert_eq(EBUSY, probe.init_errno);

    /* 业务回调里删除树被拒绝（两种守卫不得混淆）。 */
    assert_eq(-1, probe.destroy_rc);
    assert_eq(EBUSY, probe.destroy_errno);

    /* 只读查询允许，并返回最后已提交的按钮身份。 */
    assert_eq(0, probe.get_rc);
    assert_eq(button.id, probe.got.id);

    /* 选择没被偷换。 */
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(button.id, got.id);

    /* 登记没发生：other 仍未登记（对它 set 会失败）。 */
    errno = 0;
    assert_eq(-1, gui_focus_set(fx.input, other));
    assert_eq(EINVAL, errno);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_register_duplicate_updates_policy_at_capacity)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t buttons[130];
    gui_widget_t order[8];
    gui_widget_t label;
    gui_widget_t foreign;
    int count;
    int i;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);

    /* 128 项登记全部成功。 */
    for (i = 0; i < 128; i++) {
        buttons[i] = make_button(&fx, root, 10, 10);
        assert_eq(0, gui_focus_register(fx.input, buttons[i], GUI_FOCUS_TAB));
    }

    /* 第 129 项：ENOSPC。 */
    buttons[128] = make_button(&fx, root, 10, 10);
    errno = 0;
    assert_eq(-1, gui_focus_register(fx.input, buttons[128], GUI_FOCUS_TAB));
    assert_eq(ENOSPC, errno);

    /* 满表重复登记成功，且位置不变。 */
    count = gui_native_fixture_query_focus_order(&fx, order, 8);
    assert_eq(8, count);
    assert_eq(buttons[0].id, order[0].id);
    assert_eq(buttons[1].id, order[1].id);

    assert_eq(0, gui_focus_register(fx.input, buttons[0], GUI_FOCUS_CLICK));
    count = gui_native_fixture_query_focus_order(&fx, order, 8);
    assert_eq(8, count);
    assert_eq(buttons[0].id, order[0].id);
    assert_eq(buttons[1].id, order[1].id);

    /* 非按钮：EOPNOTSUPP。 */
    label = make_label(&fx, root, 10, 200);
    errno = 0;
    assert_eq(-1, gui_focus_register(fx.input, label, GUI_FOCUS_TAB));
    assert_eq(EOPNOTSUPP, errno);

    /* 失效身份：EINVAL。 */
    {
        gui_widget_t dead = make_button(&fx, root, 10, 300);

        assert_eq(0, gui_widget_destroy(fx.platform, dead));
        errno = 0;
        assert_eq(-1, gui_focus_register(fx.input, dead, GUI_FOCUS_TAB));
        assert_eq(EINVAL, errno);
    }

    /* 外平台身份：解析层就拒绝。 */
    assert_eq(0, gui_native_fixture_create_foreign_widget(fx.platform, false, &foreign));
    errno = 0;
    assert_eq(-1, gui_focus_register(fx.input, foreign, GUI_FOCUS_TAB));
    assert_eq(EINVAL, errno);
    gui_native_fixture_free_foreign_widget(fx.platform);

    /* 外 display：归属层拒绝。 */
    assert_eq(0, gui_native_fixture_create_foreign_widget(fx.platform, true, &foreign));
    errno = 0;
    assert_eq(-1, gui_focus_register(fx.input, foreign, GUI_FOCUS_TAB));
    assert_eq(EINVAL, errno);
    gui_native_fixture_free_foreign_widget(fx.platform);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_unregister_current_uses_original_successor)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t b;
    gui_widget_t c;
    gui_widget_t click_only;
    gui_widget_t disabled;
    gui_widget_t got;
    gui_widget_t receiver;


    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    b = make_button(&fx, root, 120, 10);
    c = make_button(&fx, root, 230, 10);
    click_only = make_button(&fx, root, 340, 10);
    disabled = make_button(&fx, root, 450, 10);

    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, b, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, c, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, click_only, GUI_FOCUS_CLICK));
    assert_eq(0, gui_focus_register(fx.input, disabled, GUI_FOCUS_TAB));
    assert_eq(0, gui_widget_set_enabled(fx.platform, disabled, false));

    /* 焦点在 b。 */
    assert_eq(0, gui_focus_set(fx.input, b));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(b.id, got.id);

    /* 注销当前项 → 按移除前的位置落到后继（c）。 */
    assert_eq(0, gui_focus_unregister(fx.input, b));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(c.id, got.id);

    /* 真实接收者也是 c：不能只看表成员。 */
    assert_eq(0, gui_native_fixture_query_focus_receiver(&fx, &receiver));
    assert_eq(c.id, receiver.id);

    /* 只剩一项：注销它 → NONE。 */
    assert_eq(0, gui_focus_unregister(fx.input, c));
    assert_eq(0, gui_focus_unregister(fx.input, click_only));
    assert_eq(0, gui_focus_unregister(fx.input, disabled));
    assert_eq(0, gui_focus_unregister(fx.input, a));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);
    assert_eq(0, gui_native_fixture_query_focus_receiver(&fx, &receiver));
    assert_eq(0, receiver.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_unregister_noncurrent_preserves_active_receiver)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t b;
    gui_widget_t got;
    gui_widget_t receiver;

    activate_probe_t probe;
    const uint8_t enter_down[] = { KEY_ENTER_DOWN };

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    b = make_button(&fx, root, 120, 10);

    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, b, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, b));

    memset(&probe, 0, sizeof(probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, b, activate_handler, &probe));

    /* 按住 Enter：b 是当前焦点。 */
    feed_keys(&fx, enter_down, sizeof(enter_down));
    step(&fx, 20);
    assert_eq(0, probe.calls);

    /* 注销**非**当前项 a：b 仍然接收按键，不被取消。 */
    assert_eq(0, gui_focus_unregister(fx.input, a));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(b.id, got.id);
    assert_eq(0, gui_native_fixture_query_focus_receiver(&fx, &receiver));
    assert_eq(b.id, receiver.id);

    /* 释放：b 被激活恰好一次。 */
    press_key(&fx, KEY_ENTER_UP, KEY_ENTER_UP);
    assert_eq(1, probe.calls);
    assert_eq(b.id, probe.last_target.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_native_unregister_input_destroy_widget_destroy)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t button;
    gui_widget_t got;
    gui_input_config_t icfg;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    button = make_button(&fx, root, 10, 10);
    assert_eq(0, gui_focus_register(fx.input, button, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, button));

    /* 单独销毁 input：控件仍然有效。 */
    gui_input_destroy(fx.input);
    fx.input = NULL;
    assert_true(gui_widget_is_valid(fx.platform, button));

    /* 第二个 input 没有旧登记。 */
    memset(&icfg, 0, sizeof(icfg));
    icfg.mode = GUI_INPUT_SYNTHETIC;
    icfg.require_mouse = false;
    gui_input_t *second = gui_input_create(fx.platform, &icfg);
    assert_not_null(second);
    assert_eq(0, gui_focus_get(second, &got));
    assert_eq(0, got.id);
    /* 旧身份也没有登记：set 它失败。 */
    errno = 0;
    assert_eq(-1, gui_focus_set(second, button));
    assert_eq(EINVAL, errno);
    gui_input_destroy(second);

    /* 销毁控件：身份立即失效。 */
    assert_eq(0, gui_widget_destroy(fx.platform, button));
    assert_false(gui_widget_is_valid(fx.platform, button));

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_delete_subtree_skips_all_doomed_descendants)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t box;
    gui_widget_t child;
    gui_widget_t survivor;
    gui_widget_t got;
    gui_widget_t receiver;


    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    box = make_button(&fx, root, 10, 10);
    child = make_button(&fx, box, 20, 60);
    survivor = make_button(&fx, root, 300, 10);

    /* 焦点在子按钮上；父按钮与子按钮都在册。 */
    assert_eq(0, gui_focus_register(fx.input, box, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, child, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, survivor, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, child));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(child.id, got.id);

    /* 删除整棵子树：两个条目都被摘掉，焦点跳过它们落到后继。 */
    assert_eq(0, gui_widget_destroy(fx.platform, box));
    assert_false(gui_widget_is_valid(fx.platform, box));
    assert_false(gui_widget_is_valid(fx.platform, child));

    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(survivor.id, got.id);
    assert_eq(0, gui_native_fixture_query_focus_receiver(&fx, &receiver));
    assert_eq(survivor.id, receiver.id);

    /* 登记表里不再有待删子树的条目。 */
    {
        gui_widget_t order[8];
        int count = gui_native_fixture_query_focus_order(&fx, order, 8);

        assert_eq(1, count);
        assert_eq(survivor.id, order[0].id);
    }

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_input_destroy_detaches_all_observers)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t button;
    gui_input_config_t icfg;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    button = make_button(&fx, root, 10, 10);
    assert_eq(0, gui_focus_register(fx.input, button, GUI_FOCUS_TAB));

    /* 销毁 input：焦点观察者全部被卸载。 */
    gui_input_destroy(fx.input);
    fx.input = NULL;

    /* 控件随后被销毁：不得触碰已释放的 input。 */
    assert_eq(0, gui_widget_destroy(fx.platform, button));
    assert_false(gui_widget_is_valid(fx.platform, button));

    /* 新建的 input 没有旧登记。 */
    memset(&icfg, 0, sizeof(icfg));
    icfg.mode = GUI_INPUT_SYNTHETIC;
    icfg.require_mouse = false;
    gui_input_t *second = gui_input_create(fx.platform, &icfg);
    assert_not_null(second);
    {
        gui_widget_t got;

        assert_eq(0, gui_focus_get(second, &got));
        assert_eq(0, got.id);
    }
    gui_input_destroy(second);

    gui_native_fixture_destroy(&fx);
}

/* ═══════════════════════════════════════════════════════════════════
 * Task 8：选择历史、MANUAL、清空与取消协议
 * ═══════════════════════════════════════════════════════════════════ */

TEST_FUNC(test_manual_first_tab_and_shift_tab)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t b;
    gui_widget_t c;
    gui_widget_t got;

    /* ── 正向首次 Tab → 第一个可用 Tab 项 ─────────────────────── */
    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    b = make_button(&fx, root, 120, 10);
    c = make_button(&fx, root, 230, 10);

    assert_eq(0, gui_focus_set_initial_policy(fx.input, GUI_FOCUS_INITIAL_MANUAL));
    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, b, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, c, GUI_FOCUS_TAB));

    /* MANUAL：跨 step 仍然是 NONE。 */
    step(&fx, 20);
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    /* 首次正向 Tab → a。 */
    press_key(&fx, KEY_TAB_DOWN, KEY_TAB_UP);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(a.id, got.id);

    gui_native_fixture_destroy(&fx);

    /* ── 反向首次 Tab → 最后一个可用 Tab 项 ───────────────────── */
    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    b = make_button(&fx, root, 120, 10);
    c = make_button(&fx, root, 230, 10);

    assert_eq(0, gui_focus_set_initial_policy(fx.input, GUI_FOCUS_INITIAL_MANUAL));
    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, b, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, c, GUI_FOCUS_TAB));

    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    /* Shift-Tab 首次 → c（最后一个）。 */
    {
        const uint8_t shift_tab[] = { KEY_LSHIFT_DOWN, KEY_TAB_DOWN, KEY_TAB_UP,
                                      KEY_LSHIFT_UP };

        feed_keys(&fx, shift_tab, sizeof(shift_tab));
        step(&fx, 20);
    }
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(c.id, got.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_auto_first_and_initial_policy_change)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t b;
    gui_widget_t got;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    b = make_button(&fx, root, 120, 10);

    /* AUTO_FIRST：首次可用 Tab 登记建立选择。 */
    assert_eq(0, gui_focus_set_initial_policy(fx.input, GUI_FOCUS_INITIAL_AUTO_FIRST));
    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(a.id, got.id);

    /* 改变策略不偷换已有焦点。 */
    assert_eq(0, gui_focus_set_initial_policy(fx.input, GUI_FOCUS_INITIAL_MANUAL));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(a.id, got.id);

    /* 登记新项也不改变已有选择（AUTO_FIRST 只管"首次"）。 */
    assert_eq(0, gui_focus_register(fx.input, b, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(a.id, got.id);

    gui_native_fixture_destroy(&fx);

    /* MANUAL：登记不建立选择。 */
    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);

    assert_eq(0, gui_focus_set_initial_policy(fx.input, GUI_FOCUS_INITIAL_MANUAL));
    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_stable_refresh_keeps_active_key)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t b;
    gui_widget_t got;
    activate_probe_t probe;
    const uint8_t enter_down[] = { KEY_ENTER_DOWN };

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    b = make_button(&fx, root, 120, 10);

    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, b, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, a));

    memset(&probe, 0, sizeof(probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, a, activate_handler, &probe));

    /* 真实 DOWN → 跨三次 step → UP。 */
    feed_keys(&fx, enter_down, sizeof(enter_down));
    step(&fx, 20);
    step(&fx, 200);
    step(&fx, 200);
    step(&fx, 200);

    /* 按住期间没有激活。 */
    assert_eq(0, probe.calls);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(a.id, got.id);

    /* 释放后激活一次。 */
    press_key(&fx, KEY_ENTER_UP, KEY_ENTER_UP);
    assert_eq(1, probe.calls);
    assert_eq(a.id, probe.last_target.id);

    /* 后台非当前项增删不取消当前键：再按住一次，中途加入一个新按钮。 */
    feed_keys(&fx, enter_down, sizeof(enter_down));
    step(&fx, 20);
    assert_eq(1, probe.calls);
    {
        gui_widget_t c = make_button(&fx, root, 230, 10);

        assert_eq(0, gui_focus_register(fx.input, c, GUI_FOCUS_TAB));
    }
    step(&fx, 200);
    assert_eq(1, probe.calls);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(a.id, got.id);

    press_key(&fx, KEY_ENTER_UP, KEY_ENTER_UP);
    assert_eq(2, probe.calls);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_clear_cancels_pressed_and_repeat)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t got;
    gui_widget_t receiver;

    activate_probe_t probe;
    const uint8_t enter_down[] = { KEY_ENTER_DOWN };

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);

    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, a));

    memset(&probe, 0, sizeof(probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, a, activate_handler, &probe));

    /* 按住 Enter。 */
    feed_keys(&fx, enter_down, sizeof(enter_down));
    step(&fx, 20);
    assert_eq(0, probe.calls);

    /* 显式清空：公开与后端都是 NONE。 */
    assert_eq(0, gui_focus_set(fx.input, GUI_WIDGET_NONE));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);
    assert_eq(0, gui_native_fixture_query_focus_receiver(&fx, &receiver));
    assert_eq(0, receiver.id);

    /* pump 之后旧活动键与它的重复被清除。 */
    step(&fx, 20);
    step(&fx, 500);

    /* 迟到的 UP 不激活。 */
    press_key(&fx, KEY_ENTER_UP, KEY_ENTER_UP);
    assert_eq(0, probe.calls);

    /* 重新选对象后，一整轮新键能激活。 */
    assert_eq(0, gui_focus_set(fx.input, a));
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(a.id, got.id);

    press_key(&fx, KEY_ENTER_DOWN, KEY_ENTER_UP);
    assert_eq(1, probe.calls);
    assert_eq(a.id, probe.last_target.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_manual_mouse_selection_repairs_when_hidden)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t b;
    gui_widget_t c;
    gui_widget_t got;
    bool ever_selected = false;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    b = make_button(&fx, root, 120, 10);
    c = make_button(&fx, root, 230, 10);

    assert_eq(0, gui_focus_set_initial_policy(fx.input, GUI_FOCUS_INITIAL_MANUAL));
    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, b, GUI_FOCUS_STRONG));
    assert_eq(0, gui_focus_register(fx.input, c, GUI_FOCUS_TAB));

    /* MANUAL：还没有选择。 */
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    /* 真实鼠标点击 b：这是**选择**，不是调用公共 set。 */
    click_widget(&fx, b);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(b.id, got.id);

    /* 组回调先执行的鼠标路径也必须记录 ever_selected。 */
    assert_eq(0, gui_native_fixture_query_focus_history(&fx, &ever_selected, NULL));
    assert_true(ever_selected);

    /* 隐藏 b：焦点修复到它之后的 c。 */
    assert_eq(0, gui_widget_set_visible(fx.platform, b, false));
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(c.id, got.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_click_only_focus_survives_step)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t tabbed;
    gui_widget_t click_only;
    gui_widget_t got;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    tabbed = make_button(&fx, root, 10, 10);
    click_only = make_button(&fx, root, 120, 10);

    assert_eq(0, gui_focus_register(fx.input, tabbed, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, click_only, GUI_FOCUS_CLICK));
    assert_eq(0, gui_focus_set(fx.input, tabbed));

    /* 步进不改变选择：CLICK-only 项不参与 Tab 导航。 */
    step(&fx, 20);
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(tabbed.id, got.id);

    /* 手动 Tab 也不会走到 CLICK-only 项。 */
    press_key(&fx, KEY_TAB_DOWN, KEY_TAB_UP);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(tabbed.id, got.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_hidden_ancestor_and_empty_repair)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t box;
    gui_widget_t inner;
    gui_widget_t other;
    gui_widget_t got;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    box = make_button(&fx, root, 10, 10);
    inner = make_button(&fx, root, 120, 10);
    other = make_button(&fx, root, 230, 10);

    assert_eq(0, gui_focus_register(fx.input, box, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, inner, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, other, GUI_FOCUS_TAB));

    /* 焦点在 inner；隐藏它的兄弟 box 不影响它。 */
    assert_eq(0, gui_focus_set(fx.input, inner));
    assert_eq(0, gui_widget_set_visible(fx.platform, box, false));
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(inner.id, got.id);

    /* 隐藏 inner 自己：焦点落到它之后的 other。 */
    assert_eq(0, gui_widget_set_visible(fx.platform, inner, false));
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(other.id, got.id);

    /* 全部不可用：焦点为空。 */
    assert_eq(0, gui_widget_set_visible(fx.platform, other, false));
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    /* 候选恢复：修复继续，焦点回到可用项。 */
    assert_eq(0, gui_widget_set_visible(fx.platform, inner, false));
    assert_eq(0, gui_widget_set_visible(fx.platform, other, true));
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(other.id, got.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_failed_set_preserves_explicit_clear)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t unregistered;
    gui_widget_t hidden;
    gui_widget_t foreign;
    gui_widget_t got;
    bool explicit_clear = false;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    unregistered = make_button(&fx, root, 120, 10);
    hidden = make_button(&fx, root, 230, 10);

    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, hidden, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, a));

    /* 显式清空。 */
    assert_eq(0, gui_focus_set(fx.input, GUI_WIDGET_NONE));
    assert_eq(0, gui_native_fixture_query_focus_history(&fx, NULL, &explicit_clear));
    assert_true(explicit_clear);

    /* 有效但未登记：失败，选择与清空标志都不变。 */
    errno = 0;
    assert_eq(-1, gui_focus_set(fx.input, unregistered));
    assert_eq(EINVAL, errno);

    /* 隐藏（在册但不可用）：失败，EACCES。 */
    assert_eq(0, gui_widget_set_visible(fx.platform, hidden, false));
    errno = 0;
    assert_eq(-1, gui_focus_set(fx.input, hidden));
    assert_eq(EACCES, errno);

    /* 外 display：失败，EINVAL。 */
    assert_eq(0, gui_native_fixture_create_foreign_widget(fx.platform, true, &foreign));
    errno = 0;
    assert_eq(-1, gui_focus_set(fx.input, foreign));
    assert_eq(EINVAL, errno);
    gui_native_fixture_free_foreign_widget(fx.platform);

    /* 原选择仍然是 NONE，清空标志仍在。 */
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);
    assert_eq(0, gui_native_fixture_query_focus_history(&fx, NULL, &explicit_clear));
    assert_true(explicit_clear);

    /* 后续登记与刷新都不复活它。 */
    {
        gui_widget_t later = make_button(&fx, root, 340, 10);

        assert_eq(0, gui_focus_register(fx.input, later, GUI_FOCUS_TAB));
    }
    step(&fx, 20);
    assert_eq(0, gui_focus_get(fx.input, &got));
    assert_eq(0, got.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_destroy_current_during_backend_callback_cancels)
{
    gui_native_fixture_t fx;
    gui_widget_t root;
    gui_widget_t a;
    gui_widget_t victim;
    gui_widget_t other;

    destroy_guard_probe_t probe;

    assert_eq(0, gui_native_fixture_create(&fx, true));
    root = make_root(&fx);
    a = make_button(&fx, root, 10, 10);
    victim = make_button(&fx, root, 120, 10);
    other = make_button(&fx, root, 230, 10);

    assert_eq(0, gui_focus_register(fx.input, a, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_register(fx.input, victim, GUI_FOCUS_TAB));
    assert_eq(0, gui_focus_set(fx.input, victim));

    /* 业务 ACTIVATE 回调里删除树被拒绝：两种回调不得混淆。 */
    memset(&probe, 0, sizeof(probe));
    probe.platform = fx.platform;
    probe.victim = other;
    assert_eq(0, gui_widget_set_event_handler(fx.platform, victim, destroy_guard_handler, &probe));
    click_widget(&fx, victim);
    assert_eq(1, probe.calls);
    assert_eq(-1, probe.destroy_rc);
    assert_eq(EBUSY, probe.destroy_errno);

    gui_native_fixture_destroy(&fx);

    /*
     * 后端在回调里删掉当前焦点对象：观察者不递归 read，也不把释放投给新
     * 对象。诊断(raw)登记与原生登记**互斥**，所以这里换一个新的夹具。
     */
    assert_eq(0, gui_native_fixture_create(&fx, true));
    assert_eq(0, gui_native_fixture_raw_focus_add_button(&fx));
    assert_eq(0, gui_native_fixture_raw_focus_add_button(&fx));

    /* 首个登记的可用控件建立焦点。 */
    assert_eq(1, gui_native_fixture_raw_is_focused(&fx, 0));

    /* 后端删掉当前持有焦点的对象。 */
    assert_eq(0, gui_native_fixture_raw_delete_focused(&fx));
    step(&fx, 20);

    /* 焦点落到后继上；被释放的对象不会再收到任何东西。 */
    assert_eq(1, gui_native_fixture_raw_is_focused(&fx, 1));
    assert_eq(0, gui_native_fixture_raw_is_focused(&fx, 0));

    gui_native_fixture_destroy(&fx);
}

/* ═══════════════════════════════════════════════════════════════════
 * Task 9：指针策略、激活与主题色迁移
 * ═══════════════════════════════════════════════════════════════════ */

TEST_LIST_BEGIN
    TEST_ENTRY(test_focus_registration_modes),
    TEST_ENTRY(test_callback_focus_mutation_guard),
    TEST_ENTRY(test_register_duplicate_updates_policy_at_capacity),
    TEST_ENTRY(test_unregister_current_uses_original_successor),
    TEST_ENTRY(test_unregister_noncurrent_preserves_active_receiver),
    TEST_ENTRY(test_native_unregister_input_destroy_widget_destroy),
    TEST_ENTRY(test_delete_subtree_skips_all_doomed_descendants),
    TEST_ENTRY(test_input_destroy_detaches_all_observers),
    TEST_ENTRY(test_manual_first_tab_and_shift_tab),
    TEST_ENTRY(test_auto_first_and_initial_policy_change),
    TEST_ENTRY(test_stable_refresh_keeps_active_key),
    TEST_ENTRY(test_clear_cancels_pressed_and_repeat),
    TEST_ENTRY(test_manual_mouse_selection_repairs_when_hidden),
    TEST_ENTRY(test_click_only_focus_survives_step),
    TEST_ENTRY(test_hidden_ancestor_and_empty_repair),
    TEST_ENTRY(test_failed_set_preserves_explicit_clear),
    TEST_ENTRY(test_destroy_current_during_backend_callback_cancels)
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
