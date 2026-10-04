/*
 * hosttests/cases/test_slab_idempotent_reservation.c — slab_init() must
 * be idempotent vs boot reservations (aarch64 M2/M3 plan Task 2).
 *
 * Before this test, slab_init's 8-page loop unconditionally did:
 *
 *     PMMngr.bits_map[w] |= bit;       // no-op when pre-set
 *     page->zone_struct->page_using_count++;  // DOUBLE-COUNTED if bit
 *     page->zone_struct->page_free_count--;   // pre-set
 *
 * so on x86 a clean reboot (no pre-reservation) still worked, but on
 * aarch64 the 8 frames are reserved before slab_init runs (the
 * pre-MMU M1 arena picks the very first 2 MiB-aligned window inside
 * [AARCH64_M1_ARENA_LOW, AARCH64_M1_ARENA_HI)) and slab_init
 * double-counts them: `using_count` for those 8 frames goes up twice
 * — once by the boot reservation, once by slab_init. The subsequent
 * `free_pages` (run by the arena installer to drop the metadata once
 * it's pinned by the M0 direct-map alias) sees `using_count == 2`
 * and decrements only once, leaving the bitmap and the zone counter
 * inconsistent. Spec §3.2 mandates that slab_init skip the ++/-- when
 * the bit is already set (idempotent re-entry), matching the same
 * pattern used by `pmm_reserve_boot_ranges` (test_m1_reservation).
 *
 * This test links the REAL kernel/memory/slab.c + pmm.c
 * (PMM_SHADOW_INC shadows <arch/spinlock.h>; SLAB_SHADOW_INC extends
 * that with <arch/irq.h> and <percpu/percpu.h> stubs — the production
 * <arch/irq.h> ships `pushfq; cli` inline asm that faults on the host).
 * The fixture sets up PMMngr by hand and drives slab_init twice:
 *
 *   1. With the 8 slab-page bits pre-set (boot reservation replay):
 *      assert using_count does NOT increment for those 8 frames.
 *      RED: +8 (double-counted). GREEN: +7 (only the j-loop page).
 *   2. With bits cleared (fresh state): assert each frame is counted
 *      exactly once.  1+8 = 9 total; passes for both RED and GREEN, so
 *      this case is the "no surprise" sanity check (regression guard
 *      against the fix accidentally marking twice on a clean boot).
 *
 * Brief step 1 calls out "16" as the RED increment; that figure
 * assumes 8 pre-set bits trigger +8 in the 8-page loop and +1 in the
 * j-loop on a clean second call, plus +8 again from the first call's
 * 8-page loop = +17 (or 16 if the j-loop page overlaps the first 8-
 * page loop slot on a particular layout — the brief's exact figure
 * is layout-dependent). What matters for the test is the invariant:
 * the pre-set 8 frames must NOT see ++/-- when slab_init re-enters.
 * We assert that invariant directly via using_count + the bit
 * pattern, which is layout-independent.
 */
#include "test_framework.h"
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <memory/pmm.h>
#include <memory/slab.h>

extern struct Physical_Memory_Manager PMMngr;
extern uint32_t ZONE_DMA_INDEX;

/* ── link stubs ────────────────────────────────────────
 * pmm.o references g_log_level / _log_err_impl; mock_kernel.o already
 * provides color_printk / kmalloc / kfree / kpanic (host libc wrappers)
 * so we MUST NOT re-define those here (would clash with the link of
 * mock_kernel.o). pmm_layout_calculate / pmm_arch_normalize /
 * pmm_arch_zone_split are satisfied by pmm_arch_default.o +
 * pmm_boot_production.o via the Makefile.
 *
 * `percpu_data[]` storage lives in the slab shadow header
 * (mock/slab_include/percpu/percpu.h) and is shared with
 * slab_production.o; do NOT redefine it here. */
int g_log_level = 3;   /* LOG_ERR */
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

