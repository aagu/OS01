// kernel/arch/aarch64/platform/console.c — aarch64 backend of the
// <arch/console.h> facade: PL011 serial console output and RX input
// (RP1 MSI-X on RPi5 rp1-header, direct GIC SPI on normal DTB, or
// 100 Hz BSP poll fallback).

#include <arch/console.h>
#include <arch/aarch64/console.h>
#include <arch/aarch64/uart.h>
#include <arch/aarch64/gic.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/rp1_irq.h>
#include <arch/cpu.h>
#include <core/bootinfo.h>
#include <driver/serial.h>
#include <tty/tty.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdint.h>

#define AARCH64_RP1_HEADER_UART_PA UINT64_C(0x1f00030000)

#ifdef OS01_HOST_TEST
uint64_t rp1_console_test_now(void);
uint64_t rp1_console_test_freq(void);
#define console_counter_now()  rp1_console_test_now()
#define console_counter_freq() rp1_console_test_freq()
#define console_irq_save()     0UL
#define console_irq_restore(f) ((void)(f))
#else
#include <arch/aarch64/reg.h>
#define console_counter_now()  cntpct_el0()
#define console_counter_freq() cntfrq_el0()
static inline unsigned long console_irq_save(void)
{
    unsigned long daif;
    __asm__ __volatile__("mrs %0, daif\n\tmsr daifset, #2\n\tisb"
                         : "=r"(daif) :: "memory");
    return daif;
}
static inline void console_irq_restore(unsigned long daif)
{
    __asm__ __volatile__("msr daif, %0\n\tisb" :: "r"(daif) : "memory");
}
#endif

/* PL011 RX primitives from platform/pl011.c (FR/IMSC stay private there). */
bool pl011_rx_pending(void);
uint32_t pl011_dr_read(void);
void pl011_irq_rx_enable(void);
void pl011_clear_ints(void);
extern void keyboard_set_tty(tty_t *tty) __attribute__((weak));

static tty_t *console_tty;
static bool console_rx_poll_needed;
#ifndef OS01_SELFTEST
static bool s_console_rp1_header;
static struct rp1_irq *s_console_rp1_irq;
#endif

#if defined(AARCH64_RP1_IRQ_DIAGNOSTIC) && AARCH64_RP1_IRQ_DIAGNOSTIC
static unsigned long s_diag_irq_count;
static unsigned long s_diag_irq_bytes;
static unsigned long s_diag_poll_bytes;
static unsigned s_diag_sample_n;
static unsigned long s_diag_sample;
#endif

#ifdef OS01_HOST_TEST
void aarch64_console_test_reset(void)
{
    console_tty = NULL;
    console_rx_poll_needed = false;
#ifndef OS01_SELFTEST
    s_console_rp1_header = false;
    s_console_rp1_irq = NULL;
#endif
#if defined(AARCH64_RP1_IRQ_DIAGNOSTIC) && AARCH64_RP1_IRQ_DIAGNOSTIC
    s_diag_irq_count = 0;
    s_diag_irq_bytes = 0;
    s_diag_poll_bytes = 0;
    s_diag_sample_n = 0;
    s_diag_sample = 0;
#endif
}
#endif

bool aarch64_console_uses_rp1_header(const struct boot_context *ctx)
{
    if (!boot_context_valid(ctx))
        return false;
    if ((ctx->flags & BOOT_CONTEXT_HAS_DTB) == 0u ||
        (ctx->flags & BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED) == 0u ||
        (ctx->flags & BOOT_CONTEXT_HAS_EARLY_MMIO) == 0u)
        return false;
    return ctx->early_mmio.console_base == AARCH64_RP1_HEADER_UART_PA;
}

void aarch64_console_detect_mode(const struct boot_context *ctx)
{
#ifndef OS01_SELFTEST
    s_console_rp1_header = aarch64_console_uses_rp1_header(ctx);
#else
    (void)ctx;
#endif
}

