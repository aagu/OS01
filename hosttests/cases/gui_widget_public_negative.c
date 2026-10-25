/*
 * gui_widget_public_negative.c — 负控制：值句柄**不得**能隐式传给 void*
 *
 * 这个 TU **必须编译失败**：公共句柄是值类型（身份号），不是指针。
 * 如果它能被隐式当成 void* 递出去，业务代码就能把任意地址伪装成控件
 * 身份，发布边界就破了。
 *
 * 构建里的负控制检查要求这一步编译失败；一旦它编译通过，检查即失败。
 */

#include <gui/gui.h>

static void takes_void_ptr(void *opaque)
{
    (void)opaque;
}

int gui_widget_public_negative(void)
{
    gui_widget_t widget = GUI_WIDGET_NONE;
    takes_void_ptr(widget);
    return 0;
}
