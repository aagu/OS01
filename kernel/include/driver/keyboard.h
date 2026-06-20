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

#endif
