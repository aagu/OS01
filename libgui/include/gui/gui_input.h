#ifndef _GUI_INPUT_H
#define _GUI_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <poll.h>

#include <gui/gui_platform.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明输入上下文句柄 */
typedef struct gui_input gui_input_t;

/**
 * @brief 输入来源模式枚举
 */
typedef enum {
    GUI_INPUT_DEVICES   = 0,  /* 真实设备：独占物理输入租约，读取 /dev/keyboard 与 /dev/mouse */
    GUI_INPUT_SYNTHETIC = 1   /* 合成输入：不打开输入设备、不占用 gfx 物理输入租约 */
} gui_input_mode_t;

/**
 * @brief 输入对象初始化配置参数
 */
typedef struct {
    gui_input_mode_t mode;    /* 输入来源模式（默认 GUI_INPUT_DEVICES） */
    bool require_mouse;       /* false：mouse 缺失时允许 keyboard-only 运行 */
} gui_input_config_t;

/**
 * @brief 原生按键动作枚举
 *
 * 取值在原生头内固定：GUI_KEY_DOWN = 0、GUI_KEY_UP = 1、GUI_KEY_REPEAT = 2。
 */
typedef enum {
    GUI_KEY_DOWN   = 0,  /* 按下边沿 */
    GUI_KEY_UP     = 1,  /* 释放边沿 */
    GUI_KEY_REPEAT = 2   /* 重复边沿（由 libgui 的软件重复策略产生） */
} gui_key_action_t;

/**
 * @brief 修饰键位定义
 *
 * 左右 Shift/Ctrl/Alt 分别跟踪，位序固定，不可重排。
 */
typedef enum {
    GUI_MOD_LSHIFT = 1 << 0,  /* 左 Shift */
    GUI_MOD_RSHIFT = 1 << 1,  /* 右 Shift */
    GUI_MOD_LCTRL  = 1 << 2,  /* 左 Ctrl */
    GUI_MOD_RCTRL  = 1 << 3,  /* 右 Ctrl */
    GUI_MOD_LALT   = 1 << 4,  /* 左 Alt */
    GUI_MOD_RALT   = 1 << 5,  /* 右 Alt */
    GUI_MOD_CAPS   = 1 << 6   /* Caps Lock 锁存状态 */
} gui_modifier_t;

/**
 * @brief 原生按键事件
 *
 * 物理键身份与翻译后的字符相互独立：UP 事件沿用对应 DOWN 的 physical_key，
 * 且 codepoint 恒为 0。codepoint 仅可能是 ASCII 字符或 0，绝不会是 LVGL 的按键常量。
 */
typedef struct {
    uint16_t         physical_key; /* Set 1 扫描码；E0 前缀键为 0x100 | 扫描码 */
    uint16_t         modifiers;    /* 事件处理时刻的修饰键状态快照（gui_modifier_t 位组合） */
    uint32_t         codepoint;    /* ASCII 字符或 0（无字符） */
    gui_key_action_t action;       /* 按键动作（DOWN / UP / REPEAT） */
} gui_key_event_t;

/**
 * @brief 原生指针事件
 */
typedef struct {
    int32_t x;                /* 视口坐标 X */
    int32_t y;                /* 视口坐标 Y */
    uint8_t buttons;          /* 当前按下的按钮位图 */
    uint8_t changed_buttons;  /* 本次变化的按钮位图 */
    int8_t  wheel;            /* 垂直滚轮刻度 */
} gui_pointer_event_t;

