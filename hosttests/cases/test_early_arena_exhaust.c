#include "page_table_test_runner.h"
#include <errno.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <arch/aarch64/early_arena.h>
struct Physical_Memory_Manager PMMngr;
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
/* Task 3: early_arena.c now reads kmalloc_cache_size[].size via
 * slab_layout_compute(). Same stub as test_early_arena.c. */
struct Slab_Cache kmalloc_cache_size[16] = {
    {32,      0, 0, NULL, NULL, NULL, NULL},
    {64,      0, 0, NULL, NULL, NULL, NULL},
    {128,     0, 0, NULL, NULL, NULL, NULL},
    {256,     0, 0, NULL, NULL, NULL, NULL},
    {512,     0, 0, NULL, NULL, NULL, NULL},
    {1024,    0, 0, NULL, NULL, NULL, NULL},
    {2048,    0, 0, NULL, NULL, NULL, NULL},
    {4096,    0, 0, NULL, NULL, NULL, NULL},
    {8192,    0, 0, NULL, NULL, NULL, NULL},
    {16384,   0, 0, NULL, NULL, NULL, NULL},
    {32768,   0, 0, NULL, NULL, NULL, NULL},
    {65536,   0, 0, NULL, NULL, NULL, NULL},
    {131072,  0, 0, NULL, NULL, NULL, NULL},
    {262144,  0, 0, NULL, NULL, NULL, NULL},
    {524288,  0, 0, NULL, NULL, NULL, NULL},
    {1048576, 0, 0, NULL, NULL, NULL, NULL},
};
TEST_FUNC(test_reduced_capacity_reaches_planner_no_space)
{
    const struct MEMORY_RANGE ram = {
        .phys_start = 0x40200000, .phys_end = 0x60000000, .type = MEMORY_TYPE_RAM};
    struct aarch64_early_arena out;
    assert_eq(-ENOSPC, aarch64_early_arena_plan(&ram, 1, &out));
    assert_eq(0, out.base_pa);
    PMMngr.start_brk = 0xdeadbeef;
    assert_eq(-ENOSPC, aarch64_early_arena_prepare(&ram, 1));
    assert_eq(0xdeadbeef, PMMngr.start_brk);
    assert_null(aarch64_early_arena_get());
}
TEST_LIST_BEGIN
TEST_ENTRY(test_reduced_capacity_reaches_planner_no_space), TEST_LIST_END int main(void)
{
    return PAGE_TABLE_RUN_ALL_TESTS();
}
