/*
 * hosttests/cases/test_m1_reservation.c — range-based boot reservations +
 *                                            single-frame exact claim
 *                                            (aarch64 M1 plan Task 2).
 *
 * Companion to test_m1_layout.c. Task 1 extracted the PMM metadata
 * layout calculator; Task 2 replaces the legacy "walk page indices from
 * 0 up to walk_pages" reservation loop with a strategy-driven range
 * approach (pmm_arch_boot_reservations → pmm_reserve_boot_ranges) and
 * adds pmm_claim_free_frame for arena/M0 runtime single-frame claim.
 *
 * The tests link the REAL kernel/memory/pmm.c (the production object
 * that owns PMMngr and pmm_lock) and the REAL kernel/memory/pmm_arch.c
 * (the weak-default strategy). The fixture sets up PMMngr by hand so
 * the test exercises the helper's interaction with a multi-zone, non-
 * zero RAM-base bitmap — paths the legacy loop didn't have to reason
 * about.
 *
 * Fixture: three zones (ZONE_SPAN each) interleaved with holes
 * (HOLE_SPAN each), all 2 MiB-aligned. For nonzero-base tests the
 * lowest frame is at BASE > 0; RAM-relative bit indexing must follow.
 *
 *   base ─── Z0 ─── HOLE ─── Z1 ─── HOLE ─── Z2 ─── end
 *   BASE     ZONE_SPAN    ZONE_SPAN        ZONE_SPAN
 *
 * Assertions pin:
 *   - sparse zones (range crosses zone boundaries + holes)
 *   - non-zero lowest PA (range sits in the upper half of the bitmap)
 *   - arena in second zone (single-frame claim targets zone 1)
 *   - adjacent frames (back-to-back reservation boundary)
 *   - double / overlap reservation (idempotent: second call changes nothing)
 *   - invalid ranges (start > end, unaligned, NULL pm, NULL ranges w/ count)
 *   - exact claim: lowest / highest free frame, empty range, occupied range,
 *     bitmap changes under pmm_lock, never claim a hole
 *
 * Bitmap / counter / attribute / refcount assertions verify both the
 * happy paths and the "reserved → reserved" idempotence invariant.
 */
#include "test_framework.h"
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/pmm_arch.h>

extern struct Physical_Memory_Manager PMMngr;
extern uint32_t ZONE_DMA_INDEX;

/* ── link stubs for externs pmm.o references ──────────────
 * The production pmm.c calls g_log_level / _log_err_impl / color_printk /
 * slab_init / pmm_arch_normalize / pmm_arch_zone_split / pmm_layout_calculate.
 * We provide host-side no-ops for the first four; pmm_arch_normalize +
 * zone_split are satisfied by pmm_arch_default.o (linked via Makefile);
 * pmm_layout_calculate is satisfied by pmm_boot_production.o. */
int g_log_level = 3;   /* LOG_ERR */
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* ── Fixture ────────────────────────────────────────────────
 * 3 zones * 128 frames = 384 frames total. Each frame is 2 MiB. The
 * 2-MiB-aligned regions are placed at BASE, BASE+ZONE_SPAN+HOLE_SPAN,
 * BASE+2*(ZONE_SPAN+HOLE_SPAN). Holes between zones leave NULL zone_struct
 * rows in pages_struct. The RAM-relative index of the first frame is 0. */
#define N_FRAMES_PER_ZONE 128UL
#define ZONE_SPAN         (N_FRAMES_PER_ZONE * PAGE_2M_SIZE)  /* 256 MiB */
#define HOLE_SPAN         ZONE_SPAN
#define N_ZONES           3UL
#define N_TOTAL_FRAMES    (N_ZONES * N_FRAMES_PER_ZONE)        /* 384 */
#define BITS_LENGTH       (((N_TOTAL_FRAMES + 63UL) & ~63UL) / 8UL)
#define BITS_WORDS        (BITS_LENGTH / sizeof(uint64_t))

static struct Page   fixture_pages[N_TOTAL_FRAMES];
static struct Zone   fixture_zones[N_ZONES];
static uint64_t      fixture_bits[BITS_WORDS];
static uint64_t      fixture_lowest_pa;   /* = pages_struct[0].phy_address */

