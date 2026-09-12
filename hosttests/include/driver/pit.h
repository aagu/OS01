#ifndef _KERNEL_PIT_H
#define _KERNEL_PIT_H

#include <stdint.h>

#define CLOCK_FREQUENCY 1193180
#define PIT_COMMAND 0x43
#define PIT_DATA 0x40
#define PIT_ICW 0x36

void pit_init();
void set_frequency(uint16_t hz);

#endif