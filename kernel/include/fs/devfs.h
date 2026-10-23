#ifndef _FS_DEVFS_H
#define _FS_DEVFS_H

#include <stdint.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <tty/tty.h>

// Forward declarations for poll support
struct poll_table;
struct file;
struct vma;

#define DEVFS_NAME_MAX    32
#define DEVFS_MAX_DEVICES 32

// ── Device operations table ─────────────────────────────────
// open:       returns a custom file_t.  Return -ENOSYS to fall back
//             to the default FD_DEV allocation.
// mmap:       maps device memory into user-space via VMA.
// ioctl:      legacy node-only ioctl (no per-file state).  Devices
//             that need per-file state should ALSO set ioctl_file
//             (preferred for new code).
// ioctl_file: ioctl with access to the specific file_t (e.g. to
//             reach f->dev_private).  When set, devfs_ioctl_file
//             dispatches to it first; the node callback fires only
//             when ioctl_file returns -ENOTTY.
// release_file: per-file final-close hook.  Called from file_free
//             when the file_t's reference count drops to zero,
//             BEFORE the node reference is released.  NULL means
//             no per-file cleanup; the node callback (if any) is
//             unaffected.
// read_file:  file-aware read.  Receives the specific file_t, so a
//             device can tell two open file descriptions on the same
//             node apart (e.g. the input lease needs to know WHICH
//             reader owns the physical keyboard).  The buffer is
//             always kernel memory — fd_read bounces through a kernel
//             buffer and copies to user space itself.  NULL means
//             "no file-aware read": devfs falls back to the legacy
//             node read (ops->read).  An error returned here is
//             final; devfs never retries through the node read.
// poll_file:  file-aware poll.  Same contract as read_file: NULL
//             means devfs falls back to the legacy node poll
//             (ops->poll).  A returned mask of 0 is a legitimate
//             "not ready" answer and is never upgraded to
//             always-ready.
struct devfs_ops {
    int (*open)(const char *name, struct file **out_file);
    int (*read)(struct vfs_node *, uint64_t, uint64_t, void *);
    int (*write)(struct vfs_node *, uint64_t, uint64_t, void *);
    uint32_t (*poll)(void *priv, uint32_t requested,
                     struct poll_table *pt);
    int (*mmap)(struct vfs_node *, struct vma *);
    int (*ioctl)(struct vfs_node *, int cmd, void *arg);
    int (*ioctl_file)(struct file *, int cmd, void *arg);
    void (*release_file)(struct file *);
    // ── Optional file-aware hooks (appended last: the layout above
    //    is the long-standing device ABI) ──────────────────────
    int (*read_file)(struct file *, uint64_t offset, uint64_t size,
                     void *buffer);
    uint32_t (*poll_file)(struct file *, uint32_t requested,
                          struct poll_table *pt);
};

// Register a character device that will appear under /dev/
int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops);

// Register a block device that will appear under /dev/
int devfs_register_blkdev(const char *name, struct block_device *dev);

// Initialize devfs and mount at /dev
void devfs_init(void);

// TTY device ops tables — defined in devfs.c, registered in main.c
// after keyboard_set_tty() so keyboard_get_tty() returns the correct pointer.
extern const struct devfs_ops tty_magic_ops;
extern const struct devfs_ops tty_phys_ops;

// Poll a devfs device node — resolves node->fs_data index to device,
// calls device's poll callback or returns always-ready if none.
uint32_t devfs_poll(struct vfs_node *node, uint32_t requested,
                    struct poll_table *pt);

// Poll a devfs device THROUGH a specific file_t.  Resolves f->node to
// the devfs device and prefers ops->poll_file when set; falls back to
// the legacy node poll (ops->poll, or always-ready when the device
// registers no poll callback at all) only when poll_file is absent.
// A poll_file result of 0 is a legitimate "not ready" answer and is
// returned as-is.  Returns POLLNVAL when the file has no node, is not
// a device file, or its node does not resolve to a devfs device.
uint32_t devfs_poll_file(struct file *f, uint32_t requested,
                         struct poll_table *pt);

// sys_open integration: check the devfs device's open callback,
// returning a custom file_t or NULL to fall through to FD_DEV.
// Returns 0 on success (with *out set), -ENOSYS if not a devfs node,
// or negative errno on error.
int devfs_open_node(struct vfs_node *node, const char *path, int flags,
                    struct file **out);

// Dispatch ioctl to a devfs device node — resolves node->fs_data index
// to device, calls device's ops->ioctl if present.
int devfs_ioctl_node(struct vfs_node *node, int cmd, void *arg);

// Dispatch ioctl through a specific file_t.  Resolves f->node to the
// devfs device and prefers ops->ioctl_file when set; falls back to
// ops->ioctl (node callback) when ioctl_file returns -ENOTTY.  Returns
// -ENOTTY when neither callback is registered or when f has no node.
int devfs_ioctl_file(struct file *f, int cmd, void *arg);

// Per-file release dispatch.  Resolves f->node to the devfs device and
// invokes ops->release_file when set.  No-op when f has no node or the
// device has no release_file callback.  Called from file_free BEFORE
// vfs_node_put so the device can reach f->dev_private while the node
// reference is still live.
void devfs_release_file(struct file *f);

// Retrieve private_data from a devfs node (e.g. pty_t* for PTY slave nodes).
void *devfs_get_private(struct vfs_node *node);

// Retrieve devfs_ops from an open device file.
const struct devfs_ops *devfs_get_file_ops(struct file *f);

#endif
