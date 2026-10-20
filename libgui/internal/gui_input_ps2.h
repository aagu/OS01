#ifndef _GUI_INPUT_PS2_H
#define _GUI_INPUT_PS2_H

#include <stdint.h>

#include <gui/gui_input.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 *  纯用户态 PS/2 Set 1 键盘解码器
 *
 *  内核路由（kernel/driver/input_route.c）向 ACTIVE 消费者发布的是
 *  **原始 Set 1 字节**：路由保证整段序列发布（租约边界干净），
 *  但载荷仍是未解码的字节。本解码器因此是同一套 Set 1 状态机的
 *  **独立实现**——它自行重新组帧并翻译字节。
 *
 *  本文件刻意不复用内核私有头与内核对象：
 *  kernel/include/driver/input_ps2.h 是内核私有接口，不在用户态
 *  包含路径上；用户态必须独立实现相同的语义。
 *
 *  解码器是纯函数式的状态机：不做设备 IO、不加锁、不调用 LVGL，
 *  只把字节流翻译成原生按键事件。
 * ═══════════════════════════════════════════════════════════════ */

/* E0 前缀键的物理身份：0x100 | Set 1 扫描码（如 0x11D = 右 Ctrl）。 */
#define GUI_PS2_KEY_E0 0x100u

/* 被跟踪的物理键槽位：128 个普通 Set 1 扫描码 + 128 个 E0 扫描码。
 * 扫描码的 break 位（0x80）在归一化时被去掉，因此每个槽位对应
 * 一个物理键身份。 */
#define GUI_PS2_KEY_SLOTS 256

/* 普通扫描码槽位与 E0 扫描码槽位的分界。 */
#define GUI_PS2_KEY_SLOT_E0_BASE 128u

/* 截断前缀的过期时间（毫秒）：超过后前缀被丢弃，下一个字节重新
 * 开始一个序列。 */
#define GUI_PS2_PREFIX_TIMEOUT_MS 100u

/**
 * @brief 解码器状态（可栈分配，调用方持有）
 *
 * 持有：待定前缀、按下位图、每个 DOWN 缓存的字符、当前修饰键状态。
 */
typedef struct {
    uint8_t  prefix;      /* 待定前缀字节：0 = 无，0xE0 / 0xE1 */
    uint8_t  phase;       /* 前缀内的解析阶段（内部枚举，见 input_ps2.c） */
    uint64_t prefix_ms;   /* 待定前缀的起始时刻（毫秒） */
    uint16_t modifiers;   /* 当前修饰键状态（gui_modifier_t 位组合） */
    uint8_t  pressed[GUI_PS2_KEY_SLOTS / 8]; /* 按下位图 */
    uint8_t  cached[GUI_PS2_KEY_SLOTS];       /* 每个 DOWN 缓存的字符（0 = 无字符） */
} gui_ps2_state_t;

/**
 * @brief 物理键身份对应的状态槽位下标
 *
 * @param physical_key 物理键身份（普通扫描码或 0x100 | 扫描码）
 * @return 状态数组下标（0..GUI_PS2_KEY_SLOTS-1）
 */
static inline unsigned gui_ps2_key_slot(uint16_t physical_key)
{
    if (physical_key & GUI_PS2_KEY_E0)
        return GUI_PS2_KEY_SLOT_E0_BASE + (unsigned)(physical_key & 0x7Fu);
    return (unsigned)(physical_key & 0x7Fu);
}

/**
 * @brief 初始化解码器状态
 *
 * @param st               解码器状态
 * @param initial_modifiers 初始修饰键快照，**使用公共 gui_modifier_t 位序**
 *                          （GUI_MOD_CAPS = 1 << 6），不是内核
 *                          INPUT_ROUTE_MOD_* 的瞬时位；调用方负责换算。
 */
void gui_ps2_init(gui_ps2_state_t *st, uint16_t initial_modifiers);

/**
 * @brief 送入一个 Set 1 字节
 *
 * 只报告**真实硬件边沿**：重复 make（typematic）不产生新的 DOWN 边沿，
 * 软件重复策略（首次 400 ms、后续 40 ms）由 libgui 的 LVGL 适配层负责。
 *
 * @param st     解码器状态
 * @param byte   Set 1 字节
 * @param now_ms 当前时刻（毫秒）
 * @param out    输出事件（仅当返回 1 时有效）
 * @return 1 = 产生了一个边沿事件；0 = 无事件；-1 = 参数错误（errno 已设置）
 */
int gui_ps2_feed(gui_ps2_state_t *st, uint8_t byte, uint64_t now_ms,
                 gui_key_event_t *out);

/**
 * @brief 清理超时的待定前缀
 *
 * 距前缀起始超过 GUI_PS2_PREFIX_TIMEOUT_MS 时丢弃该前缀：下一个
 * 字节重新开始一个序列，截断的前缀永远不会复活成幽灵修饰键。
 *
 * @param st     解码器状态
 * @param now_ms 当前时刻（毫秒）
 */
void gui_ps2_expire(gui_ps2_state_t *st, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_INPUT_PS2_H */
