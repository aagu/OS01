/*
 * hosttests/cases/test_page_table_contract.c — common boot direct-map contract
 *                                            (aarch64 M1 plan Task 5).
 *
 * Task 5 introduces a shared facade (kernel/include/arch/boot_memory.h)
 * with three symbols:
 *
 *   int  arch_boot_direct_map_init(void);
 *   bool arch_boot_direct_map_ready(void);
 *   int  arch_boot_direct_map_ranges(const struct MEMORY_RANGE **out,
 *                                    size_t *count);
 *
 * This test asserts the contract for the x86_64 backend (kernel/arch/
 * x86_64/memory/boot_direct_map.c). Task 6 will add the matching
 * aarch64 backend and the same assertions will run against it through
 * a per-backend suite.
 *
 * Assertions (mirrors the brief):
 *   - initial state: ready() returns false.
 *   - NULL params to ranges(): -EINVAL, NO writes to both outputs.
 *   - ranges() before ready: write *out=NULL, *count=0; return -EAGAIN.
 *   - successful init: ready=true; ranges() returns immutable merged
 *     coverage; capacity < required → -ENOSPC with *count = required.
 *   - init OOM at any intermediate-table level: -ENOMEM; ready stays
 *     false; ready() still false after; repeat init → -EALREADY with
 *     no side effects.
 *   - init validation failure (kernel_map doesn't actually cover the
 *     merged range): -EIO; ready stays false; repeat init → -EALREADY.
 *   - repeat init after success: -EALREADY with no side effects.
 *
 * Fixture: PMMngr.zones_struct is populated by hand with three RAM
 * zones separated by 2 MiB-aligned gaps. The test sets
 * ZONE_UNMAPPED_INDEX to 0 (per the brief: "ZONE_UNMAPPED_INDEX=0
 * 表示无 cutoff") for the happy-path runs so every represented zone
 * is mapped into the kernel page tables. The holes live between
 * zones and do not appear in the merged coverage.
 *
 * The test stubs vmm_init() (returns int now per the vmm.h change)
 * so we can inject OOM and validation failures without exercising
 * the real vmm.c's calloc chain. The x86 backend itself only
 * consumes vmm_init's return value plus the resulting kernel_map;
 * stubbing is therefore sufficient to exercise every contract branch.
 */
#include "page_table_test_runner.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <arch/boot_memory.h>
#include <memory/memory.h>
#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/vmm.h>

/* ── Production-symbol stubs ────────────────────────────────────
 * color_printk + slab_init are referenced transitively by the
 * headers; log_err is referenced by the production x86 backend. */
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* ── Test-controlled vmm_init + kernel_map ──────────────────────
 *
 * vmm.c exports `mmap kernel_map` and `int vmm_init(void)`. We
 * don't link vmm.c here. Instead we provide our own kernel_map and
 * our own vmm_init stub so the x86 backend can read both. The stub:
 *
 *   - records the call count (for "no side effects on -EALREADY")
 *   - returns whatever the test last set `g_vmm_rc_enforced` to
 *   - on success, walks PMMngr's zones (up to ZONE_UNMAPPED_INDEX,
 *     or every zone when ZONE_UNMAPPED_INDEX == 0) and populates
 *     kernel_map with 2 MiB huge descriptors in the kernel half
 *     (PGD slot 256 + 0..511).
 *
 * The test does NOT rely on vmm_init's internal calloc chain — the
 * real boot mapper path goes through vmm_boot_alloc_table, but the
 * contract is "vmm_init's return value + resulting kernel_map" so
 * the stub is sufficient. */
extern uint64_t *kernel_map;

#define HOST_PAGE_TABLE_PAGES 8
static uint64_t kernel_map_storage[HOST_PAGE_TABLE_PAGES * 512]
    __attribute__((aligned(4096)));

static int  g_vmm_rc_enforced = 0;
static int  g_vmm_call_count  = 0;
extern struct Physical_Memory_Manager PMMngr;
extern uint32_t ZONE_DMA_INDEX;
extern uint32_t ZONE_NORMAL_INDEX;
extern uint32_t ZONE_UNMAPPED_INDEX;

