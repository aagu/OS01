/* @os01-test
 * id: test_aarch64_aslr
 * layer: host
 * subsystem: memory
 * timing: strict
 * tier: pr
 * link: aarch64-aslr
 * scenarios: [memory.aarch64.aslr.default]
 */
/*
 * Host unit test for kernel/arch/aarch64/memory/aslr.c.
 * Verifies mmap_base & start_stack derivation bounds, 4 KiB alignment,
 * STRONG-only fail-closed behavior (-EAGAIN preserving existing values),
 * and NULL argument rejection (-EINVAL).
 */

#include "test_framework.h"
#include <arch/aarch64/aslr.h>
#include <arch/aarch64/mmu.h>
#include <random/random.h>
#include <sched/task.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Fake kernel_random_get_strong() controlled by the test. */
static bool g_strong_available = true;
static uint64_t g_entropy_seq = 0x123456789ABCDEF0ULL;

bool kernel_random_get_strong(uint8_t out[32])
{
    if (!g_strong_available) {
        /* Simulate WEAK fallback polluting the buffer before returning false. */
        memset(out, 0xA5, 32);
        return false;
    }
    uint64_t v = g_entropy_seq;
    g_entropy_seq = g_entropy_seq * 6364136223846793005ULL + 1442695040888963407ULL;
    for (int i = 0; i < 4; i++) {
        uint64_t w = v ^ ((uint64_t)i * 0x9E3779B97F4A7C15ULL);
        memcpy(out + i * 8, &w, 8);
    }
    return true;
}

static void test_aarch64_aslr_derive_bounds(void)
{
    uint64_t mmap_lo = aarch64_aslr_derive_mmap_base(0);
    uint64_t mmap_hi = aarch64_aslr_derive_mmap_base(UINT64_MAX);
    assert_eq(AARCH64_USER_MMAP_RND_BASE, mmap_lo);
    assert_eq(AARCH64_USER_MMAP_RND_END - 0x1000ULL, mmap_hi);
    assert_eq(0ULL, mmap_lo & 0xFFFULL);
    assert_eq(0ULL, mmap_hi & 0xFFFULL);
    assert_true(mmap_hi < AARCH64_USER_LIMIT);

    uint64_t stk_lo = aarch64_aslr_derive_stack_base(0);
    uint64_t stk_hi = aarch64_aslr_derive_stack_base(UINT64_MAX);
    assert_eq(AARCH64_USER_STACK_RND_BASE, stk_lo);
    assert_eq(AARCH64_USER_STACK_RND_END - 0x1000ULL, stk_hi);
    assert_eq(0ULL, stk_lo & 0xFFFULL);
    assert_eq(0ULL, stk_hi & 0xFFFULL);
    assert_true(stk_lo > AARCH64_USER_STACK_GUARD);
    assert_true(stk_hi + (uint64_t)AARCH64_PRODUCT_STACK_PAGES * 0x1000ULL <= 0x40000000ULL);
}

static void test_aarch64_aslr_null_rejected(void)
{
    assert_eq(-EINVAL, aarch64_aslr_init_mmap_base(NULL));
    assert_eq(-EINVAL, aarch64_aslr_init_stack_base(NULL));
}

static void test_aarch64_aslr_fail_closed_without_strong(void)
{
    mm_t mm;
    memset(&mm, 0, sizeof(mm));
    mm.mmap_base = 0x40000000ULL;
    mm.start_stack = AARCH64_USER_STACK_BASE;

    g_strong_available = false;
    assert_eq(-EAGAIN, aarch64_aslr_init_mmap_base(&mm));
    assert_eq(0x40000000ULL, mm.mmap_base);

    assert_eq(-EAGAIN, aarch64_aslr_init_stack_base(&mm));
    assert_eq(AARCH64_USER_STACK_BASE, mm.start_stack);
}

static void test_aarch64_aslr_strong_randomizes_both(void)
{
    g_strong_available = true;
    uint64_t mmaps[8];
    uint64_t stacks[8];
    int unique_mmap = 0;
    int unique_stack = 0;

    for (int i = 0; i < 8; i++) {
        mm_t mm;
        memset(&mm, 0, sizeof(mm));
        mm.mmap_base = 0x40000000ULL;
        mm.start_stack = AARCH64_USER_STACK_BASE;

        assert_eq(0, aarch64_aslr_init_mmap_base(&mm));
        assert_eq(0, aarch64_aslr_init_stack_base(&mm));

        assert_eq(0ULL, mm.mmap_base & 0xFFFULL);
        assert_true(mm.mmap_base >= AARCH64_USER_MMAP_RND_BASE);
        assert_true(mm.mmap_base < AARCH64_USER_MMAP_RND_END);

        assert_eq(0ULL, mm.start_stack & 0xFFFULL);
        assert_true(mm.start_stack >= AARCH64_USER_STACK_RND_BASE);
        assert_true(mm.start_stack < AARCH64_USER_STACK_RND_END);

        mmaps[i] = mm.mmap_base;
        stacks[i] = mm.start_stack;

        bool seen_m = false;
        bool seen_s = false;
        for (int j = 0; j < i; j++) {
            if (mmaps[j] == mmaps[i])
                seen_m = true;
            if (stacks[j] == stacks[i])
                seen_s = true;
        }
        if (!seen_m)
            unique_mmap++;
        if (!seen_s)
            unique_stack++;
    }

    assert_true(unique_mmap >= 7);
    assert_true(unique_stack >= 7);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_aarch64_aslr_derive_bounds),
    TEST_ENTRY(test_aarch64_aslr_null_rejected),
    TEST_ENTRY(test_aarch64_aslr_fail_closed_without_strong),
    TEST_ENTRY(test_aarch64_aslr_strong_randomizes_both),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return (__test_stats.failed == 0) ? 0 : 1;
}
