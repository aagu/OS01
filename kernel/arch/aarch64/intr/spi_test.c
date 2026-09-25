/* SPI path selftest (spec §7.4, Task 2.3b): PL011 RX -> GIC SPI(dtb) ->
 * handler table. The harness (qemutests/aarch64_gic_spi.py, Task 2.3a)
 * waits for the "armed" line, then injects one byte into the PL011 and
 * asserts the "handled count=1" line. Level-triggered contract: the
 * handler clears the device source (read DR + ICR) BEFORE EOI; EOI is
 * issued by gic_dev_dispatch after the handler returns (spec §2.3). */
#if OS01_SELFTEST
#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/gic.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/boot_log.h>

uint32_t pl011_dr_read(void);
void pl011_irq_rx_enable(void);
void pl011_clear_ints(void);

static volatile uint32_t g_spi_count;

static void pl011_rx_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)param; (void)regs;
    (void)pl011_dr_read();                /* clear device source #1 */
    pl011_clear_ints();                   /* clear device source #2 */
    uint32_t n = g_spi_count + 1;
    g_spi_count = n;
    kputs("[gic-spi] intid=");
    kputu(intid);
    kputs(" handled count=");
    kputu(n);
    kputs("\n");
}

void gic_spi_test_init(void)
{
    uint32_t intid = dtb_pl011_spi();     /* 33 on QEMU virt, from DTB */
    if (intid == 0 ||
        gic_register_handler(intid, pl011_rx_handler, 0, "pl011-rx") != 0 ||
        gic_irq_configure(intid, true, 0x00, 0x01) != 0) { /* route BSP(bit0), R1-2 */
        log_err("[gic] spi-test arm FAIL\n");
        return;
    }
    pl011_irq_rx_enable();
    kputs("[gic] spi-test armed intid=");
    kputu(intid);
    kputs("\n");
}
#endif
