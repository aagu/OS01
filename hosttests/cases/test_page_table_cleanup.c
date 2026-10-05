#include "page_table_test_runner.h"
#include <errno.h>
#include <string.h>
#include <arch/aarch64/page_table_selftest.h>
#include <arch/aarch64/early_arena.h>
static uint64_t tables[3][512] __attribute__((aligned(4096)));
static uint64_t data[512] __attribute__((aligned(4096)));
static uint64_t root[512], freed[4];
static unsigned nfree;
static bool flushed;
static struct aarch64_early_arena arena = {.base_pa = 0x40200000, .end_pa = 0x40400000};
const struct aarch64_early_arena *aarch64_early_arena_get(void) { return &arena; }
bool pmm_4k_page_allocated(uint64_t pa)
{
    for (size_t i = 0; i < 3; i++)
        if (pa == (uintptr_t)tables[i])
            return true;
    return pa == (uintptr_t)data;
}
void aarch64_tlb_flush_all(void)
{
    assert_eq(0, root[256]);
    flushed = true;
}
void free_4k_page(uint64_t pa)
{
    assert_true(flushed);
    for (size_t i = 0; i < nfree; i++)
        assert_true(pa != freed[i]);
    freed[nfree++] = pa;
}
TEST_FUNC(test_every_partial_depth_detaches_before_free)
{
    for (size_t depth = 0; depth <= 3; depth++) {
        memset(root, 0, sizeof(root));
        memset(tables, 0, sizeof(tables));
        nfree = 0;
        flushed = false;
        if (depth)
            root[256] = (uintptr_t)tables[0] | 3;
        for (size_t i = 1; i < depth; i++)
            tables[i - 1][0] = (uintptr_t)tables[i] | 3;
        assert_eq(0, aarch64_page_table_smoke_cleanup(root, (uintptr_t)data));
        assert_eq(depth + 1, nfree);
        assert_eq(0, root[256]);
    }
}
TEST_FUNC(test_unknown_table_retains_all_resources)
{
    memset(root, 0, sizeof(root));
    root[256] = 0x123003;
    nfree = 0;
    flushed = false;
    assert_eq(-EIO, aarch64_page_table_smoke_cleanup(root, (uintptr_t)data));
    assert_eq(0, nfree);
    assert_false(flushed);
}
TEST_LIST_BEGIN
TEST_ENTRY(test_every_partial_depth_detaches_before_free),
    TEST_ENTRY(test_unknown_table_retains_all_resources), TEST_LIST_END int main(void)
{
    int failed = PAGE_TABLE_RUN_ALL_TESTS();
    return failed;
}
