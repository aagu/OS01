/*
 * gui_widget_public_consumer.c — 纯类型 consumer（公共头发布边界的语法探针）
 *
 * 这个 TU **不是**测试用例：它只用来证明"只包含公共头的消费者"在
 * C11 与 C++17 下都能编译，并且只引用**本阶段已经实现**的公共符号。
 *
 * 它刻意不链接：`-fsyntax-only` 就够了（链接验证由发布审计的 target link
 * probe 负责）。
 */

#include <gui/gui.h>

/* 句柄是值类型：零值构造在 C 与 C++ 下都要成立。 */
static gui_widget_t probe_none(void)
{
    return GUI_WIDGET_NONE;
}

int gui_widget_public_consumer(void)
{
    gui_widget_t widget = probe_none();
    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;
    gui_rect_t rect = { 0, 0, 0, 0 };

    bool valid = gui_widget_is_valid(0, widget);
    int rc = gui_widget_get_kind(0, widget, &kind);

    (void)valid;
    (void)rc;
    (void)rect;
    (void)kind;
    return 0;
}
