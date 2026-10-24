/*
 * test_monitor_pmm.c — locked PMM usage snapshot (pmm_get_stats)
 *
 * Monitor plan Task 3 / monitor design spec §4
 * (docs/superpowers/specs/2026-10-09-monitor-design.md):
 *   - a fixture zone of 3 pages with 1 in use reads back
 *     T=6291456, U=2097152, F=4194304 (bytes, 64-bit math);
 *   - after whole-page alloc/free the invariant T == U + F holds and
 *     the counters return to their starting values;
 *   - in-pool 4KB alloc/free_4k does NOT lower U: a 2MB pool page is
 *     counted as used when the pool is created, and stays used while
 *     any slot (including slot 0 metadata) remains allocated;
 *   - a T=0 backend (no zones) reports zero stats — the caller judges
 *     the snapshot unavailable, pmm_get_stats does not invent values;
 *   - multi-zone backends are summed under one snapshot, inside the
 *     allocator's PMM lock, with balanced acquisition and release.
 *
 * Build: the PRODUCTION kernel/memory/pmm.c is host-compiled via the
 * same PMM_SHADOW_INC harness as test_pmm_ram_rel_index (mock
 * arch/spinlock.h + arch/cpu.h shadow the privileged-asm headers);
 * everything asserted here runs against that object.  The test TU
 * hand-builds PMMngr (the pmm_init boot_context path is out of scope
 * for a snapshot test) exactly like test_pmm_ram_rel_index.c does.
 *
 * Pool-note: alloc_4k_page's new-pool path builds the pool metadata
 * at Phy_To_Virt(page->phy_address).  On the host that VA must land
 * in real memory, so the fixture parks a static backing buffer at the
 * Phy_To_Virt image of the pool page's recorded phys address.
 */
#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <memory/pmm.h>
#include <memory/memory.h>      /* Phy_To_Virt / ARCH_PAGE_OFFSET */
#include <memory/memory_map.h>
#include <memory/pmm_arch.h>
#include <core/bootinfo.h>
#include "../mock/pmm_include/arch/spinlock.h"

extern struct Physical_Memory_Manager PMMngr;
extern uint32_t ZONE_DMA_INDEX;
extern uint32_t ZONE_NORMAL_INDEX;

/* Optional shadow-lock observers. Armed only around calls under test;
 * the ordinary allocator establishes which lock protects PMM zones. */
static int observe_locks;
static int inject_zone_transition;
static spinlock_T *allocator_lock;
static unsigned lock_acquires, lock_releases, lock_depth, lock_errors;
static struct Zone snapshot_zones[2];

void pmm_test_spinlock_acquire(spinlock_T *lock)
{
    if (!observe_locks)
        return;
    if (!allocator_lock)
        allocator_lock = lock;
    if (lock != allocator_lock || lock_depth != 0)
        lock_errors++;
    lock_acquires++;
    lock_depth++;
    if (inject_zone_transition) {
        /* A writer completed just before the snapshot acquired the lock. */
        PMMngr.zones_size = 2;
        snapshot_zones[0].page_using_count = 1;
        snapshot_zones[0].page_free_count = 2;
    }
}

void pmm_test_spinlock_release(spinlock_T *lock)
{
    if (!observe_locks)
        return;
    if (lock != allocator_lock || lock_depth != 1)
        lock_errors++;
    lock_releases++;
    if (lock_depth)
        lock_depth--;
    if (inject_zone_transition) {
        /* Another writer becomes visible immediately after unlock. */
        PMMngr.zones_size = 1;
        snapshot_zones[0].page_using_count = 0;
        snapshot_zones[0].page_free_count = 3;
    }
}

/* ── link stubs for externs pmm.o references ──────────────
 * Same set as test_pmm_ram_rel_index.c: g_log_level /
 * _log_err_impl keep pmm.c's log_err error paths linkable;
 * color_printk and slab_init are only reached on paths this
 * suite does not exercise but must still resolve at link. */
int g_log_level = 3;   /* LOG_ERR */
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* ── strong overrides of pmm_arch.c's weak dispatchers ────
 * The fixture runs the REAL pmm_init exactly once so the static
 * subpage_pools list head gets initialized (alloc_4k_page walks it
 * unconditionally; an uninitialized head would be NULL and fault).
 * The overrides hand pmm_init one 8-page RAM range and request zero
 * boot reservations, so pmm_init finishes with a clean, fully-free
 * zone.  The per-test fixtures then re-shape PMMngr on top. */
static struct MEMORY_RANGE fixture_range;

size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    (void)ctx;
    out[0] = fixture_range;
    return 1;
}

