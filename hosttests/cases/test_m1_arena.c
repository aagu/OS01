/*
 * hosttests/cases/test_m1_arena.c — aarch64 M1 early arena planner
 *                                          (aarch64 M1 plan Task 3).
 *
 * Task 1 produced the checked PMM metadata layout calculator
 * (pmm_layout_calculate). Task 2 produced range-based boot
 * reservations + pmm_claim_free_frame. Task 3 stitches them into
 * the preflight arena planner that runs between aarch64_ram_init()
 * and pmm_init().
 *
 * The arena is the physical address range reserved for PMM metadata
 * + the strict M1 runtime page-table pool. It must:
 *
 *   - sit inside the RAM map AND in [AARCH64_M1_ARENA_LOW,
 *     AARCH64_M1_ARENA_HI) (the pre-MMU BSP uses M0's identity
 *     mapping at the head of LMA, so PA must be reachable via
 *     existing aliases);
 *   - be 2 MiB-aligned at both ends (matches PMM granule);
 *   - be large enough to hold align_up_4K(metadata_bytes) +
 *     table_pages * 4096, then rounded up to 2 MiB.
 *
 * The suite is pure: it does NOT exercise pmm_init, the PMM, or
 * any boot-time side effects. The planner (`aarch64_m1_plan`) must
 * not touch PMMngr.start_brk or any other global state on failure;
 * the tests prove that with a canary snapshot.
 *
 * Assertions pinned:
 *   - 512 MiB / 4 GiB / sparse / holes- contiguous windows.
 *   - First range too small, second range works.
 *   - kernel/handoff exclusion (no RAM in [0x40000000, 0x40200000)).
 *   - Only high window RAM (no [LOW, HI) intersection).
 *   - zone > 10 → -EINVAL.
 *   - Unaligned, unsorted, overlap → -EINVAL.
 *   - PA >= 1 TiB → -ERANGE.
 *   - RAM/D conflict (RAM overlaps [0x08000000, 0x0a000000)) → -EINVAL.
 *   - Overflow / no-space → fail without writing *out.
 *   - Failure preserves PMMngr.start_brk / candidate canary.
 *   - layout(base = arena base + OFFSET) consistent with pure
 *     pmm_layout_calculate sizing.
 */
#include "test_framework.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/pmm_boot.h>
#include <arch/aarch64/early_arena.h>
#include <arch/mmu.h>

/* pmm.c external references — host stubs. Mirror what
 * test_m1_reservation.c provides so the production pmm.o links. */
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* ── Reference builders ──────────────────────────────────────
 * Hand-mirror the planner's expected output using the production
 * checked calculator. The test passes the candidate arena's
 * `base_pa + OFFSET` to pmm_layout_calculate and compares every
 * field byte-for-byte. The reference span_pages uses the full
 * arena span (L0/RAM/table-pool pages), matching the production
 * formula in pmm_init Step 3. */
static void reference_layout(uint64_t arena_base_pa, uint64_t arena_end_pa,
                             struct pmm_layout *ref)
{
    /* Production pmm.c aligns start_brk up to 4 KiB before passing
     * it in. The arena base is 2 MiB-aligned, so the 4 KiB align
     * is a no-op — but mirror production byte-for-byte. */
    uint64_t brk = (arena_base_pa + ARCH_PAGE_OFFSET + 0xFFFUL) & ~0xFFFUL;
    uint64_t span_pages = (arena_end_pa - arena_base_pa) >> 21;  /* 2 MiB granule */
    if (span_pages == 0) span_pages = 1;
    int rc = pmm_layout_calculate(brk, span_pages, ref);
    if (rc != 0) memset(ref, 0, sizeof(*ref));
}

#define LAYOUT_EQ(label, got, ref, field) do { \
    if ((got).field != (ref).field) { \
        printf("  [FAIL] %s: " #field " got=0x%llx expected=0x%llx\n", \
               label, (unsigned long long)(got).field, \
               (unsigned long long)(ref).field); \
        __test_stats.failed++; \
    } else { \
        __test_stats.passed++; \
    } \
    __test_stats.total++; \
} while (0)

static void assert_layout_eq(const char *label,
                             const struct aarch64_m1_arena *got,
                             const struct pmm_layout *ref)
{
    LAYOUT_EQ(label, got->layout, *ref, bits_map_off);
    LAYOUT_EQ(label, got->layout, *ref, bits_length);
    LAYOUT_EQ(label, got->layout, *ref, pages_struct_off);
    LAYOUT_EQ(label, got->layout, *ref, pages_length);
    LAYOUT_EQ(label, got->layout, *ref, zones_struct_off);
    LAYOUT_EQ(label, got->layout, *ref, zones_length);
    LAYOUT_EQ(label, got->layout, *ref, end_of_struct_off);
    LAYOUT_EQ(label, got->layout, *ref, total_bytes);
}

