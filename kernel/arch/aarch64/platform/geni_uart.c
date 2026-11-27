/* Minimal polled TX driver for Qualcomm QUPv3 GENI SE UART.
 * Assumes UEFI firmware (SPCR) already configured clocks, GPIOs, and 115200 8N1. */
#include <arch/aarch64/uart.h>
#include <arch/cpu.h>

#define GENI_SE_STATUS          0x040U
#define GENI_TX_TRANS_LEN       0x270U
#define GENI_M_CMD0             0x600U
#define GENI_M_IRQ_STATUS       0x610U
#define GENI_M_IRQ_CLEAR        0x618U
#define GENI_TX_FIFO0           0x700U
#define GENI_M_CMD_ACTIVE       (1U << 0)
#define GENI_M_CMD_DONE         (1U << 0)
#define GENI_UART_START_TX      (1U << 27)
#define GENI_POLL_LIMIT         100000U

void aarch64_geni_uart_putc(volatile uint32_t *base, char c)
{
    for (uint32_t i = 0; i < GENI_POLL_LIMIT; ++i) {
        if ((base[GENI_SE_STATUS / 4U] & GENI_M_CMD_ACTIVE) == 0U)
            break;
        arch_nop();
    }
    base[GENI_M_IRQ_CLEAR / 4U] = 0xffffffffU;
    base[GENI_TX_TRANS_LEN / 4U] = 1U;
    base[GENI_M_CMD0 / 4U] = GENI_UART_START_TX;
    base[GENI_TX_FIFO0 / 4U] = (uint32_t)(uint8_t)c;
    for (uint32_t i = 0; i < GENI_POLL_LIMIT; ++i) {
        if ((base[GENI_M_IRQ_STATUS / 4U] & GENI_M_CMD_DONE) != 0U)
            break;
        arch_nop();
    }
    base[GENI_M_IRQ_CLEAR / 4U] = GENI_M_CMD_DONE;
}
