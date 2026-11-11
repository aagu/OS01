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
#include <driver/fb_cache.h>
#include <arch/x86_64/fb_map.h>
#include <arch/x86_64/pte.h>

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
    volatile bool transition_seen;  /* set once the in-progress drain is seen */
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

    /* Hold the lease until the transition is *inside* its drain.  Probing
     * fb_writer_begin() is how we detect that: while a transition is in
     * progress it returns -EAGAIN without taking a lease.  Releasing only
     * after that point guarantees fb_transition_begin() observed
     * active_writers > 0 at entry and took the positive drain-wait path
     * (wait, then succeed) instead of finding the writer already gone. */
    for (int spins = 0; spins < 5000; spins++) {
        fb_lease_t probe = {0};
        int prc = fb_writer_begin(&probe, 0);
        if (probe.held) {
            fb_writer_end(&probe);
        }
        if (prc == -EAGAIN) {
            w->transition_seen = true;
            break;
        }
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
        .transition_seen = false,
        .worker_rc = -1,
    };

    pthread_t th;
    pthread_create(&th, NULL, drain_writer_thread, &w_arg);

    /* Wait until writer thread acquired lease */
    pthread_barrier_wait(&b_start);
    CHECK_EQ(0, w_arg.worker_rc);

    /* The lease is still held here: the worker only releases it once it
     * observes the in-progress drain.  Assert active_writers > 0 at entry so
     * the positive drain-wait path below is genuinely exercised. */
    CHECK_TRUE(fb_active_writers_count() > 0);

    int rc = fb_transition_begin(false);
    CHECK_EQ(0, rc);

    /* The drain had to wait for the worker, which could only release after
     * the transition was already in progress. */
    CHECK_TRUE(w_arg.transition_seen);
    CHECK_EQ(0, (int)fb_active_writers_count());

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

/* ═══════════════════════════════════════════════════════════════════
 * B2 regression: independent page-table attribute assertions.
 *
 * The expectations below are RAW BIT PATTERNS written out by hand.  They
 * deliberately do NOT reuse the implementation's flag macros
 * (PAGE_KERNEL_PMD / PAGE_USER_PTE / ...): if the test derived its
 * expectation from the same expression the implementation uses, then
 * changing the implementation back to uncacheable would keep the test
 * green -- which is exactly the blind spot this regression exists to
 * close.
 *
 * Hand-derived x86-64 expectations for a 2MiB PMD leaf entry:
 *   bit 0  P   (present)   = 1
 *   bit 1  RW  (writable)  = 1
 *   bit 3  PWT             = 0 write-back / 1 uncacheable
 *   bit 4  PCD             = 0 write-back / 1 uncacheable
 *   bit 7  PS  (2MiB page) = 1
 *   bit 12 PAT             = 0
 *   bits 51:21 = physical address
 *   => write-back leaf = 0x83, uncacheable leaf = 0x9B
 *
 * Hand-derived 4KiB user PTE expectations:
 *   bit 0 P = 1, bit 1 RW = 1 (writable) / 0 (read-only), bit 2 U = 1
 *   => writable user PTE = 0x07, read-only user PTE = 0x05
 * ═══════════════════════════════════════════════════════════════════ */
#define EXPECT_PMD_PA_MASK      0x0000FFFFFFE00000ULL   /* bits 51:21 */
#define EXPECT_PMD_LEAF_WB     0x83ULL
#define EXPECT_PMD_LEAF_UC     0x9BULL
#define EXPECT_PTE_USER_W      0x07ULL
#define EXPECT_PTE_USER_RO     0x05ULL
#define EXPECT_CACHE_BITS_MASK 0x18ULL   /* PCD (bit 4) | PWT (bit 3) */

/* Independent walk of the mock page table: kernel_map -> PGD -> PUD -> PMD.
 * The index arithmetic is re-derived here (shifts 39 / 30 / 21, the
 * x86-64 architectural values) instead of calling into the mapping code
 * under test.  Returns 0 when the walk does not reach a leaf. */
static uint64_t mock_pmd_leaf(uint64_t va)
{
    uint64_t pgd_idx = (va >> 39) & 0x1ff;
    uint64_t pud_idx = (va >> 30) & 0x1ff;
    uint64_t pmd_idx = (va >> 21) & 0x1ff;

    uint64_t pgd_entry = kernel_map[pgd_idx];
    if (!(pgd_entry & 1)) return 0;
    uint64_t *pud = (uint64_t *)(pgd_entry & ~0xFFFULL);
    uint64_t pud_entry = pud[pud_idx];
    if (!(pud_entry & 1)) return 0;
    uint64_t *pmd = (uint64_t *)(pud_entry & ~0xFFFULL);
    return pmd[pmd_idx];
}

/* The permanent framebuffer mapping always starts here (VIRT_FRAMEBUFFER_OFFSET). */
#define TEST_FB_VA_BASE 0xFFFF900000000000ULL

