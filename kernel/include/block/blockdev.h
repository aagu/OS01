#ifndef _BLOCK_BLOCKDEV_H
#define _BLOCK_BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>

#define BLOCKDEV_NAME_MAX 16

// ARCH-10: the registry is a dynamic list of individually allocated
// objects, so there is no compile-time capacity constant any more
// (the old BLOCKDEV_MAX table size was removed).  The number of block
// devices is bounded by available memory, not by a fixed table.

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

// Register a block device.  out is set to NULL on entry, populated on
// success.  The object is allocated dynamically and its address never
// changes once published, so consumers may hold the returned pointer
// for as long as the device stays registered.
//
// Failure modes: -EINVAL (bad descriptor/name/ops), -EEXIST (a device
// with the same name is already registered), -ENOMEM (allocation
// failed).  A failed registration leaves the registered set and the
// device count unchanged.
int block_device_register(const struct block_device_desc *desc, block_device_t **out);

// Unregister a block device during boot/discovery.  The device leaves
// the enumerable registry; the other devices keep their addresses and
// their enumeration positions.
//
// dev must be a live registry member — a pointer that is not a member
// returns -EINVAL and is never dereferenced.  A disk that still has
// registered partition children returns -EBUSY and nothing changes:
// roll back the children first, then the parent.
//
// The object itself is retired, not freed: a consumer (devfs, GPT, the
// AHCI port) may still hold the pointer, and an unregistered device has
// to keep answering reads with -EINVAL.  The retired object stays valid
// until power-off, exactly like the slot it replaces in the old fixed
// table.
//
// This is the boot-time rollback path only.  It must be called while
// the driver is stopped, no IRQ handler is running for the device and
// nothing references it (no mount, no open file, no partition).  It is
// not a concurrent hot-unplug API.
int block_device_unregister_boot(block_device_t *dev);

// Mark a block device as having encountered a driver/hardware error.
void block_device_mark_failed(block_device_t *dev, int error);

// Read sectors from a block device.
int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer);

// Write sectors to a block device.
int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer);

// Flush write caches on a block device.
int block_device_flush(block_device_t *dev);

// Find a registered block device by logical index.  Indexes follow the
// successful registration order; a device registered after a boot
// unregister is appended at the end of that order.
block_device_t *block_device_get(int index);

// Number of currently registered block devices.
int block_device_count(void);

// Initialize the block subsystem.  Idempotent: calling it again is a
// no-op and never resets the registry or drops published objects.
void block_device_init(void);

#ifdef OS01_HOST_TEST
// Host-test only: drop every registered device and reset the registry
// so a test case starts from a known state.
void block_device_reset_for_test(void);
#endif

#endif // _BLOCK_BLOCKDEV_H
