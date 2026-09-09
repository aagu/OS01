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
#include <kernel/bootinfo.h>
#include <kernel/log.h>
#include <kernel/arch/cpu.h>     /* arch_cpu_halt — required for fatal paths */
#include <kernel/memory_map.h>
#include <kernel/pmm.h>
#include <kernel/memory.h>       /* Virt_To_Phy, Phy_To_Virt */
#include <kernel/printk.h>       /* color_printk (public surface) */
#include <kernel/debug.h>        /* debug_mm (existing call sites) */
#include <kernel/arch/spinlock.h>
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
     * Mirror the existing pmm.c sizing math (PMMngr.start_brk + 0xFFF &
     * ~0xFFF), but with the new ram_span_pages. */
    PMMngr.bits_map = (uint64_t *)((PMMngr.start_brk + 0xFFFUL) & ~0xFFFUL);
    PMMngr.bits_size  = ram_span_pages;
    PMMngr.bits_length = ((ram_span_pages + 63) & ~63UL) / 8;
    memset(PMMngr.bits_map, 0xff, PMMngr.bits_length);
    PMMngr.pages_struct = (struct Page *)(((uint64_t)PMMngr.bits_map + PMMngr.bits_length + 0xFFFUL) & ~0xFFFUL);
    PMMngr.pages_size  = ram_span_pages;
    PMMngr.pages_length = ((ram_span_pages * sizeof(struct Page) + sizeof(long) - 1) & ~(sizeof(long) - 1));
    memset(PMMngr.pages_struct, 0, PMMngr.pages_length);
    PMMngr.zones_struct = (struct Zone *)(((uint64_t)PMMngr.pages_struct + PMMngr.pages_length + 0xFFFUL) & ~0xFFFUL);
    PMMngr.zones_size = 0;
    PMMngr.zones_length = ((MEMORY_RANGE_MAX * sizeof(struct Zone) + sizeof(long) - 1) & ~(sizeof(long) - 1));
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
     * computes the kernel-image walk bound from it. Mirror the existing
     * pmm.c:240 computation exactly. */
    PMMngr.end_of_struct =
        ((uint64_t)PMMngr.zones_struct + PMMngr.zones_length + sizeof(long) * 32)
        & ~(sizeof(long) - 1);

    /* Step 5: page-0 quirk (x86_64 historical). */
    if (PMMngr.pages_struct->phy_address == 0) {
        PMMngr.pages_struct->zone_struct = PMMngr.zones_struct;
        PMMngr.pages_struct->phy_address = 0UL;
        set_page_attribute(PMMngr.pages_struct,
                           PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
        PMMngr.pages_struct->reference_count = 1;
        PMMngr.pages_struct->age = 0;
    }

    /* Step 6: mark kernel-owned pages. RAM-relative walk with clamp:
     * unsigned underflow would otherwise corrupt the loop bound on
     * aarch64 where end_phys (kernel LMA ~0x401e0000) < lowest_ram
     * (first surviving RAM range starts at 0x40200000). */
    uint64_t end_phys = Virt_To_Phy(PMMngr.end_of_struct);
    uint64_t walk_pages = (end_phys > lowest_ram)
        ? ((end_phys - lowest_ram) >> 21) : 0;
    for (uint64_t j = 1; j <= walk_pages; j++) {
        struct Page *tmp = PMMngr.pages_struct + j;
        page_init(tmp, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
        uint64_t rel_idx = (tmp->phy_address - lowest_ram) >> 21;
        *(PMMngr.bits_map + (rel_idx >> 6)) |= 1UL << (rel_idx % 64);
        tmp->zone_struct->page_using_count++;
        tmp->zone_struct->page_free_count--;
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
        start = z->zone_start_address >> PAGE_2M_SHIFT;
        end = z->zone_end_address >> PAGE_2M_SHIFT;

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

                        *(PMMngr.bits_map + ((pageptr->phy_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (pageptr->phy_address >> PAGE_2M_SHIFT) % 64;
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
		*(PMMngr.bits_map + ((page->phy_address >> PAGE_2M_SHIFT) >> 6)) &= ~(1UL << (page->phy_address >> PAGE_2M_SHIFT) % 64);
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