/* Reset PMMngr + fixture to a sparse 3-zone layout at base_pa. */
static void setup_sparse(uint64_t base_pa)
{
    fixture_lowest_pa = base_pa;
    memset(fixture_pages, 0, sizeof(fixture_pages));
    memset(fixture_zones, 0, sizeof(fixture_zones));
    memset(fixture_bits,  0, sizeof(fixture_bits));

    for (size_t zi = 0; zi < N_ZONES; zi++) {
        uint64_t zone_start = base_pa + zi * (ZONE_SPAN + HOLE_SPAN);
        uint64_t zone_end   = zone_start + ZONE_SPAN;
        struct Zone *z = &fixture_zones[zi];
        z->zone_start_address = zone_start;
        z->zone_end_address   = zone_end;
        z->zone_length        = ZONE_SPAN;
        z->page_using_count   = 0;
        z->page_free_count    = N_FRAMES_PER_ZONE;
        z->total_pages_link   = 0;
        z->attribute          = 0;
        z->manager_struct     = &PMMngr;
        z->pages_group        = &fixture_pages[zi * N_FRAMES_PER_ZONE];
        z->pages_length       = N_FRAMES_PER_ZONE;
        for (size_t j = 0; j < N_FRAMES_PER_ZONE; j++) {
            struct Page *p = &fixture_pages[zi * N_FRAMES_PER_ZONE + j];
            p->zone_struct     = z;
            p->phy_address     = zone_start + j * PAGE_2M_SIZE;
            p->attribute       = 0;
            p->reference_count = 0;
            p->age             = 0;
            /* mark free (bit cleared) — production pmm_init Step 4 XORs
             * the bit to flip a fully-set 0xff buffer; our test starts
             * with a zero buffer so clearing the bit means free. */
            uint64_t rel_idx = (uint64_t)(p - fixture_pages);
            fixture_bits[rel_idx >> 6] &= ~(1UL << (rel_idx % 64));
        }
    }

    /* Holes: leave the corresponding pages_struct slots zone_struct=NULL
     * and the bit cleared. pages_struct is densely packed (RAM-relative
     * indices 0..N_TOTAL_FRAMES-1), so the holes between zones must NOT
     * be represented in pages_struct — i.e. pages_struct[ZONE_END_i..HOLE_END_i)
     * is NOT allocated. Our fixture therefore omits those indices entirely;
     * pages_struct has only N_TOTAL_FRAMES entries (one per represented
     * frame, packed in RAM-relative order). This mirrors pmm_init Step 4
     * which only assigns pages_struct slots to represented RAM. */
    PMMngr.pages_struct  = fixture_pages;
    PMMngr.pages_size    = N_TOTAL_FRAMES;
    PMMngr.pages_length  = N_TOTAL_FRAMES * sizeof(struct Page);
    PMMngr.bits_map      = fixture_bits;
    PMMngr.bits_size     = N_TOTAL_FRAMES;
    PMMngr.bits_length   = BITS_LENGTH;
    PMMngr.zones_struct  = fixture_zones;
    PMMngr.zones_size    = N_ZONES;
    PMMngr.zones_length  = N_ZONES * sizeof(struct Zone);
    PMMngr.start_brk     = 0;
    PMMngr.end_of_struct = 0;
    ZONE_DMA_INDEX       = 0;
}

/* Convenience: how many RAM-relative page indices fall in [pa_lo, pa_hi). */
static size_t count_reserved(uint64_t pa_lo, uint64_t pa_hi)
{
    size_t n = 0;
    for (size_t i = 0; i < N_TOTAL_FRAMES; i++) {
        uint64_t pa = fixture_pages[i].phy_address;
        if (pa >= pa_lo && pa < pa_hi &&
            (fixture_bits[i >> 6] & (1UL << (i % 64)))) n++;
    }
    return n;
}

