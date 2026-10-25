/*
 * test_gui_widget.c — 公共控件句柄与注册表契约（GUI-2b Task 1）
 *
 * 本文件是**公共契约**测试：它只包含 gui 目录下的公共头、标准库、测试框架和宿主
 * 私有 fixture 头。fixture 头允许出现后端类型，但断言本身全部通过公共
 * API 与值句柄表达。
 */

#include "test_framework.h"

#include <gui/gui.h>
#include <gui/gui_widget.h>

#include "gui_native_fixture.h"
#include "gui_test_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * 句柄必须是**值类型**：它不能被隐式当成 void* 传给后端式接口。
 *
 * 这条用编译期探针表达：如果句柄是指针形状（或者任何可以隐式转成
 * void* 的类型），下面的类型探针会返回 1，测试随即失败。
 */
#define GUI_TEST_HANDLE_IS_POINTER_LIKE(x) \
    _Generic((x), void *: 1, gui_widget_t *: 1, const gui_widget_t *: 1, default: 0)

/* The platform TU's host-only header tamper switch.  Every GUI host
 * fixture defines it; the widget fixture never flips it. */
bool g_gui_test_tamper_header = false;

TEST_FUNC(test_public_widget_types_compile)
{
    /* 种类哨兵是公共契约的一部分：恰好三种，且 COUNT 不得省略。 */
    assert_eq(3, GUI_WIDGET_KIND_COUNT);
    assert_eq(0, GUI_WIDGET_CONTAINER);
    assert_eq(1, GUI_WIDGET_LABEL);
    assert_eq(2, GUI_WIDGET_BUTTON);

    /* NONE 是零值构造表达式，不是指针、不是野值。 */
    gui_widget_t none = GUI_WIDGET_NONE;
    assert_eq(0, none.id);

    /* 默认构造（C11：聚合初始化）同样得到零身份。 */
    gui_widget_t zeroed = { 0 };
    assert_eq(0, zeroed.id);

    /* 值类型：句柄只有一个 64 位身份号。 */
    assert_true(sizeof(gui_widget_t) == sizeof(uint64_t));

    /* 值句柄不能被隐式传成 void*：它是身份，不是指针。 */
    assert_eq(0, GUI_TEST_HANDLE_IS_POINTER_LIKE(none));

    /* 句柄可以整体比较与传递（值语义）。 */
    gui_widget_t copy = none;
    assert_true(copy.id == none.id);
}

TEST_FUNC(test_empty_registry_rejects_unknown)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    /* 空注册表：fixture 不会偷偷建一个根。 */
    assert_true(fx.root.id == 0);

    /* 零句柄永远不是有效身份。 */
    assert_false(gui_widget_is_valid(fx.platform, GUI_WIDGET_NONE));

    /* 随机身份也不是有效身份（它从未被发出过）。 */
    gui_widget_t random_id = { 0x0123456789ABCDEFull };
    assert_false(gui_widget_is_valid(fx.platform, random_id));

    /* get_kind：无效身份返回 EINVAL，并且输出被清成非法哨兵。 */
    gui_widget_kind_t kind = GUI_WIDGET_CONTAINER;
    errno = 0;
    assert_eq(-1, gui_widget_get_kind(fx.platform, GUI_WIDGET_NONE, &kind));
    assert_eq(EINVAL, errno);
    assert_eq(GUI_WIDGET_KIND_COUNT, kind);

    kind = GUI_WIDGET_CONTAINER;
    errno = 0;
    assert_eq(-1, gui_widget_get_kind(fx.platform, random_id, &kind));
    assert_eq(EINVAL, errno);
    assert_eq(GUI_WIDGET_KIND_COUNT, kind);

    /* 输出指针为 NULL 是参数错误：先校验指针，不碰输出。 */
    errno = 0;
    assert_eq(-1, gui_widget_get_kind(fx.platform, GUI_WIDGET_NONE, NULL));
    assert_eq(EINVAL, errno);

    gui_native_fixture_destroy(&fx);
}

