/* @os01-test
 * id: test_x86_aslr_mmap
 * layer: host
 * subsystem: memory
 * timing: strict
 * tier: pr
 * isolation: none
 * scenarios: [memory.aslr.x86_mmap]
 */
/*
 * Host unit tests for x86_64 user mmap_base ASLR derivation, STRONG-only
 * fail-closed initialization, and mmap/munmap/mprotect window, gap-search,
 * conflict, and overflow contracts against sparse-PTE lifecycle stubs.
 */
#include "test_framework.h"
#include "lifecycle_stubs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/x86_64/aslr.h>
#include <arch/x86_64/mmu.h>
#include <arch/x86_64/user_layout.h>
#include <arch/x86_64/vma.h>
#include <fs/vfs.h>
#include <memory/mm.h>
#include <memory/vm_area.h>
#include <memory/vm_device.h>
#include <memory/vma.h>

#define PROT_RW (PROT_READ | PROT_WRITE)

/* ── Controllable STRONG entropy stub ── */

static int g_rng_calls = 0;
static bool g_rng_return_value = true;
static uint8_t g_rng_bytes[32];

bool kernel_random_get_strong(uint8_t out[32])
{
    g_rng_calls++;
    if (out)
        memcpy(out, g_rng_bytes, 32);
    return g_rng_return_value;
}

static void reset_rng_stub(bool ret_val, uint64_t first_u64_le, uint8_t fill_rest)
{
    g_rng_calls = 0;
    g_rng_return_value = ret_val;
    memset(g_rng_bytes, fill_rest, sizeof(g_rng_bytes));
    for (int i = 0; i < 8; i++)
        g_rng_bytes[i] = (uint8_t)((first_u64_le >> (i * 8)) & 0xFFU);
}

/* ── Sparse-PTE mm fixture ── */

static mm_t fixture_mm;
static uint64_t dummy_pgd[512] __attribute__((aligned(4096)));

static void reset_mm_fixture(void)
{
    lifecycle_stubs_reset();
    memset(&fixture_mm, 0, sizeof(fixture_mm));
    memset(dummy_pgd, 0, sizeof(dummy_pgd));
    list_init(&fixture_mm.vma_list);
    spin_init(&fixture_mm.lock);
    fixture_mm.pgdir = dummy_pgd;
    fixture_mm.start_code = X86_USER_MIN_ADDR;
    fixture_mm.end_code = 0x500000UL;
    fixture_mm.mmap_base = 0x40000000UL;
    assert_eq(0, mm_init_user_heap(&fixture_mm, fixture_mm.end_code));
    current->mm = &fixture_mm;
    current->addr_limit = X86_USER_ADDR_LIMIT;
    current->files = lifecycle_files_table();
}

static void teardown_mm_fixture(void)
{
    vma_free_all(&fixture_mm);
}

static void assert_vmas_non_overlapping(mm_t *mm)
{
    uint64_t prev_end = 0;
    for (list_t *pos = mm->vma_list.next; pos != &mm->vma_list; pos = pos->next) {
        vma_t *v = container_of(pos, vma_t, list);
        assert_true(v->vm_start <= v->vm_end);
        assert_true(v->vm_start >= prev_end);
        prev_end = v->vm_end;
    }
}

/* ── Fake device node for VMA_IO tests ── */

static int g_dev_mmap_calls = 0;
static uint64_t g_dev_first_pa = 0x30000000ULL;

static int test_dev_mmap_cb(struct vfs_node *node, struct vma *vma,
                            vfs_mmap_commit_fn commit, void *ctx)
{
    (void)node;
    (void)vma;
    g_dev_mmap_calls++;
    return commit(ctx, g_dev_first_pa, VM_DEVICE_CACHE_UC);
}

static const vfs_file_ops_t test_dev_ops = { .mmap = test_dev_mmap_cb };
static vfs_node_t test_dev_node;
static file_t test_dev_file;

/* ── Part 1: Pure derivation & STRONG-only init tests ── */

TEST_FUNC(test_derive_boundaries_and_mask)
{
    uint64_t b0 = x86_aslr_derive_mmap_base(0);
    uint64_t b1 = x86_aslr_derive_mmap_base(1);
    uint64_t bmax_slot = x86_aslr_derive_mmap_base((1ULL << 28) - 1ULL);
    uint64_t bwrap = x86_aslr_derive_mmap_base(1ULL << 28);
    uint64_t ball = x86_aslr_derive_mmap_base(UINT64_MAX);

    assert_eq(0x0000400000000000ULL, b0);
    assert_eq(0x0000400000001000ULL, b1);
    assert_eq(0x000040fffffff000ULL, bmax_slot);
    assert_eq(0x0000400000000000ULL, bwrap);
    assert_eq(0x000040fffffff000ULL, ball);

    uint64_t samples[5] = { b0, b1, bmax_slot, bwrap, ball };
    for (int i = 0; i < 5; i++) {
        assert_eq(0ULL, samples[i] & 0xFFFULL);
        assert_true(samples[i] >= X86_USER_MMAP_RND_BASE);
        assert_true(samples[i] < X86_USER_MMAP_RND_END);
    }
}

