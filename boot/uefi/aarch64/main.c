#include <stdint.h>

/* QEMU virt's PL011 UART: physical MMIO at 0x09000000. */
#define PL011_BASE       ((volatile uint32_t *)(uintptr_t)0x09000000U)
#define PL011_DR          0x00U
#define PL011_FR          0x18U
#define PL011_FR_TXFF     (1U << 5)

static void pl011_putc(char c)
{
    while (PL011_BASE[PL011_FR / sizeof(uint32_t)] & PL011_FR_TXFF)
        ;
    PL011_BASE[PL011_DR / sizeof(uint32_t)] = (uint8_t)c;
}

static void pl011_puts(const char *s)
{
    while (*s)
        pl011_putc(*s++);
}

/* Temporary marker while the UEFI handoff transaction is implemented. */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    pl011_puts("OS01 AArch64 UEFI build marker\r\n");
    return 0;
}
