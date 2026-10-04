/*
 * hosttests/cases/test_slab_basic_x86_count.c — x86 baseline regression
 * for slab_init's frame-count arithmetic (aarch64 M2/M3 plan Task 2
 * Step 3).
 *
 * After Task 2 wraps the j-loop and 8-page loop with the idempotent
 * `if (bits_map[...] & bit) continue` check, we need a regression guard
 * that the x86 path (no boot pre-reservation → bits_map fully zero at
 * entry) still counts every frame exactly once. The fix must NOT
 * regress this case — it must only skip the ++/-- when the bit is
 * ALREADY set. If a future edit accidentally puts the `continue` in
 * the wrong branch (e.g. when the bit is NOT set, or removes the
 * `|= bm_bit` from the post-check path) this test catches it.
 *
 * Contract pinned here:
 *   bits_map[0..N-1] = 0
 *   page_using_count = 0
 *   slab_init()
 *   → bits_map[0..N-1] marks exactly 9 distinct frames (1 j-loop
 *     page + 8 slab pages) and page_using_count = 9.
 *
 * Construction mirrors test_slab_idempotent_reservation.c so both tests
 * see the same host surface (same shadow headers, same fixture init, same
 * arch_irq.h / arch/mmu.h overrides). The shared layout also means a
 * regression in the shadow machinery fails both suites simultaneously
 * rather than masking as a slab.c bug.
 *
 * Unlike test_slab_idempotent_reservation.c this file is intentionally
 * a SINGLE-TEST suite — the x86 baseline has one invariant, not three.
 * Keeping it small lets the brief Step 3 wording land literally
 * ("assert increment = 8 + j-loop hits") without competing fixtures.
 */
#include "test_framework.h"
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

/* ── link stubs (mirror test_slab_idempotent_reservation.c) ──────
 * pmm.o references g_log_level / _log_err_impl. color_printk comes
 * from slab_color_stub.o (the MOCK_OBJS equivalent for this suite).
 * `percpu_data[]` storage lives in the slab shadow header. */
int g_log_level = 3;   /* LOG_ERR */
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

/* ── Fixture ───────────────────────────────────────────────────────
 * Same MAP_FIXED@1 GiB / 1024-page layout as test_slab_idempotent_
 * reservation.c — both tests run against the same virtual RAM base
 * so their invariants are directly comparable. */
#define N_PAGES 1024
static struct Page   fixture_pages[N_PAGES];
static struct Zone   fixture_zone;
static uint64_t      fixture_bits[N_PAGES / 64];

#define META_BUF_BYTES (8 * 1024 * 1024)
static uint8_t *meta_buf;
static uint64_t slab_start_pa;       /* 2 MiB-aligned */
static int      jloop_page_idx;      /* depends on slab_start_pa */

static void setup(void)
{
    if (!meta_buf) {
        const uint64_t target = 1UL << 30;   /* 1 GiB */
        meta_buf = (uint8_t *)mmap((void *)target, META_BUF_BYTES,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                   -1, 0);
        if (meta_buf == MAP_FAILED) {
            fprintf(stderr, "FAIL: mmap MAP_FIXED@1GiB failed\n");
            exit(2);
        }
    }
    slab_start_pa = ((uint64_t)meta_buf + PAGE_2M_SIZE - 1) & PAGE_2M_MASK;
    jloop_page_idx = (int)(slab_start_pa >> PAGE_2M_SHIFT);

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
    PMMngr.bits_length   = N_PAGES / 8;
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
}

/* ── Test: x86 baseline — bits_map empty → exactly 9 frames marked ──
 *
 * Reproduces the x86 boot sequence (no pre-reservation): bits_map
 * starts fully zero, the zone counter is zero. After slab_init
 * completes, EXACTLY 9 frames must be marked: 1 j-loop page (the
 * 2 MiB page holding slab's metadata region) + 8 slab pages (the
 * 8 pages reserved for caches 0-7). The 8-page loop's increment
 * must equal 8 (one per frame, never double-counted), and the
 * j-loop must contribute exactly 1.
 *
 * RED triggers:
 *   - Future edit removes the `|= bm_bit` and the counter delta
 *     no longer matches the bit count (off-by-one between bits_map
 *     and page_using_count).
 *   - Future edit accidentally puts `continue` on the wrong branch
 *     of the idempotency check (skips when bit is NOT set, which
 *     would leave most frames unmarked).
 *   - Future edit unconditionally skips ++/-- (the actual bug —
 *     pre-fix code) — would mark 0 frames on a clean boot, breaking
 *     the kernel immediately. */
TEST_FUNC(test_x86_baseline_marks_exactly_9_frames)
{
    setup();
    /* Sanity: fresh state, no pre-reservation. */
    assert_eq(fixture_zone.page_using_count, 0UL);
    for (int i = 0; i < N_PAGES / 64; i++) {
        assert_eq(fixture_bits[i], 0UL);
    }

    slab_init();

    /* j-loop + 8-page loop combined increment = 1 + 8 = 9. */
    assert_eq(fixture_zone.page_using_count, 9UL);
    assert_eq(fixture_zone.page_free_count, (uint64_t)(N_PAGES - 9));
    /* The j-loop page bit is set (the 8-page loop covers a different
     * 2 MiB window so it must NOT have re-set the j-loop page). */
    assert_true(fixture_bits[jloop_page_idx >> 6] &
                (1UL << (jloop_page_idx & 63)));
    /* The 8-page loop pages (jloop_page_idx+1 .. +8) are set. */
    int first_slab = jloop_page_idx + 1;
    int last_slab  = jloop_page_idx + 8;
    /* All 8 must fall within the same bits_map word in this layout
     * (slab_start_pa == 1 GiB, so the indices are 513..520, all in
     * word 8). If a future change to the fixture shifts the layout
     * across a 64-bit boundary, this assertion will fail and force
     * the layout code to revisit the per-word mask. */
    assert_eq(first_slab >> 6, last_slab >> 6);
    uint64_t expected_mask = (((1UL << 8) - 1UL)) << (first_slab & 63);
    assert_eq(fixture_bits[first_slab >> 6] & expected_mask, expected_mask);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_x86_baseline_marks_exactly_9_frames),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}