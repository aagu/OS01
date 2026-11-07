// kernel/block/bcache.c — ARCH-6 blockdev sector cache.
//
// Design: docs/superpowers/specs/2026-11-07-arch6-fs-concurrency-blockcache-design.md §5.
//
// Key contract: physical-disk (dev, lba) keys only — the blockdev wrapper
// never feeds partition devices in (their ops forward to the parent's
// wrapper, so physical blocks converge on the disk's key).
//
// Concurrency (spec §5.3):
//   - bcache_lock (irqsave) guards hash/LRU/refcnt/flags; no I/O, no
//     allocation, no other lock nesting inside it.
//   - data_lock (per-buffer irqsave) only wraps the 512-byte memcpy —
//     sector-atomic copy for unlocked raw readers/writers.
//   - BUSY is an exclusive maintenance reservation (fill / flush-drain /
//     evict).  A reservation is published with the caller's pin held, so
//     a waiter that sees BUSY and later finds the key valid can pin the
//     buffer that the maintainer kept for it.
//   - Waits use the ARM → recheck handshake: release bcache_lock, keep
//     local IRQs off across the window, wait_queue_arm, re-take the lock
//     and recheck (reserving in the same critical section when the
//     condition is satisfiable), release, then decide about schedule();
//     wait_queue_disarm after return, then restore IRQs.  Wakes run
//     outside bcache_lock.
//
// Task scope note: this file grows across Tasks 4/5/6 — Task 4 has the
// read core (free-list allocation only, exhaustion returns -ENOMEM);
// write/flush arrive in Task 5, LRU eviction + exhaustion wait in Task 6.

#include <block/bcache.h>
#include <block/blockdev.h>
#include <sync/wait.h>
#include <arch/irq.h>
#include <sched/task.h>      // schedule() for the BUSY wait path
#include <memory/slab.h>
#include <core/debug.h>
#include <string.h>
#include <errno.h>

// ── Buffer metadata ───────────────────────────────────────────────────
enum bcache_flags {
    BC_VALID = 1u << 0,
    BC_DIRTY = 1u << 1,
    BC_BUSY  = 1u << 2,
};

typedef struct bcache_buf {
    struct bcache_buf *hash_next;
    struct bcache_buf *lru_next, *lru_prev;
    struct block_device *dev;
    uint64_t    lba;
    uint32_t    flags;
    int         refcnt;
    spinlock_T  data_lock;
    uint8_t    *data;       // points into the shared data pool
} bcache_buf_t;

// ── Pool state (all under bcache_lock) ────────────────────────────────
static bcache_buf_t *g_bufs;                 // metadata pool
static uint8_t      *g_data_pool;            // BCACHE_BUFS * BCACHE_SECTOR
static bcache_buf_t *g_hash[BCACHE_BUCKETS];
static bcache_buf_t *g_free_stack[BCACHE_BUFS];
static int           g_free_top;
static bcache_buf_t *g_lru_head, *g_lru_tail;
static spinlock_T    g_bcache_lock;
static wait_queue_t  g_bcache_wq;
static int           g_bcache_ready = 0;

// Per-device waiter attribution for bcache_debug_stats (armed, not yet
// cleaned wait ops).  The cache keys on physical disks only, so a small
// table is plenty.
#define BCACHE_WAITER_SLOTS 16
static struct {
    struct block_device *dev;
    uint32_t             armed;
} g_waiter_tab[BCACHE_WAITER_SLOTS];

static inline uint32_t bcache_hash(struct block_device *dev, uint64_t lba)
{
    uint64_t h = (uint64_t)(uintptr_t)dev ^ (lba * 0x9E3779B97F4A7C15ULL);
    return (uint32_t)((h >> 13) & (BCACHE_BUCKETS - 1));
}

static inline int bcache_key_eq(const bcache_buf_t *b, struct block_device *dev, uint64_t lba)
{
    return b->dev == dev && b->lba == lba;
}

