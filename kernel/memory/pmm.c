/* kernel/memory/pmm.c — physical memory manager.
 *
 * Arch-neutral pmm_init body (RAM-relative indexing + clamp). Public
 * surface (alloc_pages, free_pages, alloc_4k_page, free_4k_page,
 * page_cow_*) is preserved byte-for-byte; only pmm_init changes form.
 * Two private-helper color_printk calls in get_page_attribute and
 * set_page_attribute are replaced with log_err.
 *
 * aarch64 link is -nostdlib -ffreestanding; libc headers (<string.h>,
 * <list.h>) are unavailable. Inline the few libc-only symbols used by
 * the public surface (list_t, list_init, list_add_to_behind) at the
 * top; forward-declare memset and slab_init so the rewritten pmm_init
 * body can call them without dragging libc into the translation unit.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <core/bootinfo.h>
#include <log/log.h>
#include <arch/cpu.h>     /* arch_cpu_halt — required for fatal paths */
#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/pmm_boot.h>      /* checked PMM metadata layout (Task 1) */
#include <memory/pmm_arch.h>      /* range-based boot reservation + claim
                                   * (Task 2): pmm_arch_boot_reservations,
                                   * pmm_reserve_boot_ranges,
                                   * pmm_claim_free_frame */
#include <memory/memory.h>       /* Virt_To_Phy, Phy_To_Virt */
#include <core/printk.h>       /* color_printk (public surface) */
#include <core/debug.h>        /* debug_mm (existing call sites) */
#include <arch/spinlock.h>
#include <kernel.h>              /* container_of */

/* ── Inlined libc-only helpers ────────────────────────────────
 * The public surface uses list_t / list_init / list_add_to_behind.
 * On aarch64 there is no <list.h>; inline the minimal set so the
 * preserved surface compiles without the libc header. */
typedef struct List {
    struct List * prev;
    struct List * next;
} list_t;

static inline void list_init(struct List * lst)
{
    lst->prev = lst;
    lst->next = lst;
}

static inline void list_add_to_behind(struct List * entry,
                                      struct List * new_entry)
{
    new_entry->next = entry->next;
    new_entry->prev = entry;
    new_entry->next->prev = new_entry;
    entry->next = new_entry;
}

/* ── Forward declarations ────────────────────────────────────
 * memset: libc on x86_64 (linked through -lk) and kernel/arch/aarch64/
 * memset.c on aarch64.  slab_init: kernel/memory/slab.c on x86_64 and
 * kernel/arch/aarch64/slab_stub.c on aarch64.  pmm_arch_normalize /
 * pmm_arch_zone_split: kernel/memory/pmm_arch.c weak default; per-arch
 * strong overrides in kernel/arch/<arch>/pmm_arch.c. */
void *memset(void *s, int c, size_t n);
size_t slab_init(void);
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out);
uint64_t pmm_arch_zone_split(void);

uint64_t page_init(struct Page * page, uint64_t flags)
{
    page->attribute |= flags;

    if (!page->reference_count || (page->attribute & PG_Shared))
    {
        page->reference_count++;
        page->zone_struct->total_pages_link++;
    }
    return 1;
}

uint64_t page_clean(struct Page * page)
{
    page->reference_count--;
    page->zone_struct->total_pages_link--;

    if (!page->reference_count)
    {
        page->attribute &= PG_PTable_Mapped;
    }
    return 1;
}

uint64_t get_page_attribute(struct Page *page)
{
    if (page == NULL)
    {
        log_err("get_page_attribute() ERROR: page == NULL\n");
        return 0;
    }
    else
    {
        return page->attribute;
    }
}

uint64_t set_page_attribute(struct Page * page, uint64_t flags)
{
    if (page == NULL)
    {
        log_err("set_page_attribute() ERROR: page == NULL\n");
        return 0;
    }
    else
    {
        page->attribute = flags;
        return 1;
    }
}

struct Physical_Memory_Manager PMMngr = {0};

uint32_t ZONE_DMA_INDEX;
uint32_t ZONE_NORMAL_INDEX;
uint32_t ZONE_UNMAPPED_INDEX;

