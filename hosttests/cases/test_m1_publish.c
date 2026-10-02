#include "m1_test_runner.h"
#include <errno.h>
#include <sys/mman.h>
#include <arch/aarch64/m1_selftest.h>
#include <arch/aarch64/runtime_tree.h>
#include <memory/pmm.h>
struct Physical_Memory_Manager PMMngr;
static struct Zone zone;
static struct Page frame = {.phy_address = 0x80000000};
static uint64_t root = 0x40210000, pa, expected;
static bool claim_allowed = true;
static unsigned releases;
static uint64_t claim_start;
static struct aarch64_runtime_tree tree = {
    .root_pa = 0x40210000, .table_base_pa = 0x40210000, .table_used_end_pa = 0x40216000};
int g_log_level = 3;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
uint64_t aarch64_runtime_root_address(void) { return (uintptr_t)&root; }
uint64_t aarch64_probe_pa_address(void) { return (uintptr_t)&pa; }
uint64_t aarch64_probe_expected_address(void) { return (uintptr_t)&expected; }
uint64_t aarch64_read_ttbr1(void) { return root; }
const struct aarch64_runtime_tree *aarch64_m1_tree_get(void) { return &tree; }
struct Page *pmm_claim_free_frame(uint64_t start, uint64_t end, bool reverse)
{
    (void)end;
    (void)reverse;
    claim_start = start;
    return claim_allowed ? &frame : NULL;
}
void free_pages(struct Page *p, int32_t n)
{
    (void)p;
    (void)n;
    releases++;
}
static uint64_t starts[8], ends[8];
static unsigned nr;
static void clean(uint64_t s, uint64_t e, uint64_t line)
{
    assert_true(line == 32 || line == 64 || line == 128);
    starts[nr] = s;
    ends[nr++] = e;
}
TEST_FUNC(test_outside_preferred_no_fallback_and_timeout_retains)
{
    zone.zone_end_address = 0xc0000000;
    PMMngr.zones_struct = &zone;
    PMMngr.zones_size = 1;
    claim_allowed = false;
    assert_eq(-ENOMEM, aarch64_m1_probe_prepare());
    assert_eq(0x80000000, claim_start);
    claim_allowed = true;
    assert_eq(0, aarch64_m1_probe_prepare());
    assert_eq(0, aarch64_m1_ap_verify(1));
    aarch64_m1_probe_finish(false);
    assert_eq(0, releases);
    assert_eq(0, aarch64_m1_ap_verify(2));
    for (uint64_t line = 32; line <= 128; line *= 2) {
        nr = 0;
        aarch64_m1_publish_ranges(clean, line);
        assert_eq(5, nr);
        assert_eq(0x6000, ends[0] - starts[0]);
        assert_eq(4096, ends[4] - starts[4]);
    }
    aarch64_m1_probe_finish(true);
    assert_eq(1, releases);
    aarch64_m1_probe_finish(true);
    assert_eq(1, releases);
}
TEST_LIST_BEGIN
TEST_ENTRY(test_outside_preferred_no_fallback_and_timeout_retains), TEST_LIST_END int main(void)
{
    void *p = mmap((void *)0x80000000, 0x200000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        return 2;
    int failed = M1_RUN_ALL_TESTS();
    return failed;
}