/* ── pmm_reserve_boot_ranges: sparse zones ─────────────────
 * Reserve the first 64 frames of zone 0 + the first 64 frames of zone 2.
 * Holes (no entries in pages_struct) must NOT flip any bits. Zone 1's
 * frames stay free. */
TEST_FUNC(test_reserve_sparse_zones)
{
    setup_sparse(0);
    uint64_t z0_start = fixture_lowest_pa;
    uint64_t z0_end   = z0_start + 64 * PAGE_2M_SIZE;
    uint64_t z2_start = fixture_lowest_pa + 2 * (ZONE_SPAN + HOLE_SPAN);
    uint64_t z2_end   = z2_start + 64 * PAGE_2M_SIZE;
    struct pmm_phys_range ranges[2] = {
        { .start = z0_start, .end = z0_end },
        { .start = z2_start, .end = z2_end },
    };

    int rc = pmm_reserve_boot_ranges(&PMMngr, ranges, 2);
    assert_eq(0, rc);
    assert_eq((size_t)64, count_reserved(z0_start, z0_end));
    assert_eq((size_t)64, count_reserved(z2_start, z2_end));
    /* Zone 1 untouched. */
    uint64_t z1_start = fixture_lowest_pa + (ZONE_SPAN + HOLE_SPAN);
    uint64_t z1_end   = z1_start + ZONE_SPAN;
    assert_eq((size_t)0, count_reserved(z1_start, z1_end));
}

/* ── pmm_reserve_boot_ranges: non-zero lowest PA ────────── */
TEST_FUNC(test_reserve_nonzero_base)
{
    const uint64_t base = 0x40000000ULL;   /* 1 GiB */
    setup_sparse(base);
    uint64_t z0_start = base;
    uint64_t z0_end   = z0_start + 32 * PAGE_2M_SIZE;
    struct pmm_phys_range ranges[1] = { { .start = z0_start, .end = z0_end } };

    int rc = pmm_reserve_boot_ranges(&PMMngr, ranges, 1);
    assert_eq(0, rc);
    /* Bit indices are RAM-relative: the first 32 bits in the bitmap
     * (i.e. the low half of word 0) must be set. */
    assert_eq((uint64_t)((1UL << 32) - 1UL), fixture_bits[0]);
    assert_eq((size_t)32, count_reserved(z0_start, z0_end));
    /* Pages struct phy_address remains absolute (RAM base + rel * 2M). */
    assert_true(fixture_pages[0].phy_address == base);
    assert_true(fixture_pages[31].phy_address == base + 31 * PAGE_2M_SIZE);
}

/* ── pmm_reserve_boot_ranges: arena in second zone ──────── */
TEST_FUNC(test_reserve_arena_in_second_zone)
{
    setup_sparse(0);
    uint64_t z1_start = fixture_lowest_pa + (ZONE_SPAN + HOLE_SPAN);
    /* Arena lives somewhere in the middle of zone 1, with a 2-frame
     * alignment on either side left free. */
    uint64_t arena_lo = z1_start + 32 * PAGE_2M_SIZE;
    uint64_t arena_hi = arena_lo  + 4 * PAGE_2M_SIZE;
    struct pmm_phys_range ranges[1] = { { .start = arena_lo, .end = arena_hi } };

    int rc = pmm_reserve_boot_ranges(&PMMngr, ranges, 1);
    assert_eq(0, rc);
    assert_eq((size_t)4, count_reserved(arena_lo, arena_hi));
    /* Frames before and after the arena remain free. */
    assert_eq((size_t)0, count_reserved(z1_start, arena_lo));
    assert_eq((size_t)0, count_reserved(arena_hi, z1_start + ZONE_SPAN));
}

/* ── pmm_reserve_boot_ranges: adjacent frames ─────────────
 * Reserve the trailing 16 frames of zone 0 AND the leading 16 frames of
 * zone 2 as two adjacent ranges. The bitmap must reflect the boundary
 * without bleed (zone 1 untouched, hole untouched). */
