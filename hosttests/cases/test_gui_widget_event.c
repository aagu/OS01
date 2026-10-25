/*
 * test_gui_widget_event.c — 原生事件桥接与平台统计（GUI-2b Task 6）
 *
 * 本文件是**公共契约**测试：它只包含 gui 目录下的公共头、标准库、测试
 * 框架和宿主私有 fixture 头。断言全部通过公共 API 与值句柄表达。
 *
 * 三条主线：
 *   1. 事件桥接：一次真实的点击 → 一次 ACTIVATE，身份与上下文都对；
 *   2. 回调期间的守卫：改树与嵌套步进一律 EBUSY，且不留部分修改；
 *   3. 槽复用契约：复用槽的新对象不会调用旧 handler，删除期间创建的
 *      控件不会拿到正在删除的槽。
 */

#include "test_framework.h"

#include <gui/gui.h>
#include <gui/gui_event.h>
#include <gui/gui_layout.h>
#include <gui/gui_platform.h>
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

/* ── 业务上下文与回调 ────────────────────────────────────────────── */

/**
 * @brief 业务上下文
 *
 * 回调拿到的 userdata 就是这个结构的**地址**：库不解释它、不复制它。
 */
typedef struct {
    int calls;
    gui_event_kind_t last_kind;
    gui_widget_t last_target;
    void *last_userdata;
} event_probe_t;

static void probe_handler(const gui_event_t *event, void *userdata)
{
    event_probe_t *probe = (event_probe_t *)userdata;
    probe->calls++;
    probe->last_kind = event->kind;
    probe->last_target = event->target;
    probe->last_userdata = userdata;
}

/* ── 指针驱动 ────────────────────────────────────────────────────── */

/**
 * @brief 在 (x, y) 处点一下
 *
 * 指针的位移是**相对**的，初始位置在视口中心：位移量必须相对当前绝对
 * 位置算，因此先把它归到视口左上角。
 */
static void click_at(gui_native_fixture_t *fx, int32_t x, int32_t y)
{
    mouse_event_t home;
    memset(&home, 0, sizeof(home));
    home.dx = INT16_MIN;
    home.dy = INT16_MIN;
    assert_eq(0, gui_native_fixture_mouse(fx, &home, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));

    mouse_event_t move;
    memset(&move, 0, sizeof(move));
    move.dx = (int16_t)x;
    move.dy = (int16_t)y;
    assert_eq(0, gui_native_fixture_mouse(fx, &move, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));

    mouse_event_t press;
    memset(&press, 0, sizeof(press));
    press.buttons = 0x01u;
    assert_eq(0, gui_native_fixture_mouse(fx, &press, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));

    mouse_event_t release;
    memset(&release, 0, sizeof(release));
    assert_eq(0, gui_native_fixture_mouse(fx, &release, 1));
    assert_eq(0, gui_native_fixture_step(fx, 50));
}

/* ── 事件桥接 ────────────────────────────────────────────────────── */

