/*
 * hosttests/cases/test_fb_state.c
 *
 * Framebuffer state coordinator, writer leases, and checked mapping tests
 * (QEMU Resolution Switcher Task 2).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <test_framework.h>

#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <driver/fb_state.h>
#include <arch/x86_64/fb_map.h>

static int g_failed = 0;

#define CHECK_TRUE(cond) do { \
    if (!(cond)) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

#define CHECK_EQ(exp, act) do { \
    long _e = (long)(exp); \
    long _a = (long)(act); \
    if (_e != _a) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s (expected %ld, got %ld)\n", __FILE__, __LINE__, #act, _e, _a); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

#define CHECK_PTR_EQ(exp, act) do { \
    void *_e = (void *)(exp); \
    void *_a = (void *)(act); \
    if (_e != _a) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s (expected %p, got %p)\n", __FILE__, __LINE__, #act, _e, _a); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

static struct fb_info make_default_info(void)
{
    struct fb_info info = {
        .width = 1024,
        .height = 768,
        .stride = 1024 * 4,
        .bpp = 32,
        .format = FB_FORMAT_RGB32,
    };
    return info;
}

/* ── Test 1: Bootstrap before first writer ── */
TEST_FUNC(test_bootstrap_before_first_writer)
{
    fb_resolution_runtime_reset();

    struct fb_info info = make_default_info();
    uint64_t phys_base = 0xe0000000ULL;
    uint64_t gop_size = 1024 * 768 * 4;

    fb_bootstrap_state(phys_base, gop_size, &info);

    /* 1. Before initial mapping is published, writer_begin must return -EAGAIN */
    fb_lease_t lease = {0};
    int rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(-EAGAIN, rc);
    CHECK_TRUE(!lease.held);

    /* 2. Publish initial mapping (simulating early boot mapping) */
    uint32_t *early_addr = (uint32_t *)0xffff80003f000000ULL;
    fb_publish_initial_mapping(early_addr, gop_size);

    /* 3. Snapshot read must now see generation 1 and published mapping */
    fb_snapshot_t snap;
    rc = fb_snapshot_read(&snap);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, snap.state.generation);
    CHECK_EQ(1024, snap.state.info.width);
    CHECK_EQ(768, snap.state.info.height);
    CHECK_PTR_EQ(early_addr, snap.addr);
    CHECK_EQ(gop_size, snap.mapped_size);

    /* 4. Trusted writer (expected_generation = 0) can now begin */
    rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(0, rc);
    CHECK_TRUE(lease.held);
    CHECK_EQ(1, lease.snapshot.state.generation);
    CHECK_PTR_EQ(early_addr, lease.snapshot.addr);
    CHECK_EQ(gop_size, lease.snapshot.mapped_size);

    /* End lease */
    fb_writer_end(&lease);
    CHECK_TRUE(!lease.held);

    /* Repeated fb_writer_end on unheld lease is no-op */
    fb_writer_end(&lease);
    CHECK_TRUE(!lease.held);

    /* 5. Matching expected_generation (1) succeeds */
    rc = fb_writer_begin(&lease, 1);
    CHECK_EQ(0, rc);
    CHECK_TRUE(lease.held);
    fb_writer_end(&lease);

    /* 6. Mismatched expected_generation (2) fails with -ESTALE */
    rc = fb_writer_begin(&lease, 2);
    CHECK_EQ(-ESTALE, rc);
    CHECK_TRUE(!lease.held);
}

/* ── Test 2: Transition drains frame across threads ── */
typedef struct {
    pthread_barrier_t *barrier_start;
    pthread_barrier_t *barrier_drain;
    volatile bool can_finish;
    int worker_rc;
} drain_worker_arg_t;

static void *drain_writer_thread(void *arg)
{
    drain_worker_arg_t *w = (drain_worker_arg_t *)arg;
    fb_lease_t lease = {0};

    int rc = fb_writer_begin(&lease, 0);
    w->worker_rc = rc;
    if (rc != 0) {
        return NULL;
    }

    /* Wait at barrier: writer holds lease */
    pthread_barrier_wait(w->barrier_start);

    /* Wait until transition is trying to drain */
    while (!w->can_finish) {
        usleep(1000);
    }

    fb_writer_end(&lease);
    return NULL;
}