/* ── Fixture ─────────────────────────────────────────────────
 * 256 frames of 2 MiB each is plenty (slab_init consumes ~22 KiB of
 * metadata + 8 × 2 MiB of virtual address space; pages_struct must
 * span PA 0 .. enough to address the highest page slab_init touches).
 * Pages 0..N_PAGES-1 are densely packed; PA = index * PAGE_2M_SIZE,
 * matching Phy_to_2M_Page's convention
 *   pages_struct[(PA - pages_struct[0].phy_address) >> PAGE_2M_SHIFT]
 * which simplifies to pages_struct[PA / PAGE_2M_SIZE] when
 * pages_struct[0].phy_address == 0. */
#define N_PAGES 1024
static struct Page   fixture_pages[N_PAGES];
static struct Zone   fixture_zone;
static uint64_t      fixture_bits[N_PAGES / 64];

/* Backing for slab_init's `end_of_struct` writes. slab_init advances
 * end_of_struct by the metadata total (~22 KiB) and then reads it as
 * the slab_page_start base. The 8-page loop then references addresses
 * [slab_page_start, slab_page_start + 8 * PAGE_2M_SIZE) — but only as
 * Phy_to_2M_Page inputs (pointer math, never loaded), so we don't need
 * to back those. We do need [slab_start_pa, slab_start_pa + ~22 KiB)
 * writable. Allocate 8 MiB and round up to a 2 MiB boundary inside
 * the allocation. */
#define META_BUF_BYTES (8 * 1024 * 1024)
static uint8_t *meta_buf;
static uint64_t slab_start_pa;     /* 2 MiB-aligned VA inside meta_buf */

/* Indices of the pages slab_init will mark in the second call
 * (computed from slab_start_pa; depends only on the 2 MiB alignment,
 * which we enforce). */
static int jloop_page_idx;         /* page index touched by the j-loop */
static int first_slab_page_idx;    /* first page index of the 8-page loop */