/* ── 像素取样（离屏画布） ────────────────────────────────────────────
 *
 * 文字断言必须落在**像素**上：只断言"槽里存的是哪串字"证明不了调用方
 * 的缓冲真的被复制了。
 */
#define REGION_W 200
#define REGION_H 40

static uint32_t g_region_a[REGION_W * REGION_H];
static uint32_t g_region_b[REGION_W * REGION_H];

static void capture_region(gui_platform_t *platform, int32_t x, int32_t y, uint32_t *out)
{
    for (uint32_t row = 0; row < REGION_H; row++) {
        for (uint32_t col = 0; col < REGION_W; col++) {
            out[row * REGION_W + col] =
                gui_platform_get_pixel(platform, (uint32_t)(x + (int32_t)col),
                                       (uint32_t)(y + (int32_t)row));
        }
    }
}

/* 区域里"有内容"的像素数：与背景取样点不同的像素。 */
static uint32_t count_ink(const uint32_t *region, uint32_t background)
{
    uint32_t ink = 0;
    for (uint32_t i = 0; i < REGION_W * REGION_H; i++) {
        if (region[i] != background) {
            ink++;
        }
    }
    return ink;
}


TEST_FUNC(test_widget_tree_lifecycle)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    /* 根：没有父控件的容器，铺满平台屏幕。 */
    gui_widget_t root;
    errno = 0;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    assert_true(root.id != 0);
    assert_true(gui_widget_is_valid(fx.platform, root));

    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;
    assert_eq(0, gui_widget_get_kind(fx.platform, root, &kind));
    assert_eq(GUI_WIDGET_CONTAINER, kind);

    gui_rect_t bounds;
    assert_eq(0, gui_widget_get_bounds(fx.platform, root, &bounds));
    assert_eq(640, bounds.width);
    assert_eq(480, bounds.height);

    /* 子项：根容器拥有 LABEL 与 BUTTON。 */
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));
    assert_true(label.id != 0);
    assert_true(button.id != 0);
    assert_true(label.id != root.id);
    assert_true(button.id != label.id);

    assert_eq(0, gui_widget_get_kind(fx.platform, label, &kind));
    assert_eq(GUI_WIDGET_LABEL, kind);
    assert_eq(0, gui_widget_get_kind(fx.platform, button, &kind));
    assert_eq(GUI_WIDGET_BUTTON, kind);

    /* 只有容器可以是根：没有父控件的标签被拒绝。 */
    gui_widget_t orphan;
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_LABEL, &orphan));
    assert_eq(EINVAL, errno);
    assert_true(orphan.id == 0);

    /* 非法种类被拒绝，输出身份清零。 */
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, root, GUI_WIDGET_KIND_COUNT, &orphan));
    assert_eq(EINVAL, errno);
    assert_true(orphan.id == 0);

    /* 空输出指针先被校验：不会造出半对象。 */
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, NULL));
    assert_eq(EINVAL, errno);

    /* clear：子项消失，容器留下。 */
    assert_eq(0, gui_widget_clear(fx.platform, root));
    assert_true(gui_widget_is_valid(fx.platform, root));
    assert_false(gui_widget_is_valid(fx.platform, label));
    assert_false(gui_widget_is_valid(fx.platform, button));

    /* clear 只对容器开放，set_text 只对标签开放。 */
    gui_widget_t label2;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label2));
    gui_widget_t button2;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button2));
    errno = 0;
    assert_eq(-1, gui_widget_clear(fx.platform, label2));
    assert_eq(EOPNOTSUPP, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_text(fx.platform, root, "x"));
    assert_eq(EOPNOTSUPP, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_text(fx.platform, button2, "x"));
    assert_eq(EOPNOTSUPP, errno);

    /* destroy 递归：删父之后所有旧身份都失效。 */
    assert_eq(0, gui_widget_destroy(fx.platform, root));
    assert_false(gui_widget_is_valid(fx.platform, root));
    assert_false(gui_widget_is_valid(fx.platform, label2));

    /* 已销毁的身份不再接受操作。 */
    errno = 0;
    assert_eq(-1, gui_widget_destroy(fx.platform, root));
    assert_eq(EINVAL, errno);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_text_copies_input)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    char buffer[16];
    memcpy(buffer, "HELLO", 6);
    assert_eq(0, gui_widget_set_text(fx.platform, label, buffer));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_rect_t bounds;
    assert_eq(0, gui_widget_get_bounds(fx.platform, label, &bounds));
    assert_true(bounds.width > 0);
    assert_true(bounds.height > 0);
    capture_region(fx.platform, bounds.x, bounds.y, g_region_a);

    /* 文字确实画在了画布上（否则后面的比较都是空洞的）。 */
    uint32_t background = gui_platform_get_pixel(fx.platform, 600, 400);
    assert_true(count_ink(g_region_a, background) > 0);

    /* 调用方紧接着就把缓冲改掉：标签必须仍然显示**旧副本**。 */
    memcpy(buffer, "WORLD", 6);
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    capture_region(fx.platform, bounds.x, bounds.y, g_region_b);
    assert_mem_eq(g_region_a, g_region_b, sizeof(g_region_a));

    /* 非空洞：换一段文字必须画出不同的像素。 */
    assert_eq(0, gui_widget_set_text(fx.platform, label, "WORLD"));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    capture_region(fx.platform, bounds.x, bounds.y, g_region_b);
    assert_true(memcmp(g_region_a, g_region_b, sizeof(g_region_a)) != 0);

    /* NULL 文字按空串处理。 */
    assert_eq(0, gui_widget_set_text(fx.platform, label, NULL));
    assert_eq(0, gui_native_fixture_step(&fx, 50));

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_slot_reuse_resets_metadata)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    /* 参考：一个从未被别的身份占用的槽。 */
    gui_widget_t reference;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &reference));
    assert_eq(0, gui_widget_set_text(fx.platform, reference, "NEW"));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    gui_rect_t reference_bounds;
    assert_eq(0, gui_widget_get_bounds(fx.platform, reference, &reference_bounds));
    capture_region(fx.platform, reference_bounds.x, reference_bounds.y, g_region_a);
    assert_eq(0, gui_widget_destroy(fx.platform, reference));

    /* 旧身份：同样的槽，带文字、隐藏、禁用，并被塞进陈旧元数据。 */
    gui_widget_t stale;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &stale));
    assert_eq(0, gui_widget_set_text(fx.platform, stale, "STALE"));
    assert_eq(0, gui_widget_set_visible(fx.platform, stale, false));
    assert_eq(0, gui_widget_set_enabled(fx.platform, stale, false));
    /* 槽下标必须在它还活着的时候记下来：销毁之后身份就查不到了。 */
    gui_native_slot_info_t stale_info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, stale, &stale_info));
    assert_eq(GUI_NATIVE_SLOT_LIVE, stale_info.state);
    assert_eq(0, gui_native_fixture_plant_slot(fx.platform, stale, 0xffff0000u, "STALE"));
    assert_eq(0, gui_widget_destroy(fx.platform, stale));
    assert_false(gui_widget_is_valid(fx.platform, stale));

    /* 复用的槽：种类不能继承。 */
    gui_widget_t reused;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &reused));
    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;
    assert_eq(0, gui_widget_get_kind(fx.platform, reused, &kind));
    assert_eq(GUI_WIDGET_BUTTON, kind);

    /* 旧身份不会随槽复活。 */
    assert_false(gui_widget_is_valid(fx.platform, stale));

    /* 复用的槽：陈旧的 handler、userdata、文字、几何缓存与标志全清。 */
    gui_native_slot_info_t info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, reused, &info));
    assert_eq(GUI_NATIVE_SLOT_LIVE, info.state);
    assert_eq(GUI_WIDGET_BUTTON, info.kind);
    assert_false(info.has_handler);
    assert_false(info.has_userdata);
    assert_false(info.has_text);
    /* 陈旧标志一个都不许留下，剩下的只有新建控件自己的默认标志。 */
    assert_eq(0u, info.flags & 0xffff0000u);
    assert_eq(GUI_NATIVE_FLAG_VISIBLE | GUI_NATIVE_FLAG_ENABLED | GUI_NATIVE_FLAG_CLICKABLE,
              info.flags);
    assert_eq(0, info.geom_x);
    assert_eq(0, info.geom_y);
    assert_eq(0, info.geom_width);
    assert_eq(0, info.geom_height);

    /* 槽真的被复用了：新旧身份落在**同一个槽下标**上。否则上面的断言
     * 只是"新槽是干净的"，根本没有测到复用。 */
    assert_eq(stale_info.index, info.index);

    assert_eq(0, gui_widget_destroy(fx.platform, reused));

    /* 复用的槽：文字、可见、使能都不能继承。 */
    gui_widget_t fresh;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &fresh));
    assert_true(fresh.id != stale.id);
    assert_eq(0, gui_widget_set_text(fx.platform, fresh, "NEW"));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    gui_rect_t fresh_bounds;
    assert_eq(0, gui_widget_get_bounds(fx.platform, fresh, &fresh_bounds));
    capture_region(fx.platform, fresh_bounds.x, fresh_bounds.y, g_region_b);

    /* 与参考逐像素相同：旧文字没留下、隐藏没留下、禁用没留下。 */
    assert_mem_eq(g_region_a, g_region_b, sizeof(g_region_a));

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_foreign_parent_rejected)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    /* 在第一个平台上建一个根。 */
    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    assert_true(root.id != 0);

    /* 平台销毁：身份不延长对象寿命。 */
    gui_native_fixture_destroy(&fx);
    assert_false(gui_widget_is_valid(NULL, root));

    /* 另一个平台：旧身份是"外平台身份"，必须被拒绝。 */
    assert_eq(0, gui_native_fixture_create(&fx, false));
    gui_widget_t child;
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &child));
    assert_eq(EINVAL, errno);
    assert_true(child.id == 0);

    /* 同一个身份在旧平台上也不再可用。 */
    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_get_kind(fx.platform, root, &kind));
    assert_eq(EINVAL, errno);
    assert_eq(GUI_WIDGET_KIND_COUNT, kind);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_ids_survive_platform_recreation)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t first;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &first));
    assert_true(first.id != 0);
    uint64_t first_id = first.id;

    /* 平台销毁并重建：身份计数器是进程全局的，不被重置。 */
    gui_native_fixture_destroy(&fx);
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t second;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &second));
    assert_true(second.id > first_id);

    /* 旧身份在新平台上不会复活。 */
    assert_false(gui_widget_is_valid(fx.platform, first));

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_old_id_never_revives_after_slot_reuse)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    gui_widget_t old_widget;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &old_widget));
    uint64_t old_id = old_widget.id;
    /* 槽下标必须在它还活着的时候记下来：销毁之后身份就查不到了。 */
    gui_native_slot_info_t old_info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, old_widget, &old_info));
    assert_eq(GUI_NATIVE_SLOT_LIVE, old_info.state);
    assert_eq(0, gui_widget_destroy(fx.platform, old_widget));
    assert_false(gui_widget_is_valid(fx.platform, old_widget));

    /* 槽回来了，身份没有回来。 */
    gui_widget_t fresh;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &fresh));
    assert_true(fresh.id != old_id);
    assert_false(gui_widget_is_valid(fx.platform, old_widget));

    /* 新旧身份落在**同一个槽下标**上：槽确实被复用了。 */
    gui_native_slot_info_t info;
    assert_eq(0, gui_native_fixture_query_slot(fx.platform, fresh, &info));
    assert_eq(old_info.index, info.index);

    gui_native_fixture_destroy(&fx);
}

