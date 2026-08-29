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
#include <kernel/arch/cpu.h>   /* arch_nop for tiny spin waits */

/* MMIO base: QEMU virt wires PL011 here (per spec §2.2). */
#define PL011_BASE       ((volatile uint32_t *)0x09000000UL)

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
    PL011_BASE[off / 4] = v;
}

static inline uint32_t pl011_r32(uint32_t off)
{
    return PL011_BASE[off / 4];
}

/* Initialise PL011: 8N1, FIFO enabled, TX+RX enabled.  QEMU ignores
 * baud divisors but we set them anyway for the RPi3 case. */
void pl011_init(void)
{
    /* Disable UART while we configure. */
    pl011_w32(PL011_CR, 0);

    /* Disable all interrupts — phase 1 is polled. */
    pl011_w32(PL011_IMSC, 0);
    pl011_w32(PL011_ICR, 0x7FF);   /* clear any pending */

    /* 16 MHz / (16 * 115200) ≈ 8.68 ⇒ IBRD=8, FBRD=44.  Either is
     * fine for QEMU; RPi3's PL011 needs accurate values to actually
     * communicate.  Phase-1 doesn't care about baud as long as the
     * FIFO+8N1 are right. */
    pl011_w32(PL011_IBRD, 8);
    pl011_w32(PL011_FBRD, 44);

    /* 8 bits, no parity, 1 stop bit; FIFO enabled. */
    pl011_w32(PL011_LCR_H, (1U << 4) | (1U << 5) | (1U << 6));

    /* Re-enable UART: TX + RX + UART enable. */
    pl011_w32(PL011_CR, (1U << 0) | (1U << 8) | (1U << 9));
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
