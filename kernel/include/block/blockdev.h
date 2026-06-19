#ifndef _BLOCK_BLOCKDEV_H
#define _BLOCK_BLOCKDEV_H

#include <stdint.h>

#define BLOCKDEV_NAME_MAX 16
#define BLOCKDEV_MAX 8

// ── Block device ──────────────────────────────────────────
typedef struct block_device {
    char     name[BLOCKDEV_NAME_MAX];
    uint32_t port_num;           // AHCI port number
    uint64_t sector_count;       // total sectors
    uint32_t sector_size;        // usually 512
    int      present;

    // Operations (dispatch to driver)
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
} block_device_t;

// ── Block device API ─────────────────────────────────────

// Register a block device (called by AHCI port init)
block_device_t *block_device_register(const char *name,
                                       uint32_t port_num,
                                       uint64_t sector_count);

// Read sectors from a block device
int block_device_read(block_device_t *dev, uint64_t lba,
                      uint32_t count, void *buffer);

// Write sectors to a block device
int block_device_write(block_device_t *dev, uint64_t lba,
                       uint32_t count, const void *buffer);

// Find a block device by index (for enumeration)
block_device_t *block_device_get(int index);

// Total number of registered block devices
int block_device_count(void);

// Initialize the block subsystem
void block_device_init(void);

#endif // _BLOCK_BLOCKDEV_H