#define SUBPAGE_4K_COUNT (PAGE_2M_SIZE / PAGE_4K_SIZE)  // 512

struct subpage_pool {
    list_t      list;
    uint64_t    base_phys;
    uint64_t    bitmap[SUBPAGE_4K_COUNT / 64];
    uint32_t    alloc_count;
    uint16_t    cow_count[SUBPAGE_4K_COUNT];  // COW refcount: how many COW PTEs map each 4KB slot
};

static list_t      subpage_pools;
static spinlock_T  subpage_lock = { .lock = 1L };
static spinlock_T  pmm_lock     = { .lock = 1L };

// Initialized explicitly in pmm_init() after slab_init().
// Do NOT use lazy init — SMP race on first concurrent alloc_4k_page().

void pmm_init(const struct boot_context *ctx)
{
    /* Real guard. Prior implementations used an NDEBUG'd assert which
     * left pmm_init re-entrant in release builds. */
    static int pmm_initialized = 0;
    if (pmm_initialized) {
        log_err("[smp] FATAL: pmm_init called twice\n");
        arch_cpu_halt();
    }
    if (!ctx) {
        log_err("[smp] FATAL: pmm_init null ctx\n");
        arch_cpu_halt();
    }
    if (!boot_context_valid(ctx)) {
        log_err("[smp] FATAL: pmm_init invalid handoff\n");
        arch_cpu_halt();
    }
    if ((ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0) {
        log_err("[smp] FATAL: pmm_init invalid handoff\n");
        arch_cpu_halt();
    }
    /* Per-format entry_size / format validation lives in each arch's
     * pmm_arch_normalize: x86_64 checks `format == E820 && entry_size
     * >= sizeof(struct E820_ENTRY)`; aarch64 checks `format == UEFI_RAW
     * && entry_size >= AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE`. Both return
     * 0 on mismatch, which the caller below translates to FATAL. */

    /* Step 1: adapter -> MEMORY_RANGE[] */
    struct MEMORY_RANGE scratch[MEMORY_RANGE_MAX];
    size_t n = pmm_arch_normalize(ctx, scratch);
    if (n == 0) {
        log_err("[smp] FATAL: pmm_arch_normalize returned no ranges\n");
        arch_cpu_halt();
    }

    /* Step 2: compute TotalMem, lowest_ram, highest_ram. */
    uint64_t TotalMem = 0;
    uint64_t lowest_ram  = UINT64_MAX;
    uint64_t highest_ram = 0;
    for (size_t i = 0; i < n; i++) {
        if (scratch[i].type != MEMORY_TYPE_RAM) continue;
        uint64_t s = scratch[i].phys_start;
        uint64_t e = scratch[i].phys_end;
        TotalMem += (e - s);
        uint64_t s_aligned = s & ~(MEMORY_RANGE_GRANULE - 1);
        if (s_aligned < lowest_ram)  lowest_ram  = s_aligned;
        uint64_t e_aligned = (e + MEMORY_RANGE_GRANULE - 1) & ~(MEMORY_RANGE_GRANULE - 1);
        if (e_aligned > highest_ram) highest_ram = e_aligned;
    }
    if (TotalMem == 0) {
        log_err("[smp] FATAL: no usable RAM after exclusions\n");
        arch_cpu_halt();
    }
    uint64_t ram_span_pages = (highest_ram - lowest_ram) / MEMORY_RANGE_GRANULE;
    if (ram_span_pages == 0) ram_span_pages = 1;   /* floor 1 */

    /* Step 3: allocate bits_map, pages_struct, zones_struct from start_brk.
     * Use the shared checked calculator (kernel/memory/pmm_boot.c) for
     * every offset, length and trailing tail. The calculator's offsets
     * are relative to the caller-aligned base_va; production pmm.c
     * aligns start_brk up to 4 KiB before assigning bits_map. */
    uint64_t brk = (PMMngr.start_brk + 0xFFFUL) & ~0xFFFUL;
    struct pmm_layout layout;
    int rc = pmm_layout_calculate(brk, ram_span_pages, &layout);
    if (rc != 0) {
        log_err("[smp] FATAL: pmm_layout_calculate failed (rc=%d)\n", rc);
        arch_cpu_halt();
    }
    if (layout.total_bytes == 0) {
        log_err("[smp] FATAL: pmm_layout_calculate returned zero total_bytes\n");
        arch_cpu_halt();
    }
    PMMngr.bits_map      = (uint64_t *)(brk + layout.bits_map_off);
    PMMngr.bits_size     = ram_span_pages;
    PMMngr.bits_length   = layout.bits_length;
    memset(PMMngr.bits_map, 0xff, PMMngr.bits_length);
    PMMngr.pages_struct  = (struct Page *)(brk + layout.pages_struct_off);
    PMMngr.pages_size    = ram_span_pages;
    PMMngr.pages_length  = layout.pages_length;
    memset(PMMngr.pages_struct, 0, PMMngr.pages_length);
    PMMngr.zones_struct  = (struct Zone *)(brk + layout.zones_struct_off);
    PMMngr.zones_size    = 0;
    PMMngr.zones_length  = layout.zones_length;
    memset(PMMngr.zones_struct, 0, PMMngr.zones_length);

    /* Step 4: walk RAM ranges, create zones. RAM-relative indexing:
     * pages_group = pages_struct + ((start - lowest_ram) >> 21), and the
     * bits_map bit is at ((start - lowest_ram) >> 21) + j. On x86_64
     * lowest_ram = 0 so this collapses to the legacy p->phy_address >> 21
     * indexing. */
    for (size_t i = 0; i < n; i++) {
        if (scratch[i].type != MEMORY_TYPE_RAM) continue;
        uint64_t start = (scratch[i].phys_start + MEMORY_RANGE_GRANULE - 1) & ~(MEMORY_RANGE_GRANULE - 1);
        uint64_t end   = scratch[i].phys_end & ~(MEMORY_RANGE_GRANULE - 1);
        if (end <= start) continue;
        if (PMMngr.zones_size >= MAX_NR_ZONES) continue;
        struct Zone *z = PMMngr.zones_struct + PMMngr.zones_size;
        PMMngr.zones_size++;
        z->zone_start_address = start;
        z->zone_end_address   = end;
        z->zone_length        = end - start;
        z->page_using_count = 0;
        z->page_free_count  = (end - start) >> 21;   /* PAGE_2M_SHIFT */
        z->total_pages_link = 0;
        z->attribute = 0;
        z->manager_struct = &PMMngr;
        z->pages_length = (end - start) >> 21;
        z->pages_group  = (struct Page *)(PMMngr.pages_struct + ((start - lowest_ram) >> 21));
        uint64_t zone_bit_base = (start - lowest_ram) >> 21;
        struct Page *p = z->pages_group;
        for (uint64_t j = 0; j < z->pages_length; j++, p++) {
            p->zone_struct = z;
            p->phy_address = start + ((uint64_t)j << 21);
            p->attribute = 0;
            p->reference_count = 0;
            p->age = 0;
            /* RAM-relative bit index: zone base + local j. */
            uint64_t rel_idx = zone_bit_base + j;
            *(PMMngr.bits_map + (rel_idx >> 6)) ^= 1UL << (rel_idx % 64);
        }
    }

    /* end_of_struct must be assigned BEFORE Step 7, because Step 7
     * computes the kernel-image walk bound from it. The shared
     * calculator already produced end_of_struct_off (the relative
     * offset from aligned brk); convert to an absolute pointer. */
    PMMngr.end_of_struct = brk + layout.end_of_struct_off;

    /* Compute the absolute PA of end_of_struct for the boot-reservation
     * strategy. pmm_layout_calculate does not populate this field
     * (Virt_To_Phy is arch-specific) — the default pmm_arch_boot_reservations
     * reads it to build the legacy prefix [0, ceil2M(metadata_end_pa)). */
    layout.metadata_end_pa = Virt_To_Phy(PMMngr.end_of_struct);

    /* Reserve every represented RAM frame the boot strategy requests.
     * Default strategy returns the legacy prefix [0, ceil2M(metadata_end_pa));
     * aarch64's strong override (Task 3) will append arena ranges
     * inside its representative zone. The helper walks pages_struct
     * once, skipping holes (NULL zone_struct), and idempotently flips
     * bitmap + counters + page_init flags only on free→reserved
     * transitions — second-call identical input is a no-op. */
    struct pmm_phys_range boot_ranges[4];
    size_t range_count = 0;
    int brc = pmm_arch_boot_reservations(&layout, boot_ranges,
                                         sizeof(boot_ranges) / sizeof(boot_ranges[0]),
                                         &range_count);
    if (brc != 0) {
        log_err("[smp] FATAL: pmm_arch_boot_reservations failed (rc=%d)\n", brc);
        arch_cpu_halt();
    }
    brc = pmm_reserve_boot_ranges(&PMMngr, boot_ranges, range_count);
    if (brc != 0) {
        log_err("[smp] FATAL: pmm_reserve_boot_ranges failed (rc=%d)\n", brc);
        arch_cpu_halt();
    }

    /* Step 7: zone index computation. */
    ZONE_DMA_INDEX = 0;
    ZONE_NORMAL_INDEX = (PMMngr.zones_size > 0) ? (PMMngr.zones_size - 1) : 0;
    ZONE_UNMAPPED_INDEX = 0;
    uint64_t threshold = pmm_arch_zone_split();
    for (uint32_t zi = 0; zi < PMMngr.zones_size; zi++) {
        struct Zone *z = PMMngr.zones_struct + zi;
        if (z->zone_start_address >= threshold && ZONE_UNMAPPED_INDEX == 0) {
            ZONE_UNMAPPED_INDEX = zi;
            ZONE_NORMAL_INDEX = (zi > 0) ? (zi - 1) : 0;
        }
    }

    /* Step 8: slab + subpage pools. Inline the subpage_pools init since
     * <list.h> is libc (not on the aarch64 include path). */
    slab_init();
    subpage_pools.prev = &subpage_pools;
    subpage_pools.next = &subpage_pools;

    pmm_initialized = 1;
}

/*
    number: pages to alloc, must < 64
    zone_select: zone select from DMA, Mapped in Pagetable, Unmapped in Pagetable
    page_flags: struct Page flags
*/
struct Page * alloc_pages(int32_t zone_select, uint64_t number, uint64_t page_flags __attribute__((unused)))
{
    if (number > 64)
    {
        color_printk(RED, BLACK, "alloc_pages() ERROR: number is invalid\n");
        return NULL;
    }

    int32_t zone_start = 0;
    int32_t zone_end = 0;
    uint64_t attribute = 0;
    uint64_t page = 0;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);

    switch (zone_select)
    {
    case ZONE_DMA:
        zone_start = 0;
        zone_end = ZONE_DMA_INDEX;
        attribute = PG_PTable_Mapped;
        break;
    case ZONE_NORMAL:
        zone_start = ZONE_DMA_INDEX;
        zone_end = ZONE_NORMAL_INDEX;
        attribute = PG_PTable_Mapped;
        break;
    case ZONE_UNMAPPED:
        zone_start = ZONE_UNMAPPED_INDEX;
        zone_end = PMMngr.zones_size - 1;
        attribute = 0;
        break;
    default:
        color_printk(RED, BLACK, "alloc_pages() ERROR: zone_select index is invalid\n");
        spin_unlock_irqrestore(&pmm_lock, flags);
        return NULL;
        break;
    }

    int32_t i;
    for (i = zone_start; i <= zone_end; i++)
    {
        struct Zone * z;
        uint64_t j;
        uint64_t start, end;
        uint64_t tmp;

        if ((PMMngr.zones_struct + i)->page_free_count < number)
            continue;
        z = PMMngr.zones_struct + i;
        /* bits_map is indexed RAM-relative (rel_idx = (PA - lowest_ram) /
         * 2 MiB). zone_start_address is the absolute PA, so convert via
         * the zone's pages_group pointer (which already points to the
         * first Page struct for this zone). */
        start = (uint64_t)(z->pages_group - PMMngr.pages_struct);
        end = start + (z->zone_end_address >> PAGE_2M_SHIFT)
                    - (z->zone_start_address >> PAGE_2M_SHIFT);

        tmp = 64 - start % 64;

        for (j = start; j < end; j += j % 64 ? tmp: 64)
        {
            uint64_t * p = PMMngr.bits_map + (j >> 6);
            uint64_t k = 0;
            uint64_t shift = j % 64;

            uint64_t num = (1UL << number) - 1;

            for (k = shift; k < 64; k++)
            {
                if (!((k ? ((*p >> k) | (*(p + 1) << (64 - k))) : *p) & (num)))
                {
                    uint32_t l;
                    page = j + k - shift;
                    for (l = 0; l < number; l++)
                    {
                        struct Page * pageptr = PMMngr.pages_struct + page + l;

                        /* bits_map is RAM-relative (pmm_init Step 4
                         * indexes it via (PA - lowest_ram) >> 21), so
                         * alloc/free must use the same index.
                         * pages_struct is densely packed in RAM-
                         * relative order, so (pageptr - pages_struct)
                         * is the right index. */
                        uint64_t rel_idx = (uint64_t)(pageptr - PMMngr.pages_struct);
                        *(PMMngr.bits_map + (rel_idx >> 6)) |= 1UL << (rel_idx % 64);
                        z->page_using_count++;
                        z->page_free_count--;
                        pageptr->attribute = attribute;
                    }
                    goto find_free_pages;
                }
            }
        }
    }

    color_printk(RED, BLACK, "alloc_pages() ERROR: no page can alloc\n");
    spin_unlock_irqrestore(&pmm_lock, flags);
    return NULL;

find_free_pages:
    spin_unlock_irqrestore(&pmm_lock, flags);
    return (struct Page *)(PMMngr.pages_struct + page);
}