static void bcache_lru_remove(bcache_buf_t *b)
{
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
    else             g_lru_head = b->lru_next;
    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
    else             g_lru_tail = b->lru_prev;
    b->lru_next = b->lru_prev = NULL;
}

static void bcache_lru_push_front(bcache_buf_t *b)
{
    b->lru_prev = NULL;
    b->lru_next = g_lru_head;
    if (g_lru_head) g_lru_head->lru_prev = b;
    g_lru_head = b;
    if (!g_lru_tail) g_lru_tail = b;
}

// ── Init ──────────────────────────────────────────────────────────────
int bcache_init(void)
{
    if (g_bcache_ready)
        return 0;

    // Metadata pool (metadata + hash buckets) and the exactly-1-MiB data
    // pool are separate allocations; neither allocation happens under any
    // lock.  On failure free what was taken — never half-publish.
    bcache_buf_t *bufs = kmalloc(sizeof(bcache_buf_t) * BCACHE_BUFS);
    if (!bufs) {
        debug_block("bcache: metadata pool allocation failed\n");
        return -ENOMEM;
    }
    memset(bufs, 0, sizeof(bcache_buf_t) * BCACHE_BUFS);

    uint8_t *data = kmalloc(BCACHE_SECTOR * BCACHE_BUFS);
    if (!data) {
        kfree(bufs);
        debug_block("bcache: data pool allocation failed\n");
        return -ENOMEM;
    }

    for (int i = 0; i < BCACHE_BUFS; i++) {
        bufs[i].data     = data + (size_t)i * BCACHE_SECTOR;
        spin_init(&bufs[i].data_lock);
        g_free_stack[i]  = &bufs[i];
    }
    g_free_top   = BCACHE_BUFS;
    g_bufs       = bufs;
    g_data_pool  = data;
    for (int i = 0; i < BCACHE_BUCKETS; i++)
        g_hash[i] = NULL;
    g_lru_head = g_lru_tail = NULL;
    spin_init(&g_bcache_lock);
    wait_queue_init(&g_bcache_wq);
    g_bcache_ready = 1;
    return 0;
}

// ── Internal helpers (caller holds g_bcache_lock) ─────────────────────
static bcache_buf_t *bcache_hash_lookup(struct block_device *dev, uint64_t lba)
{
    bcache_buf_t *b = g_hash[bcache_hash(dev, lba)];
    while (b) {
        if (bcache_key_eq(b, dev, lba)) return b;
        b = b->hash_next;
    }
    return NULL;
}

static void bcache_hash_insert(bcache_buf_t *b)
{
    uint32_t h = bcache_hash(b->dev, b->lba);
    b->hash_next   = g_hash[h];
    g_hash[h]      = b;
}