static void pl011_console_drain_rx(bool from_irq)
{
    (void)from_irq;
    while (pl011_rx_pending()) {
        uint32_t dr = pl011_dr_read();
        char c = (char)(uint8_t)dr;
        if (console_tty) {
            tty_push_input(console_tty, c);
#if defined(AARCH64_RP1_IRQ_DIAGNOSTIC) && AARCH64_RP1_IRQ_DIAGNOSTIC
            if (from_irq)
                __atomic_add_fetch(&s_diag_irq_bytes, 1UL, __ATOMIC_RELAXED);
            else
                __atomic_add_fetch(&s_diag_poll_bytes, 1UL, __ATOMIC_RELAXED);
            if (s_diag_sample_n < 8u) {
                s_diag_sample |= ((unsigned long)(uint8_t)c) << (8u * s_diag_sample_n);
                s_diag_sample_n++;
            }
#endif
        }
    }
}

#ifndef OS01_SELFTEST
/* Normal DTB direct GIC SPI handler (level-triggered EOI contract). */
static void pl011_console_rx_handler(uint32_t intid, uint64_t param,
                                     struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;

    pl011_console_drain_rx(true);
    pl011_clear_ints();
}

/* RP1 MSI-X callback: drain FIFO without writing full PL011_ICR. */
static void rp1_console_rx_cb(void *arg)
{
    (void)arg;
#if defined(AARCH64_RP1_IRQ_DIAGNOSTIC) && AARCH64_RP1_IRQ_DIAGNOSTIC
    __atomic_add_fetch(&s_diag_irq_count, 1UL, __ATOMIC_RELAXED);
#endif
    pl011_console_drain_rx(true);
}
#endif

void aarch64_console_poll(void)
{
    if (!__atomic_load_n(&console_rx_poll_needed, __ATOMIC_ACQUIRE) ||
        !console_tty)
        return;
    pl011_console_drain_rx(false);
}

void aarch64_console_diag_tick(void)
{
#if defined(AARCH64_RP1_IRQ_DIAGNOSTIC) && AARCH64_RP1_IRQ_DIAGNOSTIC
    log_info("[rp1-rx] irq=%lu irq_bytes=%lu poll_bytes=%lu sample_n=%u sample=%lx\n",
             __atomic_load_n(&s_diag_irq_count, __ATOMIC_RELAXED),
             __atomic_load_n(&s_diag_irq_bytes, __ATOMIC_RELAXED),
             __atomic_load_n(&s_diag_poll_bytes, __ATOMIC_RELAXED),
             s_diag_sample_n, s_diag_sample);
#endif
}

#ifndef OS01_SELFTEST
static void rp1_attach_fallback(const char *reason)
{
    (void)pl011_rx_irq_mask();
    __atomic_store_n(&console_rx_poll_needed, true, __ATOMIC_RELEASE);
    log_warn("aarch64: PL011 console RX fallback (%s), using 100 Hz poll fallback\n",
             reason);
}
#endif