TEST_FUNC(test_init_null_mm_rejected)
{
    reset_rng_stub(true, 0x123456789abcdef0ULL, 0xAA);
    int rc = x86_aslr_init_mmap_base(NULL);
    assert_eq(-EINVAL, rc);
    assert_eq(0, g_rng_calls);
}

TEST_FUNC(test_init_strong_success_little_endian)
{
    mm_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.mmap_base = 0x40000000ULL;

    reset_rng_stub(true, 0xdeadbeef01234567ULL, 0x5A);
    int rc = x86_aslr_init_mmap_base(&mm);
    assert_eq(0, rc);
    assert_eq(1, g_rng_calls);
    assert_eq(x86_aslr_derive_mmap_base(0xdeadbeef01234567ULL), mm.mmap_base);

    uint64_t first = mm.mmap_base;
    reset_rng_stub(true, 0xdeadbeef01234568ULL, 0x5A);
    rc = x86_aslr_init_mmap_base(&mm);
    assert_eq(0, rc);
    assert_eq(1, g_rng_calls);
    assert_true(mm.mmap_base != first);
}

TEST_FUNC(test_init_fail_closed_preserves_base)
{
    mm_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.mmap_base = 0x40000000ULL;

    /* Simulate WEAK hardware fallback writing dirty bytes before returning false. */
    reset_rng_stub(false, 0x0123456789abcdefULL, 0xCC);
    int rc = x86_aslr_init_mmap_base(&mm);
    assert_eq(-EAGAIN, rc);
    assert_eq(1, g_rng_calls);
    assert_eq(0x40000000ULL, mm.mmap_base);
}

/* ── Part 2: mmap / munmap / mprotect ASLR & window hardening tests ── */

TEST_FUNC(test_mmap_high_randomized_base_sequential)
{
    reset_mm_fixture();
    reset_rng_stub(true, 0x00123456789abcdeULL, 0x33);
    assert_eq(0, x86_aslr_init_mmap_base(&fixture_mm));
    uint64_t b = fixture_mm.mmap_base;
    assert_true(b >= X86_USER_MMAP_RND_BASE && b < X86_USER_MMAP_RND_END);

    int64_t a1 = do_mmap(0, 0x1000, PROT_RW, MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0);
    int64_t a2 = do_mmap(0, 0x1000, PROT_RW, MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0);
    assert_eq((int64_t)b, a1);
    assert_eq((int64_t)(b + 0x1000ULL), a2);
    assert_vmas_non_overlapping(&fixture_mm);
    teardown_mm_fixture();
}