/* 身份耗尽：独立子进程把计数器推到末值，主进程不受影响。 */
static int run_identity_exhaustion_child(gui_platform_t *platform)
{
    gui_native_fixture_set_identity_last_id(UINT64_MAX);

    gui_widget_t out;
    errno = 0;
    if (gui_widget_create(platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &out) != -1) {
        return 1; /* 身份号本该耗尽 */
    }
    if (errno != EOVERFLOW) {
        return 2;
    }
    /* 绝不许回绕：再来一次还是耗尽，而不是发出一个小身份号。 */
    errno = 0;
    if (gui_widget_create(platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &out) != -1) {
        return 3;
    }
    if (errno != EOVERFLOW) {
        return 4;
    }
    if (gui_native_fixture_get_identity_last_id() != UINT64_MAX) {
        return 5; /* 计数器被改动了 */
    }
    return 0;
}

TEST_FUNC(test_identity_exhaustion_does_not_wrap)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    pid_t pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        int rc = run_identity_exhaustion_child(fx.platform);
        _exit(rc);
    }

    int status = 0;
    assert_eq(pid, waitpid(pid, &status, 0));
    assert_true(WIFEXITED(status));
    assert_eq(0, WEXITSTATUS(status));

    /* 主进程的身份计数器没有被污染：后续测试照常发号。 */
    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    assert_true(root.id != 0);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_1024_widgets_and_1025th_enospc)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    /* 根也计入上限：第 1024 个原生控件是最后一个。 */
    for (uint32_t i = 1; i < GUI_NATIVE_CAPACITY; i++) {
        gui_widget_t label;
        errno = 0;
        assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));
        assert_true(label.id != 0);
    }

    /* 第 1025 个被拒绝，输出身份清零。 */
    gui_widget_t overflow;
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &overflow));
    assert_eq(ENOSPC, errno);
    assert_true(overflow.id == 0);

    gui_native_registry_info_t info;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &info));
    assert_eq(GUI_NATIVE_CAPACITY, info.count);
    assert_eq(GUI_NATIVE_CAPACITY, info.peak);
    assert_eq(GUI_NATIVE_CAPACITY, info.capacity);

    /* 容量被占满时，已有的控件不受影响。 */
    assert_true(gui_widget_is_valid(fx.platform, root));

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_create_failure_rolls_back_reserved_slot)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    gui_native_registry_info_t before;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &before));

    /* 观察者安装失败：占用的槽必须被回滚。 */
    g_gui_test_fail_widget_observer = true;
    gui_widget_t failed;
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &failed));
    assert_eq(ENOMEM, errno);
    assert_true(failed.id == 0);
    g_gui_test_fail_widget_observer = false;

    gui_native_registry_info_t after;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &after));
    assert_eq(before.count, after.count);

    /* 回滚之后槽还能用：容量没有被这次失败吃掉。 */
    gui_widget_t good;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &good));
    assert_true(good.id != 0);

    gui_native_registry_info_t final_info;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &final_info));
    assert_eq(before.count + 1, final_info.count);

    gui_native_fixture_destroy(&fx);
}