TEST_FUNC(test_transition_drains_frame)
{
    fb_resolution_runtime_reset();
    g_mock_use_real_clock = true;
    g_mock_yield_on_wait = true;

    struct fb_info info = make_default_info();
    uint64_t gop_size = 1024 * 768 * 4;
    fb_bootstrap_state(0xe0000000ULL, gop_size, &info);
    fb_publish_initial_mapping((uint32_t *)0xffff80003f000000ULL, gop_size);

    pthread_barrier_t b_start;
    pthread_barrier_init(&b_start, NULL, 2);

    drain_worker_arg_t w_arg = {
        .barrier_start = &b_start,
        .can_finish = false,
        .worker_rc = -1,
    };

    pthread_t th;
    pthread_create(&th, NULL, drain_writer_thread, &w_arg);

    /* Wait until writer thread acquired lease */
    pthread_barrier_wait(&b_start);
    CHECK_EQ(0, w_arg.worker_rc);

    /* In a separate thread or non-blocking check:
     * While transition is about to begin, simulate control mutex holder calling fb_transition_begin */
    w_arg.can_finish = false;

    /* Start transition in background, or allow worker to release after a delay */
    usleep(10000); /* 10 ms */

    /* Release worker after 20ms */
    w_arg.can_finish = true;

    int rc = fb_transition_begin(false);
    CHECK_EQ(0, rc);

    /* Hardware was not touched */
    CHECK_EQ(0, g_mock_hardware_call_count);

    /* While in transition, new writer must get -EAGAIN */
    fb_lease_t new_lease = {0};
    rc = fb_writer_begin(&new_lease, 0);
    CHECK_EQ(-EAGAIN, rc);
    CHECK_TRUE(!new_lease.held);

    /* End transition */
    fb_transition_end();

    /* New writer can now begin */
    rc = fb_writer_begin(&new_lease, 0);
    CHECK_EQ(0, rc);
    CHECK_TRUE(new_lease.held);
    fb_writer_end(&new_lease);

    pthread_join(th, NULL);
    pthread_barrier_destroy(&b_start);
}

/* ── Test 3: Drain timeout recovers admission ── */
TEST_FUNC(test_drain_timeout)
{
    fb_resolution_runtime_reset();
    g_mock_use_real_clock = false;
    g_mock_clock_ns = 1000000000ULL;
    g_mock_clock_step_ns = 100000000ULL; /* 100ms per step */

    struct fb_info info = make_default_info();
    uint64_t gop_size = 1024 * 768 * 4;
    fb_bootstrap_state(0xe0000000ULL, gop_size, &info);
    fb_publish_initial_mapping((uint32_t *)0xffff80003f000000ULL, gop_size);

    /* Acquire a lease that won't be released */
    fb_lease_t held_lease = {0};
    int rc = fb_writer_begin(&held_lease, 0);
    CHECK_EQ(0, rc);
    CHECK_TRUE(held_lease.held);

    /* Attempt transition with drain pause.
     * The fake clock will step past the 1s deadline (100ms * 11 steps > 1s).
     */
    rc = fb_transition_begin(false);
    CHECK_EQ(-EBUSY, rc);

    /* Release the stuck lease */
    fb_writer_end(&held_lease);

    /* Verify admission was restored: a new writer can begin */
    fb_lease_t new_lease = {0};
    rc = fb_writer_begin(&new_lease, 0);
    CHECK_EQ(0, rc);
    CHECK_TRUE(new_lease.held);

    /* Verify generation is still 1 */
    CHECK_EQ(1, new_lease.snapshot.state.generation);
    fb_writer_end(&new_lease);
}