/*
	page: free page start from this pointer
	number: number < 64
*/

void free_pages(struct Page * page,int32_t number)
{
	int i = 0;
	uint64_t flags = spin_lock_irqsave(&pmm_lock);

	if(page == NULL)
	{
		color_printk(RED,BLACK,"free_pages() ERROR: page is invalid\n");
		spin_unlock_irqrestore(&pmm_lock, flags);
		return ;
	}

	if(number >= 64 || number <= 0)
	{
		color_printk(RED,BLACK,"free_pages() ERROR: number is invalid\n");
		spin_unlock_irqrestore(&pmm_lock, flags);
		return ;
	}

	for(i = 0;i<number;i++,page++)
	{
		/* bits_map is RAM-relative (see alloc_pages and pmm_init
		 * Step 4); pages_struct is densely packed in the same
		 * order, so (page - pages_struct) is the right index. */
		uint64_t rel_idx = (uint64_t)(page - PMMngr.pages_struct);
		*(PMMngr.bits_map + (rel_idx >> 6)) &= ~(1UL << (rel_idx % 64));
		page->zone_struct->page_using_count--;
		page->zone_struct->page_free_count++;
		page->attribute = 0;
	}
	spin_unlock_irqrestore(&pmm_lock, flags);
}

// Find the subpage_pool containing phys, or NULL.  Must be called with
// subpage_lock held.  Extracted from free_4k_page's pool walk.
static struct subpage_pool *find_pool_locked(uint64_t phys)
{
    list_t *pos = subpage_pools.next;
    while (pos != &subpage_pools) {
        struct subpage_pool *pool =
            container_of(pos, struct subpage_pool, list);
        if (phys >= pool->base_phys &&
            phys < pool->base_phys + PAGE_2M_SIZE)
            return pool;
        pos = pos->next;
    }
    return NULL;
}

