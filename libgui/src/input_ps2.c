/*
 * input_ps2.c — 纯用户态 PS/2 Set 1 键盘解码器
 *
 * 把 PS/2 Set 1 字节流翻译成原生按键事件（gui_key_event_t）。
 *
 * 内核路由（kernel/driver/input_route.c）向 ACTIVE 消费者发布的是
 * **原始 Set 1 字节**：租约边界是整段序列，但载荷仍未解码。因此本
 * 解码器是同一套 Set 1 状态机的**独立实现**——它自行重新组帧并翻译
 * 字节，绝不复用内核私有头（kernel/include/driver/input_ps2.h）与内核
 * 对象。
 *
 * 解码器是纯的：不做设备 IO、不加锁、不调用 LVGL，只把字节翻译成
 * 事件。软件重复策略（首次 400 ms、后续 40 ms）属于 libgui 的 LVGL
 * 适配层（Task 7），本文件只报告**真实硬件边沿**。
 *
 *   - 普通 make/break 与 E0 前缀可跨任意 read 分片；
 *   - PrintScreen（E0 2A E0 37 / E0 B7 E0 AA）与 Pause
 *     （E1 1D 45 E1 9D C5）整段吞掉：不产生事件，也不污染
 *     Shift/Ctrl 状态——这正是整个 GUI-2 计划要防的幽灵 Shift；
 *   - 未知/损坏的前缀恢复到序列边界并被吞掉，绝不产生假的修饰键；
 *   - 截断的前缀超过 100 ms 后清除，晚到的字节重新开始一个序列；
 *   - 字母使用 Shift XOR Caps，数字与标点只受 Shift 影响；
 *   - 左右 Shift/Ctrl/Alt 分别跟踪，物理身份互相独立；
 *   - Caps 只在真实的 up -> down 边沿翻转，typematic 重复 make
 *     不翻转、也不产生新的 DOWN 边沿；
 *   - UP 事件沿用对应 DOWN 的物理身份，codepoint 恒为 0。
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <gui/gui_input.h>
#include <gui_input_ps2.h>

/* ── Set 1 前缀与特殊序列字节 ───────────────────────────────── */

#define PS2_E0 0xE0u
#define PS2_E1 0xE1u

/* PrintScreen make: E0 2A E0 37（其中 E0 2A 是假左 Shift）。
 * PrintScreen break: E0 B7 E0 AA（其中 E0 B7 是假右 Shift）。
 * Pause: E1 1D 45 E1 9D C5（其中 E1 1D 45 是假左 Ctrl）。 */
#define PS2_PS_MAKE_TAIL  0x37u
#define PS2_PS_BREAK_TAIL 0xAAu
#define PS2_PAUSE_CTRL    0x1Du
#define PS2_PAUSE_KEY     0x45u
#define PS2_PAUSE_TAIL_1  0x9Du
#define PS2_PAUSE_TAIL_2  0xC5u

/* ── 修饰键与 Caps 的 Set 1 身份 ─────────────────────────────── */

#define PS2_KEY_LSHIFT 0x02Au
#define PS2_KEY_RSHIFT 0x036u
#define PS2_KEY_LCTRL  0x01Du
#define PS2_KEY_RCTRL  0x11Du
#define PS2_KEY_LALT   0x038u
#define PS2_KEY_RALT   0x138u
#define PS2_KEY_CAPS   0x03Au

/* ── 解析阶段 ───────────────────────────────────────────────── */

/* 解码器的全部持久状态就是一个阶段号：每个阶段恰好知道下一个
 * 期望的字节。 */
enum {
    PS2_PH_IDLE = 0,     /* 序列边界 */
    PS2_PH_E0,           /* 已消费 E0，等待键码 */
    PS2_PH_PS_MAKE,      /* 已消费 E0 2A，等待 E0 */
    PS2_PH_PS_MAKE_E0,   /* 已消费 E0 2A E0，等待 37 */
    PS2_PH_PS_BREAK,     /* 已消费 E0 B7，等待 E0 */
    PS2_PH_PS_BREAK_E0,  /* 已消费 E0 B7 E0，等待 AA */
    PS2_PH_PAUSE_1D,     /* 已消费 E1，等待 1D */
    PS2_PH_PAUSE_45,     /* 已消费 E1 1D，等待 45 */
    PS2_PH_PAUSE_E1,     /* 已消费 E1 1D 45，等待 E1 */
    PS2_PH_PAUSE_9D,     /* 已消费 E1 1D 45 E1，等待 9D */
    PS2_PH_PAUSE_C5      /* 已消费 E1 1D 45 E1 9D，等待 C5 */
};

