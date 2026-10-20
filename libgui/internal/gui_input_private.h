#ifndef _GUI_INPUT_PRIVATE_H
#define _GUI_INPUT_PRIVATE_H

/*
 * gui_input_private.h — 输入对象的内部状态与跨文件 hook
 *
 * 本头文件是 libgui 内部桥接头：不进入 sysroot，公共应用看不到它。
 * 它承载三样东西：
 *
 *   1. 输入对象的固定容量状态（FIFO 512 项、解码器、LVGL 适配状态）；
 *   2. input.c ↔ input_lvgl.c 的固定 hook 契约；
 *   3. input.c ↔ platform.c 的生命周期契约。
 *
 * FIFO 项保存**入队时刻**的修饰键快照：事件随快照一起排队，step 投递
 * 时不得用解码器的当前状态覆盖它（那会把排队的修饰键边沿重复计算）。
 * 重复策略使用的“已投递状态”另存在 delivered_modifiers。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gui/gui_input.h>
#include <gui_input_ps2.h>
#include <gfx.h>
#include <lvgl/lvgl.h>
#include <uapi/mouse.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 固定容量（全局约束：FIFO 512 项 / 每轮 256 键盘字节 / 64 鼠标事件） ── */

#define GUI_INPUT_FIFO_CAPACITY        512
#define GUI_INPUT_DELIVERY_BUDGET      256
#define GUI_INPUT_KEYBOARD_READ_BUDGET 256  /* 每轮最多读取的键盘字节 */
#define GUI_INPUT_MOUSE_READ_BUDGET    64   /* 每轮最多读取的鼠标事件 */

/*
 * gfx 输入租约 UAPI 的修饰键快照：**bit0 = Caps Lock**，其余位保留为 0
 * （uapi/gfx.h；内核 fb_state 把 route 的 INPUT_ROUTE_MOD_CAPS 折算成
 * 1u 交给用户态，聚合位从不越过 UAPI）。
 *
 * 只有 Caps 锁存位可以植入解码器：内核在 bind 时把**当时仍按住**的键
 * 标记为隔离并吞掉它们的释放沿，快照里的聚合 Shift/Ctrl/Alt 位描述的
 * 是永远不会到达的释放——植入它们会让消费者把该修饰键永久锁死。
 * 解码器用的是公开 gui_modifier_t 位序（CAPS = 1 << 6），因此植入前
 * 必须做一次位序折算。
 */
#define GUI_INPUT_LEASE_MOD_CAPS       0x1u

/* FIFO 项的种类 */
typedef enum {
    GUI_INPUT_ITEM_KEY = 0,     /* 按键边沿 */
    GUI_INPUT_ITEM_POINTER = 1  /* 指针事件 */
} gui_input_item_kind_t;

/**
 * @brief FIFO 中的一项
 *
 * 键项携带物理身份、入队时刻的修饰键快照、字符与事件时刻；指针项携带
 * 设备原始记录（相对位移 + 按钮 + 滚轮）与本次变化的按钮位图。
 */
typedef struct {
    uint8_t  kind;          /* gui_input_item_kind_t */
    uint8_t  action;        /* gui_key_action_t（键项） */
    uint16_t physical_key;  /* Set 1 扫描码 / 0x100|扫描码 */
    uint16_t modifiers;     /* 入队时刻的修饰键快照 */
    uint32_t codepoint;     /* ASCII 字符或 0 */
    uint64_t time_ms;       /* 事件时刻 */

    /* 指针项 */
    mouse_event_t raw;      /* 设备原始记录（相对位移 / 按钮 / 滚轮） */
    uint8_t       changed_buttons; /* 本次变化的按钮位图 */
} gui_input_item_t;

/**
 * @brief LVGL 适配层的状态（由 input_lvgl.c 独占使用）
 */
typedef struct {
    lv_indev_t *keypad;      /* 本 display 的键盘 indev */
    lv_indev_t *pointer;     /* 本 display 的指针 indev */
    lv_group_t  *group;      /* 私有默认焦点组（初始为空） */

    uint32_t        pending_key;    /* 待投递的 LVGL 键值 */
    lv_indev_state_t pending_state; /* 待投递的按下/释放状态 */
    bool            pending_valid;  /* 是否存在待投递样本 */

    uint32_t active_key;    /* 当前按住键的 LVGL 键值（0 = 无键按住，UP 事件复用它） */
    uint16_t active_physical; /* 当前按住键的物理身份（Set 1 扫描码），用于拒绝错键释放 */

    int32_t cursor_x;       /* LVGL 光标的绝对位置 */
    int32_t cursor_y;
    bool    pointer_pressed;/* 左键按下状态 */

    uint32_t width;         /* 视口宽（指针位置夹紧用） */
    uint32_t height;        /* 视口高 */
} gui_input_lvgl_t;

/**
 * @brief 输入对象（内部状态）
 */
struct gui_input {
    gui_platform_t *platform;
    gui_input_mode_t mode;
    bool require_mouse;

    int keyboard_fd;   /* 借给调用方的 /dev/keyboard（-1 = 无） */
    int mouse_fd;      /* 借给调用方的 /dev/mouse（-1 = 无） */

    gfx_handle_t *gfx; /* 屏幕模式的 gfx 句柄 */
    bool bound;        /* 输入租约是否仍然有效 */
    bool acquired_here;/* 本次创建是否取得了显示所有权（回滚时必须 release） */

    gui_ps2_state_t ps2;   /* 纯用户态解码器状态 */

    gui_input_item_t items[GUI_INPUT_FIFO_CAPACITY];
    unsigned head;         /* FIFO 队首下标 */
    unsigned count;        /* FIFO 中的项数 */

    uint16_t delivered_modifiers; /* 已投递的修饰键状态（重复策略用） */
    uint8_t  pointer_buttons;     /* 上一次入队的鼠标按钮位图 */

    gui_input_lvgl_t lvgl;

    gui_key_handler_t key_handler;
    void *key_userdata;
    gui_pointer_handler_t pointer_handler;
    void *pointer_userdata;

    int latched_errno;      /* 锁存的永久错误（0 = 无） */
    gui_status_t latched_status; /* 锁存错误对应的平台状态 */
    bool stopped;           /* 已停止：不再投递、不再持有 fd */
};

/* ── input.c ↔ input_lvgl.c 的固定 hook ───────────────────────── */

int  gui_input_lvgl_create(gui_input_t *input);
void gui_input_lvgl_destroy(gui_input_t *input);
void gui_input_lvgl_cancel(gui_input_t *input);
int  gui_input_lvgl_deliver(gui_input_t *input, const gui_key_event_t *event);
int  gui_input_lvgl_pointer(gui_input_t *input, const mouse_event_t *event);

/* ── input.c ↔ platform.c 的固定 hook ─────────────────────────── */

gui_status_t gui_input_advance(gui_input_t *input, uint64_t now_ms, uint32_t *delay_ms);
void gui_input_stop(gui_input_t *input, int reason);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_INPUT_PRIVATE_H */