static void bcache_hash_remove(bcache_buf_t *b)
{
    bcache_buf_t **pp = &g_hash[bcache_hash(b->dev, b->lba)];
    while (*pp) {
        if (*pp == b) {
            *pp = b->hash_next;
            b->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static bcache_buf_t *bcache_take_free_slot(void)
{
    if (g_free_top == 0) return NULL;   // Task 6: LRU eviction replaces this
    return g_free_stack[--g_free_top];
}

static void bcache_return_slot(bcache_buf_t *b)
{
    b->dev   = NULL;
    b->lba   = 0;
    b->flags = 0;
    b->refcnt = 0;
    g_free_stack[g_free_top++] = b;
}

static void bcache_waiter_add(struct block_device *dev)
{
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    for (int i = 0; i < BCACHE_WAITER_SLOTS; i++) {
        if (g_waiter_tab[i].dev == dev || !g_waiter_tab[i].dev) {
            g_waiter_tab[i].dev = dev;
            g_waiter_tab[i].armed++;
            break;
        }
    }
    spin_unlock_irqrestore(&g_bcache_lock, flags);
}

static void bcache_waiter_sub(struct block_device *dev)
{
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    for (int i = 0; i < BCACHE_WAITER_SLOTS; i++) {
        if (g_waiter_tab[i].dev == dev && g_waiter_tab[i].armed > 0) {
            g_waiter_tab[i].armed--;
            break;
        }
    }
    spin_unlock_irqrestore(&g_bcache_lock, flags);
}

// Publish a BUSY reservation for a missing key: takes a free slot and
// hashes the BUSY entry with the caller's pin already held.  Returns the
// buffer, or NULL when no slot is available.  Caller must re-check the
// hash right before calling (done by the single lock-held caller below).
static bcache_buf_t *bcache_publish_busy(struct block_device *dev, uint64_t lba)
{
    bcache_buf_t *b = bcache_take_free_slot();
    if (!b) return NULL;
    b->dev    = dev;
    b->lba    = lba;
    b->flags  = BC_BUSY;
    b->refcnt = 1;                      // caller's pin
    bcache_hash_insert(b);
    bcache_lru_push_front(b);
    return b;
}

// Success finish for a fill/complete: data already in the buffer.
// Keeps the caller's pin; clears BUSY and sets VALID.
static void bcache_fill_success(bcache_buf_t *b)
{
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    b->flags = (b->flags & ~BC_BUSY) | BC_VALID;
    spin_unlock_irqrestore(&g_bcache_lock, flags);
    wait_queue_wake_all(&g_bcache_wq);
}

// Failure reclaim: the reservation owner releases everything — hash entry
// revoked, slot returned, pin and BUSY dropped, waiters retried.
static void bcache_fill_failed(bcache_buf_t *b)
{
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    bcache_hash_remove(b);
    bcache_lru_remove(b);
    b->refcnt--;                        // release the owner's pin
    bcache_return_slot(b);
    spin_unlock_irqrestore(&g_bcache_lock, flags);
    wait_queue_wake_all(&g_bcache_wq);
}

// Pin a clean, valid buffer and copy the sector out under data_lock.
static void bcache_pin_copy_out(bcache_buf_t *b, void *out)
{
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    b->refcnt++;
    bcache_lru_remove(b);
    bcache_lru_push_front(b);
    spin_unlock_irqrestore(&g_bcache_lock, flags);

    uint64_t dflags = spin_lock_irqsave(&b->data_lock);
    memcpy(out, b->data, BCACHE_SECTOR);
    spin_unlock_irqrestore(&b->data_lock, dflags);

    flags = spin_lock_irqsave(&g_bcache_lock);
    int last = (--b->refcnt == 0);
    spin_unlock_irqrestore(&g_bcache_lock, flags);
    if (last)
        wait_queue_wake_all(&g_bcache_wq);
}

// Drop our pin, waking pin-waiters when it reaches zero.
static void bcache_unpin(bcache_buf_t *b)
{
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    int last = (--b->refcnt == 0);
    spin_unlock_irqrestore(&g_bcache_lock, flags);
    if (last)
        wait_queue_wake_all(&g_bcache_wq);
}

// Wait until the key is no longer BUSY, then pin the entry in the same
// critical section that observed the condition (spec §5.3: a waiter that
// can progress reserves in that lock).  Returns the pinned buffer with
// *out_flags sampled; only VALID entries are pinned.
static bcache_buf_t *bcache_wait_key_ready(struct block_device *dev, uint64_t lba)
{
    uint64_t irq = arch_local_irq_save();   // held across the whole window
    for (;;) {
        bcache_buf_t *b;
        uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
        b = bcache_hash_lookup(dev, lba);
        if (b && (b->flags & BC_BUSY)) {
            // Not satisfiable yet: ARM (with the metadata lock released,
            // IRQs still off), then recheck; a satisfiable recheck
            // reserves in that critical section.
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            bcache_waiter_add(dev);
            wait_queue_arm(&g_bcache_wq);

            flags = spin_lock_irqsave(&g_bcache_lock);
            b = bcache_hash_lookup(dev, lba);
            int ready = (b && !(b->flags & BC_BUSY) && (b->flags & BC_VALID));
            int gone   = (b == NULL);
            if (ready)
                b->refcnt++;               // reserve now, in this lock
            spin_unlock_irqrestore(&g_bcache_lock, flags);

            if (ready || gone) {
                // Satisfied (reserve done above) or the reservation was
                // reclaimed by a failed fill — either way there is no
                // future producer for this window: never schedule.
                wait_queue_disarm();
                bcache_waiter_sub(dev);
                arch_local_irq_restore(irq);
                return ready ? b : NULL;
            }
            schedule();
            wait_queue_disarm();
            bcache_waiter_sub(dev);
            continue;                      // full recheck from the top
        }
        if (b && (b->flags & BC_VALID)) {
            b->refcnt++;                   // reserve in this lock
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            arch_local_irq_restore(irq);
            return b;                      // pinned
        }
        // Key vanished (failed fill reclaimed it): retry from the top —
        // never treat a failed fill's contents as a hit.
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        arch_local_irq_restore(irq);
        return NULL;
    }
}

// ── Read path ─────────────────────────────────────────────────────────
int bcache_read_sector(struct block_device *dev, uint64_t lba, void *buf)
{
    if (!g_bcache_ready)
        return -ENOMEM;
    if (!dev || !dev->present || !buf)
        return -EINVAL;
    if (dev->sector_size != BCACHE_SECTOR)
        return -EINVAL;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
        bcache_buf_t *b = bcache_hash_lookup(dev, lba);
        if (b && (b->flags & BC_BUSY)) {
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            b = bcache_wait_key_ready(dev, lba);
            if (!b)
                continue;                  // key vanished: retry the lookup
            bcache_pin_copy_out(b, buf);
            bcache_unpin(b);
            return 0;
        }
        if (b && (b->flags & BC_VALID)) {
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            bcache_pin_copy_out(b, buf);   // pins, copies, unpins
            return 0;
        }

        // Missing: publish BUSY + pin, fill outside the lock.
        b = bcache_publish_busy(dev, lba);
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        if (!b)
            return -ENOMEM;                // Task 6: exhaustion waits instead

        int ret = dev->ops->read(dev, lba, 1, b->data);   // raw ops: no recursion
        if (ret != 0) {
            bcache_fill_failed(b);
            return ret;
        }
        bcache_fill_success(b);            // keeps our pin, clears BUSY

        uint64_t dflags = spin_lock_irqsave(&b->data_lock);
        memcpy(buf, b->data, BCACHE_SECTOR);
        spin_unlock_irqrestore(&b->data_lock, dflags);

        bcache_unpin(b);
        return 0;
    }
}

// ── Wait helpers for the write/flush paths ────────────────────────────

// Reserve the key BUSY (no pin of our own) for exclusive maintenance.
// Returns the buffer with BUSY held (entry stable: BUSY blocks eviction
// and new pins), or NULL when the key vanished.  When the recheck finds
// the key free, BUSY is taken in that same critical section.
static bcache_buf_t *bcache_reserve_key_busy(struct block_device *dev, uint64_t lba)
{
    uint64_t irq = arch_local_irq_save();
    for (;;) {
        bcache_buf_t *b;
        uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
        b = bcache_hash_lookup(dev, lba);
        if (!b) {
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            arch_local_irq_restore(irq);
            return NULL;
        }
        if (!(b->flags & BC_BUSY)) {
            b->flags |= BC_BUSY;               // reserve in this lock
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            arch_local_irq_restore(irq);
            return b;
        }
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        bcache_waiter_add(dev);
        wait_queue_arm(&g_bcache_wq);

        flags = spin_lock_irqsave(&g_bcache_lock);
        b = bcache_hash_lookup(dev, lba);
        int gone = (b == NULL);
        int free_ = (b && !(b->flags & BC_BUSY));
        if (free_)
            b->flags |= BC_BUSY;
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        if (gone || free_) {
            wait_queue_disarm();
            bcache_waiter_sub(dev);
            arch_local_irq_restore(irq);
            return free_ ? b : NULL;
        }
        schedule();
        wait_queue_disarm();
        bcache_waiter_sub(dev);
        // loop: full recheck
    }
}

// Wait for the in-flight pins on our BUSY-reserved buffer to drain.  The
// buffer is stable while BUSY is held.  An already-zero pin count (or one
// observed on the post-ARM recheck) proceeds without scheduling.
static void bcache_wait_buf_pins_released(bcache_buf_t *b)
{
    uint64_t irq = arch_local_irq_save();
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
        int drained = (b->refcnt == 0);
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        if (drained) {
            arch_local_irq_restore(irq);
            return;
        }
        bcache_waiter_add(b->dev);
        wait_queue_arm(&g_bcache_wq);

        flags = spin_lock_irqsave(&g_bcache_lock);
        drained = (b->refcnt == 0);
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        if (drained) {
            wait_queue_disarm();
            bcache_waiter_sub(b->dev);
            arch_local_irq_restore(irq);
            return;
        }
        schedule();
        wait_queue_disarm();
        bcache_waiter_sub(b->dev);
    }
}

// ── Write path ────────────────────────────────────────────────────────
int bcache_write_sector(struct block_device *dev, uint64_t lba, const void *buf)
{
    if (!g_bcache_ready)
        return -ENOMEM;
    if (!dev || !dev->present || !buf)
        return -EINVAL;
    if (dev->sector_size != BCACHE_SECTOR)
        return -EINVAL;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
        bcache_buf_t *b = bcache_hash_lookup(dev, lba);
        if (b && (b->flags & BC_BUSY)) {
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            // Wait for exclusive access, then mutate the pinned buffer.
            b = bcache_reserve_key_busy(dev, lba);
            if (!b)
                continue;                  // key vanished: retry
            uint64_t dflags = spin_lock_irqsave(&b->data_lock);
            memcpy(b->data, buf, BCACHE_SECTOR);
            spin_unlock_irqrestore(&b->data_lock, dflags);
            flags = spin_lock_irqsave(&g_bcache_lock);
            b->flags |= BC_VALID | BC_DIRTY;   // bitwise: keep any new BUSY
            b->flags &= ~BC_BUSY;
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            wait_queue_wake_all(&g_bcache_wq);
            return 0;
        }
        if (b) {
            // Valid hit: pin, copy under data_lock, commit flags.
            b->refcnt++;
            bcache_lru_remove(b);
            bcache_lru_push_front(b);
            spin_unlock_irqrestore(&g_bcache_lock, flags);

            uint64_t dflags = spin_lock_irqsave(&b->data_lock);
            memcpy(b->data, buf, BCACHE_SECTOR);
            spin_unlock_irqrestore(&b->data_lock, dflags);

            flags = spin_lock_irqsave(&g_bcache_lock);
            b->flags |= BC_VALID | BC_DIRTY;   // bitwise: keep any new BUSY
            int last = (--b->refcnt == 0);
            spin_unlock_irqrestore(&g_bcache_lock, flags);
            if (last)
                wait_queue_wake_all(&g_bcache_wq);
            return 0;
        }

        // Missing key: publish BUSY + pin, no fill read needed — a whole
        // sector write overwrites the data.
        b = bcache_publish_busy(dev, lba);
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        if (!b)
            return -ENOMEM;                // Task 6: exhaustion waits instead

        uint64_t dflags = spin_lock_irqsave(&b->data_lock);
        memcpy(b->data, buf, BCACHE_SECTOR);
        spin_unlock_irqrestore(&b->data_lock, dflags);

        flags = spin_lock_irqsave(&g_bcache_lock);
        b->flags = (b->flags & ~BC_BUSY) | BC_VALID | BC_DIRTY;
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        wait_queue_wake_all(&g_bcache_wq);
        bcache_unpin(b);
        return 0;
    }
}