/* ── US ASCII 布局 ─────────────────────────────────────────── */

/* 只有**主键区**产生字符。E0 键（导航区、keypad、Enter 等）以及
 * keypad 区的普通扫描码（数字、'.'、'-'、'+'、'*'）只报告物理键
 * 身份：v1 的 keypad NumLock 布局不产生控件操作，也不得冒充文本
 * 或方向键。 */
typedef struct {
    char    base;    /* 未按 Shift 时的字符 */
    char    shifted; /* 按下 Shift 时的字符 */
    uint8_t alpha;   /* 1 = 字母（Shift XOR Caps），0 = 只受 Shift 影响 */
} ps2_ascii_t;

static const ps2_ascii_t g_ascii[0x80] = {
    [0x02] = { '1', '!', 0 },  [0x03] = { '2', '@', 0 },
    [0x04] = { '3', '#', 0 },  [0x05] = { '4', '$', 0 },
    [0x06] = { '5', '%', 0 },  [0x07] = { '6', '^', 0 },
    [0x08] = { '7', '&', 0 },  [0x09] = { '8', '*', 0 },
    [0x0A] = { '9', '(', 0 },  [0x0B] = { '0', ')', 0 },
    [0x0C] = { '-', '_', 0 },  [0x0D] = { '=', '+', 0 },
    [0x10] = { 'q', 'Q', 1 },  [0x11] = { 'w', 'W', 1 },
    [0x12] = { 'e', 'E', 1 },  [0x13] = { 'r', 'R', 1 },
    [0x14] = { 't', 'T', 1 },  [0x15] = { 'y', 'Y', 1 },
    [0x16] = { 'u', 'U', 1 },  [0x17] = { 'i', 'I', 1 },
    [0x18] = { 'o', 'O', 1 },  [0x19] = { 'p', 'P', 1 },
    [0x1A] = { '[', '{', 0 },  [0x1B] = { ']', '}', 0 },
    [0x1E] = { 'a', 'A', 1 },  [0x1F] = { 's', 'S', 1 },
    [0x20] = { 'd', 'D', 1 },  [0x21] = { 'f', 'F', 1 },
    [0x22] = { 'g', 'G', 1 },  [0x23] = { 'h', 'H', 1 },
    [0x24] = { 'j', 'J', 1 },  [0x25] = { 'k', 'K', 1 },
    [0x26] = { 'l', 'L', 1 },  [0x27] = { ';', ':', 0 },
    [0x28] = { '\'', '"', 0 }, [0x29] = { '`', '~', 0 },
    [0x2B] = { '\\', '|', 0 }, [0x2C] = { 'z', 'Z', 1 },
    [0x2D] = { 'x', 'X', 1 },  [0x2E] = { 'c', 'C', 1 },
    [0x2F] = { 'v', 'V', 1 },  [0x30] = { 'b', 'B', 1 },
    [0x31] = { 'n', 'N', 1 },  [0x32] = { 'm', 'M', 1 },
    [0x33] = { ',', '<', 0 },  [0x34] = { '.', '>', 0 },
    [0x35] = { '/', '?', 0 },  [0x39] = { ' ', ' ', 0 },
};

/* ── 位图与小工具 ───────────────────────────────────────────── */

static int ps2_is_pressed(const gui_ps2_state_t *st, unsigned slot)
{
    return (st->pressed[slot >> 3] >> (slot & 7u)) & 1u;
}

static void ps2_set_pressed(gui_ps2_state_t *st, unsigned slot)
{
    st->pressed[slot >> 3] |= (uint8_t)(1u << (slot & 7u));
}

static void ps2_clear_pressed(gui_ps2_state_t *st, unsigned slot)
{
    st->pressed[slot >> 3] &= (uint8_t)~(1u << (slot & 7u));
}

/* 物理键身份对应的修饰键位（非修饰键返回 0）。 */
static uint16_t ps2_modifier_bit(uint16_t physical)
{
    switch (physical) {
    case PS2_KEY_LSHIFT: return GUI_MOD_LSHIFT;
    case PS2_KEY_RSHIFT: return GUI_MOD_RSHIFT;
    case PS2_KEY_LCTRL:  return GUI_MOD_LCTRL;
    case PS2_KEY_RCTRL:  return GUI_MOD_RCTRL;
    case PS2_KEY_LALT:   return GUI_MOD_LALT;
    case PS2_KEY_RALT:   return GUI_MOD_RALT;
    default:             return 0;
    }
}