int pmm_arch_boot_reservations(const struct pmm_layout *layout,
                               struct pmm_phys_range *out,
                               size_t capacity, size_t *count)
{
    (void)layout; (void)out; (void)capacity;
    *count = 0;   /* reserve nothing — fixture stays fully free */
    return 0;
}

/* Backing store for pmm_init's bits_map/pages_struct/zones_struct.
 * Abandoned once the per-test fixture re-shapes PMMngr, but must hold
 * the whole calculated layout while pmm_init runs. */
static uint64_t init_metadata_pool[128 * 1024];

static void setup_once(void)
{
    static int done;
    if (done)
        return;
    done = 1;

    fixture_range.phys_start = 0x40000000ULL;
    fixture_range.phys_end   = 0x40000000ULL + 8 * PAGE_2M_SIZE;
    fixture_range.type       = MEMORY_TYPE_RAM;

    struct boot_context ctx;
    boot_context_init(&ctx);
    ctx.flags |= BOOT_CONTEXT_HAS_MEMORY_MAP;

    PMMngr.start_brk = (uint64_t)init_metadata_pool;
    pmm_init(&ctx);
}

#define ONE_PAGE   ((uint64_t)PAGE_2M_SIZE)
#define FIX_PAGES  3

static uint64_t   bits_map[16];
static struct Page pages[FIX_PAGES];
static struct Zone zone;

/* Backing store for the subpage-pool metadata alloc_4k_page builds
 * inside the "2MB pool page" (sizeof(struct subpage_pool) ≈ 1.1KB;
 * 8KB is comfortably above it and stays out of the asserted math). */
static uint64_t pool_backing[1024];

/* Reset the hand-built PMMngr to one zone of `n` entirely free pages
 * (T = n × 2MB, U = 0, F = n × 2MB). */
static void setup_zone(uint64_t n)
{
    /* pmm_init must have run once first: it initializes the static
     * subpage_pools list head that alloc_4k_page walks. */
    setup_once();

    memset(bits_map, 0, sizeof(bits_map));
    memset(pages,   0, sizeof(pages));
    memset(&zone,   0, sizeof(zone));

    for (uint64_t i = 0; i < n; i++) {
        pages[i].zone_struct     = &zone;
        pages[i].phy_address     = 0x40000000ULL + i * PAGE_2M_SIZE;
        pages[i].attribute       = 0;
        pages[i].reference_count = 0;
    }
    zone.pages_group         = pages;
    zone.pages_length        = n;
    zone.zone_start_address  = 0x40000000ULL;
    zone.zone_end_address    = 0x40000000ULL + n * PAGE_2M_SIZE;
    zone.page_using_count    = 0;
    zone.page_free_count     = n;
    zone.manager_struct      = &PMMngr;

    PMMngr.bits_map      = bits_map;
    PMMngr.pages_struct  = pages;
    PMMngr.zones_struct  = &zone;
    PMMngr.zones_size    = 1;
    ZONE_DMA_INDEX       = 0;
    ZONE_NORMAL_INDEX    = 0;
}

/* ── Tests ──────────────────────────────────────────── */

/* Spec §4.1 fixture: 3 pages managed, 1 in use → T=6291456,
 * U=2097152, F=4194304.  The page in use is taken through the REAL
 * alloc_pages path, so the counters the snapshot sums are the ones
 * the allocator itself maintains. */
TEST_FUNC(three_pages_one_in_use_snapshot)
{
    setup_zone(FIX_PAGES);
    struct Page *p = alloc_pages(ZONE_DMA, 1, 0);
    assert_not_null(p);
    assert_eq((uint64_t)1, zone.page_using_count);
    assert_eq((uint64_t)2, zone.page_free_count);

    struct pmm_stats stats;
    pmm_get_stats(&stats);
    assert_eq((uint64_t)6291456, stats.managed_bytes);
    assert_eq((uint64_t)2097152, stats.reserved_bytes);
    assert_eq((uint64_t)4194304, stats.free_bytes);
}

/* Whole-page alloc/free: T == U + F holds across the cycle and both
 * counters return to their starting values.  A repeated read must
 * not mutate anything (pure copy out of the lock). */