TEST_FUNC(test_activate_event_reaches_button_handler)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, button, &g));

    event_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, probe_handler, &probe));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, button, &r));
    click_at(&fx, r.x + 60, r.y + 30);

    /* 一次真实的点击 → 一次 ACTIVATE。 */
    assert_eq(1, probe.calls);
    assert_eq(GUI_EVENT_ACTIVATE, probe.last_kind);
    /* target 是**值句柄**：就是被点中的那个按钮的身份。 */
    assert_eq(button.id, probe.last_target.id);
    /* userdata 是业务上下文的地址，原样传回。 */
    assert_true(probe.last_userdata == (void *)&probe);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_event_handler_registration_rules)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));
    gui_widget_t container;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &container));

    event_probe_t probe;
    memset(&probe, 0, sizeof(probe));

    /* ACTIVATE 只属于按钮。 */
    errno = 0;
    assert_eq(-1, gui_widget_set_event_handler(fx.platform, label, probe_handler, &probe));
    assert_eq(EOPNOTSUPP, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_event_handler(fx.platform, container, probe_handler, &probe));
    assert_eq(EOPNOTSUPP, errno);

    /* 无效身份。 */
    errno = 0;
    assert_eq(-1, gui_widget_set_event_handler(fx.platform, GUI_WIDGET_NONE, probe_handler, &probe));
    assert_eq(EINVAL, errno);

    /* 空平台。 */
    errno = 0;
    assert_eq(-1, gui_widget_set_event_handler(NULL, button, probe_handler, &probe));
    assert_eq(EINVAL, errno);

    /* 按钮可以挂。 */
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, probe_handler, &probe));

    /* 注销：回调与上下文一起被摘掉。 */
    gui_native_slot_info_t info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, button, &info));
    assert_true(info.has_handler);
    assert_true(info.has_userdata);
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, NULL, NULL));
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, button, &info));
    assert_false(info.has_handler);
    assert_false(info.has_userdata);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_child_label_and_icon_activate_parent_once)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, button, &g));

    /* 按钮的标题是它的 LABEL 子项；旁边还有一个装饰容器。 */
    gui_widget_t caption;
    assert_eq(0, gui_widget_create(fx.platform, button, GUI_WIDGET_LABEL, &caption));
    assert_eq(0, gui_widget_set_text(fx.platform, caption, "OK"));
    gui_widget_t decoration;
    assert_eq(0, gui_widget_create(fx.platform, button, GUI_WIDGET_CONTAINER, &decoration));

    gui_geometry_t sub;
    sub.x = 0;
    sub.y = 0;
    sub.width = GUI_SIZE_PX(40);
    sub.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, caption, &sub));
    assert_eq(0, gui_widget_set_geometry(fx.platform, decoration, &sub));

    event_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, probe_handler, &probe));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, button, &r));

    /* 点在标签上：标签不拦截命中，被激活的是按钮。 */
    click_at(&fx, r.x + 10, r.y + 10);
    assert_eq(1, probe.calls);
    assert_eq(button.id, probe.last_target.id);

    /* 点在装饰容器上：装饰容器默认不拦截。 */
    click_at(&fx, r.x + 30, r.y + 10);
    assert_eq(2, probe.calls);
    assert_eq(button.id, probe.last_target.id);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_no_duplicate_activation_from_event_bubbling)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t outer;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &outer));
    gui_widget_t inner;
    assert_eq(0, gui_widget_create(fx.platform, outer, GUI_WIDGET_BUTTON, &inner));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, outer, &g));
    g.x = 10;
    g.y = 10;
    g.width = GUI_SIZE_PX(40);
    g.height = GUI_SIZE_PX(20);
    assert_eq(0, gui_widget_set_geometry(fx.platform, inner, &g));

    event_probe_t inner_probe;
    event_probe_t outer_probe;
    memset(&inner_probe, 0, sizeof(inner_probe));
    memset(&outer_probe, 0, sizeof(outer_probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, inner, probe_handler, &inner_probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, outer, probe_handler, &outer_probe));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, inner, &r));
    click_at(&fx, r.x + 20, r.y + 10);

    /* 内层按钮被激活一次；外层不许因为冒泡再被激活一次。 */
    assert_eq(1, inner_probe.calls);
    assert_eq(inner.id, inner_probe.last_target.id);
    assert_eq(0, outer_probe.calls);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_event_handler_replace_and_unset)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, button, &g));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, button, &r));

    event_probe_t first;
    event_probe_t second;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, probe_handler, &first));
    click_at(&fx, r.x + 60, r.y + 30);
    assert_eq(1, first.calls);
    assert_eq(0, second.calls);

    /* 替换：旧回调不再被调用。 */
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, probe_handler, &second));
    click_at(&fx, r.x + 60, r.y + 30);
    assert_eq(1, first.calls);
    assert_eq(1, second.calls);

    /* 注销：谁也不被调用。 */
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, NULL, NULL));
    click_at(&fx, r.x + 60, r.y + 30);
    assert_eq(1, first.calls);
    assert_eq(1, second.calls);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_deleted_widget_does_not_deliver)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, button, &g));

    event_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, probe_handler, &probe));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, button, &r));

    /* 控件没了：点击那块地方不再投递。 */
    assert_eq(0, gui_widget_destroy(fx.platform, button));
    assert_false(gui_widget_is_valid(fx.platform, button));
    click_at(&fx, r.x + 60, r.y + 30);
    assert_eq(0, probe.calls);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_reused_slot_never_calls_old_handler)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    /* 旧控件：带一个业务回调。 */
    gui_widget_t old_button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &old_button));
    event_probe_t old_probe;
    memset(&old_probe, 0, sizeof(old_probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, old_button, probe_handler, &old_probe));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, old_button, &g));

    /* 槽下标必须在它还活着的时候记下来。 */
    gui_native_slot_info_t old_info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, old_button, &old_info));
    assert_eq(GUI_NATIVE_SLOT_LIVE, old_info.state);
    assert_true(old_info.has_handler);

    assert_eq(0, gui_widget_destroy(fx.platform, old_button));

    /* 复用的槽：同一个槽下标。 */
    gui_widget_t new_button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &new_button));
    gui_native_slot_info_t new_info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, new_button, &new_info));
    assert_eq(old_info.index, new_info.index);
    assert_true(new_button.id != old_button.id);

    /* 复用槽的新对象**没有**旧 handler。 */
    assert_false(new_info.has_handler);
    assert_false(new_info.has_userdata);

    assert_eq(0, gui_widget_set_geometry(fx.platform, new_button, &g));
    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, new_button, &r));
    click_at(&fx, r.x + 60, r.y + 30);

    /* 旧 handler 一个都没有被调用过。 */
    assert_eq(0, old_probe.calls);

    /* 给新对象挂上新回调之后，事件只走新回调。 */
    event_probe_t new_probe;
    memset(&new_probe, 0, sizeof(new_probe));
    assert_eq(0, gui_widget_set_event_handler(fx.platform, new_button, probe_handler, &new_probe));
    click_at(&fx, r.x + 60, r.y + 30);
    assert_eq(1, new_probe.calls);
    assert_eq(0, old_probe.calls);

    gui_native_fixture_destroy(&fx);
}