/**
 * @brief 原生按键回调
 *
 * 回调先于控件路径被调用：返回 false 的事件随后才会送进 LVGL 的 keypad。
 *
 * 因此**应用可见的按键流是控件可见的按键流的超集**：本回调已经看到的
 * 事件，控件路径可能永远看不到它。三种情形：
 *   - 应用消费了它（返回 true）：事件不再进入控件路径；
 *   - 控件路径丢弃了它：当 LVGL 侧的按键重新就绪协议尚未完成时（屏幕切换
 *     动画或 indev 暂停导致读取被阻塞），控件路径**不会**收到该键，但本
 *     回调已经看到了它的 DOWN —— 取消协议要求此时不得投递新键；
 *   - 它没有 LVGL 键义（功能键、keypad 数字区……）：只到应用为止。
 *
 * 应用若需要"键一定到达控件"，必须等待重新就绪后再让用户重新按下。
 *
 * 重复（GUI_KEY_REPEAT）只在**真正进入控件路径**的按键上产生：应用消费
 * 掉的按键不会收到 REPEAT。
 *
 * @param event  按键事件（生命周期由 libgui 管理，回调内不得释放）
 * @param userdata 调用方上下文
 * @return true 表示事件已被消费（后续不再进入控件路径）
 */
typedef bool (*gui_key_handler_t)(const gui_key_event_t *event, void *userdata);

/**
 * @brief 原生指针回调（只读报告）
 *
 * @param event  指针事件（生命周期由 libgui 管理，回调内不得释放）
 * @param userdata 调用方上下文
 */
typedef void (*gui_pointer_handler_t)(const gui_pointer_event_t *event, void *userdata);

/**
 * @brief 创建输入对象
 *
 * @param platform 关联的平台上下文句柄
 * @param cfg 配置参数指针，若为 NULL 则等价于 GUI_INPUT_DEVICES 且 require_mouse = false
 * @return 成功返回有效句柄，失败返回 NULL 并设置 errno
 */
gui_input_t *gui_input_create(gui_platform_t *platform, const gui_input_config_t *cfg);

/**
 * @brief 销毁输入对象并释放其持有的设备与状态
 *
 * 所有权：gui_input_create() 成功返回后，输入对象挂接在平台上，其所有权
 * 归平台——gui_platform_destroy() 会自动销毁并释放仍挂接的输入对象，此后
 * 调用方不得再使用该指针，也不得再次调用 gui_input_destroy()（重复释放）。
 * 调用方若要自己掌控生命周期，必须在销毁平台**之前**先调用本函数：本函数
 * 会把输入从平台上摘除，平台随后不再持有它。
 *
 * @param input 待销毁的输入句柄指针（允许为 NULL）
 */
void gui_input_destroy(gui_input_t *input);

/**
 * @brief 取得需要参与 poll 的设备描述符
 *
 * 顺序固定为 keyboard、mouse；调用方只可借用，不得 read/close 或修改 flags。
 *
 * @param input 输入对象句柄
 * @param out   输出数组（容量为 2）
 * @return 返回有效的 fd 数量（0..2）；失败返回 -1 并设置 errno
 */
int gui_input_get_pollfds(gui_input_t *input, struct pollfd out[2]);

/**
 * @brief 查询当前是否具备鼠标输入
 *
 * @param input 输入对象句柄
 * @return true 表示存在鼠标设备
 */
bool gui_input_has_mouse(const gui_input_t *input);

/**
 * @brief 泵入输入设备数据（只读取与入队，不渲染）
 *
 * @param input 输入对象句柄
 * @param fds   poll 结果（可为 NULL，表示立即检查设备而不阻塞）
 * @param count fds 的有效个数
 * @return 0 成功；-1 失败（此时 errno 已设置，平台下一步应终止）
 */
int gui_input_dispatch(gui_input_t *input, const struct pollfd *fds, unsigned count);

/**
 * @brief 注册原生按键回调
 *
 * @param input    输入对象句柄（为 NULL 时设置 errno = EINVAL 并返回）
 * @param handler  回调函数（为 NULL 表示取消注册）
 * @param userdata 调用方上下文
 */
void gui_input_set_key_handler(gui_input_t *input, gui_key_handler_t handler,
                               void *userdata);

/**
 * @brief 注册原生指针回调（只读报告右键/中键等信息）
 *
 * @param input    输入对象句柄（为 NULL 时设置 errno = EINVAL 并返回）
 * @param handler  回调函数（为 NULL 表示取消注册）
 * @param userdata 调用方上下文
 */
void gui_input_set_pointer_handler(gui_input_t *input, gui_pointer_handler_t handler,
                                   void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_INPUT_H */
