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

/**
 * @brief 原生像素格式
 *
 * 公共层只认这五种。平台的原生格式由平台层**回读**后映射过来；映射不
 * 上时报告 UNKNOWN——那是一个**事实**，不是失败。
 */
typedef enum {
    GUI_PIXEL_FORMAT_XRGB8888 = 0, /* 32 位，每像素 4 字节 */
    GUI_PIXEL_FORMAT_RGB565,        /* 16 位 */
    GUI_PIXEL_FORMAT_ARGB8888,      /* 32 位，带 alpha */
    GUI_PIXEL_FORMAT_I8,            /* 8 位灰度 */
    GUI_PIXEL_FORMAT_UNKNOWN        /* 原生格式映射不上 */
} gui_pixel_format_t;

/**
 * @brief 平台的渲染统计
 *
 * 统计的是**真的**发生过的事，没有合成口径：
 *
 *   - `format` 是平台**回读**的原生像素格式（不是创建时写死的那个值）。
 *   - `width` / `height` 是视口尺寸。
 *   - `widget_count` 是当前**存活**的控件数（已发布且未失效）。
 *   - `pool_total` / `pool_used` / `pool_max_used` 是渲染内存池的字节数：
 *     `pool_used == pool_total - 空闲字节`，峰值是池的**历史**最高占用。
 *   - `registry_bytes` 是**控件注册表自己的**字节预算（1024 槽表的占用）。
 *     它与上面的渲染池是**两笔账**，必须分开看。
 *   - `renderer_name` 是渲染器的标识：平台层在创建时按**实际绑定的后端**
 *     定下的名字（屏幕模式是 OS01 的 2D 图形设备，离屏模式是内存画布），
 *     不是每次调用现编的字符串。
 *   - `renderer_version_*` 是渲染器的版本（major/minor/patch），来自被锁定
 *     的光栅化库版本。
 */
typedef struct {
    gui_pixel_format_t format;
    uint32_t width;
    uint32_t height;
    uint32_t widget_count;
    uint64_t pool_used;
    uint64_t pool_total;
    uint64_t pool_max_used;
    uint64_t registry_bytes;
    char renderer_name[16];
    uint16_t renderer_version_major;
    uint16_t renderer_version_minor;
    uint16_t renderer_version_patch;
} gui_platform_stats_t;

/**
 * @brief 读取平台的渲染统计
 *
 * 失败时输出结构被**清零**（不留半份数据）。
 *
 * **清零之后 `format` 字段读作 0**，也就是 `GUI_PIXEL_FORMAT_XRGB8888`
 * （枚举的零值是 XRGB8888，不是 UNKNOWN）。因此一次失败后的输出与一次
 * 成功的真实 XRGB8888 读数**在 `format` 上不可区分**。
 *
 * 调用方**必须**检查返回值。判定"这次没有数据"请用别的字段，不要拿清零
 * 后的 `format` 当真实格式：
 *
 *   - `pool_total == 0`（成功时渲染池一定非空）；
 *   - `renderer_name[0] == '\0'`（成功时渲染器名一定非空）。
 *
 * 换句话说：`format` 只有在**成功**返回时才有意义。
 *
 * @param platform 平台上下文句柄
 * @param out      输出参数：统计快照，不可为 NULL
 * @return 0 成功；-1 失败（errno = EINVAL）。失败时 `out` 被清零。
 */
int gui_platform_get_stats(gui_platform_t *platform, gui_platform_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_PLATFORM_H */
