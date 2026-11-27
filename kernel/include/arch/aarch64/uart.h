#ifndef OS01_AARCH64_UART_H
#define OS01_AARCH64_UART_H

#include <stdbool.h>
#include <stdint.h>

enum aarch64_uart_type {
    AARCH64_UART_PL011 = 0,
    AARCH64_UART_QCOM_GENI = 1,
};

struct boot_context;

void aarch64_uart_select_from_handoff(const struct boot_context *bootctx);
void aarch64_geni_uart_putc(volatile uint32_t *base, char c);

#endif /* OS01_AARCH64_UART_H */