static void setup(void)
{
    if (!meta_buf) {
        /* Use MAP_FIXED at a low address (1 GiB) so slab_init's
         * Phy_to_2M_Page arithmetic produces a small page index that
         * fits in fixture_pages[]. Without this, glibc's default mmap
         * hands back addresses in the 0x7f... range where page_index
         * >> 21 lands far outside our 256-page fixture. We don't need
         * MAP_32BIT (which requires _GNU_SOURCE on some libcs); the
         * kernel will return whatever low address we ask for as long
         * as the range is unmapped. */
        const uint64_t target = 1UL << 30;   /* 1 GiB, well above the
                                              * null page and the
                                              * kernel-vsomap region. */
        meta_buf = (uint8_t *)mmap((void *)target, META_BUF_BYTES,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                   -1, 0);
        if (meta_buf == MAP_FAILED) {
            fprintf(stderr, "FAIL: mmap MAP_FIXED@1GiB failed\n");
            exit(2);
        }
    }
    /* Round up to 2 MiB inside the allocation; the metadata footprint
     * (~22 KiB) easily fits in the remaining 6 MiB+ tail. */
    slab_start_pa = ((uint64_t)meta_buf + PAGE_2M_SIZE - 1) & PAGE_2M_MASK;
    jloop_page_idx      = (int)(slab_start_pa >> PAGE_2M_SHIFT);
    first_slab_page_idx = jloop_page_idx + 1;

    /* Reset PMMngr + fixture to a fresh state. The metadata buffer
     * is also zeroed so slab_init's first loop sees all-zero slots
     * (in production this memory is post-BSS and similarly empty). */
    memset(meta_buf, 0, META_BUF_BYTES);
    memset(fixture_pages, 0, sizeof(fixture_pages));
    memset(&fixture_zone, 0, sizeof(fixture_zone));
    memset(fixture_bits, 0, sizeof(fixture_bits));

    for (int i = 0; i < N_PAGES; i++) {
        fixture_pages[i].zone_struct     = &fixture_zone;
        fixture_pages[i].phy_address     = (uint64_t)i * PAGE_2M_SIZE;
        fixture_pages[i].attribute       = 0;
        fixture_pages[i].reference_count = 0;
        fixture_pages[i].age             = 0;
    }
    fixture_zone.pages_group         = fixture_pages;
    fixture_zone.pages_length        = N_PAGES;
    fixture_zone.zone_start_address  = 0;
    fixture_zone.zone_end_address    = N_PAGES * PAGE_2M_SIZE;
    fixture_zone.zone_length         = N_PAGES * PAGE_2M_SIZE;
    fixture_zone.page_using_count    = 0;
    fixture_zone.page_free_count     = N_PAGES;
    fixture_zone.total_pages_link    = 0;
    fixture_zone.manager_struct      = &PMMngr;

    PMMngr.pages_struct  = fixture_pages;
    PMMngr.pages_size    = N_PAGES;
    PMMngr.pages_length  = N_PAGES * sizeof(struct Page);
    PMMngr.bits_map      = fixture_bits;
    PMMngr.bits_size     = N_PAGES;
    PMMngr.bits_length   = (N_PAGES / 8);
    PMMngr.zones_struct  = &fixture_zone;
    PMMngr.zones_size    = 1;
    PMMngr.zones_length  = sizeof(struct Zone);
    PMMngr.start_brk     = 0;
    PMMngr.end_of_struct = slab_start_pa;     /* 2 MiB-aligned */
    PMMngr.start_code    = 0;
    PMMngr.end_code      = 0;
    PMMngr.end_data      = 0;
    PMMngr.end_rodata    = 0;
    ZONE_DMA_INDEX       = 0;

    /* After the metadata loop, end_of_struct = slab_start_pa + ~22 KiB,
     * which is still inside the same 2 MiB page as slab_start_pa
     * (22 KiB < 2 MiB), so:
     *   j-loop page index:         slab_start_pa >> 21
     *   slab_page_start (8-page):  slab_start_pa + 2 MiB
     *   8-page loop page indices:  (slab_start_pa >> 21) + 1 .. + 8
     */
    first_slab_page_idx = jloop_page_idx + 1;
}

/* Mark `count` consecutive pages starting at `first_idx` as
 * boot-reserved in the bitmap; return the bitmask that was set. */
static uint64_t pre_reserve(int first_idx, int count)
{
    uint64_t mask = 0;
    for (int i = 0; i < count; i++) {
        int idx = first_idx + i;
        uint64_t bit = 1UL << (idx & 63);
        mask |= bit;
        fixture_bits[idx >> 6] |= bit;
    }
    fixture_zone.page_using_count += count;
    fixture_zone.page_free_count  -= count;
    return mask;
}

/* Snapshot of `using_count` before a call, used in delta-form
 * assertions so the test stays readable. */
static uint64_t using_count_snapshot(void)
{
    return fixture_zone.page_using_count;
}

/* ── Test 1: 8-page loop is idempotent vs boot reservation ─────
 *
 * Pre-set the 8 slab pages (boot reservation replay). After slab_init:
 *   GREEN: using_count delta = +1 (only the j-loop page, which was
 *          NOT pre-reserved — its bit was clear, so the j-loop ORs
 *          it in and ++ as before).
 *   RED:   using_count delta = +9 (j-loop + 8 double-counted frames).
 *
 * Layout: jloop_page_idx is the j-loop page (NOT pre-set);
 * first_slab_page_idx..first_slab_page_idx+7 are the 8 slab pages
 * (PRE-SET, simulating boot). */
