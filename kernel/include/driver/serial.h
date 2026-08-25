#ifndef _KERNEL_SERIAL_H
#define _KERNEL_SERIAL_H
#include <stdint.h>
#include <stdbool.h>
#include <kernel/tty.h>
#include <kernel/arch/spinlock.h>

#define SERIAL_COM1 0x3f8

// Serial output lock — prevents interleaved characters when
// multiple code paths (tty_write, sys_putchar, serial_printk)
// write to the same COM1 port concurrently.
extern spinlock_T serial_lock;

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

// Send one character (busy-waits until TX buffer empty).
// Acquires serial_lock internally.  DO NOT call this from any
// code path that already holds serial_lock — use the
// write_serial_locked() inline helper instead, otherwise the
// non-recursive spinlock will deadlock.
void write_serial(char c);

// Internal unlocked variant for callers that already hold
// serial_lock (irqsave variant) — typically fast paths inside
// tty_write / serial_printk.  Implemented as an inline in the
// .c file; we expose it here so .c files that need it don't
// have to re-import the entire serial.c.
void write_serial_unlocked(char c);

// Poll UART for available data and push to the console TTY.
// Safe to call from any context — provides IRQ fallback.
void serial_poll(void);

#endif