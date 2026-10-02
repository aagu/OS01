#include "m1_test_runner.h"
#include <errno.h>
#include <memory/pmm.h>
#include <arch/aarch64/early_arena.h>
struct Physical_Memory_Manager PMMngr;
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
TEST_FUNC(test_reduced_capacity_reaches_planner_no_space)
{
    const struct MEMORY_RANGE ram = {
        .phys_start = 0x40200000, .phys_end = 0x60000000, .type = MEMORY_TYPE_RAM};
    struct aarch64_m1_arena out;
    assert_eq(-ENOSPC, aarch64_m1_plan(&ram, 1, &out));
    assert_eq(0, out.base_pa);
    PMMngr.start_brk = 0xdeadbeef;
    assert_eq(-ENOSPC, aarch64_m1_prepare(&ram, 1));
    assert_eq(0xdeadbeef, PMMngr.start_brk);
    assert_null(aarch64_m1_arena_get());
}
TEST_LIST_BEGIN
TEST_ENTRY(test_reduced_capacity_reaches_planner_no_space), TEST_LIST_END int main(void)
{
    return M1_RUN_ALL_TESTS();
}