/* Test-only sentinel: when 1, vmm_init returns 0 but skips the
 * mapping loop (kernel_map_storage is left zero). The production
 * x86 backend's validation walk must then detect the mismatch. */
int g_vmm_no_map = 0;
int g_vmm_corrupt;

/* Map one 2 MiB PA into the test kernel_map. Each unique PUD slot
 * gets its own L2 (PMD) page so non-contiguous 1 GiB buckets work
 * (e.g. PAs in PUD[0] and PUD[32] need separate L2 pages). Returns
 * 0 on success, -ENOMEM if the OOM injection is active and a slot
 * needs to be allocated. */
#include <unistd.h>
static int host_map_2m(uint64_t pa)
{
    uint64_t *pgd = &kernel_map_storage[0];        /* page 0 = L0 */
    uint64_t pgd_idx = 256u + ((pa >> 39) & 0x1ffu);
    uint64_t *pud = &kernel_map_storage[512];      /* page 1 = L1 PUD */
    uint64_t pud_idx = (pa >> 30) & 0x1ffu;
    uint64_t l2_slot = pud_idx % (HOST_PAGE_TABLE_PAGES - 2u);
    uint64_t *pmd = &kernel_map_storage[(2u + l2_slot) * 512u];
    uint64_t pmd_idx = (pa >> 21) & 0x1ffu;


    if (!(pgd[pgd_idx] & 1u)) {
        if (g_vmm_rc_enforced == -ENOMEM) return -ENOMEM;
        pgd[pgd_idx] = (uint64_t)pud | 0x3u;       /* PRESENT | RW */
    }
    if (!(pud[pud_idx] & 1u)) {
        if (g_vmm_rc_enforced == -ENOMEM) return -ENOMEM;
        pud[pud_idx] = (uint64_t)pmd | 0x3u;
    }
    if (g_vmm_corrupt) pa += 0x200000;
    pmd[pmd_idx] = (pa & ~(uint64_t)0x1fffff) | 0x83u; /* PRESENT|RW|PS */
    return 0;
}

