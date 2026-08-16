#ifndef __KEYBOARD_H__
#define __KEYBOARD_H__

#include <stdint.h>
#include <fs/vfs.h>
#include <kernel/tty.h>

void keyboard_init(void);

// Set the TTY that receives translated ASCII input.
void keyboard_set_tty(tty_t *tty);

// Poll the keyboard hardware (8042) for pending scancodes,
// translate to ASCII, and push to the registered TTY.
// Safe to call from task context or IRQ context.
void keyboard_poll(void);

// Read raw scancodes from the ring buffer (for /dev/keyboard).
// Non-blocking: returns number of bytes copied (0 if empty).
int keyboard_read_scancodes(uint8_t *buffer, int size);

// DevFS read handler for /dev/keyboard
int keyboard_devfs_read(vfs_node_t *node, uint64_t offset,
                        uint64_t size, void *buffer);

// DevFS poll handler for /dev/keyboard — POLLIN when the scancode
// ring is non-empty, otherwise registers on the scancode wait list
// (cascade-woken from IRQ context when a scancode arrives).
uint32_t keyboard_poll_dev(void *priv, uint32_t requested,
                           struct poll_table *pt);

// Get the TTY that receives translated input (for reading lflag, etc.).
tty_t *keyboard_get_tty(void);

#endif
