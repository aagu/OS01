#ifndef _BLOCK_BCACHE_H
#define _BLOCK_BCACHE_H

// ARCH-6: blockdev-level sector cache shared by every physical-disk caller
// (ext2, FAT, GPT, devfs blkdev).  Design spec:
// docs/superpowers/specs/2026-11-07-arch6-fs-concurrency-blockcache-design.md
// §5.  Physical-disk keys only — partition devices never enter the cache
// (their ops forward to the parent's block_device_read/write wrapper, so
// physical blocks converge on the underlying disk's (dev, lba) key).

#include <stdint.h>
#include <stddef.h>

struct block_device;

#define BCACHE_BUFS    2048
#define BCACHE_BUCKETS 512
#define BCACHE_SECTOR  512   // this phase: fixed 512-byte sectors only

// Initialize the buffer pool.  Returns 0, or -ENOMEM when either pool
// allocation failed (nothing half-published; safe to retry).  Idempotent
// after success.  Called from blockdev register/init paths.
int bcache_init(void);

// Cached single-sector transfer.  This phase supports 512-byte sectors;
// other sector sizes return -EINVAL.  Synchronous; may sleep waiting for
// a BUSY reservation (process context only).
int bcache_read_sector(struct block_device *dev, uint64_t lba, void *buf);
int bcache_write_sector(struct block_device *dev, uint64_t lba, const void *buf);

// Write back every dirty buffer keyed by dev.  Snapshot semantics:
// dirty keys present when the call starts are on the backend when this
// returns successfully (or the raw negative error is returned).  Keys
// dirtied concurrently after the snapshot are not required to be covered.
int bcache_flush_device(struct block_device *dev);

// Drop every cache entry keyed by dev WITHOUT writeback — only for
// silent (retired / pre-publication) devices; retired ops are gone, so
// writeback would be impossible anyway.  Must run before the device
// object is released.
void bcache_drop_device(struct block_device *dev);

#if defined(OS01_HOST_TEST) || defined(OS01_SELFTEST)

typedef struct {
    uint32_t entries;   // hash entries keyed by dev
    uint32_t pinned;    // entries with refcnt > 0
    uint32_t busy;      // entries with BUSY set
    uint32_t dirty;     // entries with DIRTY set
    uint32_t waiters;   // armed, not-yet-cleaned wait ops attributed to dev
} bcache_test_stats_t;

// Global silent reset: drop all entries, keep the pools.  Test-only.
void bcache_reset_for_test(void);

// Per-device counters (metadata-lock protected).
void bcache_debug_stats(struct block_device *dev, bcache_test_stats_t *out);

// Pin the given key (filling it via the normal read path first if needed)
// and keep the pin held, to simulate an in-flight consumer.
int bcache_debug_pin(struct block_device *dev, uint64_t lba);
void bcache_debug_unpin(struct block_device *dev, uint64_t lba);

#endif // OS01_HOST_TEST || OS01_SELFTEST

#ifdef OS01_HOST_TEST

// Deterministic wait-window injection (host only).  reserve creates a
// BUSY reservation for a MISSING key exactly like the real read-fill
// publication path, holding a test-owned pin; complete finishes it
// through the real success/reclaim helpers.
int  bcache_test_reserve_busy(struct block_device *dev, uint64_t lba);
// Returns 0, or -EINVAL when the (dev, lba) has no test-owned BUSY
// reservation (foreign key, already completed).
int  bcache_test_complete_busy(struct block_device *dev, uint64_t lba,
                               int io_result, const void *sector);

#endif // OS01_HOST_TEST

#endif // _BLOCK_BCACHE_H
