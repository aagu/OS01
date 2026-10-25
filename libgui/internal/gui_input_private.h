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

#include <gui/gui_focus.h>
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

/* ── 软件重复策略（spec §6.2.4） ────────────────────────────────── */

/* 首次重复延迟 400 ms，之后每次 40 ms；每步最多产生一个到期重复。 */
#define GUI_INPUT_REPEAT_FIRST_MS      400u
#define GUI_INPUT_REPEAT_INTERVAL_MS   40u

/* ── 焦点组（私有记录，实现见 input_focus.c） ─────────────────────── */

/*
 * 焦点登记上限 128（CLICK 也计入）：登记是一张**共享**的表，原生登记与
 * 诊断(raw)登记都占用它。
 */
#define GUI_INPUT_FOCUS_CAPACITY       128

/*
 * 登记模式：input 的**第一次成功登记**锁定模式，此后两种登记不许混用。
 */
typedef enum {
    GUI_FOCUS_MODE_UNSET = 0,      /* 还没有任何登记 */
    GUI_FOCUS_MODE_NATIVE,         /* 原生登记（公共 API） */
    GUI_FOCUS_MODE_RAW_DIAGNOSTIC  /* 诊断(raw)登记（内部 hook） */
} gui_input_focus_mode_t;

/**
 * @brief 焦点登记表的一个条目
 *
 * 条目同时承载两套入口的共同事实：登记顺序（= Tab 顺序）、原生身份
 * （诊断登记为 0）、策略与观察者归属。观察者归属决定显式注销时摘掉的是
 * 哪一类删除观察者。
 */
typedef struct {
    lv_obj_t *obj;         /* 后端对象：只在它还活着时被解引用 */
    uint64_t  native_id;   /* 原生身份（诊断(raw)登记为 0） */
    uint8_t   policy;      /* gui_focus_policy_t */
    bool      native;      /* true = 原生登记；false = 诊断(raw)登记 */
} gui_input_focus_entry_t;

/* ── 光标与滚轮（spec §7.1） ────────────────────────────────────── */

/*
 * 箭头光标：一张 16×24 的 ARGB8888 图像资源（libgui/src/cursor_icon.c），
 * 热点在左上角 (0,0)。
 *
 * 光标是**图像资源**，不是程序画出来的形状：颜色烘焙在像素里（深色
 * 主体 + 浅色描边），下面这两个常量只是「资源里烘的是什么色」的声明，
 * 供宿主契约测试与 guest 探针比对。
 */
#define GUI_INPUT_CURSOR_WIDTH         16
#define GUI_INPUT_CURSOR_HEIGHT        24
/* 光标像素颜色（XRGB8888，与离屏画布的读回格式一致）。 */
#define GUI_INPUT_CURSOR_FILL_COLOR     0x001A1C1Eu
#define GUI_INPUT_CURSOR_OUTLINE_COLOR  0x00FDFCFFu

/* 垂直滚轮每刻度滚动的像素数（正值 = 内容向下滚，scroll_y 增大）。 */
#define GUI_INPUT_WHEEL_STEP_PX   32

/*
 * gfx 输入租约 UAPI 的修饰键快照：**bit0 = Caps Lock**，其余位保留为 0
 * （uapi/gfx.h；内核 fb_state 把 route 的 INPUT_ROUTE_MOD_CAPS 折算成
 * 1u 交给用户态，聚合位从不越过 UAPI）。
 *
 * 只有 Caps 锁存位可以植入解码器：
 *   - 聚合的 Shift/Ctrl/Alt 位描述的是消费者**从未看到**的按下边沿——内核
 *     在 bind 时把当时仍按住的**所有键**（普通键与修饰键）标记为隔离并吞掉
 *     它们的释放沿（消费者不会收到孤立的 UP；spec §4.3）。
 *     消费者据此相信"修饰键按住了"，就是相信了一个它没有观察到的状态；
 *   - 而且内核根本不把这些位交给用户态：租约 UAPI 只发布 Caps 位；
 *   - Caps 锁存位不同：它是**状态**而不是边沿，消费者没有别的途径知道它，
 *     所以它必须被植入。
 *
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
    /* A blocked cancellation retains this item without replaying app callbacks. */
    bool key_prepared;
    bool key_consumed;
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

    lv_obj_t *pressed_obj;  /* 收到 PRESSED 的控件；可能已失效，使用前必须核验存活 */
    bool      cancel_pending; /* 需要在回调之外执行取消 / 重新就绪协议 */

    int32_t cursor_x;       /* LVGL 光标的绝对位置 */
    int32_t cursor_y;
    bool    pointer_pressed;/* 左键按下状态 */

    lv_obj_t *cursor;       /* 光标对象（属于 input，不属于 platform） */
    bool      cursor_shown; /* 光标当前是否可见（没有鼠标源时隐藏） */

    uint32_t width;         /* 视口宽（指针位置夹紧用） */
    uint32_t height;        /* 视口高 */
} gui_input_lvgl_t;

