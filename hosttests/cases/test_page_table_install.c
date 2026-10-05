#include "page_table_test_runner.h"
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <arch/boot_memory.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/boot_direct_map.h>
#include <memory/pmm.h>
static struct aarch64_early_arena arena;
static struct MEMORY_RANGE ram = {
    .phys_start = 0x40200000, .phys_end = 0x41000000, .type = MEMORY_TYPE_RAM};
struct Physical_Memory_Manager PMMngr;
uint32_t ZONE_NORMAL_INDEX, ZONE_UNMAPPED_INDEX;
static struct Zone zone;
static uint64_t installed, published;
static int smoke_rc, install_calls;
int g_log_level = 3;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
size_t pmm_arch_normalize(const void *ctx, struct MEMORY_RANGE *out)
{
    (void)ctx;
    *out = ram;
    return 1;
}
const struct aarch64_early_arena *aarch64_early_arena_get(void) { return &arena; }
uint64_t aarch64_read_ttbr1(void) { return installed; }
void aarch64_install_ttbr1(uint64_t pa)
{
    installed = pa;
    install_calls++;
}
uint64_t aarch64_runtime_root_address(void) { return (uintptr_t)&published; }
void aarch64_page_table_prune_warm(void) {}
int aarch64_page_table_selftest(uint64_t pa)
{
    assert_eq(pa, installed);
    assert_false(arch_boot_direct_map_ready());
    assert_eq(0, published);
    return smoke_rc;
}
void arch_boot_direct_map__test_reset(void);
static void reset(void)
{
    arch_boot_direct_map__test_reset();
    installed = published = 0;
    smoke_rc = install_calls = 0;
    arena = (struct aarch64_early_arena){.base_pa = 0x40200000,
                                      .end_pa = 0x40400000,
                                      .table_base_pa = 0x40210000,
                                      .table_end_pa = 0x40214000,
                                      .table_pages = 4};
    zone = (struct Zone){
        .zone_start_address = ram.phys_start, .zone_end_address = ram.phys_end, .pages_length = 7};
    PMMngr.zones_struct = &zone;
    PMMngr.zones_size = 1;
}
TEST_FUNC(test_ready_after_install_cleanup_and_validation)
{
    reset();
    assert_false(arch_boot_direct_map_ready());
    assert_eq(0, arch_boot_direct_map_init());
    assert_true(arch_boot_direct_map_ready());
    assert_eq(installed, published);
    assert_eq(1, install_calls);
    const struct MEMORY_RANGE *out = NULL;
    size_t count = 0;
    assert_eq(0, arch_boot_direct_map_ranges(&out, &count));
    assert_eq(1, count);
    assert_eq(ram.phys_start, out[0].phys_start);
    assert_eq(-EALREADY, arch_boot_direct_map_init());
    assert_eq(1, install_calls);
}
TEST_FUNC(test_smoke_failure_never_publishes_and_cannot_retry)
{
    reset();
    smoke_rc = -EIO;
    assert_eq(-EIO, arch_boot_direct_map_init());
    assert_false(arch_boot_direct_map_ready());
    assert_eq(0, published);
    assert_eq(-EALREADY, arch_boot_direct_map_init());
}
TEST_FUNC(test_pool_exhaustion_before_install)
{
    reset();
    arena.table_pages = 3;
    arena.table_end_pa -= 4096;
    assert_eq(-ENOMEM, arch_boot_direct_map_init());
    assert_eq(0, install_calls);
    assert_eq(0, published);
}
TEST_LIST_BEGIN
TEST_ENTRY(test_ready_after_install_cleanup_and_validation),
    TEST_ENTRY(test_smoke_failure_never_publishes_and_cannot_retry),
    TEST_ENTRY(test_pool_exhaustion_before_install), TEST_LIST_END int main(void)
{
    void *p = mmap((void *)0x40200000, 0x200000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        return 2;
    int failed = PAGE_TABLE_RUN_ALL_TESTS();
    munmap(p, 0x200000);
    return failed;
}
