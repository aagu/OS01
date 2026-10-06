#ifndef _BLOCK_BLOCKDEV_H
#define _BLOCK_BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>

#define BLOCKDEV_NAME_MAX 16
#define BLOCKDEV_MAX 8

struct block_device;
typedef struct block_device block_device_t;

// ── Block device operations ───────────────────────────────
struct block_device_ops {
    int (*read)(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer);
    int (*write)(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer);
    int (*flush)(block_device_t *dev);
};

// ── Device kind ───────────────────────────────────────────
enum block_device_kind {
    BLOCK_DISK,
    BLOCK_PARTITION,
};

// ── Block device descriptor (registration input) ──────────
struct block_device_desc {
    const char *name;
    uint64_t sector_count;
    uint32_t sector_size;
    const struct block_device_ops *ops;
    void *private_data;
    block_device_t *parent;
    enum block_device_kind kind;
};

// ── Block device structure ────────────────────────────────
struct block_device {
    char name[BLOCKDEV_NAME_MAX];
    uint64_t sector_count;
    uint32_t sector_size;
    int present;
    enum block_device_kind kind;
    const struct block_device_ops *ops;
    void *private_data;
    struct block_device *parent;
    int last_error;
};

// ── Block device API ──────────────────────────────────────

// Register a block device. out is set to NULL on entry, populated on success.
int block_device_register(const struct block_device_desc *desc, block_device_t **out);

// Unregister a block device during boot/discovery (leaves slot hole, preserves addresses).
int block_device_unregister_boot(block_device_t *dev);

// Mark a block device as having encountered a driver/hardware error.
void block_device_mark_failed(block_device_t *dev, int error);

// Read sectors from a block device.
int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer);

// Write sectors to a block device.
int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer);

// Flush write caches on a block device.
int block_device_flush(block_device_t *dev);

// Find an active block device by logical index (skips unpresent holes).
block_device_t *block_device_get(int index);

// Total number of currently registered active block devices.
int block_device_count(void);

// Initialize the block subsystem.
void block_device_init(void);

#endif // _BLOCK_BLOCKDEV_H
