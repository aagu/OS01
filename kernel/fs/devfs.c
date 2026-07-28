#include <fs/devfs.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <kernel/debug.h>
#include <kernel/arch/cpu.h>
#include <kernel/slab.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <kernel/tty.h>
#include <kernel/task.h>
#include <kernel/poll.h>
#include <kernel/file.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

// ── Internal device table ──────────────────────────────────

typedef struct devfs_device {
    char name[DEVFS_NAME_MAX];
    uint8_t type;       // VFS_CHRDEV or VFS_BLKDEV
    const struct devfs_ops *ops;  // replaced individual read/write/poll pointers
    void *private_data;
    int registered;
} devfs_device_t;

static devfs_device_t devices[DEVFS_MAX_DEVICES];
static int device_count = 0;

// The console TTY — obtained via get_dev_tty() from tty.c.

// ── Built-in device handlers ───────────────────────────────

// /dev/null — reads return EOF (0 bytes), writes are discarded
static int null_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;  // EOF
}

static int null_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)buffer;
    return (int)size;  // discard all data
}

// /dev/zero — reads return zero-filled buffer, writes are discarded
static int zero_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (buffer && size > 0)
        memset(buffer, 0, size);
    return (int)size;
}

// /dev/tty — console TTY (canonical mode, echo, keyboard input)
static int dev_tty_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    tty_t *tty = get_dev_tty();
    if (!tty || !buffer || size == 0) return 0;
    return tty_read(tty, (char *)buffer, (int)size, false);
}

static int dev_tty_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    tty_t *tty = get_dev_tty();
    if (!tty || !buffer || size == 0) return 0;
    return tty_write(tty, (const char *)buffer, (int)size);
}

// ── /dev/tty poll — delegates to tty_poll via global TTY singleton
static uint32_t dev_tty_poll(void *priv, struct poll_table *pt)
{
    (void)priv;
    tty_t *tty = get_dev_tty();
    if (!tty)
        return POLLERR;
    return tty_poll(tty, pt);
}

// ── /dev/serial — read/write the COM1 serial port ─────────
static int serial_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;

    // Blocking read: read one character at a time
    uint64_t total = 0;
    while (total < size) {
        ((char *)buffer)[total] = read_serial();
        total++;
        // For now, single-char reads per call to keep it simple
        break;
    }
    return (int)total;
}

static int serial_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;

    for (uint64_t i = 0; i < size; i++)
        write_serial(((char *)buffer)[i]);

    return (int)size;
}

// ── /dev/random — pseudorandom data from rdtsc() ──────────
// Not cryptographically secure.  Mixed with the low bits of
// TSC on each byte for per-byte variation.
static int random_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    for (uint64_t i = 0; i < size; i++) {
        uint64_t tsc = arch_cycle_counter();
        ((uint8_t *)buffer)[i] = (uint8_t)(tsc ^ (tsc >> 13) ^ (tsc >> 31));
    }
    return (int)size;
}

static int random_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)buffer;
    return (int)size;  // write accepted, data ignored (like Linux)
}

// ── Dispatch read/write via device index stored in node->fs_data ──

static int devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;

    // Block device path: sector-level read via block_device_t
    if (devices[idx].type == VFS_BLKDEV) {
        block_device_t *bdev = (block_device_t *)devices[idx].private_data;
        if (!bdev || !buffer || size == 0) return 0;
        uint32_t lba   = (uint32_t)(offset / 512);
        uint32_t count = (uint32_t)((size + 511) / 512);
        if (count == 0) return 0;
        uint8_t *tmp = kmalloc(count * 512);
        if (!tmp) return -1;
        int ret = block_device_read(bdev, lba, count, tmp);
        if (ret == 0)
            memcpy(buffer, tmp + (offset % 512), size);
        kfree(tmp);
        return (ret == 0) ? (int)size : -1;
    }

    // Character device path (existing)
    if (devices[idx].ops && devices[idx].ops->read)
        return devices[idx].ops->read(node, offset, size, buffer);
    return -1;
}