/* ── Test 11: write-back policy installs write-back leaves ──────────── */
TEST_FUNC(test_fb_cache_policy_wb_leaf_bits)
{
    fb_resolution_runtime_reset();
    fb_cache_policy_set(FB_CACHE_POLICY_WRITEBACK);

    uint64_t phys = 0xe0000000ULL;
    uint64_t size = 16 * 1024 * 1024;   /* 8 x 2MiB pages */
    uint32_t *out_addr = NULL;

    int rc = fb_x86_map_checked(phys, size, &out_addr);
    CHECK_EQ(0, rc);

    /* Every leaf of the mapped range, walked independently. */
    for (size_t i = 0; i < 8; i++) {
        uint64_t leaf = mock_pmd_leaf(TEST_FB_VA_BASE + i * 0x200000ULL);
        uint64_t expect_pa = phys + i * 0x200000ULL;

        CHECK_EQ(expect_pa & EXPECT_PMD_PA_MASK, leaf & EXPECT_PMD_PA_MASK);
        CHECK_EQ(EXPECT_PMD_LEAF_WB, leaf & ~EXPECT_PMD_PA_MASK);
        CHECK_EQ(0, leaf & EXPECT_CACHE_BITS_MASK);   /* PCD=0, PWT=0 */
        CHECK_EQ(0, leaf & 0x1000);                  /* PAT=0 */
        CHECK_EQ(0x01, leaf & 0x01);                    /* present */
        CHECK_EQ(0x02, leaf & 0x02);                    /* writable */
        CHECK_EQ(0x80, leaf & 0x80);                    /* 2MiB page */
        CHECK_EQ(0, leaf & 0x04);                    /* supervisor only */
    }

    fb_cache_policy_set(FB_CACHE_POLICY_UNCACHEABLE);
}

/* ── Test 12: uncacheable policy installs uncacheable leaves ───────── */
TEST_FUNC(test_fb_cache_policy_uc_leaf_bits)
{
    fb_resolution_runtime_reset();
    fb_cache_policy_set(FB_CACHE_POLICY_UNCACHEABLE);

    uint64_t phys = 0xe0000000ULL;
    uint64_t size = 16 * 1024 * 1024;
    uint32_t *out_addr = NULL;

    int rc = fb_x86_map_checked(phys, size, &out_addr);
    CHECK_EQ(0, rc);

    for (size_t i = 0; i < 8; i++) {
        uint64_t leaf = mock_pmd_leaf(TEST_FB_VA_BASE + i * 0x200000ULL);
        uint64_t expect_pa = phys + i * 0x200000ULL;

        CHECK_EQ(expect_pa & EXPECT_PMD_PA_MASK, leaf & EXPECT_PMD_PA_MASK);
        CHECK_EQ(EXPECT_PMD_LEAF_UC, leaf & ~EXPECT_PMD_PA_MASK);
        CHECK_EQ(EXPECT_CACHE_BITS_MASK, leaf & EXPECT_CACHE_BITS_MASK); /* UC */
    }

    fb_cache_policy_set(FB_CACHE_POLICY_UNCACHEABLE);
}

/* ── Test 13: the /dev/fb alias carries the same memory type ─────────
 *
 * The user alias maps the SAME physical pages as the kernel mapping, so
 * it must not be uncacheable while the kernel mapping is write-back
 * (x86 forbids memory-type aliasing).  The expectation is again a hand
 * written bit pattern, not the implementation's expression. */
TEST_FUNC(test_fb_cache_policy_alias_matches_kernel_mapping)
{
    fb_resolution_runtime_reset();
    fb_cache_policy_set(FB_CACHE_POLICY_WRITEBACK);

    uint64_t phys = 0xe0000000ULL;
    uint32_t *out_addr = NULL;
    int rc = fb_x86_map_checked(phys, 8 * 1024 * 1024, &out_addr);
    CHECK_EQ(0, rc);

    uint64_t leaf = mock_pmd_leaf(TEST_FB_VA_BASE);
    CHECK_EQ(EXPECT_PMD_LEAF_WB, leaf & ~EXPECT_PMD_PA_MASK);

    /* The flags fb_mmap() installs for the user alias. */
    uint64_t user_w  = fb_x86_user_pte_flags(FB_CACHE_POLICY_WRITEBACK, true);
    uint64_t user_ro = fb_x86_user_pte_flags(FB_CACHE_POLICY_WRITEBACK, false);

    CHECK_EQ(EXPECT_PTE_USER_W,  user_w  & ~EXPECT_CACHE_BITS_MASK);
    CHECK_EQ(EXPECT_PTE_USER_RO, user_ro & ~EXPECT_CACHE_BITS_MASK);
    CHECK_EQ(0, user_w  & EXPECT_CACHE_BITS_MASK);
    CHECK_EQ(0, user_ro & EXPECT_CACHE_BITS_MASK);

    /* Alias consistency: the alias and the kernel mapping must agree on
     * the memory type of the shared physical pages. */
    CHECK_EQ(leaf & EXPECT_CACHE_BITS_MASK, user_w & EXPECT_CACHE_BITS_MASK);
    CHECK_EQ(leaf & EXPECT_CACHE_BITS_MASK, user_ro & EXPECT_CACHE_BITS_MASK);

    /* And with the uncacheable policy the alias must be uncacheable too. */
    uint64_t uc_w  = fb_x86_user_pte_flags(FB_CACHE_POLICY_UNCACHEABLE, true);
    uint64_t uc_ro = fb_x86_user_pte_flags(FB_CACHE_POLICY_UNCACHEABLE, false);
    CHECK_EQ(EXPECT_CACHE_BITS_MASK, uc_w  & EXPECT_CACHE_BITS_MASK);
    CHECK_EQ(EXPECT_CACHE_BITS_MASK, uc_ro & EXPECT_CACHE_BITS_MASK);
    CHECK_EQ(EXPECT_PTE_USER_W,  uc_w  & ~EXPECT_CACHE_BITS_MASK);
    CHECK_EQ(EXPECT_PTE_USER_RO, uc_ro & ~EXPECT_CACHE_BITS_MASK);

    fb_cache_policy_set(FB_CACHE_POLICY_UNCACHEABLE);
}