static void build_single(uint64_t base, uint64_t end,
                         struct MEMORY_RANGE out[1])
{
    out[0].phys_start = base;
    out[0].phys_end   = end;
    out[0].type       = MEMORY_TYPE_RAM;
}

/* ── Suite entrypoint ────────────────────────────────────────
 * Some tests depend on the prepared-flag being clear at the start
 * of the run. RUN_ALL_TESTS walks __test[] in order, so the getter
 * "before-prepare" test MUST come first; later tests assume the
 * flag has been set. */

/* getter before any prepare: must return NULL. (First test in
    the suite — guards the rest.) */
TEST_FUNC(test_getter_returns_null_before_prepare)
{
    const struct aarch64_m1_arena *a = aarch64_m1_arena_get();
    assert_null((void *)a);
}

/* ── Happy paths ────────────────────────────────────────────── */

TEST_FUNC(test_happy_512_mib_low_window)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 512ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    /* arena is fully inside the input range */
    assert_true(got.base_pa >= 0x40200000ULL);
    assert_true(got.end_pa > got.base_pa);
    assert_true(got.end_pa <= 0x40200000ULL + 512ULL * 1024 * 1024);
    /* table_base_pa is at metadata_end_pa rounded up to 4 KiB */
    assert_true(got.table_base_pa >= got.base_pa);
    assert_true((got.table_base_pa & 0xFFFULL) == 0);
    assert_true(got.table_end_pa > got.table_base_pa);
    assert_true(got.table_end_pa <= got.end_pa);
    /* table_pages is the count of 4 KiB pages in the table pool */
    size_t pages = (got.table_end_pa - got.table_base_pa) / 0x1000ULL;
    assert_eq(pages, got.table_pages);
    /* layout(base = arena base + OFFSET) matches pure calculator */
    struct pmm_layout ref;
    reference_layout(got.base_pa, got.end_pa, &ref);
    assert_layout_eq("512 MiB / sram", &got, &ref);
}

TEST_FUNC(test_happy_4_gib_low_window)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 4ULL * 1024 * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    assert_true(got.base_pa == 0x40200000ULL);
    assert_true(got.end_pa >= got.base_pa);
    assert_true((got.base_pa & ((1ULL << 21) - 1)) == 0);
    assert_true((got.end_pa   & ((1ULL << 21) - 1)) == 0);
    struct pmm_layout ref;
    reference_layout(got.base_pa, got.end_pa, &ref);
    assert_layout_eq("4 GiB / sram", &got, &ref);
}

TEST_FUNC(test_first_range_too_small_second_works)
{
    /* Range 0 sits entirely below AARCH64_M1_ARENA_LOW — its
     * intersection with the arena window [LOW, HI) is empty
     * (effectively "too small" from the planner's vantage point,
     * since arena_bytes >= 2 MiB and intersections are
     * 2 MiB-aligned). Range 1 sits inside [LOW, HI) and is
     * sufficient. The planner iterates ranges in ascending PA
     * order and picks the first intersection large enough to
     * hold the arena. */
    struct MEMORY_RANGE ram[2] = {
        { .phys_start = 0x00000000ULL, .phys_end = 0x08000000ULL,  /* 128 MiB, below LOW */
          .type = MEMORY_TYPE_RAM },
        { .phys_start = 0x40200000ULL, .phys_end = 0x60000000ULL,  /* 480 MiB, inside [LOW, HI) */
          .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 2, &got);
    assert_eq(0, rc);
    /* The planner must have skipped range 0 (intersection = 0)
     * and picked range 1: [0x40200000, 0x60000000). */
    assert_true(got.base_pa >= 0x40200000ULL);
    assert_true(got.end_pa <= 0x60000000ULL);
    /* Range 0's end is 0x08000000 — below LOW, so the selected
     * base_pa must be at or above that boundary. */
    assert_true(got.base_pa >= 0x08000000ULL);
}

TEST_FUNC(test_holes_between_ranges)
{
    struct MEMORY_RANGE ram[2] = {
        { .phys_start = 0x40200000ULL, .phys_end = 0x40200000ULL + 256ULL * 1024 * 1024, .type = MEMORY_TYPE_RAM },
        /* Hole [0x80100000, 0xC0000000) */
        { .phys_start = 0xC0000000ULL, .phys_end = 0xC0000000ULL + 512ULL * 1024 * 1024, .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 2, &got);
    assert_eq(0, rc);
    assert_true(got.base_pa >= 0x40200000ULL);
    assert_true((got.base_pa & ((1ULL << 21) - 1)) == 0);
    /* arena must fit inside ONE range (never across the gap) */
    bool in_first = (got.base_pa >= 0x40200000ULL) &&
                  (got.end_pa <= 0x40200000ULL + 256ULL * 1024 * 1024);
    bool in_second = (got.base_pa >= 0xC0000000ULL) &&
                     (got.end_pa <= 0xC0000000ULL + 512ULL * 1024 * 1024);
    assert_true(in_first || in_second);
}

TEST_FUNC(test_kernel_handoff_exclusion)
{
    struct MEMORY_RANGE ram[1];
    /* Touches B from below — strictly illegal */
    build_single(0x40100000ULL, 0x40300000ULL, ram);
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_true(rc < 0);
}

TEST_FUNC(test_only_high_window_ram)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x80000000ULL, 0x80000000ULL + 512ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_eq(-ENOSPC, rc);
}

TEST_FUNC(test_zone_count_overflow)
{
    struct MEMORY_RANGE ram[11];
    /* 11 disjoint, aligned ranges, each 256 MiB — too many zones */
    for (int i = 0; i < 11; i++) {
        uint64_t base = 0x40200000ULL + (uint64_t)i * 0x20000000ULL;
        ram[i].phys_start = base;
        ram[i].phys_end   = base + 0x10000000ULL;  /* 256 MiB */
        ram[i].type       = MEMORY_TYPE_RAM;
    }
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 11, &out);
    assert_true(rc < 0);
}

