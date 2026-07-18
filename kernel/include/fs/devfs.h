#ifndef _FS_DEVFS_H
#define _FS_DEVFS_H

#include <stdint.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <kernel/tty.h>

// Forward declaration for poll support
struct poll_table;

#define DEVFS_NAME_MAX    32
#define DEVFS_MAX_DEVICES 16

// Register a character device that will appear under /dev/
// poll callback (nullable): if non-NULL, called by fd_poll(FD_DEV)
// to check device readiness.
int devfs_register_chrdev(const char *name, void *private_data,
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *),
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *),
    uint32_t (*poll)(void *priv, struct poll_table *pt));

// Register a block device that will appear under /dev/
int devfs_register_blkdev(const char *name, struct block_device *dev);

// Initialize devfs and mount at /dev
void devfs_init(void);

// Poll a devfs device node — resolves node->fs_data index to device,
// calls device's poll callback or returns always-ready if none.
uint32_t devfs_poll(struct vfs_node *node, struct poll_table *pt);

#endif