TEST_FUNC(page_alloc_free_keeps_identity)
{
    setup_zone(FIX_PAGES);

    struct pmm_stats base;
    pmm_get_stats(&base);
    assert_eq((uint64_t)6291456, base.managed_bytes);
    assert_eq((uint64_t)6291456, base.free_bytes);
    assert_eq(base.managed_bytes, base.reserved_bytes + base.free_bytes);

    struct Page *a = alloc_pages(ZONE_DMA, 1, 0);
    struct Page *b = alloc_pages(ZONE_DMA, 1, 0);
    assert_not_null(a);
    assert_not_null(b);

    struct pmm_stats used;
    pmm_get_stats(&used);
    assert_eq((uint64_t)6291456, used.managed_bytes);
    assert_eq((uint64_t)4194304, used.reserved_bytes);
    assert_eq((uint64_t)2097152, used.free_bytes);
    assert_eq(used.managed_bytes, used.reserved_bytes + used.free_bytes);

    free_pages(a, 1);
    free_pages(b, 1);

    struct pmm_stats back;
    pmm_get_stats(&back);
    assert_eq((uint64_t)6291456, back.managed_bytes);
    assert_eq((uint64_t)0,       back.reserved_bytes);
    assert_eq((uint64_t)6291456, back.free_bytes);
    assert_eq(back.managed_bytes, back.reserved_bytes + back.free_bytes);

    /* Read is non-mutating: identical snapshot afterwards. */
    struct pmm_stats again;
    pmm_get_stats(&again);
    assert_eq(back.managed_bytes,  again.managed_bytes);
    assert_eq(back.reserved_bytes, again.reserved_bytes);
    assert_eq(back.free_bytes,     again.free_bytes);
}

/* Spec §4.2: a 2MB pool page is already counted in U when the pool
 * is created; in-pool 4KB alloc/free does NOT change U — the pool is
 * not returned to the PMM while slot 0 (metadata) is still held. */
TEST_FUNC(pool_4k_alloc_free_does_not_lower_U)
{
    setup_zone(2);   /* page 0 will become the pool page */

    /* Park the pool metadata backing at the Phy_To_Virt image of the
     * phys address recorded in pages[0], so alloc_4k_page's new-pool
     * path writes into real host memory. */
    pages[0].phy_address =
        (uint64_t)pool_backing - (uint64_t)ARCH_PAGE_OFFSET;

    struct pmm_stats before;
    pmm_get_stats(&before);

    uint64_t slot1 = alloc_4k_page();
    assert_true(slot1 != 0);

    /* New pool: the whole 2MB page moved into U (slot 0 metadata +
     * slot 1 handed out), F dropped by one page. */
    struct pmm_stats pooled;
    pmm_get_stats(&pooled);
    assert_eq(before.managed_bytes,  pooled.managed_bytes);
    assert_eq(before.reserved_bytes + ONE_PAGE, pooled.reserved_bytes);
    assert_eq(before.free_bytes    - ONE_PAGE, pooled.free_bytes);
    assert_eq(pooled.managed_bytes, pooled.reserved_bytes + pooled.free_bytes);

    /* Second 4KB slot from the SAME pool: no new 2MB page, U/F unchanged. */
    uint64_t slot2 = alloc_4k_page();
    assert_true(slot2 != 0 && slot2 != slot1);
    struct pmm_stats second;
    pmm_get_stats(&second);
    assert_eq(pooled.reserved_bytes, second.reserved_bytes);
    assert_eq(pooled.free_bytes,     second.free_bytes);

    /* In-pool frees: U must NOT come back down (spec §4.2 — the pool
     * is not returned to the PMM; a dropping MemUsed here would not
     * prove a leak either way). */
    free_4k_page(slot1);
    struct pmm_stats after_one;
    pmm_get_stats(&after_one);
    assert_eq(second.reserved_bytes, after_one.reserved_bytes);
    assert_eq(second.free_bytes,     after_one.free_bytes);

    free_4k_page(slot2);
    struct pmm_stats after_all;
    pmm_get_stats(&after_all);
    assert_eq(pooled.reserved_bytes, after_all.reserved_bytes);
    assert_eq(pooled.free_bytes,     after_all.free_bytes);
    assert_eq(after_all.managed_bytes,
              after_all.reserved_bytes + after_all.free_bytes);
}

/* T=0 backend (no zones at all): all-zero stats.  Unavailability is
 * the caller's judgement (spec §4.1 "T=0 为 unavailable") — the
 * snapshot itself must report zeros, not garbage or -1.  A NULL out
 * pointer must be tolerated, not dereferenced. */
TEST_FUNC(zero_backend_reports_zero_stats)
{
    setup_zone(FIX_PAGES);
    PMMngr.zones_size = 0;

    struct pmm_stats stats = { 0xdead, 0xdead, 0xdead };
    pmm_get_stats(&stats);
    assert_eq((uint64_t)0, stats.managed_bytes);
    assert_eq((uint64_t)0, stats.reserved_bytes);
    assert_eq((uint64_t)0, stats.free_bytes);

    pmm_get_stats(NULL);   /* must not fault */
}

