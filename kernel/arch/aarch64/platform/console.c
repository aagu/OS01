// kernel/arch/aarch64/platform/console.c — aarch64 backend of the
// <arch/console.h> facade: PL011 serial console output and the PL011 RX
// IRQ input producer for the console TTY.
//
// There is no framebuffer text console on aarch64 yet (ramfb exists but is
// not a glyph console), so the console TTY output is the PL011 serial line
// — the same wire printk uses.  Input: the PL011 RX interrupt drains the
// FIFO into tty_push_input, mirroring the x86 COM1 RX path.

#include <arch/console.h>
#include <arch/aarch64/gic.h>
#include <arch/aarch64/dtb.h>
#include <driver/serial.h>
#include <tty/tty.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdint.h>

/* PL011 RX primitives from platform/pl011.c (FR/IMSC stay private there). */
bool pl011_rx_pending(void);
uint32_t pl011_dr_read(void);
void pl011_irq_rx_enable(void);
void pl011_clear_ints(void);

static tty_t *console_tty;

/* Level-triggered contract (spec §2.3): drain the RX FIFO (each DR read
 * clears the device source) + ICR BEFORE EOI; EOI is issued by
 * gic_dev_dispatch after the handler returns.  RX bytes feed the console
 * TTY exactly like the x86 serial IRQ path does. */
static void pl011_console_rx_handler(uint32_t intid, uint64_t param,
                                     struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;

    while (pl011_rx_pending()) {
        uint32_t dr = pl011_dr_read();
        char c = (char)(uint8_t)dr;
        if (console_tty)
            tty_push_input(console_tty, c);
    }
    pl011_clear_ints();
}

void arch_console_input_attach(struct tty_struct *tty)
{
    console_tty = tty;

#ifdef OS01_SELFTEST
    /* The GIC SPI selftest (gic_spi_test_init) owns the PL011 RX interrupt
     * in selftest builds — it registers its own handler for the same
     * intid and its harness injects bytes with count assertions.  Leave
     * the console input-free there; the console still works for output. */
    return;
#endif

    uint32_t intid = dtb_pl011_spi();   /* QEMU virt: 33 (SPI 1), from DTB */
    if (intid == 0 ||
        gic_register_handler(intid, pl011_console_rx_handler, 0,
                             "pl011-console") != 0 ||
        gic_irq_configure(intid, true, 0x00, 0x01) != 0) {
        /* Input is a degrade, not a boot failure: the console still works
         * for output (same wire as printk). */
        log_warn("aarch64: PL011 console RX IRQ not armed (intid=%u)\n",
                 intid);
        return;
    }
    pl011_irq_rx_enable();
    log_info("aarch64: PL011 console RX armed intid=%u\n", intid);
}

void console_init(void)
{
    /* The aarch64 console has no framebuffer text state to reset: the
     * PL011 serial line IS the console, and the tty layer arms the RX
     * IRQ through tty_boot_init -> arch_console_input_attach. */
}

void arch_console_putchar(char c)
{
    // tty_write() holds serial_lock across the buffer; use the unlocked
    // primitive (identical discipline to the x86 console_putchar).
    write_serial_unlocked(c);
}

/* Framebuffer console coordination stubs: on aarch64 the kernel console
 * is PL011 serial rather than a framebuffer text console, so surrendering
 * or resizing the framebuffer does not need to pause/resume a glyph console. */
void console_surrender_fb(void) {}
void console_force_enable_locked(void) {}
void console_set_active_locked(bool active) { (void)active; }
void console_notify_resize_locked(void) {}
