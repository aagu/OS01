#ifndef _GUI_PLATFORM_H
#define _GUI_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 平台运行模式枚举
 */
typedef enum {
    GUI_MODE_ONSCREEN  = 0,  /* 屏幕模式：绑定 /dev/fb 与 /dev/gfx0，受前台协调器管控 */
    GUI_MODE_OFFSCREEN = 1,  /* 离屏模式：纯内存画布，用于自动化单测、headless smoke */
} gui_mode_t;

/**
 * @brief 平台单步运行及事件状态码
 */
typedef enum {
    GUI_STATUS_OK              = 0,   /* 正常推进，画面已按需提交 */
    GUI_STATUS_EAGAIN          = 1,   /* present 暂时失败，保留视图，按等待时间重试 */
    GUI_STATUS_LOST_OWNERSHIP  = 2,   /* 终止态：失去前台所有权 (EACCES)，需销毁 */
    GUI_STATUS_DISPLAY_CHANGED = 3,   /* 分辨率或内核视图失效 (ESTALE)，需重建或重启 */
    GUI_STATUS_ERROR           = -1   /* 严重底层设备或内存错误 */
} gui_status_t;

/**
 * @brief 平台初始化配置参数
 */
typedef struct {
    gui_mode_t mode;               /* 运行模式 (默认 GUI_MODE_ONSCREEN) */
    uint32_t width;                /* 离屏必填；屏幕模式必须为 0，自动查询 */
    uint32_t height;               /* 离屏必填；屏幕模式必须为 0，自动查询 */
    uint32_t buffer_lines;         /* 局部绘制缓冲行数，若为 0 则使用默认值 40 行 */
} gui_config_t;

/* 不透明平台上下文句柄 */
typedef struct gui_platform gui_platform_t;

/**
 * @brief 初始化并创建 GUI 平台后端上下文
 *
 * @param cfg 配置参数指针，若为 NULL 则使用屏幕模式默认参数
 * @return 成功返回有效句柄，失败返回 NULL 并设置 errno
 */
gui_platform_t *gui_platform_create(const gui_config_t *cfg);

/**
 * @brief 销毁平台后端上下文，释放所有绘制缓冲、映射与设备句柄
 *
 * 若平台上仍挂接输入对象（gui_input_create 成功返回），本函数会先销毁并
 * 释放它：输入对象的所有权在挂接后归平台，调用方此后不得再使用该指针，
 * 也不得再次调用 gui_input_destroy()。要自己掌控输入生命周期，必须在销毁
 * 平台之前先销毁输入。
 *
 * @param platform 待销毁的平台句柄指针（允许为 NULL）
 */
void gui_platform_destroy(gui_platform_t *platform);

/**
 * @brief 查询当前平台的有效像素宽度
 */
uint32_t gui_platform_get_width(const gui_platform_t *platform);

/**
 * @brief 查询当前平台的有效像素高度
 */
uint32_t gui_platform_get_height(const gui_platform_t *platform);

/**
 * @brief 查询当前平台运行模式
 */
gui_mode_t gui_platform_get_mode(const gui_platform_t *platform);

/**
 * @brief 单步迭代 GUI 定时器、脏矩形光栅化与受控帧呈现
 *
 * @param platform 平台上下文句柄
 * @param out_next_delay_ms 输出参数：建议的下一次事件 poll 等待时间（毫秒）。可为 NULL
 * @return 运行状态码 (gui_status_t)
 */
gui_status_t gui_platform_step(gui_platform_t *platform, uint32_t *out_next_delay_ms);

/**
 * @brief 在离屏模式下读取指定坐标处的像素值 (XRGB8888 格式)
 *
 * @param platform 平台上下文句柄
 * @param x 横坐标
 * @param y 纵坐标
 * @return 成功返回 32 位颜色值 (0x00RRGGBB)；越界或非离屏模式返回 0xFFFFFFFFu
 */
uint32_t gui_platform_get_pixel(const gui_platform_t *platform, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_PLATFORM_H */