/* Σzone: two zones with different counters sum into one snapshot. */
TEST_FUNC(multi_zone_sums_all_zones)
{
    static struct Zone zones[2];
    static struct Page pages1[3];
    static struct Page pages2[2];

    setup_zone(FIX_PAGES);
    /* Re-shape into two zones sharing pages_struct/bits_map. */
    memset(pages1, 0, sizeof(pages1));
    memset(pages2, 0, sizeof(pages2));
    memset(zones,  0, sizeof(zones));

    for (int i = 0; i < 3; i++) {
        pages1[i].zone_struct = &zones[0];
        pages1[i].phy_address = 0x40000000ULL + (uint64_t)i * PAGE_2M_SIZE;
    }
    for (int i = 0; i < 2; i++) {
        pages2[i].zone_struct = &zones[1];
        pages2[i].phy_address = 0x40600000ULL + (uint64_t)i * PAGE_2M_SIZE;
    }
    zones[0].pages_group      = pages1;
    zones[0].pages_length     = 3;
    zones[0].page_using_count = 1;
    zones[0].page_free_count  = 2;
    zones[1].pages_group      = pages2;
    zones[1].pages_length     = 2;
    zones[1].page_using_count = 2;
    zones[1].page_free_count  = 0;

    PMMngr.pages_struct = pages1;
    PMMngr.zones_struct = zones;
    PMMngr.zones_size   = 2;

    struct pmm_stats stats;
    pmm_get_stats(&stats);
    assert_eq((uint64_t)5 * ONE_PAGE, stats.managed_bytes);
    assert_eq((uint64_t)3 * ONE_PAGE, stats.reserved_bytes);
    assert_eq((uint64_t)2 * ONE_PAGE, stats.free_bytes);
    assert_eq(stats.managed_bytes, stats.reserved_bytes + stats.free_bytes);
}

/* Distinct states before acquisition, while held, and after release
 * make missing, late, or early locking observable in the actual result. */
TEST_FUNC(snapshot_sums_all_zones_inside_allocator_lock)
{
    setup_zone(FIX_PAGES);
    allocator_lock = NULL;
    lock_acquires = lock_releases = lock_depth = lock_errors = 0;
    observe_locks = 1;
    struct Page *allocated = alloc_pages(ZONE_DMA, 1, 0);
    observe_locks = 0;
    assert_not_null(allocated);
    assert_not_null(allocator_lock);
    assert_eq(1, lock_acquires);
    assert_eq(1, lock_releases);
    assert_eq(0, lock_depth);
    assert_eq(0, lock_errors);

    snapshot_zones[0] = zone;  /* pre-lock state: T=3, U=1, F=2 pages */
    memset(&snapshot_zones[1], 0, sizeof(snapshot_zones[1]));
    snapshot_zones[1].pages_length = 2;
    snapshot_zones[1].page_using_count = 2;
    PMMngr.zones_struct = snapshot_zones;
    PMMngr.zones_size = 1;
    lock_acquires = lock_releases = lock_depth = lock_errors = 0;
    inject_zone_transition = observe_locks = 1;
    struct pmm_stats stats;
    pmm_get_stats(&stats);
    observe_locks = inject_zone_transition = 0;

    /* Only the locked state has two zones: T=5, U=3, F=2 pages. */
    assert_eq((uint64_t)10485760, stats.managed_bytes);
    assert_eq((uint64_t)6291456, stats.reserved_bytes);
    assert_eq((uint64_t)4194304, stats.free_bytes);
    assert_eq(1, lock_acquires);
    assert_eq(1, lock_releases);
    assert_eq(0, lock_depth);
    assert_eq(0, lock_errors);
    assert_eq((uint64_t)1, PMMngr.zones_size);
    assert_eq((uint64_t)0, snapshot_zones[0].page_using_count);

    /* A NULL output must return before entering the lock at all. */
    lock_acquires = lock_releases = 0;
    observe_locks = 1;
    pmm_get_stats(NULL);
    observe_locks = 0;
    assert_eq(0, lock_acquires);
    assert_eq(0, lock_releases);
}

TEST_LIST_BEGIN
    TEST_ENTRY(three_pages_one_in_use_snapshot),
    TEST_ENTRY(page_alloc_free_keeps_identity),
    TEST_ENTRY(pool_4k_alloc_free_does_not_lower_U),
    TEST_ENTRY(zero_backend_reports_zero_stats),
    TEST_ENTRY(multi_zone_sums_all_zones),
    TEST_ENTRY(snapshot_sums_all_zones_inside_allocator_lock),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