TEST_FUNC(test_invalid_call_leaves_registry_unchanged)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    gui_native_registry_info_t before;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &before));

    gui_widget_t out;
    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;
    gui_rect_t rect;
    gui_widget_t unknown = { 0 };
    unknown.id = 0x5eed5eedull;

    /* 一批非法调用：一个都不能改动注册表。 */
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_LABEL, &out));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, root, GUI_WIDGET_KIND_COUNT, &out));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_create(NULL, root, GUI_WIDGET_LABEL, &out));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, NULL));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, unknown, GUI_WIDGET_LABEL, &out));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, label, GUI_WIDGET_LABEL, &out));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_destroy(fx.platform, unknown));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_clear(fx.platform, label));
    assert_eq(EOPNOTSUPP, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_text(fx.platform, root, "x"));
    assert_eq(EOPNOTSUPP, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_visible(fx.platform, unknown, true));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_set_enabled(fx.platform, unknown, true));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_get_kind(fx.platform, unknown, &kind));
    assert_eq(EINVAL, errno);
    errno = 0;
    assert_eq(-1, gui_widget_get_bounds(fx.platform, unknown, &rect));
    assert_eq(EINVAL, errno);

    gui_native_registry_info_t after;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &after));
    assert_eq(before.count, after.count);
    assert_eq(before.peak, after.peak);

    /* 活着的控件一点没变。 */
    assert_true(gui_widget_is_valid(fx.platform, root));
    assert_true(gui_widget_is_valid(fx.platform, label));
    assert_eq(0, gui_widget_get_kind(fx.platform, label, &kind));
    assert_eq(GUI_WIDGET_LABEL, kind);

    gui_native_fixture_destroy(&fx);
}

