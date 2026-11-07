// hosttests/cases/test_bcache.c — ARCH-6 block cache unit tests.
//
// Links the production kernel/block/bcache.c and kernel/sync/wait.c
// against the arch6_bcache_runtime host substrate.  Fake devices are
// registered through the production block_device_register (which also
// initializes the cache pools — except init_oom_retry, which runs
// first and drives bcache_init directly under allocation injection).
//
// Wait-window cases use the HOST_TEST reserve/complete hooks: the fake
// ops->read NEVER blocks, so any real scheduling inside bcache must be
// driven by complete_busy from the wait_queue_arm wrapper (see the
// wraps at the bottom of this file).

#include "test_framework.h"
#include <block/bcache.h>
#include <block/blockdev.h>
#include <sync/wait.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

// ── Allocator with injection (same surface as test_block_device_ops) ──
static int  s_kmalloc_calls = 0;
static int  s_kfree_calls   = 0;
static int  s_kmalloc_fail_at = 0;   // 1-based call ordinal to fail; 0 = never
static bool s_kmalloc_failed  = false;

void *kmalloc(size_t size)
{
    s_kmalloc_calls++;
    if (s_kmalloc_fail_at && s_kmalloc_calls == s_kmalloc_fail_at) {
        s_kmalloc_failed = true;
        return NULL;
    }
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    s_kfree_calls++;
    free(ptr);
    return 0;
}

// ── Fake block device ─────────────────────────────────────────────────
#define FAKE_SECTORS 64
#define BIG_SECTORS  (BCACHE_BUFS + 16)

enum fake_mode {
    FAKE_OK = 0,
    FAKE_FAIL_FIRST_READ,   // next read returns -EIO once
};

static uint8_t s_small_backend[FAKE_SECTORS][BCACHE_SECTOR];
static uint8_t s_big_backend[BIG_SECTORS + 1][BCACHE_SECTOR];

// Optional per-write hook (used to inject events inside a victim
// writeback, e.g. publishing the requested key to exercise the
// alloc-time recheck).
static void (*s_write_hook)(block_device_t *dev, uint64_t lba) = NULL;

typedef struct {
    uint8_t      (*backend)[BCACHE_SECTOR];
    uint32_t       nsectors;
    int            reads;
    int            writes;
    int            last_count;
    uint64_t       last_lba;
    enum fake_mode mode;
    int            fail_writes;    // when set, writes return -EIO until cleared
} fake_dev_t;

static fake_dev_t s_fake;
static fake_dev_t s_big;
static fake_dev_t s_devB;

static int fake_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    fake_dev_t *f = (fake_dev_t *)dev->private_data;
    f->reads++;                        // every backend call counts
    f->last_lba   = lba;
    f->last_count = (int)count;
    if (f->mode == FAKE_FAIL_FIRST_READ) {
        f->mode = FAKE_OK;
        return -EIO;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (lba + i < f->nsectors)
            memcpy((uint8_t *)buf + i * BCACHE_SECTOR, f->backend[lba + i],
                   BCACHE_SECTOR);
    }
    return 0;
}

static int fake_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    fake_dev_t *f = (fake_dev_t *)dev->private_data;
    f->writes++;
    f->last_lba   = lba;
    f->last_count = (int)count;
    if (s_write_hook) s_write_hook(dev, lba);
    if (f->fail_writes)
        return -EIO;
    for (uint32_t i = 0; i < count; i++) {
        if (lba + i < f->nsectors)
            memcpy(f->backend[lba + i], (const uint8_t *)buf + i * BCACHE_SECTOR,
                   BCACHE_SECTOR);
    }
    return 0;
}

static int fake_flush_err(block_device_t *dev)
{
    (void)dev;
    return -EIO;
}

static const struct block_device_ops fake_ops = {
    .read  = fake_read,
    .write = fake_write,
    .flush = NULL,
};

static block_device_t *s_disk = NULL;

static const uint8_t PATTERN_A = 0xA7;

static void fake_init(fake_dev_t *f, uint8_t (*backend)[BCACHE_SECTOR],
                      uint32_t nsectors)
{
    memset(f, 0, sizeof(*f));
    f->backend  = backend;
    f->nsectors = nsectors;
    for (uint32_t i = 0; i < nsectors; i++)
        memset(backend[i], PATTERN_A, BCACHE_SECTOR);
}

static void fake_register(void)
{
    fake_init(&s_fake, s_small_backend, FAKE_SECTORS);
    s_write_hook = NULL;
    s_kmalloc_calls = 0;
    s_kfree_calls   = 0;
    s_kmalloc_fail_at = 0;
    struct block_device_desc desc = {
        .name         = "bcachefake",
        .sector_count = FAKE_SECTORS,
        .sector_size  = BCACHE_SECTOR,
        .ops          = &fake_ops,
        .private_data = &s_fake,
        .parent       = NULL,
        .kind         = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&desc, &s_disk));
    assert_not_null(s_disk);
}


