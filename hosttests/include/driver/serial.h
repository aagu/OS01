#ifndef _KERNEL_SERIAL_H
#define _KERNEL_SERIAL_H
#include <stdint.h>
#include <stdbool.h>

#define SERIAL_COM1 0x3f8

void init_serial();
// receiving data
char read_serial();
// sending data
void write_serial(char c);

#endif