/* Test TU's vmm_init. */
int vmm_init(void)
{
    g_vmm_call_count++;
    if (g_vmm_rc_enforced != 0) return g_vmm_rc_enforced;

    memset(kernel_map_storage, 0, sizeof(kernel_map_storage));
    if (g_vmm_no_map) return 0;       /* success but kernel_map stays zero */
    uint32_t cutoff = (ZONE_UNMAPPED_INDEX != 0)
                      ? ZONE_UNMAPPED_INDEX
                      : PMMngr.zones_size;
    for (uint32_t i = 0; i < cutoff; i++) {
        struct Zone *z = PMMngr.zones_struct + i;
        struct Page *p = z->pages_group;
        for (uint64_t j = 0; j < z->pages_length; j++, p++) {
            int rc = host_map_2m(p->phy_address);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

/* Provide the kernel_map symbol the production x86 backend reads. */
uint64_t *kernel_map = kernel_map_storage;

/* ── Test-only state reset hook (weak in production) ───────────── */
extern void arch_boot_direct_map__test_reset(void);

/* ── PMMngr fixture ─────────────────────────────────────────────
 * Three 2 MiB-aligned RAM zones, each 64 frames (128 MiB), with
 * gaps between them. pages / bits arrays are statically sized for
 * the fixture only. */
#define FIXTURE_FRAMES_PER_ZONE 64UL
#define FIXTURE_PAGE_FRAMES     (FIXTURE_FRAMES_PER_ZONE * 3UL)
#define FIXTURE_BITS_WORDS      ((FIXTURE_PAGE_FRAMES + 63UL) / 64UL)

static struct Page g_pages[FIXTURE_PAGE_FRAMES];
static struct Zone g_zones[3];
static uint64_t    g_bits[FIXTURE_BITS_WORDS];

struct Physical_Memory_Manager PMMngr;
uint32_t ZONE_DMA_INDEX;
uint32_t ZONE_NORMAL_INDEX;
uint32_t ZONE_UNMAPPED_INDEX;

static void fixture_init_pmmngr(uint32_t unmapped_index)
{
    memset(&PMMngr, 0, sizeof(PMMngr));
    memset(g_pages, 0, sizeof(g_pages));
    memset(g_zones, 0, sizeof(g_zones));
    memset(g_bits,  0, sizeof(g_bits));

    const uint64_t bases[3] = { 0x200000ULL,
                                0x10000000ULL,
                                0x40000000ULL };
    PMMngr.pages_struct = g_pages;
    PMMngr.pages_size   = FIXTURE_PAGE_FRAMES;
    PMMngr.zones_struct = g_zones;
    PMMngr.zones_size   = 3u;
    PMMngr.bits_map     = g_bits;
    PMMngr.bits_size    = FIXTURE_PAGE_FRAMES;

    uint64_t total_frames = 0;
    for (uint32_t i = 0; i < 3; i++) {
        g_zones[i].zone_start_address = bases[i];
        g_zones[i].zone_end_address   = bases[i] + FIXTURE_FRAMES_PER_ZONE * 0x200000ULL;
        g_zones[i].zone_length        = FIXTURE_FRAMES_PER_ZONE * 0x200000ULL;
        g_zones[i].pages_group        = &g_pages[total_frames];
        g_zones[i].pages_length       = FIXTURE_FRAMES_PER_ZONE;
        g_zones[i].page_free_count    = FIXTURE_FRAMES_PER_ZONE;
        for (uint64_t j = 0; j < FIXTURE_FRAMES_PER_ZONE; j++) {
            g_pages[total_frames + j].zone_struct = &g_zones[i];
            g_pages[total_frames + j].phy_address =
                bases[i] + j * 0x200000ULL;
        }
        total_frames += FIXTURE_FRAMES_PER_ZONE;
    }
    ZONE_DMA_INDEX      = 0;
    ZONE_NORMAL_INDEX   = 0;
    ZONE_UNMAPPED_INDEX = unmapped_index;
    g_vmm_rc_enforced   = 0;
}

/* ── Tests ────────────────────────────────────────────────────── */

TEST_FUNC(test_initial_not_ready)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    assert_false(arch_boot_direct_map_ready());
}

TEST_FUNC(test_ranges_null_arg_returns_einval)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    /* Both NULL → -EINVAL, no writes. */
    int rc = arch_boot_direct_map_ranges(NULL, NULL);
    assert_eq(-EINVAL, rc);
    /* NULL out alone. */
    size_t count = 0xDEAD;
    rc = arch_boot_direct_map_ranges(NULL, &count);
    assert_eq(-EINVAL, rc);
    /* NULL count alone — out must remain untouched. */
    const struct MEMORY_RANGE *out2 = (const struct MEMORY_RANGE *)0xDEAD;
    rc = arch_boot_direct_map_ranges(&out2, NULL);
    assert_eq(-EINVAL, rc);
    assert_true(out2 == (const struct MEMORY_RANGE *)0xDEAD);
}

TEST_FUNC(test_ranges_not_ready_writes_null_zero_and_eagain)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    const struct MEMORY_RANGE *out = (const struct MEMORY_RANGE *)0xDEAD;
    size_t count = 0xDEAD;
    int rc = arch_boot_direct_map_ranges(&out, &count);
    assert_eq(-EAGAIN, rc);
    assert_null(out);
    assert_eq(0, count);
}

TEST_FUNC(test_init_succeeds_and_makes_ready)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);            /* ZONE_UNMAPPED_INDEX=0 → no cutoff */
    g_vmm_rc_enforced = 0;             /* happy path */
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);
    assert_true(arch_boot_direct_map_ready());
}

