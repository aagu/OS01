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

TEST_LIST_BEGIN
    TEST_ENTRY(test_public_widget_types_compile),
    TEST_ENTRY(test_empty_registry_rejects_unknown),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