void arch_console_input_attach(struct tty_struct *tty)
{
    if (keyboard_set_tty)
        keyboard_set_tty(tty);
#ifdef OS01_SELFTEST
    console_tty = tty;
    return;
#else
    unsigned long irq_flags = console_irq_save();
    console_tty = tty;

    if (s_console_rp1_header) {
#if defined(AARCH64_RP1_IRQ_FORCE_POLL) && AARCH64_RP1_IRQ_FORCE_POLL
        rp1_attach_fallback("forced-poll");
        console_irq_restore(irq_flags);
        return;
#endif
        const struct aarch64_rp1_irq_desc *desc = aarch64_rp1_irq_desc_get();
        const struct aarch64_rp1_irq_platform *plat = aarch64_rp1_irq_platform_get();
        if (!desc || !desc->valid || !desc->uart_valid || !plat) {
            rp1_attach_fallback("no-provider");
            console_irq_restore(irq_flags);
            return;
        }

        if (rp1_irq_prepare(desc->uart_source, RP1_IRQ_LEVEL_HIGH,
                            rp1_console_rx_cb, NULL, &s_console_rp1_irq) != 0) {
            rp1_attach_fallback("prepare-failed");
            console_irq_restore(irq_flags);
            return;
        }

#if defined(AARCH64_RP1_IRQ_DIAGNOSTIC) && AARCH64_RP1_IRQ_DIAGNOSTIC
        log_info("[rp1-rx] pre-arm\n");
        {
            uint64_t freq = console_counter_freq();
            uint64_t wait_ticks = (freq / 1000ULL) * 100ULL;
            uint64_t t0 = console_counter_now();
            while ((console_counter_now() - t0) < wait_ticks) {
                if (pl011_rx_pending())
                    break;
            }
        }
#endif

        if (pl011_rp1_rx_irq_enable() != 0) {
            bool mask_ok = (pl011_rx_irq_mask() == 0);
            (void)rp1_irq_abort(s_console_rp1_irq, mask_ok);
            s_console_rp1_irq = NULL;
            __atomic_store_n(&console_rx_poll_needed, true, __ATOMIC_RELEASE);
            log_warn("aarch64: PL011 console RX fallback (imsc-enable-failed), using 100 Hz poll fallback\n");
            console_irq_restore(irq_flags);
            return;
        }

        if (rp1_irq_commit(s_console_rp1_irq) != 0) {
            bool mask_ok = (pl011_rx_irq_mask() == 0);
            (void)rp1_irq_abort(s_console_rp1_irq, mask_ok);
            s_console_rp1_irq = NULL;
            __atomic_store_n(&console_rx_poll_needed, true, __ATOMIC_RELEASE);
            log_warn("aarch64: PL011 console RX fallback (commit-failed), using 100 Hz poll fallback\n");
            console_irq_restore(irq_flags);
            return;
        }

        __atomic_store_n(&console_rx_poll_needed, false, __ATOMIC_RELEASE);
        log_info("aarch64: PL011 console RX armed via RP1 source=%u intid=%u\n",
                 (unsigned)desc->uart_source,
                 (unsigned)rp1_irq_intid(s_console_rp1_irq));
        console_irq_restore(irq_flags);
        return;
    }

    uint32_t intid = dtb_pl011_spi();   /* QEMU virt: 33 (SPI 1), from DTB */
    uint8_t bsp_target = gic_target_bit_get(0);
    if (intid == 0 || bsp_target == 0 ||
        gic_register_handler(intid, pl011_console_rx_handler, 0,
                             "pl011-console") != 0) {
        __atomic_store_n(&console_rx_poll_needed, true, __ATOMIC_RELEASE);
        log_warn("aarch64: PL011 console RX IRQ not armed (intid=%u), using 100 Hz poll fallback\n",
                 intid);
        console_irq_restore(irq_flags);
        return;
    }
    if (gic_irq_configure(intid, true, 0x00, bsp_target) != 0) {
        (void)gic_unregister_handler(intid);
        __atomic_store_n(&console_rx_poll_needed, true, __ATOMIC_RELEASE);
        log_warn("aarch64: PL011 console RX IRQ config failed (intid=%u), using 100 Hz poll fallback\n",
                 intid);
        console_irq_restore(irq_flags);
        return;
    }
    pl011_irq_rx_enable();
    log_info("aarch64: PL011 console RX armed intid=%u\n", intid);
    console_irq_restore(irq_flags);
#endif
}

void console_init(void) {}

void arch_console_putchar(char c)
{
    write_serial_unlocked(c);
}

void console_surrender_fb(void) {}
void console_force_enable_locked(void) {}
void console_set_active_locked(bool active) { (void)active; }
void console_notify_resize_locked(void) {}