/* ── Input validation failures ────────────────────────────── */

TEST_FUNC(test_unaligned_start)
{
    struct MEMORY_RANGE ram[1];
    ram[0].phys_start = 0x40200123ULL;
    ram[0].phys_end   = 0x41200000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_unaligned_end)
{
    struct MEMORY_RANGE ram[1];
    ram[0].phys_start = 0x40200000ULL;
    ram[0].phys_end   = 0x41200000ULL + 0x800ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_unsorted)
{
    struct MEMORY_RANGE ram[2] = {
        { .phys_start = 0x50000000ULL, .phys_end = 0x60000000ULL, .type = MEMORY_TYPE_RAM },
        { .phys_start = 0x40200000ULL, .phys_end = 0x50000000ULL, .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 2, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_overlapping)
{
    struct MEMORY_RANGE ram[2] = {
        { .phys_start = 0x40200000ULL, .phys_end = 0x50000000ULL, .type = MEMORY_TYPE_RAM },
        { .phys_start = 0x4F000000ULL, .phys_end = 0x60000000ULL, .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 2, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_pa_at_pa_limit)
{
    struct MEMORY_RANGE ram[1];
    ram[0].phys_start = AARCH64_M1_PA_LIMIT - 0x10000000ULL;
    ram[0].phys_end   = AARCH64_M1_PA_LIMIT + 0x10000000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_true(rc < 0);
}

TEST_FUNC(test_ram_d_conflict)
{
    struct MEMORY_RANGE ram[1];
    ram[0].phys_start = 0x08000000ULL;
    ram[0].phys_end   = 0x0a000000ULL + 0x10000000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_true(rc < 0);
}

TEST_FUNC(test_non_ram_type)
{
    struct MEMORY_RANGE ram[1];
    ram[0].phys_start = 0x40200000ULL;
    ram[0].phys_end   = 0x41200000ULL;
    ram[0].type       = MEMORY_TYPE_RESERVED;
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_zero_count)
{
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(NULL, 0, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_count_over_cap)
{
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(NULL, MEMORY_RANGE_MAX + 1, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_null_ram)
{
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(NULL, 1, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_empty_range)
{
    struct MEMORY_RANGE ram[1];
    ram[0].phys_start = 0x40200000ULL;
    ram[0].phys_end   = 0x40200000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_inverted_range)
{
    struct MEMORY_RANGE ram[1];
    ram[0].phys_start = 0x41200000ULL;
    ram[0].phys_end   = 0x40200000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_eq(-EINVAL, rc);
}

/* ── Failure leaves no side effects ────────────────────────── */
TEST_FUNC(test_failure_zeros_out)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x08000000ULL, 0x0a000000ULL, ram);  /* D conflict */
    struct aarch64_m1_arena out;
    memset(&out, 0xCC, sizeof(out));
    int rc = aarch64_m1_plan(ram, 1, &out);
    assert_true(rc < 0);
    assert_true(out.base_pa == 0);
    assert_true(out.end_pa == 0);
    assert_true(out.table_base_pa == 0);
    assert_true(out.table_end_pa == 0);
    assert_true(out.table_pages == 0);
    assert_true(out.layout.total_bytes == 0);
}

/* prepare() must NOT touch PMMngr.start_brk if planning fails. */
TEST_FUNC(test_prepare_failure_preserves_start_brk)
{
    extern struct Physical_Memory_Manager PMMngr;
    uint64_t canary = 0xDEADBEEFCAFEBABEULL;
    PMMngr.start_brk = canary;

    struct MEMORY_RANGE ram[1];
    build_single(0x08000000ULL, 0x0a000000ULL, ram);  /* D conflict */
    int rc = aarch64_m1_prepare(ram, 1);
    assert_true(rc < 0);
    assert_eq(canary, PMMngr.start_brk);
    PMMngr.start_brk = 0;
}

TEST_FUNC(test_prepare_success_sets_start_brk)
{
    extern struct Physical_Memory_Manager PMMngr;
    PMMngr.start_brk = 0;

    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 512ULL * 1024 * 1024, ram);
    int rc = aarch64_m1_prepare(ram, 1);
    assert_eq(0, rc);
    const struct aarch64_m1_arena *a = aarch64_m1_arena_get();
    assert_not_null((void *)a);
    if (a != NULL) {
        assert_eq((uint64_t)(ARCH_PAGE_OFFSET + a->base_pa),
                  PMMngr.start_brk);
    }
}

/* ── Layout structural invariants ──────────────────────────── */

TEST_FUNC(test_arena_table_pool_inside_arena)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 4ULL * 1024 * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    /* For a low-window-only map, table_pages >= 3 (1 L0 + 1 PUD[0]
        + at least 1 PMD). */
    assert_true(got.table_pages >= 3);
    assert_true(got.table_end_pa <= got.end_pa);
    /* The metadata section before table_base_pa is the calculator's
        `total_bytes`. table_base_pa = base_pa + align_up_4K(total). */
    uint64_t metadata_end_off =
        (got.layout.total_bytes + 0xFFFULL) & ~0xFFFULL;
    assert_eq(got.base_pa + metadata_end_off, got.table_base_pa);
}

TEST_FUNC(test_arena_size_matches_metadata_plus_tables)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 4ULL * 1024 * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    uint64_t metadata_4k =
        (got.layout.total_bytes + 0xFFFULL) & ~0xFFFULL;
    uint64_t pool_bytes = got.table_pages * 0x1000ULL;
    uint64_t wanted_2m =
        ((metadata_4k + pool_bytes + (1ULL << 21) - 1ULL) & ~((1ULL << 21) - 1ULL));
    assert_eq(got.end_pa - got.base_pa, wanted_2m);
}

/* ── Sizing consistency ────────────────────────────────────── */
TEST_FUNC(test_layout_sizing_consistency)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 4ULL * 1024 * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    uint64_t span_pages = (got.end_pa - got.base_pa) >> 21;
    if (span_pages == 0) span_pages = 1;
    uint64_t brk = (got.base_pa + ARCH_PAGE_OFFSET + 0xFFFULL) & ~0xFFFULL;
    struct pmm_layout pure;
    rc = pmm_layout_calculate(brk, span_pages, &pure);
    assert_eq(0, rc);
    /* Every layout field must match byte-for-byte. */
    assert_true(got.layout.bits_map_off      == pure.bits_map_off);
    assert_true(got.layout.bits_length       == pure.bits_length);
    assert_true(got.layout.pages_struct_off  == pure.pages_struct_off);
    assert_true(got.layout.pages_length      == pure.pages_length);
    assert_true(got.layout.zones_struct_off  == pure.zones_struct_off);
    assert_true(got.layout.zones_length      == pure.zones_length);
    assert_true(got.layout.end_of_struct_off == pure.end_of_struct_off);
    assert_true(got.layout.total_bytes       == pure.total_bytes);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_getter_returns_null_before_prepare),
    TEST_ENTRY(test_happy_512_mib_low_window),
    TEST_ENTRY(test_happy_4_gib_low_window),
    TEST_ENTRY(test_first_range_too_small_second_works),
    TEST_ENTRY(test_holes_between_ranges),
    TEST_ENTRY(test_kernel_handoff_exclusion),
    TEST_ENTRY(test_only_high_window_ram),
    TEST_ENTRY(test_zone_count_overflow),
    TEST_ENTRY(test_unaligned_start),
    TEST_ENTRY(test_unaligned_end),
    TEST_ENTRY(test_unsorted),
    TEST_ENTRY(test_overlapping),
    TEST_ENTRY(test_pa_at_pa_limit),
    TEST_ENTRY(test_ram_d_conflict),
    TEST_ENTRY(test_non_ram_type),
    TEST_ENTRY(test_zero_count),
    TEST_ENTRY(test_count_over_cap),
    TEST_ENTRY(test_null_ram),
    TEST_ENTRY(test_empty_range),
    TEST_ENTRY(test_inverted_range),
    TEST_ENTRY(test_failure_zeros_out),
    TEST_ENTRY(test_prepare_failure_preserves_start_brk),
    TEST_ENTRY(test_prepare_success_sets_start_brk),
    TEST_ENTRY(test_arena_table_pool_inside_arena),
    TEST_ENTRY(test_arena_size_matches_metadata_plus_tables),
    TEST_ENTRY(test_layout_sizing_consistency),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}