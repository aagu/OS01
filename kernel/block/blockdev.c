#include <block/blockdev.h>
#include <core/debug.h>
#include <errno.h>
#include <string.h>

// ── Global device table ───────────────────────────────────
static block_device_t block_devices[BLOCKDEV_MAX];
static int block_device_initialized = 0;

// ── Initialization ───────────────────────────────────────
void block_device_init(void)
{
    memset(block_devices, 0, sizeof(block_devices));
    block_device_initialized = 1;
    debug_block("block: subsystem initialized\n");
}

// ── Registration ─────────────────────────────────────────
int block_device_register(const struct block_device_desc *desc, block_device_t **out)
{
    if (out)
        *out = NULL;

    if (!desc || !desc->name || desc->name[0] == '\0')
        return -EINVAL;

    if (strlen(desc->name) >= BLOCKDEV_NAME_MAX)
        return -EINVAL;

    // read op is required; write and flush are optional
    if (!desc->ops || !desc->ops->read)
        return -EINVAL;

    if (!block_device_initialized)
        block_device_init();

    // Reject duplicate names among currently present devices
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (block_devices[i].present && strcmp(block_devices[i].name, desc->name) == 0)
            return -EEXIST;
    }

    // Find first free slot (reusing holes left by unregister)
    int slot = -1;
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (!block_devices[i].present) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        debug_block("block: max devices reached\n");
        return -ENOSPC;
    }

    block_device_t *dev = &block_devices[slot];
    memset(dev, 0, sizeof(block_device_t));
    strncpy(dev->name, desc->name, BLOCKDEV_NAME_MAX - 1);
    dev->sector_count = desc->sector_count;
    dev->sector_size  = desc->sector_size ? desc->sector_size : 512;
    dev->present      = 1;
    dev->kind         = desc->kind;
    dev->ops          = desc->ops;
    dev->private_data = desc->private_data;
    dev->parent       = desc->parent;
    dev->last_error   = 0;

    if (out)
        *out = dev;

    debug_block("block: registered %s (%lu sectors, %lu MB, kind=%d)\n",
                dev->name, dev->sector_count,
                dev->sector_count * dev->sector_size / 1024 / 1024,
                dev->kind);
    return 0;
}

// ── Boot unregistration (leaves hole, preserves addresses) ───
int block_device_unregister_boot(block_device_t *dev)
{
    if (!dev || !dev->present)
        return -EINVAL;

    if (dev < block_devices || dev >= block_devices + BLOCKDEV_MAX)
        return -EINVAL;

    debug_block("block: unregistering %s\n", dev->name);
    dev->present = 0;
    dev->ops = NULL;
    dev->private_data = NULL;
    return 0;
}

// ── Mark device error ─────────────────────────────────────
void block_device_mark_failed(block_device_t *dev, int error)
{
    if (!dev)
        return;
    dev->last_error = error;
}

// ── Read ─────────────────────────────────────────────────
int block_device_read(block_device_t *dev, uint64_t lba,
                      uint32_t count, void *buffer)
{
    if (!dev || !dev->present) {
        debug_block("block: read on invalid device\n");
        return -EINVAL;
    }

    if (count > 0 && !buffer)
        return -EINVAL;

    if (lba + count < lba)
        return -EINVAL;

    if (count == 0) {
        if (lba <= dev->sector_count)
            return 0;
        return -EINVAL;
    }

    if (lba + count > dev->sector_count) {
        debug_block("block: read past end of device\n");
        return -EINVAL;
    }

    if (!dev->ops || !dev->ops->read)
        return -EINVAL;

    int ret = dev->ops->read(dev, lba, count, buffer);
    if (ret != 0)
        dev->last_error = ret;
    return ret;
}

// ── Write ─────────────────────────────────────────────────
int block_device_write(block_device_t *dev, uint64_t lba,
                       uint32_t count, const void *buffer)
{
    if (!dev || !dev->present) {
        debug_block("block: write on invalid device\n");
        return -EINVAL;
    }

    if (!dev->ops || !dev->ops->write)
        return -EROFS;

    if (count > 0 && !buffer)
        return -EINVAL;

    if (lba + count < lba)
        return -EINVAL;

    if (count == 0) {
        if (lba <= dev->sector_count)
            return 0;
        return -EINVAL;
    }

    if (lba + count > dev->sector_count) {
        debug_block("block: write past end of device\n");
        return -EINVAL;
    }

    int ret = dev->ops->write(dev, lba, count, buffer);
    if (ret != 0)
        dev->last_error = ret;
    return ret;
}

// ── Flush ─────────────────────────────────────────────────
int block_device_flush(block_device_t *dev)
{
    if (!dev || !dev->present)
        return -EINVAL;

    if (!dev->ops || !dev->ops->flush)
        return 0;

    int ret = dev->ops->flush(dev);
    if (ret != 0)
        dev->last_error = ret;
    return ret;
}

// ── Enumeration ──────────────────────────────────────────
block_device_t *block_device_get(int index)
{
    if (index < 0)
        return NULL;

    int current = 0;
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (block_devices[i].present) {
            if (current == index)
                return &block_devices[i];
            current++;
        }
    }
    return NULL;
}

int block_device_count(void)
{
    int count = 0;
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (block_devices[i].present)
            count++;
    }
    return count;
}