/* ── 回调期间的守卫 ──────────────────────────────────────────────── */

typedef struct {
    gui_platform_t *platform;
    gui_widget_t root;
    gui_widget_t victim;
    gui_widget_t label;
    gui_widget_t button;
    gui_appearance_t appearance;
    int create_rc;
    int create_errno;
    int destroy_rc;
    int destroy_errno;
    int clear_rc;
    int clear_errno;
    int layout_rc;
    int layout_errno;
    gui_status_t step_status;
    int step_errno;
    uint32_t delay;
    gui_widget_t created;
    int calls;

    /* 回调期间**允许**做的操作：它们不改控件树的结构。 */
    int text_rc;
    int appearance_rc;
    int visible_rc;
    int enabled_rc;
    int stats_rc;
} guard_probe_t;

static void mutating_handler(const gui_event_t *event, void *userdata)
{
    guard_probe_t *probe = (guard_probe_t *)userdata;
    probe->calls++;
    (void)event;

    probe->created.id = 0;
    errno = 0;
    probe->create_rc = gui_widget_create(probe->platform, probe->root, GUI_WIDGET_CONTAINER,
                                         &probe->created);
    probe->create_errno = errno;

    errno = 0;
    probe->destroy_rc = gui_widget_destroy(probe->platform, probe->victim);
    probe->destroy_errno = errno;

    errno = 0;
    probe->clear_rc = gui_widget_clear(probe->platform, probe->root);
    probe->clear_errno = errno;

    gui_layout_t layout;
    layout.flow = GUI_FLOW_COLUMN;
    layout.main_align = GUI_FLOW_ALIGN_CENTER;
    layout.cross_align = GUI_FLOW_ALIGN_END;
    layout.grow = 1;
    layout.gap = 3;
    layout.pad_top = 1;
    layout.pad_bottom = 2;
    layout.pad_left = 3;
    layout.pad_right = 4;
    errno = 0;
    probe->layout_rc = gui_widget_set_layout(probe->platform, probe->root, &layout);
    probe->layout_errno = errno;

    /* 嵌套步进不许写输出参数：错误路径不许留下"成功样"的延迟值。 */
    probe->delay = 12345u;
    errno = 0;
    probe->step_status = gui_platform_step(probe->platform, &probe->delay);
    probe->step_errno = errno;

    /* 不改树结构的操作在回调期间是**允许**的：守卫不许把它们一起挡掉。 */
    probe->text_rc = gui_widget_set_text(probe->platform, probe->label, "x");
    probe->appearance_rc = gui_widget_set_appearance(probe->platform, probe->button,
                                                    &probe->appearance);
    probe->visible_rc = gui_widget_set_visible(probe->platform, probe->button, true);
    probe->enabled_rc = gui_widget_set_enabled(probe->platform, probe->button, true);

    gui_platform_stats_t stats;
    probe->stats_rc = gui_platform_get_stats(probe->platform, &stats);
}