// COW refcount helpers
// All acquire subpage_lock internally.  Caller must hold no locks.

void page_cow_get(uint64_t phys)
{
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        int slot = (int)((phys - pool->base_phys) >> PAGE_4K_SHIFT);
        pool->cow_count[slot]++;
    }
    spin_unlock_irqrestore(&subpage_lock, flags);
}

bool page_cow_put(uint64_t phys)
{
    bool reached_zero = false;
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        int slot = (int)((phys - pool->base_phys) >> PAGE_4K_SHIFT);
        if (pool->cow_count[slot] > 0) {
            pool->cow_count[slot]--;
            if (pool->cow_count[slot] == 0)
                reached_zero = true;
        }
    }
    spin_unlock_irqrestore(&subpage_lock, flags);
    return reached_zero;
}

uint16_t page_cow_refs(uint64_t phys)
{
    uint16_t refs = 0;
    uint64_t flags = spin_lock_irqsave(&subpage_lock);
    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        int slot = (int)((phys - pool->base_phys) >> PAGE_4K_SHIFT);
        refs = pool->cow_count[slot];
    }
    spin_unlock_irqrestore(&subpage_lock, flags);
    return refs;
}

uint64_t alloc_4k_page(void)
{
    // subpage_pools is initialized in pmm_init() — no lazy init needed.

    uint64_t flags = spin_lock_irqsave(&subpage_lock);

    // Search existing pools
    list_t *pos = subpage_pools.next;
    while (pos != &subpage_pools) {
        struct subpage_pool *pool =
            container_of(pos, struct subpage_pool, list);
        if (pool->alloc_count < SUBPAGE_4K_COUNT) {
            for (int i = 0; i < (int)(SUBPAGE_4K_COUNT / 64); i++) {
                if (pool->bitmap[i] == (uint64_t)-1) continue;
                int bit = __builtin_ctzll(~pool->bitmap[i]);
                pool->bitmap[i] |= (1ULL << bit);
                pool->alloc_count++;
                int slot = i * 64 + bit;
                pool->cow_count[slot] = 0;   // fresh page, no COW references
                uint64_t phys = pool->base_phys
                              + (uint64_t)slot * PAGE_4K_SIZE;
                spin_unlock_irqrestore(&subpage_lock, flags);
                return phys;
            }
        }
        pos = pos->next;
    }

    // No free slot — allocate a new 2MB pool
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) {
        spin_unlock_irqrestore(&subpage_lock, flags);
        return 0;
    }
    struct subpage_pool *pool =
        (struct subpage_pool *)Phy_To_Virt(pg->phy_address);
    list_init(&pool->list);
    pool->base_phys = pg->phy_address;
    memset(pool->bitmap, 0, sizeof(pool->bitmap));
    pool->alloc_count = 0;

    // Slot 0: used by subpage_pool struct itself
    pool->bitmap[0] |= 1;
    pool->alloc_count = 1;
    list_add_to_behind(&subpage_pools, &pool->list);

    // Return slot 1 as the first free slot
    pool->bitmap[0] |= (1ULL << 1);
    pool->alloc_count++;
    pool->cow_count[1] = 0;             // fresh slot 1, no COW references

    uint64_t phys = pool->base_phys + PAGE_4K_SIZE;
    spin_unlock_irqrestore(&subpage_lock, flags);
    return phys;
}