/* ── Test 4: Boot probe transition does not block ── */
TEST_FUNC(test_boot_probe_transition)
{
    fb_resolution_runtime_reset();

    struct fb_info info = make_default_info();
    uint64_t gop_size = 1024 * 768 * 4;
    fb_bootstrap_state(0xe0000000ULL, gop_size, &info);
    fb_publish_initial_mapping((uint32_t *)0xffff80003f000000ULL, gop_size);

    /* Case A: active writer present -> boot_probe returns -EBUSY immediately */
    fb_lease_t held_lease = {0};
    int rc = fb_writer_begin(&held_lease, 0);
    CHECK_EQ(0, rc);

    rc = fb_transition_begin(true);
    CHECK_EQ(-EBUSY, rc);

    fb_writer_end(&held_lease);

    /* Case B: no active writer -> boot_probe returns 0 */
    rc = fb_transition_begin(true);
    CHECK_EQ(0, rc);

    fb_transition_end();
}

/* ── Test 5: Failed state rejects all operations ── */
TEST_FUNC(test_failed_state)
{
    fb_resolution_runtime_reset();

    struct fb_info info = make_default_info();
    uint64_t gop_size = 1024 * 768 * 4;
    fb_bootstrap_state(0xe0000000ULL, gop_size, &info);
    fb_publish_initial_mapping((uint32_t *)0xffff80003f000000ULL, gop_size);

    /* Mark backend as failed */
    fb_mark_failed();

    /* 1. fb_writer_begin returns -EIO */
    fb_lease_t lease = {0};
    int rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(-EIO, rc);
    CHECK_TRUE(!lease.held);

    /* 2. fb_snapshot_read returns -EIO */
    fb_snapshot_t snap;
    rc = fb_snapshot_read(&snap);
    CHECK_EQ(-EIO, rc);

    /* 3. fb_transition_begin returns -EIO */
    rc = fb_transition_begin(false);
    CHECK_EQ(-EIO, rc);

    /* 4. fb_transition_end does not reopen admission */
    fb_transition_end();

    rc = fb_writer_begin(&lease, 0);
    CHECK_EQ(-EIO, rc);
}

/* ── Test 6: Checked map validation ── */
TEST_FUNC(test_checked_map_validation)
{
    fb_resolution_runtime_reset();

    uint32_t *out_addr = NULL;

    /* 1. NULL out_addr */
    int rc = fb_x86_map_checked(0x20000000ULL, 2 * 1024 * 1024, NULL);
    CHECK_EQ(-EINVAL, rc);

    /* 2. Zero size */
    rc = fb_x86_map_checked(0x20000000ULL, 0, &out_addr);
    CHECK_EQ(-EINVAL, rc);

    /* 3. Unaligned phys (not 2MiB aligned) */
    rc = fb_x86_map_checked(0x20001000ULL, 2 * 1024 * 1024, &out_addr);
    CHECK_EQ(-EINVAL, rc);

    /* 4. Unaligned size (not 2MiB aligned) */
    rc = fb_x86_map_checked(0x20000000ULL, 4096, &out_addr);
    CHECK_EQ(-EINVAL, rc);

    /* 5. Address range overflow */
    rc = fb_x86_map_checked(UINT64_MAX - (2 * 1024 * 1024) + 1, 4 * 1024 * 1024, &out_addr);
    CHECK_EQ(-EINVAL, rc);
}

/* ── Test 7: Checked map allocation failure and rollback ── */
TEST_FUNC(test_checked_map_allocation_failure_and_rollback)
{
    fb_resolution_runtime_reset();

    struct fb_info info = make_default_info();
    uint64_t old_mapped_size = 4 * 1024 * 1024;
    fb_bootstrap_state(0xe0000000ULL, old_mapped_size, &info);
    fb_publish_initial_mapping((uint32_t *)0xffffa00000000000ULL, old_mapped_size);

    /* Configure allocation to fail on the 2nd table allocation */
    g_mock_alloc_table_fail_countdown = 1;

    uint32_t *out_addr = (uint32_t *)0x12345;
    int rc = fb_x86_map_checked(0xe0000000ULL, 16 * 1024 * 1024, &out_addr);
    CHECK_EQ(-ENOMEM, rc);
    CHECK_PTR_EQ(NULL, out_addr);

    /* Verify snapshot mapped_size was not enlarged */
    fb_snapshot_t snap;
    rc = fb_snapshot_read(&snap);
    CHECK_EQ(0, rc);
    CHECK_EQ(old_mapped_size, snap.mapped_size);
}

