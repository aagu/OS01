/*
 * hosttests/cases/probe_gui_ps2_public.c — D02 的独立消费者探针。
 *
 * 这个 TU 代表“一个不碰 libgui 内部、也不使用 LVGL 的独立应用”：
 * 它只包含**已发布**的公开头 <gui/gui_input_ps2.h>，只调用 gui_ps2_*
 * 三个函数，并且不引用平台层、输入适配层或任何 LVGL 符号。
 *
 * 它由 qemutests/gui_build_audit.py 驱动：
 *   - 用目标工具链、只用发布 generation 的包含路径编译（不得出现仓库
 *     源头的包含路径，也不得出现 libgui/internal）；
 *   - 按目标链接顺序 crt0.o → sigreturn_trampoline.o → 本对象 →
 *     -lgui -lgfx -lc 链接，并输出链接 map；
 *   - 审计据此断言：链接进来的对象只有解码器本身与 libc，平台层、
 *     完整 input 适配层与 LVGL 对象一个都不许出现。
 *
 * 因此这里的业务调用面必须**只有** gui_ps2_*：多写一个别的调用，
 * 审计就会在链接 map 上看到它。
 */

#include <gui/gui_input_ps2.h>

/* The probe drives the decoder the way an application would: init with the
 * allowed modifier snapshot (0 or GUI_MOD_CAPS), feed a Set 1 byte, and
 * expire a stale prefix. */

static int probe_feed(gui_ps2_state_t *st, uint8_t byte, uint64_t now_ms,
                      gui_key_event_t *out)
{
    return gui_ps2_feed(st, byte, now_ms, out);
}

int main(void)
{
    gui_ps2_state_t st;
    gui_key_event_t event;

    gui_ps2_init(&st, 0);
    gui_ps2_expire(&st, 0);

    /* 'a' make (Set 1 0x1E) then break (0x9E) */
    if (probe_feed(&st, 0x1Eu, 10u, &event) != 1)
        return 1;
    if (event.action != GUI_KEY_DOWN)
        return 2;
    if (probe_feed(&st, 0x9Eu, 20u, &event) != 1)
        return 3;
    if (event.action != GUI_KEY_UP)
        return 4;

    /* an E0-prefixed key: the prefix and the scan code may arrive in
     * different reads, and the decoder keeps the pairing. */
    if (probe_feed(&st, 0xE0u, 30u, &event) != 0)
        return 5;
    if (probe_feed(&st, 0x75u, 31u, &event) != 1)
        return 6;
    if ((event.physical_key & GUI_PS2_KEY_E0) == 0)
        return 7;

    /* a truncated prefix expires after GUI_PS2_PREFIX_TIMEOUT_MS */
    if (probe_feed(&st, 0xE0u, 40u, &event) != 0)
        return 8;
    gui_ps2_expire(&st, 40u + GUI_PS2_PREFIX_TIMEOUT_MS + 1u);
    if (probe_feed(&st, 0x75u, 1000u, &event) != 1)
        return 9;
    if ((event.physical_key & GUI_PS2_KEY_E0) != 0)
        return 10;

    return 0;
}
