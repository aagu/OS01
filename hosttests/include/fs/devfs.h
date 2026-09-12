#ifndef _FS_DEVFS_H
#define _FS_DEVFS_H

#include <stdint.h>
#include <fs/vfs.h>

#define DEVFS_NAME_MAX    32
#define DEVFS_MAX_DEVICES 16

// Register a character device that will appear under /dev/
//   name        — device name (e.g. "null", "serial")
//   private_data — opaque pointer passed to read/write handlers
//   read        — read handler (may be NULL if device is write-only)
//   write       — write handler (may be NULL if device is read-only)
// Returns 0 on success, -1 on failure (table full).
int devfs_register_chrdev(const char *name, void *private_data,
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *),
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *));

// Initialize devfs and mount at /dev
void devfs_init(void);

#endif