void free_4k_page(uint64_t phys)
{
    if (!phys) return;

    uint64_t flags = spin_lock_irqsave(&subpage_lock);

    struct subpage_pool *pool = find_pool_locked(phys);
    if (pool) {
        uint64_t offset = phys - pool->base_phys;
        uint32_t slot = (uint32_t)(offset / PAGE_4K_SIZE);
        if (slot < SUBPAGE_4K_COUNT) {
            int word = slot / 64;
            int bit  = slot % 64;
            if (pool->bitmap[word] & (1ULL << bit)) {
                pool->bitmap[word] &= ~(1ULL << bit);
                pool->alloc_count--;
            }
        }
    }

    spin_unlock_irqrestore(&subpage_lock, flags);
}

/* ── Range-based boot reservation + single-frame claim ──────────────
 *
 * Both helpers share the convention that frames are indexed RAM-
 * relatively (pages_struct + (PA - lowest_ram)/2M, and the bitmap
 * follows the same order). See pmm_init Step 4 and alloc_pages for
 * the full derivation.
 *
 * pmm_reserve_boot_ranges is called from pmm_init (single-threaded)
 * with the strategy's range list. We do NOT acquire pmm_lock — the
 * PMM has no other readers during init, and the legacy prefix loop
 * was lock-free too.
 *
 * pmm_claim_free_frame DOES acquire pmm_lock — it is the canonical
 * "give me one free frame in this window" allocator that runtime
 * code (aarch64 preflight, etc.) calls once init is over. */