/* 当前是否有 Ctrl/Alt 被按住：组合键不产生字符。 */
static int ps2_ctrl_alt_active(uint16_t modifiers)
{
    return (modifiers & (GUI_MOD_LCTRL | GUI_MOD_RCTRL |
                         GUI_MOD_LALT  | GUI_MOD_RALT)) != 0;
}

/* 按当前修饰键快照翻译一个主键区扫描码。 */
static uint32_t ps2_translate(uint8_t code, int extended, uint16_t modifiers)
{
    const ps2_ascii_t *entry;
    int shift;
    int caps;

    /* E0 键（导航区、keypad、Enter 等）只报告物理身份，不产生字符。 */
    if (extended)
        return 0;

    entry = &g_ascii[code];
    if (entry->base == 0)
        return 0;   /* 未知扫描码：无字符 */

    shift = (modifiers & (GUI_MOD_LSHIFT | GUI_MOD_RSHIFT)) != 0;
    caps  = (modifiers & GUI_MOD_CAPS) != 0;

    if (entry->alpha)
        return (uint32_t)((shift ^ caps) ? entry->shifted : entry->base);
    return (uint32_t)(shift ? entry->shifted : entry->base);
}

/* ── 序列恢复 ───────────────────────────────────────────────── */

/* 序列完成：回到序列边界，下一个字节开始一个新序列。 */
static void ps2_sequence_done(gui_ps2_state_t *st)
{
    st->phase  = PS2_PH_IDLE;
    st->prefix = 0;
}

/* 损坏的序列：整个片段被吞掉并恢复到序列边界。绝不把已消费的字节
 * 重新解释成独立修饰键，因此永远不会产生假的 Shift/Ctrl。 */
static int ps2_corrupt(gui_ps2_state_t *st)
{
    ps2_sequence_done(st);
    return 0;
}

/* 特殊序列（PrintScreen / Pause）：整段吞掉，不产生按键事件，也不
 * 触碰修饰键状态。 */
static int ps2_swallow(gui_ps2_state_t *st)
{
    ps2_sequence_done(st);
    return 0;
}

/* ── 边沿产生 ───────────────────────────────────────────────── */

/* 处理一个完整键序列（普通或 E0 前缀）的按下/释放边沿。 */
static int ps2_emit(gui_ps2_state_t *st, uint8_t code, int is_break, int extended,
                    gui_key_event_t *out)
{
    uint16_t physical = extended ? (uint16_t)(GUI_PS2_KEY_E0 | code)
                                  : (uint16_t)code;
    unsigned slot = gui_ps2_key_slot(physical);
    uint16_t modbit = ps2_modifier_bit(physical);
    uint32_t codepoint;

    if (is_break) {
        int was_pressed = ps2_is_pressed(st, slot);

        /* 物理修饰状态必须收敛：初始快照可能带入了修饰键。 */
        if (modbit)
            st->modifiers &= (uint16_t)~modbit;

        /* 没有对应的按下边沿：不是可归属的真实边沿，不产生 UP 事件。 */
        if (!was_pressed)
            return 0;

        ps2_clear_pressed(st, slot);
        st->cached[slot] = 0;

        out->physical_key = physical;
        out->modifiers    = st->modifiers;
        out->codepoint    = 0;          /* UP 恒无字符 */
        out->action       = GUI_KEY_UP;
        return 1;
    }

    /* typematic 重复 make：保持按住状态，但不是新的按下边沿。 */
    if (ps2_is_pressed(st, slot))
        return 0;
    ps2_set_pressed(st, slot);

    /* 先更新状态，再报告事件：事件的 modifiers 是处理时刻的快照。 */
    if (physical == PS2_KEY_CAPS)
        st->modifiers ^= GUI_MOD_CAPS;  /* 只在真实的 up -> down 边沿翻转 */
    else if (modbit)
        st->modifiers |= modbit;

    codepoint = 0;
    if (!modbit && physical != PS2_KEY_CAPS && !ps2_ctrl_alt_active(st->modifiers))
        codepoint = ps2_translate(code, extended, st->modifiers);

    st->cached[slot] = (uint8_t)codepoint;

    out->physical_key = physical;
    out->modifiers    = st->modifiers;
    out->codepoint    = codepoint;
    out->action       = GUI_KEY_DOWN;
    return 1;
}

/* ── 公共接口 ───────────────────────────────────────────────── */