// ── Flush (spec §5.5: DIRTY-key snapshot + per-key BUSY drain) ────────
int bcache_flush_device(struct block_device *dev)
{
    if (!g_bcache_ready)
        return -ENOMEM;
    if (!dev)
        return -EINVAL;

    // Target array outside any lock.  Flush entries are one-shot per call;
    // a device can hold at most BCACHE_BUFS dirty keys.
    uint64_t (*targets)[2] = kmalloc(sizeof(uint64_t) * 2 * BCACHE_BUFS);
    if (!targets) {
        debug_block("bcache: flush target allocation failed\n");
        return -ENOMEM;
    }

    // Snapshot: the linearization boundary of this flush.  Pinned and
    // BUSY keys are included — they are drained below.
    uint32_t ntargets = 0;
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    for (int i = 0; i < BCACHE_BUFS; i++) {
        bcache_buf_t *b = &g_bufs[i];
        if (b->dev == dev && (b->flags & BC_DIRTY) && ntargets < BCACHE_BUFS) {
            targets[ntargets][0] = (uint64_t)(uintptr_t)b->dev;
            targets[ntargets][1] = b->lba;
            ntargets++;
        }
    }
    spin_unlock_irqrestore(&g_bcache_lock, flags);

    int first_err = 0;
    for (uint32_t t = 0; t < ntargets; t++) {
        uint64_t lba = targets[t][1];
        for (;;) {
            // Reserve BUSY: blocks new pins, eviction, and concurrent
            // flushes on this key.  A vanished or clean key means the
            // writeback already succeeded elsewhere (evict / other flush).
            bcache_buf_t *b = bcache_reserve_key_busy(dev, lba);
            if (!b)
                break;
            uint64_t chk = spin_lock_irqsave(&g_bcache_lock);
            int still_dirty = (b->flags & BC_DIRTY) != 0;
            spin_unlock_irqrestore(&g_bcache_lock, chk);
            if (!still_dirty) {
                chk = spin_lock_irqsave(&g_bcache_lock);
                b->flags &= ~BC_BUSY;
                spin_unlock_irqrestore(&g_bcache_lock, chk);
                wait_queue_wake_all(&g_bcache_wq);
                break;
            }

            // Drain in-flight pins, then write back outside every lock.
            bcache_wait_buf_pins_released(b);
            int ret = dev->ops->write(dev, lba, 1, b->data);

            chk = spin_lock_irqsave(&g_bcache_lock);
            b->flags &= ~BC_BUSY;
            if (ret == 0)
                b->flags &= ~BC_DIRTY;     // only success clears DIRTY
            spin_unlock_irqrestore(&g_bcache_lock, chk);
            wait_queue_wake_all(&g_bcache_wq);

            if (ret != 0) {
                if (!first_err)
                    first_err = ret;
                break;                     // keep key/data/DIRTY on failure
            }
            break;
        }
    }

    kfree(targets);
    return first_err;
}