static void fake_unregister(void)
{
    if (s_disk) {
        block_device_unregister_boot(s_disk);
        s_disk = NULL;
    }
    bcache_reset_for_test();
}

// ── Wait-window scripting ─────────────────────────────────────────────
// wait_queue_arm is wrapped at link time.  A case installs an action and
// a phase (s_arm_phase: 1 = run before the real ARM, 2 = run after it);
// the action drives bcache_test_complete_busy (fill windows) or
// bcache_debug_unpin (flush-drain windows), covering both halves of the
// ARM→recheck handshake.
static int  s_arm_calls  = 0;
static int  s_completes  = 0;
static int  s_arm_phase  = 0;
static void (*s_arm_action)(void) = NULL;
static uint64_t s_exhaustion_unpin_lba;

void __real_wait_queue_arm(wait_queue_t *wq);

static void run_busy_complete(void)
{
    uint8_t tmp[BCACHE_SECTOR];
    memset(tmp, 0x5C, sizeof(tmp));
    s_completes++;
    bcache_test_complete_busy(s_disk, 5, 0, tmp);
    s_arm_action = NULL;
    s_arm_phase  = 0;
}

static void run_busy_complete_err(void)
{
    s_completes++;
    bcache_test_complete_busy(s_disk, 5, -EIO, NULL);
    s_arm_action = NULL;
    s_arm_phase  = 0;
}

static void run_unpin_action(void)
{
    s_completes++;
    bcache_debug_unpin(s_disk, 8);    // release the pin a flush waits on
    s_arm_action = NULL;
    s_arm_phase  = 0;
}

static void run_exhaustion_unpin(void)
{
    s_completes++;
    bcache_debug_unpin(s_disk, s_exhaustion_unpin_lba);
    s_arm_action = NULL;
    s_arm_phase  = 0;
}

void __wrap_wait_queue_arm(wait_queue_t *wq)
{
    s_arm_calls++;
    if (s_arm_phase == 1 && s_arm_action) s_arm_action();
    __real_wait_queue_arm(wq);
    if (s_arm_phase == 2 && s_arm_action) s_arm_action();
}

void __real_wait_queue_sleep(wait_queue_t *wq);
void __wrap_wait_queue_sleep(wait_queue_t *wq)
{
    __real_wait_queue_sleep(wq);
}

extern int arch6_bcache_schedule_count;

// ── Cases ─────────────────────────────────────────────────────────────

TEST_FUNC(init_oom_retry) {
    // Runs first: the pools are not yet initialized (no register happened).
    // Every allocation point of bcache_init must fail cleanly (no partial
    // publish, everything it took gets freed) and a retry must succeed.
    s_kmalloc_calls = 0;
    s_kfree_calls   = 0;

    s_kmalloc_fail_at = 2;          // data pool fails: metadata pool must be freed
    assert_eq(-ENOMEM, bcache_init());
    assert_true(s_kmalloc_failed);
    // one allocation failed (NULL, nothing to free), the other was freed
    assert_eq(s_kmalloc_calls - 1, s_kfree_calls);

    s_kmalloc_calls  = 0;           // ordinals are per-attempt
    s_kfree_calls    = 0;
    s_kmalloc_fail_at = 1;          // metadata pool fails: nothing to free
    assert_eq(-ENOMEM, bcache_init());
    assert_eq(s_kmalloc_calls - 1, s_kfree_calls);

    s_kmalloc_fail_at = 0;
    assert_eq(0, bcache_init());

    // Idempotent after success; reset keeps the pools (no allocation).
    int before = s_kmalloc_calls;
    assert_eq(0, bcache_init());
    bcache_reset_for_test();
    assert_eq(before, s_kmalloc_calls);
}

TEST_FUNC(read_miss_fills) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0, sizeof(buf));

    assert_eq(0, bcache_read_sector(s_disk, 5, buf));
    assert_eq(1, s_fake.reads);
    assert_eq(PATTERN_A, buf[0]);

    uint8_t buf2[BCACHE_SECTOR];
    assert_eq(0, bcache_read_sector(s_disk, 5, buf2));
    assert_eq(1, s_fake.reads);                 // served from cache
    assert_mem_eq(buf, buf2, BCACHE_SECTOR);

    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(1, st.entries);
    assert_eq(0, st.pinned);
    assert_eq(0, st.busy);
    assert_eq(0, st.waiters);
    fake_unregister();
}