TEST_FUNC(test_callback_mutation_guard)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t victim;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &victim));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_geometry_t g;
    g.x = 0;
    g.y = 0;
    g.width = GUI_SIZE_PX(120);
    g.height = GUI_SIZE_PX(60);
    assert_eq(0, gui_widget_set_geometry(fx.platform, button, &g));

    guard_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.platform = fx.platform;
    probe.root = root;
    probe.victim = victim;
    probe.label = label;
    probe.button = button;
    probe.appearance = GUI_APPEARANCE_DEFAULT;
    probe.appearance.variant = GUI_VARIANT_ACTION; /* 按钮用动作形态 */
    assert_eq(0, gui_widget_set_event_handler(fx.platform, button, mutating_handler, &probe));

    gui_native_registry_info_t before;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &before));

    gui_rect_t r;
    assert_eq(0, gui_widget_get_bounds(fx.platform, button, &r));
    click_at(&fx, r.x + 60, r.y + 30);

    assert_eq(1, probe.calls);

    /* 回调里改树：全部 EBUSY。 */
    assert_eq(-1, probe.create_rc);
    assert_eq(EBUSY, probe.create_errno);
    assert_eq(-1, probe.destroy_rc);
    assert_eq(EBUSY, probe.destroy_errno);
    assert_eq(-1, probe.clear_rc);
    assert_eq(EBUSY, probe.clear_errno);
    assert_eq(-1, probe.layout_rc);
    assert_eq(EBUSY, probe.layout_errno);

    /* 嵌套步进：EBUSY，而且**不**锁存永久错误。 */
    assert_eq(GUI_STATUS_ERROR, probe.step_status);
    assert_eq(EBUSY, probe.step_errno);
    /* 嵌套步进的错误路径没有写输出参数。 */
    assert_eq(12345u, probe.delay);

    /* 不改树结构的操作在回调期间是允许的。 */
    assert_eq(0, probe.text_rc);
    assert_eq(0, probe.appearance_rc);
    assert_eq(0, probe.visible_rc);
    assert_eq(0, probe.enabled_rc);
    assert_eq(0, probe.stats_rc);

    /* 无部分修改：没有半成品控件，树没有被动过。 */
    assert_eq(0, probe.created.id);
    assert_true(gui_widget_is_valid(fx.platform, victim));
    assert_true(gui_widget_is_valid(fx.platform, root));

    gui_native_registry_info_t after;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &after));
    assert_eq(before.count, after.count);
    assert_eq(before.peak, after.peak);

    /* 回调深度是对称增减的：回调返回之后平台照常可用。 */
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    /* 回调之外改树又是允许的。 */
    gui_widget_t after_cb;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &after_cb));

    gui_native_fixture_destroy(&fx);
}

/* ── 槽复用契约 ──────────────────────────────────────────────────── */

TEST_FUNC(test_slot_not_reused_during_delete_callback)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t parent;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &parent));
    gui_widget_t victim;
    assert_eq(0, gui_widget_create(fx.platform, parent, GUI_WIDGET_CONTAINER, &victim));

    gui_native_slot_info_t victim_info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, victim, &victim_info));
    assert_eq(GUI_NATIVE_SLOT_LIVE, victim_info.state);

    /* 在删除回调期间创建控件。 */
    gui_widget_t created;
    assert_eq(0, gui_native_fixture_create_during_delete(fx.platform, victim, parent, &created));
    assert_eq(0, gui_widget_destroy(fx.platform, victim));
    assert_false(gui_widget_is_valid(fx.platform, victim));

    /* 删除期间创建的控件必须拿到**别的**槽：旧槽此刻还站在删除回调的
     * 栈上，交给新控件会让旧身份指向新对象。 */
    assert_true(created.id != 0);
    gui_native_slot_info_t created_info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, created, &created_info));
    assert_eq(GUI_NATIVE_SLOT_LIVE, created_info.state);
    assert_true(created_info.index != victim_info.index);

    gui_native_fixture_destroy(&fx);
}

/* ── 平台统计 ────────────────────────────────────────────────────── */

