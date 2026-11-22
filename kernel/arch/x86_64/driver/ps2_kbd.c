/* kernel/driver/ps2_kbd.c — x86 PS/2 (i8042) keyboard producer */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <driver/keyboard.h>
#include <driver/i8042.h>
#include <driver/input_route.h>
#include <intr/interrupt.h>
#include <arch/irq.h>
#include <core/debug.h>

/* ═══════════════════════════════════════════════════════════
 *  Byte consumer — dispatched by the i8042 demux layer
 * ═══════════════════════════════════════════════════════════
 *
 * Called by i8042_pump() with the i8042 lock HELD and local IRQs
 * disabled: must not touch the i8042 ports, re-take the i8042 lock,
 * sleep, or copy user memory.  Lock order: i8042 → route → NORMAL
 * queue/poll.
 */
static void kbd_consume(uint8_t sc)
{
    input_route_keyboard_byte(sc);
}

/* ── IRQ1 handler ──────────────────────────────────────────────
 * The IRQ line is only a wake-up hint: the shared i8042 layer owns
 * the bytes and decides per byte (status bit 5) whether it is a
 * keyboard or aux (mouse) byte. */
static void keyboard_handler(uint64_t nr __attribute__((unused)),
                             uint64_t parameter __attribute__((unused)),
                             pt_regs_t *regs __attribute__((unused)))
{
    (void)i8042_pump();
}

/* Drain the 8042 output buffer through the shared demux layer. */
void keyboard_poll(void)
{
    (void)i8042_pump();
}

/* Enable keyboard IRQ generation at the PS/2 controller level. */
static int keyboard_enable_irq(void)
{
    uint8_t cmd;
    if (i8042_read_command_byte(&cmd) != I8042_OK)
        return -1;

    debug_irq("kbd: PS/2 command byte was %#x", cmd);

    cmd |= 0x01;   /* enable IRQ1 */
    cmd |= 0x04;   /* set system flag */
    cmd &= ~0x08;  /* ensure keyboard is enabled */

    if (i8042_write_command_byte(cmd) != I8042_OK)
        return -1;

    debug_irq(" → %#x\n", cmd);
    return 0;
}

int keyboard_init(void)
{
    /* Initialise the route and NORMAL sink first (idempotent) so non-PS/2
     * keyboard input remains usable even if i8042 probing fails. */
    keyboard_input_init();

    i8042_init();
    i8042_set_kbd_consumer(kbd_consume);

    if (keyboard_enable_irq() != 0)
        goto fail;

    uint32_t kbd_gsi = arch_irq_isa_to_gsi(1);
    if (register_irq(kbd_gsi, NULL, &keyboard_handler, 0, IRQF_TRIGGER_EDGE, "keyboard") != 1) {
        uint8_t cmd;
        if (i8042_read_command_byte(&cmd) == I8042_OK)
            i8042_write_command_byte(cmd & ~(uint8_t)0x01);
        goto fail;
    }

    return 0;

fail:
    i8042_set_kbd_consumer(NULL);
    return -1;
}