/* 50 轮建树/拆树之后，原生计数、后端池与分配/释放配对都要回到基线。 */
TEST_FUNC(test_tree_churn_returns_to_baseline)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    size_t pool_free_before = 0;
    assert_eq(0, gui_native_fixture_pool_free(fx.platform, &pool_free_before));
    gui_native_registry_info_t before;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &before));

    for (int round = 0; round < 50; round++) {
        gui_widget_t root;
        assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
        for (int i = 0; i < 4; i++) {
            gui_widget_t child;
            assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &child));
            assert_eq(0, gui_widget_set_text(fx.platform, child, "churn"));
        }
        assert_eq(0, gui_widget_destroy(fx.platform, root));
    }

    gui_native_registry_info_t after;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &after));
    assert_eq(before.count, after.count);
    /* 峰值是**历史高水位**：它只涨不跌，证明这 50 轮真的占过槽。 */
    assert_true(after.peak >= before.peak);
    assert_true(after.peak >= 5u);

    size_t pool_free_after = 0;
    assert_eq(0, gui_native_fixture_pool_free(fx.platform, &pool_free_after));
    /* 分配与释放配对：后端池回到基线（容许后端自己的碎片余量）。 */
    assert_true(pool_free_after + 4096 >= pool_free_before);

    gui_native_fixture_destroy(&fx);
}