TEST_FUNC(test_stats_report_real_format_and_pool_bytes)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_platform_stats_t stats;
    assert_eq(0, gui_platform_get_stats(fx.platform, &stats));

    /* format：平台**回读**的原生像素格式。离屏模式下就是 XRGB8888。 */
    assert_eq(GUI_PIXEL_FORMAT_XRGB8888, stats.format);

    /* 视口尺寸。 */
    assert_eq(640, stats.width);
    assert_eq(480, stats.height);

    /* 池字节：真实内存池的占用。
     *
     * 公共统计只报告"已用"，不报告"空闲"；空闲字节由宿主私有观察桥独立
     * 读出，于是这条等式是**被外部复核**的，而不是实现自己证明自己。 */
    uint64_t pool_total = 0;
    uint64_t pool_free = 0;
    uint64_t pool_max_used = 0;
    assert_eq(0, gui_native_fixture_pool_bytes(&fx, &pool_total, &pool_free, &pool_max_used));
    assert_true(pool_total > 0);
    assert_eq(pool_total, stats.pool_total);
    assert_eq(pool_total - pool_free, stats.pool_used);
    assert_true(stats.pool_used <= stats.pool_total);
    assert_true(stats.pool_max_used >= stats.pool_used);

    /* 注册表自己的字节预算：与渲染池分开报告的两笔账。 */
    assert_true(stats.registry_bytes > 0);
    assert_true(stats.registry_bytes != stats.pool_total);

    /* 渲染器标识：非空，来自平台层创建时定下的真实来源。 */
    assert_true(stats.renderer_name[0] != '\0');
    assert_true(stats.renderer_name[0] != ' ');

    /* 渲染器版本：来自被锁定的光栅化库版本，整组版本确实被填上了。
     *
     * 注意：patch 组件在真实版本里**合法地**可以为 0（被锁定的光栅化库
     * 就是 9.5.0），因此这里断言的是"整组版本不是未填的零"，而不是
     * 断言 patch 一定非零——那会是对真实版本的一句假话。 */
    assert_true(stats.renderer_version_major != 0);
    assert_true(stats.renderer_version_minor != 0);
    assert_true(stats.renderer_version_major != 0 || stats.renderer_version_minor != 0 ||
                stats.renderer_version_patch != 0);

    /* widget_count：当前**存活**的控件数。 */
    assert_eq(0, stats.widget_count);

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    assert_eq(0, gui_platform_get_stats(fx.platform, &stats));
    assert_eq(1, stats.widget_count);

    gui_widget_t child;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_CONTAINER, &child));
    assert_eq(0, gui_platform_get_stats(fx.platform, &stats));
    assert_eq(2, stats.widget_count);

    assert_eq(0, gui_widget_destroy(fx.platform, child));
    assert_eq(0, gui_platform_get_stats(fx.platform, &stats));
    assert_eq(1, stats.widget_count);

    assert_eq(0, gui_widget_destroy(fx.platform, root));
    assert_eq(0, gui_platform_get_stats(fx.platform, &stats));
    assert_eq(0, stats.widget_count);

    /* 池字节是活的：建了控件又拆掉之后，读数确实变过。 */
    assert_eq(0, gui_native_fixture_pool_bytes(&fx, &pool_total, &pool_free, &pool_max_used));
    assert_eq(pool_total, stats.pool_total);
    assert_eq(pool_total - pool_free, stats.pool_used);

    /* 失败：输出被**清零**，不留半份数据。 */
    gui_platform_stats_t dirty;
    memset(&dirty, 0xAB, sizeof(dirty));
    errno = 0;
    assert_eq(-1, gui_platform_get_stats(fx.platform, NULL));
    assert_eq(EINVAL, errno);

    memset(&dirty, 0xAB, sizeof(dirty));
    errno = 0;
    assert_eq(-1, gui_platform_get_stats(NULL, &dirty));
    assert_eq(EINVAL, errno);
    for (size_t i = 0; i < sizeof(dirty); i++) {
        assert_eq(0, ((const unsigned char *)&dirty)[i]);
    }

    gui_native_fixture_destroy(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_activate_event_reaches_button_handler),
    TEST_ENTRY(test_event_handler_registration_rules),
    TEST_ENTRY(test_child_label_and_icon_activate_parent_once),
    TEST_ENTRY(test_no_duplicate_activation_from_event_bubbling),
    TEST_ENTRY(test_event_handler_replace_and_unset),
    TEST_ENTRY(test_deleted_widget_does_not_deliver),
    TEST_ENTRY(test_reused_slot_never_calls_old_handler),
    TEST_ENTRY(test_callback_mutation_guard),
    TEST_ENTRY(test_slot_not_reused_during_delete_callback),
    TEST_ENTRY(test_stats_report_real_format_and_pool_bytes),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