/**
 * @brief 焦点组的私有记录（实现见 input_focus.c）
 *
 * 记录本身就是"存活证明"：条目在对象收到 LV_EVENT_DELETE 时被摘掉，因此
 * 记录里的指针一定是活对象。摘除只比较指针值，从不解引用失效指针。
 *
 * **唯一选择状态**：当前选择、是否发生过选择、是否被显式清空、修复起点
 * 全部在这里。原生身份与诊断登记共用同一张表、同一套提交路径。
 */
typedef struct {
    gui_input_focus_entry_t entries[GUI_INPUT_FOCUS_CAPACITY]; /* 登记顺序 = Tab 顺序 */
    unsigned count;                             /* 已登记的条目数 */

    lv_obj_t *focus;                            /* 当前选择（NULL = NONE） */
    uint64_t  focus_id;                         /* 当前选择的原生身份（0 = 诊断登记或 NONE） */
    bool      ever_selected;                    /* 是否发生过一次成功的选择 */
    bool      explicit_clear;                   /* 显式清空：保持 NONE 直到下一次成功选择 */
    bool      repair_pending;                   /* 焦点失效但暂时找不到候选 */
    bool      repair_bounded;                   /* 修复搜索有界（受控路径）；false = 后端路径绕回 */
    unsigned  repair_index;                     /* 修复起点（表内下标） */
    bool      dirty;                            /* 后端组与记录不一致：需要重新投影 */

    uint8_t   mode;                             /* gui_input_focus_mode_t */
    uint8_t   initial;                          /* gui_focus_initial_t */

    unsigned  projection_depth;                 /* 投影深度：>0 时屏蔽组回调的中间选择 */
    unsigned  delete_depth;                     /* 删除批次深度 */
} gui_input_focus_t;

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
    uint64_t delivered_events;    /* 已投递的输入事件总数（真实统计） */

    /* 软件重复策略：首次 400 ms、后续 40 ms，由 libgui 的单调时钟驱动。 */
    uint64_t repeat_deadline_ms;  /* 下一次重复的期限 */
    uint16_t repeat_physical;     /* 重复键的物理身份 */
    uint32_t repeat_char;         /* 重复键缓存的字符（DOWN 时的缓存值） */
    bool     repeat_armed;        /* 重复策略是否就绪 */

    gui_input_focus_t focus;       /* 私有焦点组记录 */

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

/*
 * 取消 / 重新就绪协议（spec §6.2.2）：
 *   停重复 → 清未投递事件 → wait_release → 显式 RELEASED read →
 *   清旧控件视觉 → 之后才允许投递新键。
 * 返回 false 表示重新就绪尚未完成（read 被屏幕动画 / indev 暂停阻塞），
 * 此时不允许投递新键，调用方必须稍后重试。
 */
bool gui_input_lvgl_cancel_key(gui_input_t *input);

/* 新的按键按下：取消旧活动键。
 *
 * 软件重复**不在这里**武装：必须等应用消费与否的结论，见
 * gui_input_lvgl_arm_repeat()。 */
void gui_input_lvgl_key_down(gui_input_t *input, const gui_key_event_t *event);

/* 一次 DOWN 之后的软件重复维护：只有真正进入 LVGL 的按键才武装重复。 */
void gui_input_lvgl_repeat_after_down(gui_input_t *input,
                                      const gui_key_event_t *event,
                                      bool reached_lvgl, uint64_t now_ms);

/* 该键是否参与软件重复（可打印字符、编辑键、方向键）。 */
bool gui_input_lvgl_repeatable(gui_input_t *input, const gui_key_event_t *event);

int  gui_input_lvgl_deliver(gui_input_t *input, const gui_key_event_t *event);
int  gui_input_lvgl_pointer(gui_input_t *input, const mouse_event_t *event);

/* 被消费的释放仍然必须释放 LVGL 侧的活动键，否则它会一直保持按下。 */
void gui_input_lvgl_force_release(gui_input_t *input, uint16_t physical_key);