void gui_ps2_init(gui_ps2_state_t *st, uint16_t initial_modifiers)
{
    if (!st)
        return;

    st->prefix    = 0;
    st->phase     = PS2_PH_IDLE;
    st->prefix_ms = 0;
    st->modifiers = initial_modifiers;
    memset(st->pressed, 0, sizeof(st->pressed));
    memset(st->cached, 0, sizeof(st->cached));
}

void gui_ps2_expire(gui_ps2_state_t *st, uint64_t now_ms)
{
    if (!st || st->prefix == 0)
        return;

    if (now_ms - st->prefix_ms >= GUI_PS2_PREFIX_TIMEOUT_MS) {
        st->phase  = PS2_PH_IDLE;
        st->prefix = 0;
    }
}

int gui_ps2_feed(gui_ps2_state_t *st, uint8_t byte, uint64_t now_ms,
                 gui_key_event_t *out)
{
    if (!st || !out) {
        errno = EINVAL;
        return -1;
    }

    /* 超时的待定前缀必须先丢弃：新字节不得复活一个幽灵前缀。 */
    gui_ps2_expire(st, now_ms);

    switch (st->phase) {
    case PS2_PH_IDLE:
        if (byte == PS2_E0) {
            st->prefix    = PS2_E0;
            st->phase     = PS2_PH_E0;
            st->prefix_ms = now_ms;
            return 0;
        }
        if (byte == PS2_E1) {
            st->prefix    = PS2_E1;
            st->phase     = PS2_PH_PAUSE_1D;
            st->prefix_ms = now_ms;
            return 0;
        }
        /* 普通 make/break 自身即是一个完整序列。 */
        return ps2_emit(st, (uint8_t)(byte & 0x7Fu), (byte & 0x80u) != 0, 0, out);

    case PS2_PH_E0:
        /* E0 2A 与 E0 B7 打开 PrintScreen 的两个序列。它们**不是**
         * 左/右 Shift 的按下，绝不能更新修饰键状态。 */
        if (byte == 0x2Au) {
            st->phase = PS2_PH_PS_MAKE;
            return 0;
        }
        if (byte == 0xB7u) {
            st->phase = PS2_PH_PS_BREAK;
            return 0;
        }
        /* 前缀之后不能再出现前缀：E0 E0 / E0 E1 不是合法序列。 */
        if (byte == PS2_E0 || byte == PS2_E1)
            return ps2_corrupt(st);
        /* 普通的 E0 前缀键在扫描码之后即完成。 */
        ps2_sequence_done(st);
        return ps2_emit(st, (uint8_t)(byte & 0x7Fu), (byte & 0x80u) != 0, 1, out);

    case PS2_PH_PS_MAKE:
        if (byte != PS2_E0)
            return ps2_corrupt(st);
        st->phase = PS2_PH_PS_MAKE_E0;
        return 0;

    case PS2_PH_PS_MAKE_E0:
        if (byte != PS2_PS_MAKE_TAIL)
            return ps2_corrupt(st);
        return ps2_swallow(st);

    case PS2_PH_PS_BREAK:
        if (byte != PS2_E0)
            return ps2_corrupt(st);
        st->phase = PS2_PH_PS_BREAK_E0;
        return 0;

    case PS2_PH_PS_BREAK_E0:
        if (byte != PS2_PS_BREAK_TAIL)
            return ps2_corrupt(st);
        return ps2_swallow(st);

    case PS2_PH_PAUSE_1D:
        if (byte != PS2_PAUSE_CTRL)
            return ps2_corrupt(st);
        st->phase = PS2_PH_PAUSE_45;
        return 0;

    case PS2_PH_PAUSE_45:
        if (byte != PS2_PAUSE_KEY)
            return ps2_corrupt(st);
        st->phase = PS2_PH_PAUSE_E1;
        return 0;

    case PS2_PH_PAUSE_E1:
        if (byte != PS2_E1)
            return ps2_corrupt(st);
        st->phase = PS2_PH_PAUSE_9D;
        return 0;

    case PS2_PH_PAUSE_9D:
        if (byte != PS2_PAUSE_TAIL_1)
            return ps2_corrupt(st);
        st->phase = PS2_PH_PAUSE_C5;
        return 0;

    case PS2_PH_PAUSE_C5:
        if (byte != PS2_PAUSE_TAIL_2)
            return ps2_corrupt(st);
        return ps2_swallow(st);

    default:
        return ps2_corrupt(st);
    }
}
