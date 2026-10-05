#include "page_table_test_runner.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
static uint64_t tables[2][512] __attribute__((aligned(4096)));
static int allocations, fail_at;
void *__wrap_calloc(size_t n, size_t size)
{
    (void)n;
    (void)size;
    if (allocations++ == fail_at)
        return NULL;
    return tables[allocations - 1];
}
TEST_FUNC(test_each_intermediate_oom_leaves_descriptor_invalid)
{
    uint64_t root[512] = {0}, pa = 0;
    allocations = 0;
    fail_at = 0;
    assert_eq(-ENOMEM, vmm_get_next_level_checked(root, 0, PAGE_KERNEL_PGD, &pa));
    assert_eq(0, root[0]);
    allocations = 0;
    fail_at = 1;
    memset(tables, 0, sizeof(tables));
    assert_eq(0, vmm_get_next_level_checked(root, 0, PAGE_KERNEL_PGD, &pa));
    uint64_t *next = (uint64_t *)(uintptr_t)pa;
    assert_eq(-ENOMEM, vmm_get_next_level_checked(next, 0, PAGE_KERNEL_PUD, &pa));
    assert_eq(0, next[0]);
}
TEST_LIST_BEGIN
TEST_ENTRY(test_each_intermediate_oom_leaves_descriptor_invalid), TEST_LIST_END int main(void)
{
    int failed = PAGE_TABLE_RUN_ALL_TESTS();
    return failed;
}
