/* aarch64 phase 1: minimal PL011 UART driver, polled only.
 *
 * QEMU's virt machine wires the PL011 to MMIO @ 0x09000000.  In the
 * `-nographic -serial mon:stdio` invocation the host sees every byte
 * written to DR.  No real baud config is needed for QEMU, but we still
 * programme LCR_H + CR because (a) the reset value of TXFF in FR may
 * vary, and (b) Phase 2 (RPi3) requires correct programming.
 *
 * Per controller ruling R2 we do NOT reuse kernel/printk.c — it pulls
 * in x86 serial headers.  A tiny `kputs(char*)` lives here instead;
 * main.c calls it directly.
 */

#include <stdint.h>
#include <arch/cpu.h>   /* arch_nop for tiny spin waits */

/* Runtime PL011 base comes from dtb_pl011_base() once dtb_init() has run;
 * pre-DTB early boot falls back to QEMU virt's 0x09000000 (matching head.S).
 * The high alias is the only alias that survives a private user root in
 * TTBR0_EL1: the bootstrap high alias and the M1 tree both cover this Device
 * PA, while the low identity map belongs to TTBR0 and disappears with the user
 * root. */
#include <arch/aarch64/dtb.h>
#include <arch/mmu.h>
#define PL011_EARLY_FALLBACK_PA 0x09000000UL
#define PL011_DEFAULT_CLK_HZ    16000000U
#define PL011_BAUD_RATE         115200U

static inline volatile uint32_t *pl011_base(void)
{
    uint64_t pa = dtb_pl011_base();
    if (!pa)
        pa = PL011_EARLY_FALLBACK_PA;
    return (volatile uint32_t *)(ARCH_PAGE_OFFSET + pa);
}

/* Register offsets (32-bit, little-endian). */
#define PL011_DR         0x00   /* Data Register               */
#define PL011_FR         0x18   /* Flag Register               */
#define PL011_IBRD       0x24   /* Integer Baud-Rate Divisor   */
#define PL011_FBRD       0x28   /* Fractional Baud-Rate Divisor*/
#define PL011_LCR_H      0x2C   /* Line Control Register       */
#define PL011_CR         0x30   /* Control Register            */
#define PL011_IMSC       0x38   /* Interrupt Mask Set/Clear    */
#define PL011_ICR        0x44   /* Interrupt Clear Register    */

/* FR bit positions. */
#define PL011_FR_TXFF    (1U << 5)    /* Transmit FIFO full   */
#define PL011_FR_BUSY    (1U << 3)    /* UART busy            */

static inline void pl011_w32(uint32_t off, uint32_t v)
{
    pl011_base()[off / 4] = v;
}

static inline uint32_t pl011_r32(uint32_t off)
{
    return pl011_base()[off / 4];
}

static void pl011_calc_divisors(uint32_t clk_hz, uint32_t baud,
                                uint32_t *ibrd_out, uint32_t *fbrd_out)
{
    if (clk_hz == 0)
        clk_hz = PL011_DEFAULT_CLK_HZ;
    uint64_t div64 = ((uint64_t)clk_hz * 4U + baud / 2U) / baud;
    uint64_t ibrd = div64 >> 6;
    uint64_t fbrd = div64 & 0x3FU;
    if (ibrd == 0 || ibrd > 0xFFFFU) {
        ibrd = 8;
        fbrd = 44;
    }
    *ibrd_out = (uint32_t)ibrd;
    *fbrd_out = (uint32_t)fbrd;
}

/* Initialise PL011: 8N1, FIFO enabled, TX+RX enabled.  QEMU ignores
 * baud divisors, but real hardware requires them derived from the DTB
 * uartclk frequency. */
void pl011_init(void)
{
    uint32_t ibrd = 0, fbrd = 0;

    /* Disable UART while we configure. */
    pl011_w32(PL011_CR, 0);

    /* Disable all interrupts — phase 1 is polled. */
    pl011_w32(PL011_IMSC, 0);
    pl011_w32(PL011_ICR, 0x7FF);   /* clear any pending */

    pl011_calc_divisors(dtb_pl011_clock(), PL011_BAUD_RATE, &ibrd, &fbrd);
    pl011_w32(PL011_IBRD, ibrd);
    pl011_w32(PL011_FBRD, fbrd);

    /* 8 bits, no parity, 1 stop bit; FIFO enabled. */
    pl011_w32(PL011_LCR_H, (1U << 4) | (1U << 5) | (1U << 6));

    /* Re-enable UART: TX + RX + UART enable. */
    pl011_w32(PL011_CR, (1U << 0) | (1U << 8) | (1U << 9));
}

/* ── RX interrupt path (GIC Phase 1, spec §7.4) ── Only RXIM is enabled;
 * polled TX continues to drive the console. The helpers below are the
 * narrow surface spi_test.c consumes; nothing else should poke IMSC/ICR. */

/* Read DR: also clears the RX FIFO data + status, which is required
 * before EOI for level-triggered interrupts (spec §2.3). */
uint32_t pl011_dr_read(void)
{
    return pl011_r32(PL011_DR);
}

/* Enable only the RX interrupt mask bit (RXIM = bit4); all other
 * sources stay masked. */
void pl011_irq_rx_enable(void)
{
    pl011_w32(PL011_IMSC, 1U << 4);
}

/* Clear all asserted PL011 interrupts (write-1-to-clear). */
void pl011_clear_ints(void)
{
    pl011_w32(PL011_ICR, 0x7FF);
}

/* Write a single byte.  Waits until the TX FIFO has room. */
void pl011_putc(char c)
{
    /* FR.TXFF set ⇒ transmit FIFO is full; spin. */
    while (pl011_r32(PL011_FR) & PL011_FR_TXFF) {
        arch_nop();
    }
    pl011_w32(PL011_DR, (uint32_t)(uint8_t)c);
    /* If we're writing a '\n', also send '\r' so terminals behave. */
    if (c == '\n') {
        while (pl011_r32(PL011_FR) & PL011_FR_TXFF) {
            arch_nop();
        }
        pl011_w32(PL011_DR, '\r');
    }
}

/* Write a NUL-terminated string.  Phase 1 only emits the boot message,
 * so we don't need a printf. */
void kputs(const char *s)
{
    while (*s) {
        pl011_putc(*s++);
    }
}

/* Write an unsigned 64-bit integer in decimal.  Task 3's tick counter
 * uses this to print a per-second summary; phase 1 has no printf and
 * no libc, so we hand-roll a single helper here.  Max 21 characters
 * for a uint64_t; we use 24 to stay safe with future 128-bit widening. */
void kputu(uint64_t v)
{
    char buf[24];
    int i = 0;
    if (v == 0) {
        pl011_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = '0' + (char)(v % 10);
        v /= 10;
    }
    while (i > 0) {
        pl011_putc(buf[--i]);
    }
}

/* Write an unsigned 64-bit integer in hexadecimal (no leading "0x").
 * Used by the boot/dtb/gic print paths to make address dumps
 * legible. */
void kputx(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    int i = 16;
    if (v == 0) {
        pl011_putc('0');
        return;
    }
    while (v > 0 && i >= 0) {
        buf[--i] = hex[v & 0xFU];
        v >>= 4;
    }
    while (i < 16) {
        pl011_putc(buf[i++]);
    }
}