TEST_FUNC(read_error_retries) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];

    s_fake.mode = FAKE_FAIL_FIRST_READ;
    assert_eq(-EIO, bcache_read_sector(s_disk, 9, buf));
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.entries);
    assert_eq(0, st.pinned);
    assert_eq(0, st.busy);

    assert_eq(0, bcache_read_sector(s_disk, 9, buf));
    assert_eq(2, s_fake.reads);
    assert_eq(PATTERN_A, buf[0]);
    fake_unregister();
}

TEST_FUNC(reset_drops_entries) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];
    assert_eq(0, bcache_read_sector(s_disk, 1, buf));
    assert_eq(0, bcache_read_sector(s_disk, 2, buf));
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(2, st.entries);

    bcache_reset_for_test();
    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.entries);

    assert_eq(0, bcache_read_sector(s_disk, 1, buf));
    bcache_debug_stats(s_disk, &st);
    assert_eq(1, st.entries);
    fake_unregister();
}

TEST_FUNC(unregister_drops_entries) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];
    assert_eq(0, bcache_read_sector(s_disk, 3, buf));
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(1, st.entries);

    // Boot unregister must silently drop the cache entries for the
    // retired device — the ops are gone, so writeback is impossible.
    assert_eq(0, block_device_unregister_boot(s_disk));
    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.entries);
    s_disk = NULL;
    bcache_reset_for_test();
}

TEST_FUNC(busy_wake_before_arm) {
    fake_register();
    uint8_t pattern[BCACHE_SECTOR];
    memset(pattern, 0x5C, sizeof(pattern));

    assert_eq(0, bcache_test_reserve_busy(s_disk, 5));
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(1, st.entries);
    assert_eq(1, st.busy);
    assert_eq(1, st.pinned);          // test-owned pin

    s_arm_calls = 0;
    s_completes = 0;
    s_arm_phase = 1;                  // complete right before ARM
    s_arm_action = run_busy_complete;
    uint8_t buf[BCACHE_SECTOR];
    assert_eq(0, bcache_read_sector(s_disk, 5, buf));
    assert_mem_eq(pattern, buf, BCACHE_SECTOR);
    assert_eq(0, s_fake.reads);       // filled by the scripted producer
    assert_eq(1, s_arm_calls);        // one wait window, no lost wake
    assert_eq(1, s_completes);
    assert_eq(0, arch6_bcache_schedule_count);  // recheck satisfied: no sleep

    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.pinned);
    assert_eq(0, st.busy);
    assert_eq(0, st.waiters);
    fake_unregister();
}

TEST_FUNC(busy_wake_after_arm) {
    fake_register();
    uint8_t pattern[BCACHE_SECTOR];
    memset(pattern, 0x5C, sizeof(pattern));

    assert_eq(0, bcache_test_reserve_busy(s_disk, 5));

    s_arm_calls = 0;
    s_completes = 0;
    s_arm_phase = 2;                  // complete right after ARM
    s_arm_action = run_busy_complete;
    uint8_t buf[BCACHE_SECTOR];
    assert_eq(0, bcache_read_sector(s_disk, 5, buf));
    assert_mem_eq(pattern, buf, BCACHE_SECTOR);
    assert_eq(1, s_arm_calls);
    assert_eq(1, s_completes);
    assert_eq(0, arch6_bcache_schedule_count);

    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.pinned);
    assert_eq(0, st.busy);
    assert_eq(0, st.waiters);
    fake_unregister();
}

TEST_FUNC(busy_error_complete_then_retry) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];

    assert_eq(0, bcache_test_reserve_busy(s_disk, 5));
    s_arm_calls = 0;
    s_completes = 0;
    s_arm_phase = 1;                  // complete with -EIO before ARM
    s_arm_action = run_busy_complete_err;
    // The failed fill was reclaimed, so the waiter retries fresh and
    // succeeds — a failed fill is never served as a hit.
    assert_eq(0, bcache_read_sector(s_disk, 5, buf));
    assert_eq(PATTERN_A, buf[0]);
    assert_eq(1, s_completes);
    assert_eq(1, s_fake.reads);       // retry did its own backend read

    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(1, st.entries);
    assert_eq(0, st.pinned);
    assert_eq(0, st.busy);
    fake_unregister();
}

TEST_FUNC(complete_rejects_foreign_or_repeat) {
    fake_register();
    uint8_t pattern[BCACHE_SECTOR];
    memset(pattern, 0x33, sizeof(pattern));

    assert_eq(0, bcache_test_reserve_busy(s_disk, 6));
    assert_eq(0, bcache_test_complete_busy(s_disk, 6, 0, pattern));  // first
    assert_eq(-EINVAL, bcache_test_complete_busy(s_disk, 6, 0, pattern)); // repeat
    assert_eq(-EINVAL, bcache_test_complete_busy(s_disk, 7, 0, pattern)); // foreign key

    // Reserve again; the key-exists rejection still applies.
    assert_eq(0, bcache_test_reserve_busy(s_disk, 7));
    assert_eq(-EEXIST, bcache_test_reserve_busy(s_disk, 7));
    assert_eq(0, bcache_test_complete_busy(s_disk, 7, 0, pattern));
    fake_unregister();
}