TEST_FUNC(test_8page_loop_idempotent_vs_boot_reservation)
{
    setup();
    /* Pre-set the 8 slab pages only; j-loop page is intentionally
     * left free so the test isolates the 8-page loop's ++/-- on
     * pre-set frames. */
    uint64_t slab_mask = pre_reserve(first_slab_page_idx, 8);
    assert_eq(using_count_snapshot(), 8UL);

    slab_init();

    /* GREEN: only the j-loop page contributes +1. RED: +1 + 8 = +9. */
    assert_eq(fixture_zone.page_using_count, 9UL);
    assert_eq(fixture_zone.page_free_count, (uint64_t)(N_PAGES - 9));
    /* The 8 pre-set bits are still set (OR no-op + skipped ++), and
     * the j-loop page bit is now set too (its OR ran). */
    assert_true((fixture_bits[first_slab_page_idx >> 6] &
                 ((uint64_t)slab_mask)) == slab_mask);
    assert_true(fixture_bits[jloop_page_idx >> 6] &
                (1UL << (jloop_page_idx & 63)));
    /* page_init's reference_count was incremented for the j-loop
     * page but NOT for any of the 8 pre-set frames. */
    assert_eq(fixture_pages[jloop_page_idx].reference_count, 1UL);
    for (int i = 0; i < 8; i++) {
        assert_eq(fixture_pages[first_slab_page_idx + i].reference_count, 0UL);
    }
}

/* ── Test 2: clean boot — each frame marked exactly once ──────
 *
 * No pre-reservation. After slab_init:
 *   using_count = 9 (1 j-loop page + 8 slab pages).
 *
 * Passes for both RED and GREEN — this is the regression guard
 * against the fix accidentally NOT marking frames on a clean boot
 * (which would silently lose 8 frames from the zone's using_count
 * tally and confuse any subsequent free_pages on those frames). */
TEST_FUNC(test_clean_boot_marks_each_frame_once)
{
    setup();
    assert_eq(using_count_snapshot(), 0UL);

    slab_init();

    assert_eq(fixture_zone.page_using_count, 9UL);
    assert_eq(fixture_zone.page_free_count, (uint64_t)(N_PAGES - 9));
    /* All 9 bits are set: j-loop page + 8 slab pages. */
    assert_true(fixture_bits[jloop_page_idx >> 6] &
                (1UL << (jloop_page_idx & 63)));
    for (int i = 0; i < 8; i++) {
        int idx = first_slab_page_idx + i;
        assert_true(fixture_bits[idx >> 6] & (1UL << (idx & 63)));
    }
}

/* ── Test 3: j-loop idempotent vs boot reservation ─────────────
 *
 * Mirror of test 1 for the j-loop: pre-set the j-loop page (and the
 * 8 slab pages, which are also pre-reserved in any real boot), then
 * call slab_init. Both loops must skip ++/-- for pre-set frames.
 *   GREEN: using_count unchanged at 9 (initial = 8 slab + 1 j-loop).
 *   RED:   using_count += 9 (both loops ++ on every pre-set frame).
 *
 * This is the bug-bugged "all 9 pre-set" case — the strongest
 * possible regression for the fix. */
TEST_FUNC(test_jloop_and_8page_both_idempotent)
{
    setup();
    pre_reserve(jloop_page_idx, 1);
    pre_reserve(first_slab_page_idx, 8);
    assert_eq(using_count_snapshot(), 9UL);

    slab_init();

    assert_eq(fixture_zone.page_using_count, 9UL);
    assert_eq(fixture_zone.page_free_count, (uint64_t)(N_PAGES - 9));
    /* Every touched page has reference_count = 0 (page_init skipped
     * because the bit was already set, so ++ never ran). */
    assert_eq(fixture_pages[jloop_page_idx].reference_count, 0UL);
    for (int i = 0; i < 8; i++) {
        assert_eq(fixture_pages[first_slab_page_idx + i].reference_count, 0UL);
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_8page_loop_idempotent_vs_boot_reservation),
    TEST_ENTRY(test_clean_boot_marks_each_frame_once),
    TEST_ENTRY(test_jloop_and_8page_both_idempotent),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}