void bcache_drop_device(struct block_device *dev)
{
    if (!g_bcache_ready || !dev)
        return;
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    for (int i = 0; i < BCACHE_BUCKETS; i++) {
        bcache_buf_t **pp = &g_hash[i];
        while (*pp) {
            bcache_buf_t *b = *pp;
            if (b->dev == dev) {
                *pp = b->hash_next;
                b->hash_next = NULL;
                bcache_lru_remove(b);
                bcache_return_slot(b);
                continue;
            }
            pp = &b->hash_next;
        }
    }
    spin_unlock_irqrestore(&g_bcache_lock, flags);
}

// ── Test hooks ────────────────────────────────────────────────────────
#if defined(OS01_HOST_TEST) || defined(OS01_SELFTEST)

void bcache_reset_for_test(void)
{
    if (!g_bcache_ready) return;
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    for (int i = 0; i < BCACHE_BUCKETS; i++)
        g_hash[i] = NULL;
    g_lru_head = g_lru_tail = NULL;
    g_free_top = 0;
    for (int i = 0; i < BCACHE_BUFS; i++) {
        bcache_buf_t *b = &g_bufs[i];
        b->dev = NULL;
        b->lba = 0;
        b->flags = 0;
        b->refcnt = 0;
        g_free_stack[i] = b;
    }
    g_free_top = BCACHE_BUFS;
    for (int i = 0; i < BCACHE_WAITER_SLOTS; i++) {
        g_waiter_tab[i].dev = NULL;
        g_waiter_tab[i].armed = 0;
    }
    spin_unlock_irqrestore(&g_bcache_lock, flags);
}

