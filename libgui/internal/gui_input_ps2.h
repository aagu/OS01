#ifndef _GUI_INPUT_PS2_H
#define _GUI_INPUT_PS2_H

/*
 * libgui/internal/gui_input_ps2.h — 兼容转发头（COMPAT FORWARDER）。
 *
 * 纯用户态 PS/2 解码器的接口已经公开为 <gui/gui_input_ps2.h>：类型
 * 定义、槽位常量与三个函数都在那里，本文件**不再维护第二份定义**。
 *
 * 这个转发头只为仍在使用旧包含路径的内部实现保留。新的代码——尤其是
 * 独立的应用消费者——应当直接包含公开头 <gui/gui_input_ps2.h>，
 * 并且不得为此添加 libgui/internal 的包含路径。
 */

#include <gui/gui_input_ps2.h>

#endif /* _GUI_INPUT_PS2_H */