TEST_FUNC(test_reserve_adjacent_frames)
{
    setup_sparse(0);
    uint64_t z0_lo = fixture_lowest_pa + (ZONE_SPAN - 16 * PAGE_2M_SIZE);
    uint64_t z0_hi = fixture_lowest_pa + ZONE_SPAN;
    uint64_t z2_lo = fixture_lowest_pa + 2 * (ZONE_SPAN + HOLE_SPAN);
    uint64_t z2_hi = z2_lo + 16 * PAGE_2M_SIZE;
    struct pmm_phys_range ranges[2] = {
        { .start = z0_lo, .end = z0_hi },
        { .start = z2_lo, .end = z2_hi },
    };
    int rc = pmm_reserve_boot_ranges(&PMMngr, ranges, 2);
    assert_eq(0, rc);
    assert_eq((size_t)16, count_reserved(z0_lo, z0_hi));
    assert_eq((size_t)16, count_reserved(z2_lo, z2_hi));
    /* The trailing 16 frames of zone 0 are at indices [112,128) in
     * pages_struct; verify the corresponding bits in the bitmap. */
    uint64_t word = fixture_bits[112 >> 6];
    uint64_t mask = ((1UL << 16) - 1UL) << (112 % 64);
    assert_eq(mask, word);
    /* Zone 2's leading 16 frames are at indices [256,272). */
    word = fixture_bits[256 >> 6];
    mask = ((1UL << 16) - 1UL) << (256 % 64);
    assert_eq(mask, word);
}

/* ── pmm_reserve_boot_ranges: double reservation is idempotent ── */
TEST_FUNC(test_reserve_idempotent)
{
    setup_sparse(0);
    uint64_t z0_lo = fixture_lowest_pa;
    uint64_t z0_hi = z0_lo + 8 * PAGE_2M_SIZE;
    struct pmm_phys_range ranges[1] = { { .start = z0_lo, .end = z0_hi } };

    int rc = pmm_reserve_boot_ranges(&PMMngr, ranges, 1);
    assert_eq(0, rc);
    /* Capture state after first call. */
    uint64_t bits_after_first[BITS_WORDS];
    memcpy(bits_after_first, fixture_bits, sizeof(fixture_bits));
    size_t using0_first = fixture_zones[0].page_using_count;
    size_t free0_first  = fixture_zones[0].page_free_count;
    size_t link0_first  = fixture_zones[0].total_pages_link;

    /* Second call: must not change anything. */
    rc = pmm_reserve_boot_ranges(&PMMngr, ranges, 1);
    assert_eq(0, rc);
    assert_eq(0, memcmp(bits_after_first, fixture_bits, sizeof(fixture_bits)));
    assert_eq(using0_first, fixture_zones[0].page_using_count);
    assert_eq(free0_first,  fixture_zones[0].page_free_count);
    assert_eq(link0_first,  fixture_zones[0].total_pages_link);
}

/* ── pmm_reserve_boot_ranges: overlapping ranges ──────────
 * Two ranges overlap by 4 frames. The overlap region must be reserved
 * exactly once; total counters across the union match the union size. */
TEST_FUNC(test_reserve_overlapping_ranges)
{
    setup_sparse(0);
    uint64_t r1_lo = fixture_lowest_pa;
    uint64_t r1_hi = r1_lo + 32 * PAGE_2M_SIZE;
    uint64_t r2_lo = r1_lo + 28 * PAGE_2M_SIZE;     /* overlap = 4 frames */
    uint64_t r2_hi = r1_lo + 64 * PAGE_2M_SIZE;
    struct pmm_phys_range r1 = { .start = r1_lo, .end = r1_hi };
    struct pmm_phys_range r2 = { .start = r2_lo, .end = r2_hi };

    int rc = pmm_reserve_boot_ranges(&PMMngr, r1.start ? &r1 : &r1, 1);
    assert_eq(0, rc);
    rc = pmm_reserve_boot_ranges(&PMMngr, &r2, 1);
    assert_eq(0, rc);
    /* Union spans [r1_lo, r2_hi) = 64 frames. */
    assert_eq((size_t)64, count_reserved(r1_lo, r2_hi));
    assert_eq((uint64_t)64, fixture_zones[0].page_using_count);
    assert_eq((uint64_t)(N_FRAMES_PER_ZONE - 64), fixture_zones[0].page_free_count);
}

