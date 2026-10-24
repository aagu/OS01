#ifndef _GUI_PLATFORM_PRIVATE_H
#define _GUI_PLATFORM_PRIVATE_H

/*
 * gui_platform_private.h — 平台共享状态（内部桥接头）
 *
 * `struct gui_platform` 从 platform.c 原样迁移到这里：字段、顺序、注释
 * 一律不变，只在末尾新增一个 `gui_input_t *input`（GUI-2 的每平台唯一
 * 输入对象）。
 *
 * 公共的 `gui_platform_t` 仍然是不透明类型，`gui_config_t` 不变：本头
 * 文件不进入 sysroot，只在 libgui 内部与宿主测试之间共享。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include <gfx.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gui_platform {
    gui_mode_t mode;
    uint32_t width;
    uint32_t height;
    uint32_t buffer_lines;

    /* 绘制缓冲（匿名 mmap 分配） */
    uint32_t *draw_buf;
    size_t draw_buf_bytes;

    /* 离屏模式影子画幅（匿名 mmap 分配，仅离屏模式有效） */
    uint32_t *shadow_canvas;
    size_t shadow_canvas_bytes;

    /* 屏幕模式关联设备 */
    gfx_handle_t *gfx;
    bool acquired;
    bool pending_present;         /* flush 后置位，present 成功后清除 */
    gui_status_t terminal_status; /* OK 表示仍活跃，其余终止态锁存 */
    int terminal_errno;
    int flush_error;              /* flush 无返回值，供 step 收敛错误 */
    uint64_t retry_deadline_ms;   /* EAGAIN 限频退避截止时间 (CLOCK_MONOTONIC ms) */

    /* 内部关联对象 */
    lv_display_t *disp;
    uint32_t flush_count;

    /* GUI-3：Material 主题。与平台同生共死 —— 主题的样式缓冲在 LVGL
     * 内存池里，必须在 lv_deinit() 之前释放。 */
    lv_theme_t *theme;

    /* GUI-2：每平台至多一个输入对象（NULL 表示未挂接） */
    gui_input_t *input;
};

/* ── 平台内部辅助（platform.c 实现，input.c 使用） ───────────── */

/**
 * @brief 平台是否仍然存活（未进入终止态）
 */
bool gui_platform_alive(const gui_platform_t *platform);

/**
 * @brief 平台是否已经挂接了输入对象
 */
bool gui_platform_input_attached(const gui_platform_t *platform);

/**
 * @brief 挂接输入对象（每平台至多一个）
 */
void gui_platform_attach_input(gui_platform_t *platform, gui_input_t *input);

/**
 * @brief 解除输入对象挂接（只解除同一个对象的挂接）
 */
void gui_platform_detach_input(gui_platform_t *platform, gui_input_t *input);

/**
 * @brief 取得平台的 gfx 句柄（离屏模式返回 NULL）
 */
gfx_handle_t *gui_platform_gfx_handle(gui_platform_t *platform);

/**
 * @brief 确保平台持有显示所有权
 *
 * 平台尚未 acquire 时在此 acquire；*acquired_here 置位表示**本次调用**
 * 取得了所有权（失败回滚时必须 release），否则平台原有的显示所有权
 * 不受影响。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_platform_acquire_view(gui_platform_t *platform, bool *acquired_here);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_PLATFORM_PRIVATE_H */