static int devfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;

    // Block device path: read-modify-write for unaligned edges
    if (devices[idx].type == VFS_BLKDEV) {
        block_device_t *bdev = (block_device_t *)devices[idx].private_data;
        if (!bdev || !buffer || size == 0) return 0;
        uint32_t lba   = (uint32_t)(offset / 512);
        uint32_t count = (uint32_t)((size + 511) / 512);
        if (count == 0) return 0;
        uint8_t *tmp = kmalloc(count * 512);
        if (!tmp) return -1;
        int ret = block_device_read(bdev, lba, count, tmp);
        if (ret == 0) {
            memcpy(tmp + (offset % 512), buffer, size);
            ret = block_device_write(bdev, lba, count, tmp);
        }
        kfree(tmp);
        return (ret == 0) ? (int)size : -1;
    }

    // Character device path (existing)
    if (devices[idx].ops && devices[idx].ops->write)
        return devices[idx].ops->write(node, offset, size, buffer);
    return -1;
}

// ── devfs_mmap — dispatch mmap for device-backed mmap ────────
static int devfs_mmap(vfs_node_t *node, struct vma *vma)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -EINVAL;
    if (devices[idx].ops && devices[idx].ops->mmap)
        return devices[idx].ops->mmap(node, vma);
    return -ENODEV;
}

// ── devfs_poll — dispatch poll for fd_poll(FD_DEV) ─────────
// node->fs_data holds the device index.  Resolve it and call
// the device's poll callback, or return always-ready if none.
uint32_t devfs_poll(vfs_node_t *node, poll_table_t *pt)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return POLLNVAL;

    devfs_device_t *dev = &devices[idx];

    if (dev->ops && dev->ops->poll)
        return dev->ops->poll(dev->private_data, pt);

    // No poll callback: default to always ready
    return POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
}

// ── devfs_ioctl_node — dispatch ioctl to a devfs device ─────
int devfs_ioctl_node(vfs_node_t *node, int cmd, void *arg)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -ENODEV;
    if (devices[idx].ops && devices[idx].ops->ioctl)
        return devices[idx].ops->ioctl(node, cmd, arg);
    return -ENOTTY;
}

// ── readdir: enumerate registered devices ───────────────────

static int devfs_readdir(vfs_node_t *node, uint64_t index, vfs_dirent_t *entry)
{
    if (node->type != VFS_DIR) return -1;

    uint64_t dev_idx = 0;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (!devices[i].registered) continue;
        if (dev_idx == index) {
            size_t nlen = strlen(devices[i].name);
            if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
            memcpy(entry->name, devices[i].name, nlen);
            entry->name[nlen] = '\0';
            entry->type = devices[i].type;
            entry->size = 0;
            entry->ino = i;  // store table index for vfs_lookup → node->fs_data
            return 0;
        }
        dev_idx++;
    }

    entry->name[0] = '\0';  // end of directory
    return 0;
}

// ── Single devfs operations vector ──────────────────────────
// All devfs nodes share this vector.  The read/write dispatch
// above routes to the correct device handler based on the
// device index stored in node->fs_data during vfs_lookup.

static struct vfs_ops devfs_ops = {
    .read    = devfs_read,
    .write   = devfs_write,
    .readdir = devfs_readdir,
    .mmap    = devfs_mmap,
};

// ── devfs_open_node — sys_open integration ──────────────────
// Called from SYS_open after vfs_lookup.  If the node is a devfs
// device node with an open callback, returns a custom file_t.
// Otherwise returns -ENOSYS so SYS_open falls through to FD_VFS.
int devfs_open_node(vfs_node_t *node, const char *path, int flags, file_t **out)
{
    if (!out) return -EINVAL;

    // Guard: only process devfs device nodes.
    if (node->type != VFS_CHRDEV && node->type != VFS_BLKDEV)
        return -ENOSYS;

    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -ENODEV;

    if (devices[idx].ops && devices[idx].ops->open) {
        int rc = devices[idx].ops->open(path, out);
        if (rc == 0 && *out) {
            (*out)->flags = flags;
            return 0;
        }
        if (rc != -ENOSYS) return rc;
    }

    // Default: FD_DEV
    *out = file_alloc();
    if (!*out) return -ENOMEM;
    (*out)->type = FD_DEV;
    (*out)->node = vfs_node_get(node);
    (*out)->flags = flags;
    return 0;
}

// ── Initialise devfs and register built-in devices ──────────

static const struct devfs_ops null_ops = {
    .read  = null_read,
    .write = null_write,
};