/* ── pmm_reserve_boot_ranges: invalid inputs ────────────── */
TEST_FUNC(test_reserve_invalid_inputs)
{
    setup_sparse(0);
    /* count > 0 with NULL ranges */
    struct pmm_phys_range dummy = { .start = 0, .end = PAGE_2M_SIZE };
    assert_eq(-EINVAL, pmm_reserve_boot_ranges(&PMMngr, NULL, 1));
    /* NULL pm */
    assert_eq(-EINVAL, pmm_reserve_boot_ranges(NULL, &dummy, 1));
    /* start > end */
    struct pmm_phys_range bad = { .start = PAGE_2M_SIZE, .end = 0 };
    assert_eq(-EINVAL, pmm_reserve_boot_ranges(&PMMngr, &bad, 1));
    /* start == end (zero-length) */
    struct pmm_phys_range zero = { .start = 0, .end = 0 };
    assert_eq(-EINVAL, pmm_reserve_boot_ranges(&PMMngr, &zero, 1));
    /* unaligned start */
    struct pmm_phys_range unaligned = { .start = 1, .end = PAGE_2M_SIZE };
    assert_eq(-EINVAL, pmm_reserve_boot_ranges(&PMMngr, &unaligned, 1));
    /* unaligned end */
    struct pmm_phys_range unaligned_end = {
        .start = 0, .end = PAGE_2M_SIZE + 1
    };
    assert_eq(-EINVAL, pmm_reserve_boot_ranges(&PMMngr, &unaligned_end, 1));
    /* count == 0 is a valid no-op */
    assert_eq(0, pmm_reserve_boot_ranges(&PMMngr, NULL, 0));
    /* nothing got reserved by any of the failed calls */
    assert_eq((size_t)0, count_reserved(0, UINT64_MAX));
}

/* ── pmm_reserve_boot_ranges: bitmap/counter/attribute assertions ──
 * Verify the production helpers actually mark pages with the right
 * attribute, the right flags, the right counter delta. */
TEST_FUNC(test_reserve_sets_attributes_and_counters)
{
    setup_sparse(0);
    uint64_t lo = fixture_lowest_pa + 10 * PAGE_2M_SIZE;
    uint64_t hi = lo + 6 * PAGE_2M_SIZE;
    struct pmm_phys_range ranges[1] = { { .start = lo, .end = hi } };

    int rc = pmm_reserve_boot_ranges(&PMMngr, ranges, 1);
    assert_eq(0, rc);
    for (size_t i = 0; i < N_TOTAL_FRAMES; i++) {
        struct Page *p = &fixture_pages[i];
        bool in_range = (p->phy_address >= lo && p->phy_address < hi);
        if (in_range) {
            /* Reserved: bit set, attribute carries PTable_Mapped +
             * Kernel_Init + Kernel, refcount > 0 (page_init bumped it). */
            uint64_t word = fixture_bits[i >> 6];
            assert_true(word & (1UL << (i % 64)));
            assert_true(p->attribute & PG_PTable_Mapped);
            assert_true(p->attribute & PG_Kernel_Init);
            assert_true(p->attribute & PG_Kernel);
            assert_true(p->reference_count >= 1);
        } else {
            assert_eq(0, fixture_bits[i >> 6] & (1UL << (i % 64)));
        }
    }
    assert_eq((uint64_t)6, fixture_zones[0].page_using_count);
    assert_eq((uint64_t)(N_FRAMES_PER_ZONE - 6), fixture_zones[0].page_free_count);
}