/*
 * 标记"需要执行取消协议"：只能在回调之外真正执行（回调里递归读 indev
 * 是不允许的）。
 */
void gui_input_lvgl_mark_cancel(gui_input_t *input);

/* 每步一次的适配层维护：重试待处理的取消协议。 */
bool gui_input_lvgl_pump(gui_input_t *input);

/* 焦点对象消失（LV_EVENT_DELETE）：适配层需要执行取消协议。 */
void gui_input_focus_lost(gui_input_t *input, lv_obj_t *obj);

/* ── 焦点组（实现见 input_focus.c） ────────────────────────────── */

int      gui_input_focus_create(gui_input_t *input);
void     gui_input_focus_destroy(gui_input_t *input);
int      gui_input_focus_add(gui_input_t *input, lv_obj_t *obj);
void     gui_input_focus_remove(gui_input_t *input, lv_obj_t *obj);
int      gui_input_focus_set(gui_input_t *input, lv_obj_t *obj);
lv_obj_t *gui_input_focus_current(gui_input_t *input);
bool     gui_input_focus_alive(gui_input_t *input, lv_obj_t *obj);
bool     gui_input_focus_navigate(gui_input_t *input, bool forward);
bool     gui_input_focus_is_text_input(gui_input_t *input);
bool     gui_input_focus_refresh(gui_input_t *input);

/* ── 唯一焦点引擎的公共入口桥（focus.c 使用） ──────────────────── */

/* 登记模式（UNSET / NATIVE / RAW_DIAGNOSTIC）。 */
uint8_t  gui_input_focus_mode(gui_input_t *input);

/* 原生登记：登记或更新策略（保留位置）。 */
int      gui_input_focus_register_native(gui_input_t *input, lv_obj_t *obj,
                                          uint64_t native_id, gui_focus_policy_t policy);

/* 原生注销：摘条目 + 摘观察者 + 按移除前的位置修复选择。 */
int      gui_input_focus_unregister_native(gui_input_t *input, uint64_t native_id);

/* 原生显式设置选择；显式清空（native_id == 0）。 */
int      gui_input_focus_set_native(gui_input_t *input, uint64_t native_id);

/* 读回当前选择的原生身份（0 = NONE 或诊断登记）。 */
uint64_t gui_input_focus_current_native_id(gui_input_t *input);

/* 首次焦点建立策略。 */
void     gui_input_focus_set_initial(gui_input_t *input, gui_focus_initial_t initial);

/*
 * 删除批次：公共 destroy / clear 在删树前把整棵待删子树标成 DESTROYING，
 * 然后开始批次。批次期间组被整体 detach 到真实 NONE，后端不会转焦；
 * 批次结束后选一次跳过整棵待删子树的后继，并提交一次最终投影。
 */
void     gui_input_focus_delete_batch_begin(gui_input_t *input);
void     gui_input_focus_delete_batch_end(gui_input_t *input);

/*
 * 控件自身或祖先被隐藏 / 禁用 → 不可用。
 *
 * 焦点候选与滚轮目标共用这一条判定：两处的"可用"语义必须一致
 * （LVGL 的 hit-test 只处理隐藏，禁用只在对象自身上判断）。
 */
bool gui_input_focus_object_usable(lv_obj_t *obj);

/* ── input.c ↔ platform.c 的固定 hook ─────────────────────────── */

gui_status_t gui_input_advance(gui_input_t *input, uint64_t now_ms, uint32_t *delay_ms);
void gui_input_stop(gui_input_t *input, int reason);

#ifdef OS01_HOST_TEST
/*
 * 私有测试注入：把合成的 Set 1 字节喂进**同一条**解码器 / FIFO 路径。
 *
 * 只在宿主测试构建（-DOS01_HOST_TEST）里存在：目标构建不编译它，因此这个
 * 接口永远不会进入公共 sysroot。它也只接受合成输入（GUI_INPUT_SYNTHETIC）。
 */
int gui_input_test_feed_keybytes(gui_input_t *input, const uint8_t *bytes, size_t len);

/*
 * 私有测试注入：把合成的鼠标事件喂进**同一条** FIFO / 适配层路径。
 *
 * 规则与上面的键盘桥接相同：只在宿主构建里存在、只接受合成输入、
 * 不进入公共 sysroot、不出现在任何公共头文件里。
 */
int gui_input_test_feed_mouse(gui_input_t *input, const mouse_event_t *events,
                              size_t count);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _GUI_INPUT_PRIVATE_H */
