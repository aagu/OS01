#include <fs/devfs.h>
#include <fs/vfs.h>
#include <kernel/printk.h>
#include <driver/serial.h>
#include <string.h>
#include <stdlib.h>

// ── Internal device table ──────────────────────────────────

typedef struct devfs_device {
    char name[DEVFS_NAME_MAX];
    uint8_t type;       // VFS_CHRDEV or VFS_BLKDEV
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
    void *private_data;
    int registered;
} devfs_device_t;

static devfs_device_t devices[DEVFS_MAX_DEVICES];
static int device_count = 0;

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

// /dev/serial — read/write the COM1 serial port
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

// ── Dispatch read/write via device index stored in node->fs_data ──

static int devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;
    if (devices[idx].read)
        return devices[idx].read(node, offset, size, buffer);
    return -1;
}

static int devfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;
    if (devices[idx].write)
        return devices[idx].write(node, offset, size, buffer);
    return -1;
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
};

// ── Initialise devfs and register built-in devices ──────────

void devfs_init(void)
{
    memset(devices, 0, sizeof(devices));
    device_count = 0;

    // Mount devfs at /dev
    int ret = vfs_mount("/dev", NULL, &devfs_ops, NULL);
    if (ret == 0) {
        serial_printk("devfs: mounted at /dev\n");
    } else {
        serial_printk("devfs: mount FAILED\n");
        return;
    }

    // Register built-in character devices
    devfs_register_chrdev("null",   NULL, null_read,   null_write);
    devfs_register_chrdev("zero",   NULL, zero_read,   null_write);  // zero write = discard
    devfs_register_chrdev("serial", NULL, serial_read, serial_write);
}

// ── Public API ──────────────────────────────────────────────

int devfs_register_chrdev(const char *name, void *private_data,
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *),
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *))
{
    if (device_count >= DEVFS_MAX_DEVICES)
        return -1;

    int idx = device_count;
    size_t nlen = strlen(name);
    if (nlen >= DEVFS_NAME_MAX) nlen = DEVFS_NAME_MAX - 1;
    memcpy(devices[idx].name, name, nlen);
    devices[idx].name[nlen] = '\0';

    devices[idx].type = VFS_CHRDEV;
    devices[idx].read = read;
    devices[idx].write = write;
    devices[idx].private_data = private_data;
    devices[idx].registered = 1;
    device_count++;

    serial_printk("devfs: registered '%s' (chrdev)\n", name);
    return 0;
}
