/*
 * test/cases/test_pmm_ram_rel_index.c — regression: alloc_pages /
 * free_pages must index bits_map RAM-relative.
 *
 * pmm_init Step 4 initializes bits_map with RAM-relative indices
 * (rel_idx = (PA - lowest_ram) >> 21 — the pages_struct slot), but
 * alloc_pages/free_pages historically flipped bits at the ABSOLUTE
 * physical index (phy_address >> 21). On x86_64 QEMU lowest_ram == 0
 * so the two coincide and the bug is invisible; with a nonzero RAM
 * base (aarch64 DRAM at 0x40000000, or any reserved region below
 * the first RAM range) alloc/free flip bits at the wrong index and
 * the allocator can hand out already-owned pages.
 *
 * This suite links the REAL kernel/memory/pmm.c (see the
 * test_pmm_ram_rel_index rules in test/Makefile) and drives
 * alloc_pages/free_pages against a hand-built PMMngr whose RAM base
 * is 0x40000000 (1 GiB), where absolute index (0x200) != relative
 * index (0).
 */
#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <kernel/pmm.h>

extern struct Physical_Memory_Manager PMMngr;
extern uint32_t ZONE_DMA_INDEX;

/* ── link stubs for externs pmm.o references ──────────────
 * g_log_level / _log_err_impl keep pmm.c's log_err error paths
 * linkable; color_printk and slab_init are only reached on paths
 * this suite does not exercise but must still resolve at link. */
int g_log_level = 3;   /* LOG_ERR */
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

#define N_PAGES 64

/* bits_map sized for the RAM-relative span (1 word) plus slack: the
 * buggy absolute-index write for a page at PA 0x40000000 lands in
 * word (0x200 >> 6) == 8, which must stay inside the buffer (no
 * heap corruption) while remaining provably NOT the bit the
 * allocator should flip. */
static uint64_t   bits_map[16];
static struct Page pages[N_PAGES];
static struct Zone  zone;

static void setup(void)
{
    memset(bits_map, 0, sizeof(bits_map));
    memset(pages,   0, sizeof(pages));
    memset(&zone,   0, sizeof(zone));

    /* Nonzero RAM base: DRAM window [0x40000000, 0x40000000 + 128 MiB). */
    const uint64_t base = 0x40000000ULL;
    for (int i = 0; i < N_PAGES; i++) {
        pages[i].zone_struct     = &zone;
        pages[i].phy_address     = base + (uint64_t)i * PAGE_2M_SIZE;
        pages[i].attribute       = 0;
        pages[i].reference_count = 0;
    }
    zone.pages_group         = pages;
    zone.pages_length        = N_PAGES;
    zone.zone_start_address  = base;
    zone.zone_end_address    = base + N_PAGES * PAGE_2M_SIZE;
    zone.page_using_count    = 0;
    zone.page_free_count     = N_PAGES;
    zone.manager_struct      = &PMMngr;

    PMMngr.bits_map      = bits_map;
    PMMngr.pages_struct  = pages;
    PMMngr.zones_struct  = &zone;
    PMMngr.zones_size    = 1;
    ZONE_DMA_INDEX       = 0;
}

static int bits_map_is_zero_except(uint64_t mask_word0)
{
    if (bits_map[0] != mask_word0) return 0;
    for (size_t i = 1; i < sizeof(bits_map) / sizeof(bits_map[0]); i++)
        if (bits_map[i] != 0) return 0;
    return 1;
}

TEST_FUNC(test_alloc_flips_ram_relative_bit)
{
    setup();
    struct Page *p = alloc_pages(ZONE_DMA, 1, 0);
    assert_not_null(p);
    /* First free page is pages_struct[0] (relative index 0), and the
     * allocator must flip bits_map bit 0 — not the absolute-index
     * bit for PA 0x40000000 (word 8, bit 0). */
    assert_true(p == &pages[0]);
    assert_true(bits_map_is_zero_except(1UL));
}

TEST_FUNC(test_alloc_two_pages_sets_two_bits)
{
    setup();
    struct Page *p1 = alloc_pages(ZONE_DMA, 1, 0);
    struct Page *p2 = alloc_pages(ZONE_DMA, 1, 0);
    assert_not_null(p1);
    assert_not_null(p2);
    assert_true(p1 == &pages[0]);
    assert_true(p2 == &pages[1]);
    assert_true(bits_map_is_zero_except(3UL));
    assert_eq(zone.page_using_count, 2UL);
    assert_eq(zone.page_free_count, (uint64_t)(N_PAGES - 2));
}

TEST_FUNC(test_free_clears_ram_relative_bit)
{
    setup();
    struct Page *p = alloc_pages(ZONE_DMA, 1, 0);
    assert_not_null(p);
    assert_true(bits_map_is_zero_except(1UL));

    free_pages(p, 1);
    /* The freed page must clear relative bit 0, leaving the map
     * entirely free again. */
    assert_true(bits_map_is_zero_except(0UL));
    assert_eq(zone.page_using_count, 0UL);
    assert_eq(zone.page_free_count, (uint64_t)N_PAGES);
}

TEST_FUNC(test_alloc_after_free_reuses_page)
{
    setup();
    struct Page *p = alloc_pages(ZONE_DMA, 1, 0);
    assert_not_null(p);
    free_pages(p, 1);
    /* With the bit correctly cleared, the same page must come back. */
    struct Page *q = alloc_pages(ZONE_DMA, 1, 0);
    assert_not_null(q);
    assert_true(q == p);
    assert_true(bits_map_is_zero_except(1UL));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_alloc_flips_ram_relative_bit),
    TEST_ENTRY(test_alloc_two_pages_sets_two_bits),
    TEST_ENTRY(test_free_clears_ram_relative_bit),
    TEST_ENTRY(test_alloc_after_free_reuses_page),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
