#pragma once
/* BOOT-2 boot_irq_time mock: port I/O backed by an in-memory model so
 * the REAL 8259A IMR read-modify-write is exercised.  The model lives
 * in the runtime; the ICW init sequence is not modelled — the tests
 * seed the IMR bytes directly and only OCW1 (0x21 / 0xA1) traffic is
 * meaningful. */
#include <stdint.h>
void arch_outb(uint16_t port, uint8_t value);
uint8_t arch_inb(uint16_t port);

/* IMR model accessors for the test (runtime-defined). */
uint8_t boot2_imr_get(uint16_t port);
void boot2_imr_set(uint16_t port, uint8_t value);
/* Pending latch: a mask-bit clear while IF==0 latches the bit. */
unsigned long boot2_pending_get(void);
