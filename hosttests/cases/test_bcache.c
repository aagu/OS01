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

enum fake_mode {
    FAKE_OK = 0,
    FAKE_FAIL_FIRST_READ,   // next read returns -EIO once
};

typedef struct {
    uint8_t        backend[FAKE_SECTORS][BCACHE_SECTOR];
    int            reads;
    int            writes;
    int            last_count;
    uint64_t       last_lba;
    enum fake_mode mode;
    int            fail_writes;    // when set, writes return -EIO once... persistently until cleared
} fake_dev_t;

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
    f->last_lba   = lba;
    f->last_count = (int)count;
    for (uint32_t i = 0; i < count; i++) {
        if (lba + i < FAKE_SECTORS)
            memcpy((uint8_t *)buf + i * BCACHE_SECTOR, f->backend[lba + i],
                   BCACHE_SECTOR);
    }
    return 0;
}

static int fake_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    fake_dev_t *f = (fake_dev_t *)dev->private_data;
    f->writes++;
    f->last_lba   = lba;
    f->last_count = (int)count;
    if (f->fail_writes)
        return -EIO;
    for (uint32_t i = 0; i < count; i++) {
        if (lba + i < FAKE_SECTORS)
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

static fake_dev_t      s_fake;
static block_device_t *s_disk = NULL;

static const uint8_t PATTERN_A = 0xA7;

static void fake_reset(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    for (int i = 0; i < FAKE_SECTORS; i++)
        memset(s_fake.backend[i], PATTERN_A, BCACHE_SECTOR);
}

static void fake_register(void)
{
    fake_reset();
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
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