static const struct devfs_ops zero_ops = {
    .read  = zero_read,
    .write = null_write,
};

static const struct devfs_ops random_ops = {
    .read  = random_read,
    .write = random_write,
};

static const struct devfs_ops serial_ops = {
    .read  = serial_read,
    .write = serial_write,
};

const struct devfs_ops tty_phys_ops = {
    .read  = dev_tty_read,
    .write = dev_tty_write,
    .poll  = dev_tty_poll,
    .ioctl = tty_phys_ioctl,
};

// ── /dev/tty magic: opens the controlling terminal ────────────
static int tty_magic_open(const char *name, file_t **out_file)
{
    if (strcmp(name, "/dev/tty") != 0) return -ENOSYS;

    void *target = NULL;
    if (current->ctty_type == CTTY_PTY)
        target = current->ctty;
    else
        target = keyboard_get_tty();

    for (int i = 0; i < device_count; i++) {
        if (devices[i].private_data == target && devices[i].type == VFS_CHRDEV
            && devices[i].registered) {
            vfs_node_t *node = calloc(1, sizeof(vfs_node_t));  // NOT vfs_node_alloc()
            if (!node) return -ENOMEM;
            node->type = VFS_CHRDEV;
            node->fs_data = (void *)(uintptr_t)i;
            node->ops = &devfs_ops;
            node->refcount = 1;
            *out_file = file_alloc();
            if (!*out_file) { free(node); return -ENOMEM; }
            (*out_file)->type = FD_DEV;
            (*out_file)->node = node;
            return 0;
        }
    }
    return -ENXIO;
}

const struct devfs_ops tty_magic_ops = {
    .open  = tty_magic_open,
    .read  = dev_tty_read,
    .write = dev_tty_write,
    .poll  = dev_tty_poll,
    .ioctl = tty_phys_ioctl,
};

void devfs_init(void)
{
    memset(devices, 0, sizeof(devices));
    device_count = 0;

    // Mount devfs at /dev
    int ret = vfs_mount("/dev", NULL, &devfs_ops, NULL);
    if (ret == 0) {
        debug_fs("devfs: mounted at /dev\n");
    } else {
        debug_fs("devfs: mount FAILED\n");
        return;
    }

    // Register built-in character devices
    devfs_register_chrdev("null",   NULL, &null_ops);
    devfs_register_chrdev("zero",   NULL, &zero_ops);
    devfs_register_chrdev("random", NULL, &random_ops);
    devfs_register_chrdev("serial", NULL, &serial_ops);
    // NOTE: /dev/tty and /dev/tty0 are registered in main.c
    // after keyboard_set_tty() so keyboard_get_tty() returns
    // the correct pointer.
}

// ── Public API ──────────────────────────────────────────────

int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops)
{
    if (device_count >= DEVFS_MAX_DEVICES)
        return -1;

    int idx = device_count;
    size_t nlen = strlen(name);
    if (nlen >= DEVFS_NAME_MAX) nlen = DEVFS_NAME_MAX - 1;
    memcpy(devices[idx].name, name, nlen);
    devices[idx].name[nlen] = '\0';

    devices[idx].type = VFS_CHRDEV;
    devices[idx].ops = ops;
    devices[idx].private_data = private_data;
    devices[idx].registered = 1;
    device_count++;

    debug_fs("devfs: registered '%s' (chrdev)\n", name);
    return 0;
}

int devfs_register_blkdev(const char *name, block_device_t *dev)
{
    if (device_count >= DEVFS_MAX_DEVICES)
        return -1;

    int idx = device_count;
    size_t nlen = strlen(name);
    if (nlen >= DEVFS_NAME_MAX) nlen = DEVFS_NAME_MAX - 1;
    memcpy(devices[idx].name, name, nlen);
    devices[idx].name[nlen] = '\0';

    devices[idx].type = VFS_BLKDEV;
    devices[idx].ops = NULL;            // block device dispatch is separate
    devices[idx].private_data = dev;    // block_device_t *
    devices[idx].registered = 1;
    device_count++;

    debug_fs("devfs: registered blkdev '%s'\n", name);
    return 0;
}

// ── devfs_get_private — retrieve private_data from a devfs node ──
void *devfs_get_private(struct vfs_node *node)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return NULL;
    return devices[idx].private_data;
}