TEST_FUNC(test_ranges_returns_merged_coverage)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    g_vmm_rc_enforced = 0;
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);

    const struct MEMORY_RANGE *out = NULL;
    size_t cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out, &cap);
    assert_eq(0, rc);
    assert_not_null((void *)out);
    /* Three zones, two gaps → three ranges (gaps merged out). */
    assert_eq(3, cap);
    /* Sorted by start. */
    assert_true(out[0].phys_start == 0x200000ULL);
    assert_true(out[0].phys_end   == 0x200000ULL + 64ULL * 0x200000ULL);
    assert_true(out[1].phys_start == 0x10000000ULL);
    assert_true(out[2].phys_start == 0x40000000ULL);
    /* All are RAM. */
    for (size_t i = 0; i < cap; i++) {
        assert_eq((int)MEMORY_TYPE_RAM, (int)out[i].type);
    }
}

TEST_FUNC(test_ranges_count_is_output_only)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    g_vmm_rc_enforced = 0;
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);

    const struct MEMORY_RANGE *out = NULL;
    size_t cap = 1;                   /* way too small */
    rc = arch_boot_direct_map_ranges(&out, &cap);
    assert_eq(0, rc);
    assert_eq(3, cap);                 /* required count written back */
    assert_not_null(out);
}

TEST_FUNC(test_init_oom_returns_enomen_no_null_descriptor)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    /* Force vmm_init to fail at the FIRST intermediate allocation
     * (PGD → PUD). This simulates the brief's "每级 allocation
     * 注入 OOM" check: the resulting kernel_map must NOT contain a
     * NULL PGD descriptor (PA=0 would be a NULL descriptor). */
    g_vmm_rc_enforced = -ENOMEM;
    int rc = arch_boot_direct_map_init();
    assert_eq(-ENOMEM, rc);
    /* State stays FAILED; ready() returns false. */
    assert_false(arch_boot_direct_map_ready());
    /* Repeat call must be -EALREADY with no side effects (vmm_init
     * must not be called again). */
    int prev_calls = g_vmm_call_count;
    rc = arch_boot_direct_map_init();
    assert_eq(-EALREADY, rc);
    assert_eq(prev_calls, g_vmm_call_count);
}

TEST_FUNC(test_init_validation_failure_returns_eio)
{
    /* vmm_init succeeds but leaves kernel_map empty (no descriptors).
     * The brief's validation walk must then detect that kernel_map
     * doesn't cover the merged coverage and return -EIO. We use
     * g_vmm_no_map to make the stub skip the mapping loop while
     * still returning 0 (success). */
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    g_vmm_rc_enforced = 0;
    g_vmm_no_map = 1;                 /* vmm_init succeeds but maps nothing */
    int rc = arch_boot_direct_map_init();
    assert_eq(-EIO, rc);
    assert_false(arch_boot_direct_map_ready());
    g_vmm_no_map = 0;
    /* Repeat → -EALREADY with no side effects. */
    int prev_calls = g_vmm_call_count;
    rc = arch_boot_direct_map_init();
    assert_eq(-EALREADY, rc);
    assert_eq(prev_calls, g_vmm_call_count);
}

TEST_FUNC(test_repeat_init_after_success_returns_ealready_no_side_effects)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    g_vmm_rc_enforced = 0;
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);
    int prev_calls = g_vmm_call_count;
    /* ranges() works. */
    const struct MEMORY_RANGE *out = NULL;
    size_t cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out, &cap);
    assert_eq(0, rc);
    /* Repeat init. */
    rc = arch_boot_direct_map_init();
    assert_eq(-EALREADY, rc);
    /* vmm_init must NOT have been called again. */
    assert_eq(prev_calls, g_vmm_call_count);
    /* ranges() still works, returning the same merged coverage. */
    out = NULL;
    cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out, &cap);
    assert_eq(0, rc);
    assert_eq(3, cap);
}