/* ── pmm_claim_free_frame: lowest free frame ────────────── */
TEST_FUNC(test_claim_lowest_free_frame)
{
    setup_sparse(0);
    /* Reserve the first 4 frames so claim picks index 4. */
    uint64_t lo = fixture_lowest_pa;
    uint64_t hi = lo + 4 * PAGE_2M_SIZE;
    struct pmm_phys_range r[1] = { { .start = lo, .end = hi } };
    pmm_reserve_boot_ranges(&PMMngr, r, 1);

    struct Page *p = pmm_claim_free_frame(fixture_lowest_pa,
                                         fixture_lowest_pa + 64 * PAGE_2M_SIZE,
                                         /*from_end=*/false);
    assert_not_null(p);
    /* Lowest free frame in zone 0 is at index 4 (phy_address = base + 4*2M). */
    assert_eq(fixture_lowest_pa + 4 * PAGE_2M_SIZE, p->phy_address);
    assert_eq(4, p - fixture_pages);  /* relative index = 4 within zone 0 */
    assert_true(p->attribute & PG_PTable_Mapped);
    assert_true(p->zone_struct == &fixture_zones[0]);
    /* Bit set, counters updated. */
    uint64_t rel_idx = (uint64_t)(p - fixture_pages);
    assert_true(fixture_bits[rel_idx >> 6] & (1UL << (rel_idx % 64)));
}

/* ── pmm_claim_free_frame: highest free frame ───────────── */
TEST_FUNC(test_claim_highest_free_frame)
{
    setup_sparse(0);
    /* Reserve all but the last 2 frames of zone 0. */
    uint64_t lo = fixture_lowest_pa;
    uint64_t hi = lo + (N_FRAMES_PER_ZONE - 2) * PAGE_2M_SIZE;
    struct pmm_phys_range r[1] = { { .start = lo, .end = hi } };
    pmm_reserve_boot_ranges(&PMMngr, r, 1);

    struct Page *p = pmm_claim_free_frame(fixture_lowest_pa,
                                         fixture_lowest_pa + ZONE_SPAN,
                                         /*from_end=*/true);
    assert_not_null(p);
    /* Highest free frame in zone 0 is at index N_FRAMES_PER_ZONE - 1
     * (frames [0, N_FRAMES_PER_ZONE - 2) are reserved). */
    assert_eq(fixture_lowest_pa + (N_FRAMES_PER_ZONE - 1) * PAGE_2M_SIZE,
              p->phy_address);
}

/* ── pmm_claim_free_frame: arena in zone 1 ──────────────── */
TEST_FUNC(test_claim_arena_in_second_zone)
{
    setup_sparse(0);
    /* Request a single frame inside zone 1 — must skip zone 0 and the
     * hole (pages_struct carries no entries for holes, so the scan
     * naturally skips them) and return the lowest free zone-1 frame. */
    uint64_t z1_lo = fixture_lowest_pa + (ZONE_SPAN + HOLE_SPAN);
    uint64_t z1_hi = z1_lo + ZONE_SPAN;
    struct Page *p = pmm_claim_free_frame(z1_lo, z1_hi, /*from_end=*/false);
    assert_not_null(p);
    assert_true(p->phy_address == z1_lo);
    assert_true(p->zone_struct == &fixture_zones[1]);
}

/* ── pmm_claim_free_frame: empty / occupied ranges ─────── */
TEST_FUNC(test_claim_empty_or_occupied_returns_null)
{
    setup_sparse(0);
    /* Empty range (end <= start). */
    assert_null(pmm_claim_free_frame(0, 0, false));
    assert_null(pmm_claim_free_frame(PAGE_2M_SIZE, 0, false));
    /* Occupied range: reserve everything in zone 0, then ask for it. */
    struct pmm_phys_range r[1] = {
        { .start = fixture_lowest_pa, .end = fixture_lowest_pa + ZONE_SPAN }
    };
    pmm_reserve_boot_ranges(&PMMngr, r, 1);
    assert_null(pmm_claim_free_frame(fixture_lowest_pa,
                                     fixture_lowest_pa + ZONE_SPAN, false));
    /* Range entirely inside a hole (between zone 0 and zone 1) — no
     * represented RAM, no free frames, must return NULL. */
    uint64_t hole_lo = fixture_lowest_pa + ZONE_SPAN;
    uint64_t hole_hi = hole_lo + HOLE_SPAN;
    assert_null(pmm_claim_free_frame(hole_lo, hole_hi, false));
}

