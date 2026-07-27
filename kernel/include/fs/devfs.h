#ifndef _FS_DEVFS_H
#define _FS_DEVFS_H

#include <stdint.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <kernel/tty.h>

// Forward declarations for poll support
struct poll_table;
struct file;
struct vma;

#define DEVFS_NAME_MAX    32
#define DEVFS_MAX_DEVICES 16

// ── Device operations table ─────────────────────────────────
// open:  returns a custom file_t.  Return -ENOSYS to fall back
//        to the default FD_DEV allocation.
// mmap:  maps device memory into user-space via VMA.
// ioctl: device-specific control operations.
struct devfs_ops {
    int (*open)(const char *name, struct file **out_file);
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *);
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *);
    uint32_t (*poll)(void *priv, struct poll_table *pt);
    // Guard against mmap macro from kernel/vmm.h (mmap = uint64_t*)
#ifdef mmap
#undef mmap
#define DEVFS_MMAP_RESTORE
#endif
    int (*mmap)(struct vfs_node *, struct vma *);
#ifdef DEVFS_MMAP_RESTORE
#define mmap uint64_t*
#undef DEVFS_MMAP_RESTORE
#endif
    int (*ioctl)(struct vfs_node *, int cmd, void *arg);
};

// Register a character device that will appear under /dev/
int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops);

// Register a block device that will appear under /dev/
int devfs_register_blkdev(const char *name, struct block_device *dev);

// Initialize devfs and mount at /dev
void devfs_init(void);

// Poll a devfs device node — resolves node->fs_data index to device,
// calls device's poll callback or returns always-ready if none.
uint32_t devfs_poll(struct vfs_node *node, struct poll_table *pt);

// sys_open integration: check the devfs device's open callback,
// returning a custom file_t or NULL to fall through to FD_DEV.
// Returns 0 on success (with *out set), -ENOSYS if not a devfs node,
// or negative errno on error.
int devfs_open_node(struct vfs_node *node, const char *path, int flags,
                    struct file **out);

#endif