/* ── Test 14: a failed mapping restores the previous attributes ────── */
TEST_FUNC(test_fb_cache_policy_rollback_preserves_attributes)
{
    fb_resolution_runtime_reset();
    fb_cache_policy_set(FB_CACHE_POLICY_WRITEBACK);

    uint64_t phys = 0xe0000000ULL;
    uint64_t size = 8 * 1024 * 1024;
    uint32_t *out_addr = NULL;

    int rc = fb_x86_map_checked(phys, size, &out_addr);
    CHECK_EQ(0, rc);

    uint64_t leaves[4];
    for (size_t i = 0; i < 4; i++) {
        leaves[i] = mock_pmd_leaf(TEST_FB_VA_BASE + i * 0x200000ULL);
        CHECK_EQ(EXPECT_PMD_LEAF_WB, leaves[i] & ~EXPECT_PMD_PA_MASK);
    }

    /* Re-map the same range with a corrupted physical address: the
     * verification must fail and the rollback must restore the previous
     * entries bit for bit -- cache attributes included. */
    g_mock_corrupt_pmd_phys = true;
    rc = fb_x86_map_checked(phys, size, &out_addr);
    CHECK_EQ(-EIO, rc);
    CHECK_PTR_EQ(NULL, out_addr);

    for (size_t i = 0; i < 4; i++) {
        uint64_t leaf = mock_pmd_leaf(TEST_FB_VA_BASE + i * 0x200000ULL);
        CHECK_EQ(leaves[i], leaf);
        CHECK_EQ(EXPECT_PMD_LEAF_WB, leaf & ~EXPECT_PMD_PA_MASK);
    }

    fb_cache_policy_set(FB_CACHE_POLICY_UNCACHEABLE);
}

/* ── Test 15: the early boot alias is revoked ──────────────────────── */
TEST_FUNC(test_fb_early_alias_revocation)
{
    /* Stand-in for the boot PUD[0] PMD table that holds the early alias. */
    uint64_t table[512];
    memset(table, 0, sizeof(table));

    const uint64_t early_va = 0xFFFF80003F000000ULL;  /* VIRT_FRAMEBUFFER_EARLY */
    const uint64_t size = 4 * 1024 * 1024;            /* 2 x 2MiB pages */
    const uint64_t leaf_uc = 0xe0000000ULL | EXPECT_PMD_LEAF_UC;

    size_t idx0 = (size_t)((early_va) >> 21) & 0x1ff;
    size_t idx1 = (size_t)((early_va + 2 * 1024 * 1024) >> 21) & 0x1ff;
    size_t nbr  = (idx1 + 1) & 0x1ff;

    table[idx0] = leaf_uc;
    table[idx1] = leaf_uc;
    table[nbr]  = 0xdeadbeefULL;   /* must survive untouched */

    int revoked = fb_x86_revoke_early_alias(table, early_va, size);
    CHECK_EQ(2, revoked);
    CHECK_EQ(0, table[idx0]);
    CHECK_EQ(0, table[idx1]);
    CHECK_EQ(0xdeadbeefULL, table[nbr]);

    /* Idempotent: nothing left to clear. */
    CHECK_EQ(0, fb_x86_revoke_early_alias(table, early_va, size));
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
    TEST_ENTRY(test_fb_cache_policy_wb_leaf_bits),
    TEST_ENTRY(test_fb_cache_policy_uc_leaf_bits),
    TEST_ENTRY(test_fb_cache_policy_alias_matches_kernel_mapping),
    TEST_ENTRY(test_fb_cache_policy_rollback_preserves_attributes),
    TEST_ENTRY(test_fb_early_alias_revocation),
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
