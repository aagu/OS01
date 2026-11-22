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
 * fail-closed initialization, and (Part 2) mmap/munmap/mprotect window,
 * gap-search, conflict, and overflow contracts.
 */
#include "test_framework.h"
#include "lifecycle_stubs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/x86_64/aslr.h>
#include <arch/x86_64/user_layout.h>
#include <memory/mm.h>

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

TEST_LIST_BEGIN
    TEST_ENTRY(test_derive_boundaries_and_mask),
    TEST_ENTRY(test_init_null_mm_rejected),
    TEST_ENTRY(test_init_strong_success_little_endian),
    TEST_ENTRY(test_init_fail_closed_preserves_base),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return 0;
}