void bcache_debug_stats(struct block_device *dev, bcache_test_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!g_bcache_ready || !dev) return;
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    for (int i = 0; i < BCACHE_BUFS; i++) {
        bcache_buf_t *b = &g_bufs[i];
        if (b->dev != dev) continue;
        out->entries++;
        if (b->refcnt > 0) out->pinned++;
        if (b->flags & BC_BUSY)  out->busy++;
        if (b->flags & BC_DIRTY) out->dirty++;
    }
    for (int i = 0; i < BCACHE_WAITER_SLOTS; i++) {
        if (g_waiter_tab[i].dev == dev)
            out->waiters += g_waiter_tab[i].armed;
    }
    spin_unlock_irqrestore(&g_bcache_lock, flags);
}

int bcache_debug_pin(struct block_device *dev, uint64_t lba)
{
    if (!g_bcache_ready || !dev)
        return -EINVAL;
    uint8_t tmp[BCACHE_SECTOR];
    int ret = bcache_read_sector(dev, lba, tmp);   // fill through the real path
    if (ret != 0)
        return ret;
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    bcache_buf_t *b = bcache_hash_lookup(dev, lba);
    if (!b || !(b->flags & BC_VALID)) {
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        return -ENOENT;
    }
    b->refcnt++;
    spin_unlock_irqrestore(&g_bcache_lock, flags);
    return 0;
}

