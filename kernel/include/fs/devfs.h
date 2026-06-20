#ifndef _FS_DEVFS_H
#define _FS_DEVFS_H

#include <stdint.h>
#include <fs/vfs.h>
#include <kernel/tty.h>

#define DEVFS_NAME_MAX    32
#define DEVFS_MAX_DEVICES 16

// Register a character device that will appear under /dev/
int devfs_register_chrdev(const char *name, void *private_data,
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *),
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *));

// Initialize devfs and mount at /dev
void devfs_init(void);

// Set the console TTY that /dev/tty reads from / writes to.
void devfs_set_tty(tty_t *tty);

#endif
