#ifndef _GUI_NATIVE_FIXTURE_H
#define _GUI_NATIVE_FIXTURE_H

/*
 * gui_native_fixture.h — 宿主私有的原生控件 fixture（GUI-2b）
 *
 * 这是**宿主专用**的私有 fixture：它可以出现后端类型（LVGL、鼠标事件），
 * 但它不进入公共头，也不进入目标库。公共契约测试自身只包含
 * gui 目录下的公共头、标准库、测试框架和本头。
 *
 * fixture 建在既有的 `gui_test_runtime` mock 之上（假时钟、分配故障注入），
 * 不复制第二份 mock。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include <gui/gui_widget.h>
#include <uapi/mouse.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 宿主原生控件 fixture
 */
typedef struct {
    gui_platform_t *platform; /* 离屏平台（640x480） */
    gui_input_t *input;       /* 可选：合成输入 */
    gui_widget_t root;        /* 根控件身份；由测试显式创建，初始为 NONE */
    uint32_t delay;           /* 最近一次 step 给出的建议等待时间 */
} gui_native_fixture_t;

/**
 * @brief 建立 fixture
 *
 * 先复位宿主 mock（`gui_test_reset()`），再建一个明确的 OFFSCREEN
 * 640x480 平台；`with_input` 为真时再挂一个 SYNTHETIC 输入。
 *
 * `root` 初始化为 `GUI_WIDGET_NONE`：**注册表测试不会偷偷多出一个根**，
 * 需要根的测试自己显式创建。
 *
 * @return 0 成功；-1 失败（errno 已设置；失败时反向清理并保留 errno）
 */
int gui_native_fixture_create(gui_native_fixture_t *fx, bool with_input);

/**
 * @brief 拆除 fixture（幂等）
 *
 * 释放顺序：输入 → 平台；随后清空所有指针。
 */
void gui_native_fixture_destroy(gui_native_fixture_t *fx);

/**
 * @brief 推进假时钟并跑一次真实的 `gui_platform_step()`
 *
 * @return 0 成功；-1 失败（平台状态不是 OK）
 */
int gui_native_fixture_step(gui_native_fixture_t *fx, uint64_t advance_ms);

/**
 * @brief 把合成的 Set 1 字节喂进真实的解码器/FIFO 路径
 *
 * 注入之后由测试显式 step。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_keys(gui_native_fixture_t *fx, const uint8_t *bytes, size_t len);

/**
 * @brief 把合成的鼠标事件喂进真实的 FIFO/适配层路径
 *
 * 注入之后由测试显式 step。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_mouse(gui_native_fixture_t *fx, const mouse_event_t *events, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_NATIVE_FIXTURE_H */