void bcache_debug_unpin(struct block_device *dev, uint64_t lba)
{
    if (!g_bcache_ready || !dev)
        return;
    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    bcache_buf_t *b = bcache_hash_lookup(dev, lba);
    int last = 0;
    if (b && b->refcnt > 0)
        last = (--b->refcnt == 0);
    spin_unlock_irqrestore(&g_bcache_lock, flags);
    if (last)
        wait_queue_wake_all(&g_bcache_wq);
}

#endif // OS01_HOST_TEST || OS01_SELFTEST

#ifdef OS01_HOST_TEST

int bcache_test_reserve_busy(struct block_device *dev, uint64_t lba)
{
    if (!g_bcache_ready || !dev)
        return -EINVAL;
    if (dev->sector_size != BCACHE_SECTOR)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    if (bcache_hash_lookup(dev, lba)) {
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        return -EEXIST;
    }
    bcache_buf_t *b = bcache_publish_busy(dev, lba);
    spin_unlock_irqrestore(&g_bcache_lock, flags);
    return b ? 0 : -ENOMEM;
}

int bcache_test_complete_busy(struct block_device *dev, uint64_t lba,
                              int io_result, const void *sector)
{
    if (!g_bcache_ready || !dev)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_bcache_lock);
    bcache_buf_t *b = bcache_hash_lookup(dev, lba);
    // Ownership check: only the key's own test reservation can complete,
    // and only once (a completed entry is no longer BUSY).
    if (!b || !(b->flags & BC_BUSY) || b->dev != dev || b->lba != lba) {
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        return -EINVAL;
    }
    if (io_result == 0 && !sector) {
        spin_unlock_irqrestore(&g_bcache_lock, flags);
        return -EINVAL;
    }
    spin_unlock_irqrestore(&g_bcache_lock, flags);

    if (io_result == 0) {
        memcpy(b->data, sector, BCACHE_SECTOR);
        bcache_fill_success(b);            // real success finish
        bcache_unpin(b);                   // release the test pin
        return 0;
    }
    bcache_fill_failed(b);                 // real reclaim path
    return 0;
}

#endif // OS01_HOST_TEST