TEST_FUNC(sector_size_mismatch_rejected) {
    fake_register();
    // A non-512 sector device must not enter the cache.
    static fake_dev_t f2;
    memset(&f2, 0, sizeof(f2));
    static const struct block_device_ops ops2 = { .read = fake_read, .write = fake_write };
    struct block_device_desc desc = {
        .name = "bcachefake4k", .sector_count = 16, .sector_size = 4096,
        .ops = &ops2, .private_data = &f2, .parent = NULL, .kind = BLOCK_DISK,
    };
    block_device_t *d4k = NULL;
    assert_eq(0, block_device_register(&desc, &d4k));
    uint8_t buf[4096];
    assert_eq(-EINVAL, bcache_read_sector(d4k, 0, buf));
    bcache_test_stats_t st;
    bcache_debug_stats(d4k, &st);
    assert_eq(0, st.entries);
    block_device_unregister_boot(d4k);
    bcache_reset_for_test();
    fake_unregister();
}


// ── Task 5: write-back, flush and key convergence ─────────────────────

// Partition device over the fake disk: forwards rel LBAs to the parent
// at +32, mirroring gpt.c partition_read/write.
#define PART_OFFSET 32

static int part_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    return block_device_read(dev->parent, lba + PART_OFFSET, count, buf);
}

static int part_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return block_device_write(dev->parent, lba + PART_OFFSET, count, buf);
}

static int part_flush(block_device_t *dev)
{
    return block_device_flush(dev->parent);
}

static const struct block_device_ops part_ops = {
    .read = part_read, .write = part_write, .flush = part_flush,
};

TEST_FUNC(partition_key_convergence) {
    fake_register();
    struct block_device_desc pdesc = {
        .name = "bcachefake1", .sector_count = 16, .sector_size = BCACHE_SECTOR,
        .ops = &part_ops, .private_data = NULL,
        .parent = s_disk, .kind = BLOCK_PARTITION,
    };
    block_device_t *part = NULL;
    assert_eq(0, block_device_register(&pdesc, &part));

    uint8_t buf[BCACHE_SECTOR];
    // Child read fills exactly one cache entry keyed by the PARENT.
    assert_eq(0, block_device_read(part, 1, 1, buf));
    assert_eq(1, s_fake.reads);
    // Child re-read and parent direct read of the same absolute LBA: hits.
    assert_eq(0, block_device_read(part, 1, 1, buf));
    assert_eq(1, s_fake.reads);
    assert_eq(0, block_device_read(s_disk, PART_OFFSET + 1, 1, buf));
    assert_eq(1, s_fake.reads);
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(1, st.entries);
    bcache_debug_stats(part, &st);
    assert_eq(0, st.entries);

    // Child write: cached (backend untouched), parent read sees it.
    memset(buf, 0x66, sizeof(buf));
    assert_eq(0, block_device_write(part, 1, 1, buf));
    assert_eq(0, s_fake.writes);
    memset(buf, 0, sizeof(buf));
    assert_eq(0, block_device_read(s_disk, PART_OFFSET + 1, 1, buf));
    assert_eq(0x66, buf[0]);
    assert_eq(1, s_fake.reads);

    // Child flush drains through the parent's cache onto the backend.
    assert_eq(0, block_device_flush(part));
    assert_eq(1, s_fake.writes);
    assert_eq(0x66, s_fake.backend[PART_OFFSET + 1][0]);

    // Roll the partition child back first — the parent cannot retire
    // while a child holds it (-EBUSY).
    assert_eq(0, block_device_unregister_boot(part));
    bcache_test_stats_t st2;
    bcache_debug_stats(part, &st2);
    assert_eq(0, st2.entries);
    fake_unregister();
}

TEST_FUNC(write_back_read_your_writes) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x71, sizeof(buf));

    // Wrapper write: backend untouched, wrapper read returns the new value.
    assert_eq(0, block_device_write(s_disk, 7, 1, buf));
    assert_eq(0, s_fake.writes);
    uint8_t rbuf[BCACHE_SECTOR];
    assert_eq(0, block_device_read(s_disk, 7, 1, rbuf));
    assert_eq(0x71, rbuf[0]);
    // Backend still holds the old pattern.
    assert_eq(PATTERN_A, s_fake.backend[7][0]);

    // Read-then-write-then-read coherence on a hot key.
    assert_eq(0, block_device_read(s_disk, 8, 1, rbuf));   // fill hot
    int reads_after_fill = s_fake.reads;
    memset(buf, 0x72, sizeof(buf));
    assert_eq(0, block_device_write(s_disk, 8, 1, buf));
    assert_eq(0, block_device_read(s_disk, 8, 1, rbuf));
    assert_eq(0x72, rbuf[0]);
    assert_eq(reads_after_fill, s_fake.reads);             // no refetch
    fake_unregister();
}