TEST_FUNC(test_adjacent_zones_merged_into_single_coverage)
{
    arch_boot_direct_map__test_reset();
    /* Two zones that share a boundary (zone1 end == zone2 start).
     * Coverage should merge into a single entry. */
    memset(&PMMngr, 0, sizeof(PMMngr));
    memset(g_pages, 0, sizeof(g_pages));
    memset(g_zones, 0, sizeof(g_zones));
    PMMngr.pages_struct = g_pages;
    PMMngr.pages_size   = 128;
    PMMngr.zones_struct = g_zones;
    PMMngr.zones_size   = 2u;
    PMMngr.bits_map     = g_bits;
    PMMngr.bits_size    = 128;
    g_zones[0].zone_start_address = 0x200000ULL;
    g_zones[0].zone_end_address   = 0x200000ULL + 64ULL * 0x200000ULL;
    g_zones[0].zone_length        = 64ULL * 0x200000ULL;
    g_zones[0].pages_group        = &g_pages[0];
    g_zones[0].pages_length       = 64UL;
    g_zones[1].zone_start_address = 0x200000ULL + 64ULL * 0x200000ULL;
    g_zones[1].zone_end_address   = 0x200000ULL + 128ULL * 0x200000ULL;
    g_zones[1].zone_length        = 64ULL * 0x200000ULL;
    g_zones[1].pages_group        = &g_pages[64];
    g_zones[1].pages_length       = 64UL;
    for (uint64_t j = 0; j < 64; j++) {
        g_pages[j].zone_struct = &g_zones[0];
        g_pages[j].phy_address = g_zones[0].zone_start_address + j * 0x200000ULL;
    }
    for (uint64_t j = 0; j < 64; j++) {
        g_pages[64 + j].zone_struct = &g_zones[1];
        g_pages[64 + j].phy_address = g_zones[1].zone_start_address + j * 0x200000ULL;
    }
    ZONE_UNMAPPED_INDEX = 0;
    g_vmm_rc_enforced = 0;
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);
    const struct MEMORY_RANGE *out = NULL;
    size_t cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out, &cap);
    assert_eq(0, rc);
    assert_eq(1, cap);                 /* adjacent zones merged */
    assert_true(out[0].phys_start == 0x200000ULL);
    assert_true(out[0].phys_end   == 0x200000ULL + 128ULL * 0x200000ULL);
}

TEST_FUNC(test_ranges_immutable_after_ready)
{
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    g_vmm_rc_enforced = 0;
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);
    const struct MEMORY_RANGE *out1 = NULL;
    size_t cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out1, &cap);
    assert_eq(0, rc);
    /* Second call returns the same pointer and same count. */
    const struct MEMORY_RANGE *out2 = NULL;
    cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out2, &cap);
    assert_eq(0, rc);
    assert_true(out1 == out2);
    assert_eq(3, cap);
    /* Mutate PMMngr; the next ranges() call must NOT pick up the
     * change because the merged coverage was frozen at init time. */
    g_zones[0].zone_end_address += 0x200000ULL;
    cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out2, &cap);
    assert_eq(0, rc);
    assert_eq(3, cap);
    assert_true(out2[0].phys_end == 0x200000ULL + 64ULL * 0x200000ULL);
}

TEST_FUNC(test_ranges_zero_input_count_succeeds)
{
    /* Brief: "有效输出先 NULL/0" — caller passes count=0 (query
     * mode). The contract returns -ENOSPC with required count. */
    arch_boot_direct_map__test_reset();
    fixture_init_pmmngr(0);
    g_vmm_rc_enforced = 0;
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);
    const struct MEMORY_RANGE *out = NULL;
    size_t cap = 0;
    rc = arch_boot_direct_map_ranges(&out, &cap);
    assert_eq(0, rc);
    assert_eq(3, cap);
    assert_not_null(out);
}