/* 输入的生命周期与控件树互不干扰：单独关输入不删控件，新输入不继承登记。 */
TEST_FUNC(test_input_lifecycle_leaves_widgets_alone)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, true));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    assert_true(gui_widget_is_valid(fx.platform, root));

    /* 单独关输入：控件树不受影响。 */
    assert_not_null(fx.input);
    gui_input_destroy(fx.input);
    fx.input = NULL;
    assert_true(gui_widget_is_valid(fx.platform, root));

    /* 新输入不继承任何登记。 */
    gui_input_config_t icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.mode = GUI_INPUT_SYNTHETIC;
    icfg.require_mouse = false;
    fx.input = gui_input_create(fx.platform, &icfg);
    assert_not_null(fx.input);
    assert_true(gui_widget_is_valid(fx.platform, root));

    gui_native_registry_info_t info;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &info));
    assert_eq(1u, info.count);

    gui_native_fixture_destroy(&fx);
}

/* 平台表分配失败：平台创建必须失败并干净回滚。 */
TEST_FUNC(test_platform_table_fault_rolls_back)
{
    gui_test_reset();

    gui_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = GUI_MODE_OFFSCREEN;
    cfg.width = 640;
    cfg.height = 480;
    cfg.buffer_lines = 40;

    g_gui_test_fail_widget_registry = true;
    errno = 0;
    gui_platform_t *platform = gui_platform_create(&cfg);
    assert_null(platform);
    assert_eq(ENOMEM, errno);
    g_gui_test_fail_widget_registry = false;

    /* 没有半成品平台：紧接着创建成功。 */
    platform = gui_platform_create(&cfg);
    assert_not_null(platform);
    gui_platform_destroy(platform);
}

/* 后端在注册表背后自行删除对象：身份必须立刻失效。 */
TEST_FUNC(test_backend_delete_invalidates_identity)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));

    gui_native_registry_info_t before;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &before));

    /* 内部 fixture：后端直接删掉原生对象。 */
    assert_eq(0, gui_native_fixture_backend_delete(fx.platform, label));

    /* 公开面只看值句柄：身份已经没了。 */
    assert_false(gui_widget_is_valid(fx.platform, label));
    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;
    errno = 0;
    assert_eq(-1, gui_widget_get_kind(fx.platform, label, &kind));
    assert_eq(EINVAL, errno);
    assert_eq(GUI_WIDGET_KIND_COUNT, kind);

    /* 槽被回收：容量没有被这次外部删除吃掉。 */
    gui_widget_t fresh;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &fresh));
    gui_native_registry_info_t after;
    assert_eq(0, gui_native_fixture_query_registry(fx.platform, &after));
    assert_eq(before.count, after.count);

    gui_native_fixture_destroy(&fx);
}

