#pragma once
#include <stdint.h>
void arch_outb(uint16_t port, uint8_t value);
uint8_t arch_inb(uint16_t port);