TEST_FUNC(test_mmap_gap_conflicts_and_insufficient_front_gap)
{
    /* Subcase 1: MAP_FIXED occupies [B, B + 0x2000) -> non-fixed 0x1000 gets B + 0x2000 */
    reset_mm_fixture();
    reset_rng_stub(true, 0x0000000000123456ULL, 0x11);
    assert_eq(0, x86_aslr_init_mmap_base(&fixture_mm));
    uint64_t b = fixture_mm.mmap_base;

    assert_eq((int64_t)b, do_mmap(b, 0x2000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
    assert_eq((int64_t)(b + 0x2000ULL), do_mmap(0, 0x1000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_vmas_non_overlapping(&fixture_mm);
    teardown_mm_fixture();

    /* Subcase 2: MAP_FIXED straddles [B - 0x1000, B + 0x1000) -> non-fixed 0x1000 gets B + 0x1000 */
    reset_mm_fixture();
    fixture_mm.mmap_base = b;
    assert_eq((int64_t)(b - 0x1000ULL), do_mmap(b - 0x1000ULL, 0x2000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
    assert_eq((int64_t)(b + 0x1000ULL), do_mmap(0, 0x1000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_vmas_non_overlapping(&fixture_mm);
    teardown_mm_fixture();

    /* Subcase 3: MAP_FIXED at [B + 0x1000, B + 0x3000) (1-page front gap):
     * non-fixed 0x2000 MUST skip VMA to B + 0x3000; then 0x1000 fills [B, B + 0x1000). */
    reset_mm_fixture();
    fixture_mm.mmap_base = b;
    assert_eq((int64_t)(b + 0x1000ULL), do_mmap(b + 0x1000ULL, 0x2000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
    assert_eq((int64_t)(b + 0x3000ULL), do_mmap(0, 0x2000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_vmas_non_overlapping(&fixture_mm);
    assert_eq((int64_t)b, do_mmap(0, 0x1000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_vmas_non_overlapping(&fixture_mm);
    teardown_mm_fixture();

    /* Subcase 4: low MAP_FIXED at [0x40000000, 0x40001000) leaves high B untouched */
    reset_mm_fixture();
    fixture_mm.mmap_base = b;
    assert_eq((int64_t)0x40000000ULL, do_mmap(0x40000000ULL, 0x1000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
    assert_eq((int64_t)b, do_mmap(0, 0x1000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_vmas_non_overlapping(&fixture_mm);
    teardown_mm_fixture();
}

TEST_FUNC(test_mmap_fixed_zero_and_canonical_ceiling)
{
    reset_mm_fixture();

    /* Reject MAP_FIXED(0) zero-page mapping */
    assert_eq(-EINVAL, do_mmap(0, 0x1000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));

    /* Top valid canonical page [0x00007FFFFFFFE000, 0x00007FFFFFFFF000) succeeds */
    uint64_t top_valid = 0x00007FFFFFFFE000ULL;
    assert_eq((int64_t)top_valid, do_mmap(top_valid, 0x1000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
    assert_eq(0, do_mprotect(top_valid, 0x1000, PROT_READ));
    assert_eq(0, do_munmap(top_valid, 0x1000));

    /* Even if addr_limit is raised to kernel/UINT64_MAX, top guard & non-canonical are rejected */
    uint64_t limits[3] = {
        X86_USER_ADDR_LIMIT,
        0xffff800000000000ULL,
        UINT64_MAX
    };
    uint64_t top_guard = 0x00007FFFFFFFF000ULL;
    uint64_t non_canon = 0x0000800000000000ULL;

    for (int i = 0; i < 3; i++) {
        current->addr_limit = limits[i];
        int flushes_before = lifecycle_state.flush_count;
        int frees_before = lifecycle_state.total_frees;

        assert_eq(-ENOMEM, do_mmap(top_guard, 0x1000, PROT_RW,
                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
        assert_eq(-ENOMEM, do_mmap(top_valid, 0x2000, PROT_RW,
                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
        assert_eq(-ENOMEM, do_mmap(non_canon, 0x1000, PROT_RW,
                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));

        assert_eq(-EINVAL, do_munmap(top_guard, 0x1000));
        assert_eq(-EINVAL, do_munmap(non_canon, 0x1000));
        assert_eq(-EINVAL, do_mprotect(top_guard, 0x1000, PROT_READ));
        assert_eq(-EINVAL, do_mprotect(non_canon, 0x1000, PROT_READ));

        assert_eq(flushes_before, lifecycle_state.flush_count);
        assert_eq(frees_before, lifecycle_state.total_frees);
    }

    teardown_mm_fixture();
}

TEST_FUNC(test_mmap_exhaustion_no_wrap_and_overflows)
{
    reset_mm_fixture();
    fixture_mm.mmap_base = X86_USER_MMAP_MAX_ADDR - 0x2000ULL;

    assert_eq((int64_t)(X86_USER_MMAP_MAX_ADDR - 0x1000ULL),
              do_mmap(X86_USER_MMAP_MAX_ADDR - 0x1000ULL, 0x1000, PROT_RW,
                      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));

    /* Window [max - 0x2000, max) has only 1 free page; 0x2000 must NOT wrap below mmap_base */
    assert_eq(-ENOMEM, do_mmap(0, 0x2000, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_eq((int64_t)(X86_USER_MMAP_MAX_ADDR - 0x2000ULL),
              do_mmap(0, 0x1000, PROT_RW,
                      MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_vmas_non_overlapping(&fixture_mm);

    /* Overflow and oversized length guards */
    assert_eq(-EINVAL, do_mmap(0, UINT64_MAX, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_eq(-EINVAL, do_mmap(0, UINT64_MAX - 0x100ULL, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_eq(-ENOMEM, do_mmap(0, 0x0000400000000000ULL, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_eq(-EINVAL, do_mmap(0x40000000ULL, 0xfffffffff0000000ULL, PROT_RW,
              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));

    teardown_mm_fixture();
}

TEST_FUNC(test_mmap_bad_low_base_clamps_without_overlap)
{
    uint64_t bad_bases[3] = { 0ULL, 0x500000ULL, X86_USER_STACK_BASE };

    for (int i = 0; i < 3; i++) {
        reset_mm_fixture();
        assert_eq((int64_t)X86_USER_STACK_END,
                  do_mmap(X86_USER_STACK_END, 0x2000, PROT_RW,
                          MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));
        fixture_mm.mmap_base = bad_bases[i];

        int64_t got = do_mmap(0, 0x1000, PROT_RW,
                              MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0);
        assert_eq((int64_t)(X86_USER_STACK_END + 0x2000ULL), got);
        assert_vmas_non_overlapping(&fixture_mm);
        teardown_mm_fixture();
    }

    /* Unaligned mmap_base rounds up without overlap */
    reset_mm_fixture();
    uint64_t b = 0x0000400010000000ULL;
    fixture_mm.mmap_base = b + 0x123ULL;
    assert_eq((int64_t)(b + 0x1000ULL),
              do_mmap(0, 0x1000, PROT_RW, MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    teardown_mm_fixture();

    /* Extreme overflow mmap_base near UINT64_MAX returns -ENOMEM */
    reset_mm_fixture();
    fixture_mm.mmap_base = UINT64_MAX - 0x100ULL;
    assert_eq(-ENOMEM,
              do_mmap(0, 0x1000, PROT_RW, MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    teardown_mm_fixture();
}

TEST_FUNC(test_mmap_advisory_hint_does_not_bypass_aslr)
{
    reset_mm_fixture();
    reset_rng_stub(true, 0x0000000000abcdefULL, 0x22);
    assert_eq(0, x86_aslr_init_mmap_base(&fixture_mm));
    uint64_t b = fixture_mm.mmap_base;

    assert_eq((int64_t)b,
              do_mmap(0x40000000ULL, 0x1000, PROT_RW,
                      MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_eq((int64_t)(b + 0x1000ULL),
              do_mmap(0x500000ULL, 0x1000, PROT_RW,
                      MAP_ANONYMOUS | MAP_PRIVATE, (uint64_t)-1, 0));
    assert_vmas_non_overlapping(&fixture_mm);
    teardown_mm_fixture();
}

TEST_FUNC(test_device_mmap_high_base_and_exhaustion_refcount)
{
    reset_mm_fixture();
    memset(&test_dev_node, 0, sizeof(test_dev_node));
    test_dev_node.f_ops = &test_dev_ops;
    memset(&test_dev_file, 0, sizeof(test_dev_file));
    test_dev_file.type = FD_VFS;
    test_dev_file.node = &test_dev_node;
    test_dev_file.refcount = 1;
    assert_eq(0, lifecycle_files_install(3, &test_dev_file));

    reset_rng_stub(true, 0x0000000000fedcbaULL, 0x44);
    assert_eq(0, x86_aslr_init_mmap_base(&fixture_mm));
    uint64_t b = fixture_mm.mmap_base;

    g_dev_mmap_calls = 0;
    int64_t got = do_mmap(0, 0x1000, PROT_RW, MAP_SHARED, 3, 0);
    assert_eq((int64_t)b, got);
    assert_eq(1, g_dev_mmap_calls);
    assert_eq(lifecycle_state.vfs_get_calls, lifecycle_state.vfs_put_calls);
    assert_eq(lifecycle_state.files_get_calls, lifecycle_state.files_put_calls);
    teardown_mm_fixture();

    /* Exhausted window: device mmap returns -ENOMEM without calling _dev_mmap and balances refs */
    reset_mm_fixture();
    assert_eq(0, lifecycle_files_install(3, &test_dev_file));
    fixture_mm.mmap_base = X86_USER_MMAP_MAX_ADDR - 0x1000ULL;
    assert_eq((int64_t)(X86_USER_MMAP_MAX_ADDR - 0x1000ULL),
              do_mmap(X86_USER_MMAP_MAX_ADDR - 0x1000ULL, 0x1000, PROT_RW,
                      MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, (uint64_t)-1, 0));

    g_dev_mmap_calls = 0;
    assert_eq(-ENOMEM, do_mmap(0, 0x1000, PROT_RW, MAP_SHARED, 3, 0));
    assert_eq(0, g_dev_mmap_calls);
    assert_eq(lifecycle_state.vfs_get_calls, lifecycle_state.vfs_put_calls);
    assert_eq(lifecycle_state.files_get_calls, lifecycle_state.files_put_calls);
    teardown_mm_fixture();
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_derive_boundaries_and_mask),
    TEST_ENTRY(test_init_null_mm_rejected),
    TEST_ENTRY(test_init_strong_success_little_endian),
    TEST_ENTRY(test_init_fail_closed_preserves_base),
    TEST_ENTRY(test_mmap_high_randomized_base_sequential),
    TEST_ENTRY(test_mmap_gap_conflicts_and_insufficient_front_gap),
    TEST_ENTRY(test_mmap_fixed_zero_and_canonical_ceiling),
    TEST_ENTRY(test_mmap_exhaustion_no_wrap_and_overflows),
    TEST_ENTRY(test_mmap_bad_low_base_clamps_without_overlap),
    TEST_ENTRY(test_mmap_advisory_hint_does_not_bypass_aslr),
    TEST_ENTRY(test_device_mmap_high_base_and_exhaustion_refcount),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return 0;
}
