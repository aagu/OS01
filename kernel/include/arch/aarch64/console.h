#ifndef ARCH_AARCH64_CONSOLE_H
#define ARCH_AARCH64_CONSOLE_H

/* Poll PL011 RX FIFO on BSP timer tick when no PL011 RX IRQ is armed
 * (e.g. RPi5 where DTB PL011 SPI intid=0).  No-op when IRQ-driven. */
void aarch64_console_poll(void);

#endif /* ARCH_AARCH64_CONSOLE_H */