TEST_FUNC(flush_persists_to_backend) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x73, sizeof(buf));
    assert_eq(0, block_device_write(s_disk, 9, 1, buf));
    assert_eq(0, s_fake.writes);

    assert_eq(0, block_device_flush(s_disk));
    assert_eq(1, s_fake.writes);
    assert_eq(0x73, s_fake.backend[9][0]);

    // Second flush: nothing dirty left, no extra backend writes.
    assert_eq(0, block_device_flush(s_disk));
    assert_eq(1, s_fake.writes);
    fake_unregister();
}

TEST_FUNC(flush_waits_for_pin) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x74, sizeof(buf));
    assert_eq(0, block_device_write(s_disk, 8, 1, buf));

    // Hold a pin on the dirty key, then flush: the flusher must wait.
    assert_eq(0, bcache_debug_pin(s_disk, 8));
    s_arm_calls = 0;
    s_completes = 0;
    s_arm_phase = 2;                  // release the pin after the flush ARMed
    s_arm_action = run_unpin_action;
    assert_eq(0, block_device_flush(s_disk));
    assert_eq(1, s_completes);        // unpin happened inside the window
    assert_eq(1, s_fake.writes);      // backend got the new value
    assert_eq(0x74, s_fake.backend[8][0]);

    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.pinned);
    assert_eq(0, st.busy);
    assert_eq(0, st.waiters);
    assert_eq(0, st.dirty);

    // Same drain with the release BEFORE the flush ARM (phase 1).
    memset(buf, 0x75, sizeof(buf));
    assert_eq(0, block_device_write(s_disk, 8, 1, buf));
    assert_eq(0, bcache_debug_pin(s_disk, 8));
    s_arm_calls = 0;
    s_completes = 0;
    s_arm_phase = 1;
    s_arm_action = run_unpin_action;
    assert_eq(0, block_device_flush(s_disk));
    assert_eq(1, s_completes);
    assert_eq(0x75, s_fake.backend[8][0]);
    fake_unregister();
}

TEST_FUNC(flush_failed_write_keeps_dirty) {
    fake_register();
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x77, sizeof(buf));
    assert_eq(0, block_device_write(s_disk, 10, 1, buf));

    // Backend write fails: flush reports the error, keeps key/data/DIRTY.
    s_fake.fail_writes = 1;
    assert_eq(-EIO, block_device_flush(s_disk));
    assert_eq(-EIO, s_disk->last_error);
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(1, st.dirty);
    assert_eq(0, st.busy);
    assert_eq(0, st.pinned);
    assert_eq(PATTERN_A, s_fake.backend[10][0]);   // backend still old
    uint8_t rbuf[BCACHE_SECTOR];
    assert_eq(0, block_device_read(s_disk, 10, 1, rbuf));
    assert_eq(0x77, rbuf[0]);                      // cache still new

    // Recover and retry.
    s_fake.fail_writes = 0;
    assert_eq(0, block_device_flush(s_disk));
    assert_eq(0x77, s_fake.backend[10][0]);
    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.dirty);
    fake_unregister();
}

TEST_FUNC(flush_without_driver_callback) {
    fake_register();                      // fake_ops.flush == NULL
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x78, sizeof(buf));
    assert_eq(0, block_device_write(s_disk, 11, 1, buf));
    assert_eq(0, block_device_flush(s_disk));   // software writeback ran anyway
    assert_eq(0x78, s_fake.backend[11][0]);
    fake_unregister();
}

TEST_FUNC(driver_flush_error_reported) {
    fake_register();
    static const struct block_device_ops fops = {
        .read = fake_read, .write = fake_write, .flush = fake_flush_err,
    };
    struct block_device_desc desc = {
        .name = "bcachefakef", .sector_count = FAKE_SECTORS, .sector_size = BCACHE_SECTOR,
        .ops = &fops, .private_data = &s_fake, .parent = NULL, .kind = BLOCK_DISK,
    };
    block_device_t *dev = NULL;
    assert_eq(0, block_device_register(&desc, &dev));

    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x79, sizeof(buf));
    assert_eq(0, block_device_write(dev, 12, 1, buf));
    // Driver flush fails AFTER the cache write-back completed.
    assert_eq(-EIO, block_device_flush(dev));
    assert_eq(-EIO, dev->last_error);
    assert_eq(0x79, s_fake.backend[12][0]);   // write-back happened first
    block_device_unregister_boot(dev);
    bcache_reset_for_test();
    fake_unregister();
}


// ── Task 6: LRU eviction, recoverable dirty evict, exhaustion wait ────