/* ── Test 8: Checked map corrupted phys verification failure ── */
TEST_FUNC(test_checked_map_corrupted_phys_and_rollback)
{
    fb_resolution_runtime_reset();

    struct fb_info info = make_default_info();
    uint64_t old_mapped_size = 4 * 1024 * 1024;
    fb_bootstrap_state(0xe0000000ULL, old_mapped_size, &info);
    fb_publish_initial_mapping((uint32_t *)0xffffa00000000000ULL, old_mapped_size);

    /* Configure verification to fail */
    g_mock_corrupt_pmd_phys = true;

    uint32_t *out_addr = (uint32_t *)0x12345;
    int rc = fb_x86_map_checked(0xe0000000ULL, 16 * 1024 * 1024, &out_addr);
    CHECK_EQ(-EIO, rc);
    CHECK_PTR_EQ(NULL, out_addr);

    /* Verify snapshot mapped_size was not enlarged */
    fb_snapshot_t snap;
    rc = fb_snapshot_read(&snap);
    CHECK_EQ(0, rc);
    CHECK_EQ(old_mapped_size, snap.mapped_size);
}

/* ── Test 9: Checked map success ── */
TEST_FUNC(test_checked_map_success)
{
    fb_resolution_runtime_reset();

    uint64_t phys_base = 0xe0000000ULL;
    uint64_t map_size = 16 * 1024 * 1024;
    uint32_t *out_addr = NULL;

    int rc = fb_x86_map_checked(phys_base, map_size, &out_addr);
    CHECK_EQ(0, rc);
    CHECK_PTR_EQ((void *)0xFFFF900000000000ULL, (void *)out_addr);

    /* TLB shootdown or flush must have been called */
    CHECK_TRUE(g_mock_tlb_shootdown_count > 0 || g_mock_flush_tlb_count > 0);
}

/* ── Test 10: Test hook set_generation ── */
TEST_FUNC(test_test_set_generation_hook)
{
    fb_resolution_runtime_reset();

    struct fb_info info = make_default_info();
    uint64_t gop_size = 1024 * 768 * 4;
    fb_bootstrap_state(0xe0000000ULL, gop_size, &info);
    fb_publish_initial_mapping((uint32_t *)0xffff80003f000000ULL, gop_size);

    /* Initial generation is 1 */
    fb_snapshot_t snap;
    int rc = fb_snapshot_read(&snap);
    CHECK_EQ(0, rc);
    CHECK_EQ(1, snap.state.generation);

    /* Use test hook to advance generation to 42 */
    fb_state__test_set_generation(42);

    rc = fb_snapshot_read(&snap);
    CHECK_EQ(0, rc);
    CHECK_EQ(42, snap.state.generation);

    /* Old generation 1 is now rejected with -ESTALE */
    fb_lease_t lease = {0};
    rc = fb_writer_begin(&lease, 1);
    CHECK_EQ(-ESTALE, rc);

    /* Generation 42 is accepted */
    rc = fb_writer_begin(&lease, 42);
    CHECK_EQ(0, rc);
    CHECK_TRUE(lease.held);
    fb_writer_end(&lease);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_bootstrap_before_first_writer),
    TEST_ENTRY(test_transition_drains_frame),
    TEST_ENTRY(test_drain_timeout),
    TEST_ENTRY(test_boot_probe_transition),
    TEST_ENTRY(test_failed_state),
    TEST_ENTRY(test_checked_map_validation),
    TEST_ENTRY(test_checked_map_allocation_failure_and_rollback),
    TEST_ENTRY(test_checked_map_corrupted_phys_and_rollback),
    TEST_ENTRY(test_checked_map_success),
    TEST_ENTRY(test_test_set_generation_hook),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    if (g_failed > 0) {
        printf("test_fb_state: %d failures\n", g_failed);
        return 1;
    }
    return 0;
}