/* 后端 fatal OOM：已有的独立子进程退出契约不变。 */
static int run_backend_fatal_oom_child(gui_platform_t *platform)
{
    size_t free_bytes = 0;
    if (gui_native_fixture_pool_free(platform, &free_bytes) != 0) {
        return 10;
    }
    if (free_bytes < 65536) {
        return 11;
    }

    gui_widget_t root;
    if (gui_widget_create(platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root) != 0) {
        return 12;
    }
    gui_widget_t label;
    if (gui_widget_create(platform, root, GUI_WIDGET_LABEL, &label) != 0) {
        return 14;
    }

    /* 一段比后端池还大的文字：桥接里的原生分配无法满足，后端按既有契约
     * 直接终止进程（LV_ASSERT_HANDLER = _exit(1)）。 */
    size_t huge = free_bytes - 256;
    char *text = (char *)malloc(huge + 1);
    if (!text) {
        return 13;
    }
    memset(text, 'A', huge);
    text[huge] = '\0';
    (void)gui_widget_set_text(platform, label, text);

    /* 走到这里说明 OOM 没有按契约终止：失败。 */
    return 0;
}

TEST_FUNC(test_backend_fatal_oom_exits_in_subprocess)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    pid_t pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        int rc = run_backend_fatal_oom_child(fx.platform);
        _exit(rc);
    }

    int status = 0;
    assert_eq(pid, waitpid(pid, &status, 0));
    /* 后端 fatal OOM 的既有退出契约：进程直接退出，状态码 1。 */
    assert_true(WIFEXITED(status));
    assert_eq(1, WEXITSTATUS(status));

    gui_native_fixture_destroy(&fx);
}

/* 平台关闭：原生树随平台一起消失，池回到基线。 */
TEST_FUNC(test_platform_destroy_releases_widget_tree)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    for (int i = 0; i < 8; i++) {
        gui_widget_t child;
        assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &child));
        assert_eq(0, gui_widget_set_text(fx.platform, child, "gone"));
    }

    /* 带活着的树销毁平台：原生树先删，display 后删，注册表最后释放。 */
    gui_native_fixture_destroy(&fx);

    /* 紧接着的平台必须完好：没有半释放的后端状态。 */
    gui_native_fixture_t again;
    assert_eq(0, gui_native_fixture_create(&again, false));
    gui_widget_t fresh;
    assert_eq(0, gui_widget_create(again.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &fresh));
    assert_true(gui_widget_is_valid(again.platform, fresh));
    gui_native_fixture_destroy(&again);
}

/* 发现项 1：BUTTON 可以作父，LABEL 是叶子。 */
TEST_FUNC(test_button_can_parent_label_child)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));

    /* BUTTON 可以拥有子项（Task 10 把卡片容器换成 BUTTON，按钮的标题
     * 必须是 LABEL 子项）。 */
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_widget_t caption;
    errno = 0;
    assert_eq(0, gui_widget_create(fx.platform, button, GUI_WIDGET_LABEL, &caption));
    assert_eq(0, errno);

    /* 标题的文字真的生效（像素可观测）。 */
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    gui_rect_t bounds;
    assert_eq(0, gui_widget_get_bounds(fx.platform, caption, &bounds));
    capture_region(fx.platform, bounds.x, bounds.y, g_region_a);

    char buffer[16];
    memcpy(buffer, "BACK", 5);
    assert_eq(0, gui_widget_set_text(fx.platform, caption, buffer));
    assert_eq(0, gui_native_fixture_step(&fx, 50));
    capture_region(fx.platform, bounds.x, bounds.y, g_region_b);
    assert_true(memcmp(g_region_a, g_region_b, sizeof(g_region_a)) != 0);

    /* BUTTON 也可以拥有 CONTAINER 子项。 */
    gui_widget_t panel;
    assert_eq(0, gui_widget_create(fx.platform, button, GUI_WIDGET_CONTAINER, &panel));
    assert_true(gui_widget_is_valid(fx.platform, panel));

    /* LABEL 是叶子：不能作父。 */
    gui_widget_t orphan;
    errno = 0;
    assert_eq(-1, gui_widget_create(fx.platform, caption, GUI_WIDGET_LABEL, &orphan));
    assert_eq(EINVAL, errno);
    assert_true(orphan.id == 0);

    gui_native_fixture_destroy(&fx);
}