/* Returns the absolute PA covered by a Zone's free frames. The legacy
 * loop computed `end_phys - lowest_ram`; we keep the same math but
 * express it via pages_struct so the reservation logic doesn't have
 * to know lowest_ram. */
static int pa_in_ranges(uint64_t pa, const struct pmm_phys_range *ranges,
                        size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (pa >= ranges[i].start && pa < ranges[i].end) return 1;
    }
    return 0;
}

int pmm_reserve_boot_ranges(struct Physical_Memory_Manager *pm,
                            const struct pmm_phys_range *ranges,
                            size_t count)
{
    if (!pm) return -EINVAL;
    if (count > 0 && !ranges) return -EINVAL;
    if (count == 0) return 0;

    /* First pass: validate input shapes. Endpoints must be strictly
     * increasing and 2 MiB-aligned (we flip bits in a 2 MiB-indexed
     * bitmap — partial frames are not representable). */
    for (size_t i = 0; i < count; i++) {
        if (ranges[i].end <= ranges[i].start) return -EINVAL;
        if ((ranges[i].start & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
        if ((ranges[i].end   & (PAGE_2M_SIZE - 1)) != 0) return -EINVAL;
    }

    /* Second pass: walk pages_struct once. For each represented frame
     * whose PA falls in any range, mark it reserved. Frames already
     * reserved (bit set) are skipped — this is what makes the helper
     * idempotent. Frames outside represented RAM (NULL zone_struct,
     * i.e. holes between sparse zones) are naturally skipped because
     * pages_struct has no entries for them. */
    for (uint64_t i = 0; i < pm->pages_size; i++) {
        struct Page *p = pm->pages_struct + i;
        if (!p->zone_struct) continue;       /* hole */
        if (!pa_in_ranges(p->phy_address, ranges, count)) continue;
        uint64_t rel_idx = i;                /* pages_struct is RAM-relative */
        uint64_t word = rel_idx >> 6;
        uint64_t mask = 1UL << (rel_idx % 64);
        if (pm->bits_map[word] & mask) continue;  /* already reserved */
        page_init(p, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
        pm->bits_map[word] |= mask;
        p->zone_struct->page_using_count++;
        p->zone_struct->page_free_count--;
    }
    return 0;
}

struct Page *pmm_claim_free_frame(uint64_t start_pa, uint64_t end_pa,
                                  bool from_end)
{
    if (end_pa <= start_pa) return NULL;

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    struct Page *result = NULL;

    /* Match alloc_pages' bitmap convention: rel_idx = (page - pages_struct),
     * word = rel_idx >> 6, mask = 1UL << (rel_idx % 64). Skip frames whose
     * zone_struct is NULL — those are holes, NOT free RAM — so we never
     * hand out a Page whose PA falls outside represented RAM. */
    if (from_end) {
        for (uint64_t i = PMMngr.pages_size; i > 0; i--) {
            struct Page *p = PMMngr.pages_struct + (i - 1);
            if (!p->zone_struct) continue;
            uint64_t pa = p->phy_address;
            if (pa < start_pa || pa >= end_pa) continue;
            uint64_t rel_idx = i - 1;
            uint64_t word = rel_idx >> 6;
            uint64_t mask = 1UL << (rel_idx % 64);
            if (PMMngr.bits_map[word] & mask) continue;  /* in use */
            PMMngr.bits_map[word] |= mask;
            p->zone_struct->page_using_count++;
            p->zone_struct->page_free_count--;
            p->attribute = PG_PTable_Mapped;
            result = p;
            break;
        }
    } else {
        for (uint64_t i = 0; i < PMMngr.pages_size; i++) {
            struct Page *p = PMMngr.pages_struct + i;
            if (!p->zone_struct) continue;
            uint64_t pa = p->phy_address;
            if (pa < start_pa || pa >= end_pa) continue;
            uint64_t rel_idx = i;
            uint64_t word = rel_idx >> 6;
            uint64_t mask = 1UL << (rel_idx % 64);
            if (PMMngr.bits_map[word] & mask) continue;  /* in use */
            PMMngr.bits_map[word] |= mask;
            p->zone_struct->page_using_count++;
            p->zone_struct->page_free_count--;
            p->attribute = PG_PTable_Mapped;
            result = p;
            break;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return result;
}