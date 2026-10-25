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

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

TEST_LIST_BEGIN
    TEST_ENTRY(test_public_widget_types_compile),
    TEST_ENTRY(test_empty_registry_rejects_unknown),
    TEST_ENTRY(test_widget_tree_lifecycle),
    TEST_ENTRY(test_text_copies_input),
    TEST_ENTRY(test_slot_reuse_resets_metadata),
    TEST_ENTRY(test_foreign_parent_rejected),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