/* ── pmm_claim_free_frame: never claims a hole ──────────── */
TEST_FUNC(test_claim_skips_hole)
{
    setup_sparse(0);
    /* Range starts in zone 0, extends through the hole into zone 1.
     * Claim must return a zone-0 frame (zone 1 is hole-free in the
     * pages_struct index space; we want to prove the scan doesn't
     * confuse hole PAs with frames). */
    uint64_t lo = fixture_lowest_pa;
    uint64_t hi = fixture_lowest_pa + ZONE_SPAN + HOLE_SPAN;   /* covers hole */
    struct Page *p = pmm_claim_free_frame(lo, hi, false);
    assert_not_null(p);
    assert_true(p->phy_address == fixture_lowest_pa);
    assert_true(p->zone_struct == &fixture_zones[0]);
}

/* ── pmm_claim_free_frame: bitmap + counter update under lock ─
 * After claim returns, the corresponding bit is set and counters
 * move. Verifies the function is atomic w.r.t. the bitmap.
 * (Renamed from test_claim_atomic_bitmap_update per review M2 —
 * "atomic" is misleading because the host shadow spinlock is a
 * no-op; this test really exercises the bitmap + counter side
 * effects of a single successful claim.) */
TEST_FUNC(test_claim_updates_bitmap_and_counters)
{
    setup_sparse(0);
    struct Page *p = pmm_claim_free_frame(fixture_lowest_pa,
                                         fixture_lowest_pa + ZONE_SPAN,
                                         /*from_end=*/false);
    assert_not_null(p);
    size_t idx = (size_t)(p - fixture_pages);
    assert_true(fixture_bits[idx >> 6] & (1UL << (idx % 64)));
    assert_eq((uint64_t)1, p->zone_struct->page_using_count);
    assert_eq((uint64_t)(N_FRAMES_PER_ZONE - 1),
              p->zone_struct->page_free_count);
}

/* ── pmm_claim_free_frame: invalid range returns NULL ───── */
TEST_FUNC(test_claim_invalid_range)
{
    setup_sparse(0);
    /* end == start (zero length) */
    assert_null(pmm_claim_free_frame(PAGE_2M_SIZE, PAGE_2M_SIZE, false));
    /* end < start */
    assert_null(pmm_claim_free_frame(PAGE_2M_SIZE, 0, false));
    /* PA entirely above highest represented RAM — no frames exist there */
    uint64_t above = fixture_lowest_pa + 3 * (ZONE_SPAN + HOLE_SPAN);
    assert_null(pmm_claim_free_frame(above, above + PAGE_2M_SIZE, false));
}

/* ── pmm_claim_free_frame: nonzero base ─────────────────── */
TEST_FUNC(test_claim_nonzero_base)
{
    const uint64_t base = 0x100000000ULL;   /* 4 GiB */
    setup_sparse(base);
    struct Page *p = pmm_claim_free_frame(base, base + ZONE_SPAN, false);
    assert_not_null(p);
    assert_eq(base, p->phy_address);
    assert_eq(0, p - fixture_pages);
    /* Relative index 0 → bit 0 set in word 0. */
    assert_true(fixture_bits[0] & 1UL);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_reserve_sparse_zones),
    TEST_ENTRY(test_reserve_nonzero_base),
    TEST_ENTRY(test_reserve_arena_in_second_zone),
    TEST_ENTRY(test_reserve_adjacent_frames),
    TEST_ENTRY(test_reserve_idempotent),
    TEST_ENTRY(test_reserve_overlapping_ranges),
    TEST_ENTRY(test_reserve_invalid_inputs),
    TEST_ENTRY(test_reserve_sets_attributes_and_counters),
    TEST_ENTRY(test_claim_lowest_free_frame),
    TEST_ENTRY(test_claim_highest_free_frame),
    TEST_ENTRY(test_claim_arena_in_second_zone),
    TEST_ENTRY(test_claim_empty_or_occupied_returns_null),
    TEST_ENTRY(test_claim_skips_hole),
    TEST_ENTRY(test_claim_updates_bitmap_and_counters),
    TEST_ENTRY(test_claim_invalid_range),
    TEST_ENTRY(test_claim_nonzero_base),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
