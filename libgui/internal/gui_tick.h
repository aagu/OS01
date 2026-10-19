#ifndef _GUI_TICK_H
#define _GUI_TICK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 重置 tick 模块内部状态及锁存的错误码
 *
 * 注意：单实例运行模型下，仅当确认无活动平台上下文时方可为新实例调用 reset；
 * 或在平台 destroy 完成后调用。严禁在已有活动上下文时或每次 step 时重置。
 */
void gui_tick_reset(void);

/**
 * @brief 获取当前单调时间（毫秒）
 *
 * @param out 输出 64 位单调毫秒时间
 * @return 0 成功；-1 失败（此时 errno 已设置，且内部锁存首个错误）
 */
int gui_tick_now_ms(uint64_t *out);

/**
 * @brief LVGL 专用的 tick 回调函数
 *
 * 遵循 LVGL tick 接口规范，返回单调毫秒的低 32 位（允许自然回绕）。
 * 若底层时钟读取失败，内部锁存时钟错误并返回上次成功的 tick 值。
 *
 * @return 32 位单调毫秒数
 */
uint32_t gui_tick_get_cb(void);

/**
 * @brief 查询内部锁存的时钟错误码
 *
 * @return 0 表示无错误；正数表示已锁存的正 errno。一旦锁存错误，不再发起系统调用。
 */
int gui_tick_error(void);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_TICK_H */