static void fill_pool_dirty(block_device_t *dev, fake_dev_t *f, uint32_t count)
{
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x81, sizeof(buf));
    for (uint32_t i = 0; i < count; i++) {
        assert_eq(0, block_device_write(dev, i, 1, buf));
    }
}

TEST_FUNC(evict_writes_back) {
    fake_init(&s_big, s_big_backend, BIG_SECTORS);
    s_write_hook = NULL;
    struct block_device_desc desc = {
        .name = "bcachefakebig", .sector_count = BIG_SECTORS, .sector_size = BCACHE_SECTOR,
        .ops = &fake_ops, .private_data = &s_big, .parent = NULL, .kind = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&desc, &s_disk));

    fill_pool_dirty(s_disk, &s_big, BIG_SECTORS);   // BCACHE_BUFS+16 dirty writes

    // The 16 earliest writes were evicted to make room: write-back on
    // evict must have pushed them to the backend already (no flush yet).
    for (uint32_t i = 0; i < 16; i++)
        assert_eq(0x81, s_big.backend[i][0]);

    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(BCACHE_BUFS, st.entries);

    // Resident entries serve from the cache.
    uint8_t buf[BCACHE_SECTOR];
    assert_eq(0, block_device_read(s_disk, 100, 1, buf));
    assert_eq(0x81, buf[0]);

    // Explicit flush drains the rest.
    assert_eq(0, block_device_flush(s_disk));
    for (uint32_t i = 0; i < BIG_SECTORS; i++)
        assert_eq(0x81, s_big.backend[i][0]);
    block_device_unregister_boot(s_disk);
    s_disk = NULL;
    bcache_reset_for_test();
}

TEST_FUNC(lru_preserves_hot) {
    fake_init(&s_big, s_big_backend, BIG_SECTORS);
    s_write_hook = NULL;
    struct block_device_desc desc = {
        .name = "bcachefakehot", .sector_count = BIG_SECTORS, .sector_size = BCACHE_SECTOR,
        .ops = &fake_ops, .private_data = &s_big, .parent = NULL, .kind = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&desc, &s_disk));

    fill_pool_dirty(s_disk, &s_big, BCACHE_BUFS);   // pool exactly full
    uint8_t buf[BCACHE_SECTOR];
    assert_eq(0, block_device_read(s_disk, 0, 1, buf));   // make LBA0 hot
    int reads_baseline = s_big.reads;

    uint8_t wbuf[BCACHE_SECTOR];
    memset(wbuf, 0x82, sizeof(wbuf));
    for (uint32_t i = 0; i < 8; i++)
        assert_eq(0, block_device_write(s_disk, BCACHE_BUFS + i, 1, wbuf));

    // Hot entry survived; cold tails were evicted instead.  LBA0 holds
    // the fill pattern (fill_pool_dirty wrote it).
    assert_eq(0, block_device_read(s_disk, 0, 1, buf));
    assert_eq(0x81, buf[0]);
    assert_eq(reads_baseline, s_big.reads);   // no backend read for LBA0
    block_device_unregister_boot(s_disk);
    s_disk = NULL;
    bcache_reset_for_test();
}

TEST_FUNC(evict_error_preserves_victim) {
    fake_init(&s_big, s_big_backend, BIG_SECTORS + 1);
    s_write_hook = NULL;
    struct block_device_desc desc = {
        .name = "bcachefakeev", .sector_count = BIG_SECTORS + 1, .sector_size = BCACHE_SECTOR,
        .ops = &fake_ops, .private_data = &s_big, .parent = NULL, .kind = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&desc, &s_disk));

    // LBA0 written FIRST so it is the LRU tail (oldest) when the pool
    // overflows; it is dirty, so eviction must write it back.
    fill_pool_dirty(s_disk, &s_big, BIG_SECTORS);

    s_big.fail_writes = 1;              // victim write-back will fail
    uint8_t buf[BCACHE_SECTOR];
    memset(buf, 0x83, sizeof(buf));
    assert_eq(-EIO, block_device_write(s_disk, BIG_SECTORS, 1, buf));
    // wait — write of the NEW key succeeds without backend; the victim
    // write-back failure surfaces on the new request.
    s_big.fail_writes = 0;

    // The victim kept its key, data and DIRTY; nothing stuck BUSY.
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(BCACHE_BUFS, st.entries);
    assert_eq(0, st.busy);
    assert_eq(0, st.pinned);
    uint8_t rbuf[BCACHE_SECTOR];
    assert_eq(0, block_device_read(s_disk, 0, 1, rbuf));   // victim data intact
    assert_eq(0x81, rbuf[0]);

    // Recovered: the eviction now succeeds and the victim is written back.
    assert_eq(0, block_device_write(s_disk, BIG_SECTORS, 1, buf));
    block_device_unregister_boot(s_disk);
    s_disk = NULL;
    bcache_reset_for_test();
}

