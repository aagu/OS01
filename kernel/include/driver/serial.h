#ifndef _KERNEL_SERIAL_H
#define _KERNEL_SERIAL_H
#include <stdint.h>
#include <stdbool.h>
#include <kernel/tty.h>

#define SERIAL_COM1 0x3f8

// UART init (called early, before interrupt system is up)
void init_serial(void);

// Register COM1 IRQ4 handler (called after IRQ system is up)
void init_serial_irq(void);

// Set the console TTY for serial input to push to
void serial_set_tty(tty_t *tty);

// Read one character (blocking — waits until data available)
char read_serial(void);

// Check if data is available without blocking
bool serial_received(void);

// Send one character (busy-waits until TX buffer empty)
void write_serial(char c);

#endif