TEST_FUNC(test_holes_between_zones_collapsed_in_coverage)
{
    /* Brief: "holes" between zones must not appear in the merged
     * coverage. */
    arch_boot_direct_map__test_reset();
    memset(&PMMngr, 0, sizeof(PMMngr));
    memset(g_pages, 0, sizeof(g_pages));
    memset(g_zones, 0, sizeof(g_zones));
    PMMngr.pages_struct = g_pages;
    PMMngr.pages_size   = 64;
    PMMngr.zones_struct = g_zones;
    PMMngr.zones_size   = 2u;
    PMMngr.bits_map     = g_bits;
    PMMngr.bits_size    = 64;
    /* Zone 0: 32 frames. Zone 1: 32 frames at a disjoint PA. */
    g_zones[0].zone_start_address = 0x200000ULL;
    g_zones[0].zone_end_address   = 0x200000ULL + 32ULL * 0x200000ULL;
    g_zones[0].zone_length        = 32ULL * 0x200000ULL;
    g_zones[0].pages_group        = &g_pages[0];
    g_zones[0].pages_length       = 32UL;
    g_zones[1].zone_start_address = 0x80000000ULL;
    g_zones[1].zone_end_address   = 0x80000000ULL + 32ULL * 0x200000ULL;
    g_zones[1].zone_length        = 32ULL * 0x200000ULL;
    g_zones[1].pages_group        = &g_pages[32];
    g_zones[1].pages_length       = 32UL;
    for (uint64_t j = 0; j < 32; j++) {
        g_pages[j].zone_struct = &g_zones[0];
        g_pages[j].phy_address = g_zones[0].zone_start_address + j * 0x200000ULL;
    }
    for (uint64_t j = 0; j < 32; j++) {
        g_pages[32 + j].zone_struct = &g_zones[1];
        g_pages[32 + j].phy_address = g_zones[1].zone_start_address + j * 0x200000ULL;
    }
    ZONE_UNMAPPED_INDEX = 0;
    g_vmm_rc_enforced = 0;
    int rc = arch_boot_direct_map_init();
    assert_eq(0, rc);
    const struct MEMORY_RANGE *out = NULL;
    size_t cap = MEMORY_RANGE_MAX;
    rc = arch_boot_direct_map_ranges(&out, &cap);
    assert_eq(0, rc);
    assert_eq(2, cap);                 /* two zones → two ranges */
    assert_true(out[0].phys_start == 0x200000ULL);
    assert_true(out[0].phys_end   == 0x200000ULL + 32ULL * 0x200000ULL);
    assert_true(out[1].phys_start == 0x80000000ULL);
    assert_true(out[1].phys_end   == 0x80000000ULL + 32ULL * 0x200000ULL);
}

TEST_FUNC(test_cutoff_reports_only_mapped_zone_subset)
{
    arch_boot_direct_map__test_reset(); fixture_init_pmmngr(1);
    assert_eq(0,arch_boot_direct_map_init());
    const struct MEMORY_RANGE *out; size_t count=0;
    assert_eq(0,arch_boot_direct_map_ranges(&out,&count));
    assert_eq(1,count);
    assert_eq(0x08200000,out[0].phys_end);
}
TEST_FUNC(test_validation_rejects_wrong_leaf_pa)
{
    arch_boot_direct_map__test_reset(); fixture_init_pmmngr(0);
    g_vmm_corrupt=1;
    assert_eq(-EIO,arch_boot_direct_map_init());
    assert_false(arch_boot_direct_map_ready()); g_vmm_corrupt=0;
}
TEST_LIST_BEGIN
    TEST_ENTRY(test_cutoff_reports_only_mapped_zone_subset),
    TEST_ENTRY(test_validation_rejects_wrong_leaf_pa),
    TEST_ENTRY(test_initial_not_ready),
    TEST_ENTRY(test_ranges_null_arg_returns_einval),
    TEST_ENTRY(test_ranges_not_ready_writes_null_zero_and_eagain),
    TEST_ENTRY(test_init_succeeds_and_makes_ready),
    TEST_ENTRY(test_ranges_returns_merged_coverage),
    TEST_ENTRY(test_ranges_count_is_output_only),
    TEST_ENTRY(test_ranges_zero_input_count_succeeds),
    TEST_ENTRY(test_init_oom_returns_enomen_no_null_descriptor),
    TEST_ENTRY(test_init_validation_failure_returns_eio),
    TEST_ENTRY(test_repeat_init_after_success_returns_ealready_no_side_effects),
    TEST_ENTRY(test_adjacent_zones_merged_into_single_coverage),
    TEST_ENTRY(test_ranges_immutable_after_ready),
    TEST_ENTRY(test_holes_between_zones_collapsed_in_coverage),
TEST_LIST_END

int main(void)
{
    int failed = PAGE_TABLE_RUN_ALL_TESTS();
    return failed;
}
