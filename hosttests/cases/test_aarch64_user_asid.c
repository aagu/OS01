/* @os01-test
 * id: test_aarch64_user_asid
 * layer: host
 * subsystem: memory
 * timing: strict
 * tier: pr
 * link: aarch64-user-asid
 * scenarios: [memory.asid.pool_reclaim]
 */
/* Host unit test for the AArch64 8-bit ASID pool and TLBI reclaim contract:
 *   1. Allocate 255 non-zero unique tags (1..255); 256th allocation returns 0.
 *   2. Release one non-zero tag; inside the TLBI completion callback, the slot
 *      must STILL be occupied so a concurrent allocation returns 0.
 *   3. After aarch64_asid_release() returns, the next allocation returns the
 *      same tag.
 *   4. Releasing tag 0 is a no-op (does not invoke TLBI or allocate tag 0).
 */
#include "test_framework.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/aarch64/user_asid.h>

static bool g_in_tlbi_cb;
static int g_tlbi_calls;
static uint8_t g_tlbi_last_asid;
static uint8_t g_alloc_during_tlbi;

/* Strong override of the weak TLBI hook in user_asid.c. */
void aarch64_asid_tlbi(uint8_t asid)
{
    g_tlbi_calls++;
    g_tlbi_last_asid = asid;
    if (g_in_tlbi_cb)
        g_alloc_during_tlbi = aarch64_asid_alloc();
}

TEST_FUNC(test_asid_pool_exhaust_and_reclaim_after_tlbi)
{
    bool seen[256];
    uint8_t tags[255];
    memset(seen, 0, sizeof(seen));

    for (int i = 0; i < 255; i++) {
        uint8_t tag = aarch64_asid_alloc();
        assert_true(tag >= 1 && tag <= 255);
        assert_false(seen[tag]);
        seen[tag] = true;
        tags[i] = tag;
    }

    /* 256th allocation must fall back to 0. */
    assert_eq(0, (int)aarch64_asid_alloc());

    /* Releasing tag 0 must be a no-op. */
    g_tlbi_calls = 0;
    g_in_tlbi_cb = false;
    aarch64_asid_release(0);
    assert_eq(0, g_tlbi_calls);
    assert_eq(0, (int)aarch64_asid_alloc());

    /* Release one non-zero tag; inside TLBI callback, alloc must still return 0. */
    uint8_t target = tags[42];
    g_tlbi_calls = 0;
    g_tlbi_last_asid = 0;
    g_alloc_during_tlbi = 0xff;
    g_in_tlbi_cb = true;
    aarch64_asid_release(target);
    g_in_tlbi_cb = false;

    assert_eq(1, g_tlbi_calls);
    assert_eq((int)target, (int)g_tlbi_last_asid);
    assert_eq(0, (int)g_alloc_during_tlbi);

    /* After TLBI completion, allocating must return the same tag. */
    uint8_t reallocated = aarch64_asid_alloc();
    assert_eq((int)target, (int)reallocated);

    /* Clean up all 255 tags. */
    for (int i = 0; i < 255; i++) {
        aarch64_asid_release(tags[i]);
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_asid_pool_exhaust_and_reclaim_after_tlbi),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
