#ifndef __KEYBOARD_H__
#define __KEYBOARD_H__

#include <stdint.h>
#include <fs/vfs.h>

void keyboard_init(void);

// Read raw scancodes from the ring buffer.
// Copies up to `size` scancodes into `buffer`.
// Non-blocking: returns number of bytes copied (0 if empty).
int keyboard_read_scancodes(uint8_t *buffer, int size);

// DevFS read handler — used when registering /dev/keyboard
int keyboard_devfs_read(vfs_node_t *node, uint64_t offset,
                        uint64_t size, void *buffer);

#endif