// recheck script: inside the victim write-back, free one slot and publish
// the requested key through the real reserve+complete path.
static fake_dev_t *s_recheck_victim_dev;
static block_device_t *s_recheck_key_dev;   // device of the requested key
static uint64_t s_recheck_key_lba;
static int s_recheck_ran = 0;

static void evict_recheck_write_hook(block_device_t *dev, uint64_t lba)
{
    (void)dev; (void)lba;
    if (!s_recheck_ran) {
        s_recheck_ran = 1;
        // Free capacity, then publish+complete the requested key.
        bcache_test_drop_key(s_recheck_key_dev, 5);   // a filled, clean key
        uint8_t tmp[BCACHE_SECTOR];
        memset(tmp, 0x84, sizeof(tmp));
        assert_eq(0, bcache_test_reserve_busy(s_recheck_key_dev, s_recheck_key_lba));
        assert_eq(0, bcache_test_complete_busy(s_recheck_key_dev, s_recheck_key_lba, 0, tmp));
    }
}

TEST_FUNC(evict_rechecks_requested_key) {
    fake_init(&s_big, s_big_backend, BIG_SECTORS + 1);
    s_write_hook = NULL;
    struct block_device_desc desc = {
        .name = "bcachefakerc", .sector_count = BIG_SECTORS + 1, .sector_size = BCACHE_SECTOR,
        .ops = &fake_ops, .private_data = &s_big, .parent = NULL, .kind = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&desc, &s_disk));
    fake_init(&s_devB, s_small_backend, FAKE_SECTORS);
    struct block_device_desc bdesc = {
        .name = "bcachefakeb", .sector_count = FAKE_SECTORS, .sector_size = BCACHE_SECTOR,
        .ops = &fake_ops, .private_data = &s_devB, .parent = NULL, .kind = BLOCK_DISK,
    };
    block_device_t *devB = NULL;
    assert_eq(0, block_device_register(&bdesc, &devB));

    // Fill the pool with clean A entries, keeping B's dirty entry oldest
    // (LRU tail).  Order: write B first, then read-fill A entries.
    uint8_t wbuf[BCACHE_SECTOR];
    memset(wbuf, 0x85, sizeof(wbuf));
    assert_eq(0, block_device_write(devB, 3, 1, wbuf));    // B dirty, oldest
    uint8_t touch[BCACHE_SECTOR];
    for (uint32_t i = 0; i < BCACHE_BUFS - 1; i++)
        assert_eq(0, block_device_read(s_disk, i, 1, touch));  // A clean fill

    // Request a NEW A key: victim = B's dirty entry (LRU tail).
    s_recheck_ran = 0;
    s_recheck_victim_dev = &s_devB;
    s_recheck_key_dev = s_disk;
    s_recheck_key_lba = BCACHE_BUFS + 5;
    s_write_hook = evict_recheck_write_hook;

    uint8_t buf[BCACHE_SECTOR];
    int reads_before_request = s_big.reads;
    assert_eq(0, block_device_read(s_disk, BCACHE_BUFS + 5, 1, buf));
    assert_eq(0x84, buf[0]);                 // served by the scripted fill
    assert_eq(reads_before_request, s_big.reads);   // no backend read for it

    // Victim write-back landed on B's backend; B's entry stayed resident
    // (no double key for the request; the freed A slot was reused by the
    // scripted publication, the victim returned clean).
    assert_eq(0x85, s_devB.backend[3][0]);
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    bcache_test_stats_t stb;
    bcache_debug_stats(devB, &stb);
    assert_eq(BCACHE_BUFS - 1, st.entries);  // -1 dropped, +1 published
    assert_eq(1, stb.entries);               // victim stayed with B
    assert_eq(0, stb.dirty);                 // write-back cleared it

    s_write_hook = NULL;
    block_device_unregister_boot(devB);
    block_device_unregister_boot(s_disk);
    s_disk = NULL;
    bcache_reset_for_test();
}

// exhaustion script: release one pin so the waiter can make progress.
static void exhaustion_schedule_script(void)
{
    bcache_debug_unpin(s_disk, s_exhaustion_unpin_lba);
}