/* 发现项 3：命中与显示组的默认行为。 */
TEST_FUNC(test_native_hit_test_and_display_defaults)
{
    gui_native_fixture_t fx;
    assert_eq(0, gui_native_fixture_create(&fx, false));

    gui_widget_t root;
    assert_eq(0, gui_widget_create(fx.platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &root));
    gui_widget_t label;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_LABEL, &label));
    gui_widget_t button;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button));

    gui_native_backend_info_t backend;

    /* BUTTON 可点击。 */
    assert_eq(0, gui_native_fixture_query_backend(fx.platform, button, &backend));
    assert_true(backend.clickable);

    /* LABEL 不抢命中。 */
    assert_eq(0, gui_native_fixture_query_backend(fx.platform, label, &backend));
    assert_false(backend.clickable);

    /* 装饰性 CONTAINER 默认不可点击。 */
    assert_eq(0, gui_native_fixture_query_backend(fx.platform, root, &backend));
    assert_false(backend.clickable);

    /* 每个对象都在本平台的 display 上，且不属于任何 lv_group。 */
    assert_true(backend.platform_display);
    assert_false(backend.in_group);
    assert_eq(0, gui_native_fixture_query_backend(fx.platform, label, &backend));
    assert_true(backend.platform_display);
    assert_false(backend.in_group);
    assert_eq(0, gui_native_fixture_query_backend(fx.platform, button, &backend));
    assert_true(backend.platform_display);
    assert_false(backend.in_group);

    /* 原生创建不走 lv_display_get_default()：把 LVGL 的默认 display 清掉
     * 之后，创建出来的对象仍然落在本平台的 display 上。若实现取了默认
     * display，这里会拿到空指针而直接失败。 */
    assert_eq(0, gui_native_fixture_set_default_display_none());
    gui_widget_t button2;
    assert_eq(0, gui_widget_create(fx.platform, root, GUI_WIDGET_BUTTON, &button2));
    assert_eq(0, gui_native_fixture_query_backend(fx.platform, button2, &backend));
    assert_true(backend.platform_display);
    assert_false(backend.default_display);
    assert_eq(0, gui_native_fixture_restore_default_display(fx.platform));

    gui_native_fixture_destroy(&fx);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_public_widget_types_compile),
    TEST_ENTRY(test_empty_registry_rejects_unknown),
    TEST_ENTRY(test_widget_tree_lifecycle),
    TEST_ENTRY(test_text_copies_input),
    TEST_ENTRY(test_slot_reuse_resets_metadata),
    TEST_ENTRY(test_foreign_parent_rejected),
    TEST_ENTRY(test_ids_survive_platform_recreation),
    TEST_ENTRY(test_old_id_never_revives_after_slot_reuse),
    TEST_ENTRY(test_identity_exhaustion_does_not_wrap),
    TEST_ENTRY(test_1024_widgets_and_1025th_enospc),
    TEST_ENTRY(test_create_failure_rolls_back_reserved_slot),
    TEST_ENTRY(test_invalid_call_leaves_registry_unchanged),
    TEST_ENTRY(test_tree_churn_returns_to_baseline),
    TEST_ENTRY(test_input_lifecycle_leaves_widgets_alone),
    TEST_ENTRY(test_platform_table_fault_rolls_back),
    TEST_ENTRY(test_backend_delete_invalidates_identity),
    TEST_ENTRY(test_backend_fatal_oom_exits_in_subprocess),
    TEST_ENTRY(test_platform_destroy_releases_widget_tree),
    TEST_ENTRY(test_button_can_parent_label_child),
    TEST_ENTRY(test_native_hit_test_and_display_defaults),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