TEST_FUNC(exhaustion_waits_for_unpin) {
    fake_init(&s_big, s_big_backend, BIG_SECTORS);
    s_write_hook = NULL;
    struct block_device_desc desc = {
        .name = "bcachefakeex", .sector_count = BIG_SECTORS, .sector_size = BCACHE_SECTOR,
        .ops = &fake_ops, .private_data = &s_big, .parent = NULL, .kind = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&desc, &s_disk));

    // Pin every slot: fill + pin BCACHE_BUFS distinct keys.
    for (uint32_t i = 0; i < BCACHE_BUFS; i++)
        assert_eq(0, bcache_debug_pin(s_disk, i));
    bcache_test_stats_t st;
    bcache_debug_stats(s_disk, &st);
    assert_eq(BCACHE_BUFS, st.entries);
    assert_eq(BCACHE_BUFS, st.pinned);

    // A new key must wait (no free slot, all pinned) — the schedule
    // script releases one pin and the request completes.
    s_exhaustion_unpin_lba = 5;
    arch6_bcache_set_schedule_script(exhaustion_schedule_script);
    uint8_t buf[BCACHE_SECTOR];
    assert_eq(0, block_device_read(s_disk, BCACHE_BUFS, 1, buf));
    arch6_bcache_set_schedule_script(NULL);
    assert_eq(PATTERN_A, buf[0]);

    bcache_debug_stats(s_disk, &st);
    assert_eq(BCACHE_BUFS, st.entries);      // LBA5's slot reused for the new key
    assert_eq(0, st.busy);
    assert_eq(0, st.waiters);

    // Drain the remaining pins; the cache must come out clean.
    for (uint32_t i = 0; i < BCACHE_BUFS; i++) {
        if (i == 5) continue;                // its entry was reused
        bcache_debug_unpin(s_disk, i);
    }
    bcache_debug_stats(s_disk, &st);
    assert_eq(0, st.pinned);
    assert_eq(0, st.pinned);
    block_device_unregister_boot(s_disk);
    s_disk = NULL;
    bcache_reset_for_test();
}

TEST_FUNC(exhaustion_wake_windows) {
    fake_init(&s_big, s_big_backend, BIG_SECTORS);
    s_write_hook = NULL;
    struct block_device_desc desc = {
        .name = "bcachefakeew", .sector_count = BIG_SECTORS, .sector_size = BCACHE_SECTOR,
        .ops = &fake_ops, .private_data = &s_big, .parent = NULL, .kind = BLOCK_DISK,
    };
    assert_eq(0, block_device_register(&desc, &s_disk));

    for (uint32_t round = 0; round < 2; round++) {
        for (uint32_t i = 0; i < BCACHE_BUFS; i++)
            assert_eq(0, bcache_debug_pin(s_disk, i));

        // Release the LAST pin either before or after the waiter's ARM.
        s_exhaustion_unpin_lba = BCACHE_BUFS - 1;
        s_arm_phase = round == 0 ? 1 : 2;
        s_arm_action = run_exhaustion_unpin;
        s_completes = 0;
        uint8_t buf[BCACHE_SECTOR];
        assert_eq(0, block_device_read(s_disk, BCACHE_BUFS, 1, buf));
        assert_eq(PATTERN_A, buf[0]);
        assert_eq(1, s_completes);
        s_arm_phase = 0;
        s_arm_action = NULL;
        s_completes = 0;

        bcache_test_stats_t st;
        bcache_debug_stats(s_disk, &st);
        assert_eq(0, st.busy);
        assert_eq(0, st.waiters);
        // Drain remaining pins for the next round.
        for (uint32_t i = 0; i < BCACHE_BUFS - 1; i++)
            bcache_debug_unpin(s_disk, i);
        bcache_debug_unpin(s_disk, BCACHE_BUFS);
        bcache_reset_for_test();
    }
    block_device_unregister_boot(s_disk);
    s_disk = NULL;
    bcache_reset_for_test();
}

TEST_LIST_BEGIN
    TEST_ENTRY(init_oom_retry),
    TEST_ENTRY(read_miss_fills),
    TEST_ENTRY(read_error_retries),
    TEST_ENTRY(reset_drops_entries),
    TEST_ENTRY(unregister_drops_entries),
    TEST_ENTRY(busy_wake_before_arm),
    TEST_ENTRY(busy_wake_after_arm),
    TEST_ENTRY(busy_error_complete_then_retry),
    TEST_ENTRY(complete_rejects_foreign_or_repeat),
    TEST_ENTRY(sector_size_mismatch_rejected),
    TEST_ENTRY(partition_key_convergence),
    TEST_ENTRY(write_back_read_your_writes),
    TEST_ENTRY(flush_persists_to_backend),
    TEST_ENTRY(flush_waits_for_pin),
    TEST_ENTRY(flush_failed_write_keeps_dirty),
    TEST_ENTRY(flush_without_driver_callback),
    TEST_ENTRY(driver_flush_error_reported),
    TEST_ENTRY(evict_writes_back),
    TEST_ENTRY(lru_preserves_hot),
    TEST_ENTRY(evict_error_preserves_victim),
    TEST_ENTRY(evict_rechecks_requested_key),
    TEST_ENTRY(exhaustion_waits_for_unpin),
    TEST_ENTRY(exhaustion_wake_windows),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
