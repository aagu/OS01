#include <stdint.h>
#include <stddef.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/arch/x86_64/string.h>
#include <kernel/debug.h>
#include <kernel/slab.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <kernel.h>
#include <list.h>

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
        color_printk(RED, BLACK, "get_page_attribute() ERROR: page == NULL\n");
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
        color_printk(RED, BLACK, "set_page_attribute() ERROR: page == NULL\n");
        return 0;
    }
    else
    {
        page->attribute = flags;
        return 1;
    }
}

struct Physical_Memory_Manager PMMngr = {{0},0};

uint32_t ZONE_DMA_INDEX;
uint32_t ZONE_NORMAL_INDEX;
uint32_t ZONE_UNMAPPED_INDEX;

#define SUBPAGE_4K_COUNT (PAGE_2M_SIZE / PAGE_4K_SIZE)  // 512

struct subpage_pool {
    list_t      list;
    uint64_t    base_phys;
    uint64_t    bitmap[SUBPAGE_4K_COUNT / 64];
    int         alloc_count;
    uint16_t    cow_count[SUBPAGE_4K_COUNT];  // COW refcount: how many COW PTEs map each 4KB slot
};

static list_t      subpage_pools;
static spinlock_T  subpage_lock = { .lock = 1L };

// Initialized explicitly in pmm_init() after slab_init().
// Do NOT use lazy init — SMP race on first concurrent alloc_4k_page().

void pmm_init(struct MEMORY_INFO E820_Info)
{
    uint32_t i, j;
    uint64_t TotalMem = 0;
    struct E820_ENTRY *p = (struct E820_ENTRY *)(uintptr_t)E820_Info.E820_Entry;

    debug_mm("Display Physics Address MAP,Type(1:RAM,2:ROM or Reserved,3:ACPI Reclaim Memory,4:ACPI NVS Memory,Others:Undefine)\n");
    for (i = 0; i < E820_Info.E820_Entry_count; i++)
    {
        debug_mm("Address:%#018lx\tLength:%#018lx\tType:%2d\n",p->address,p->length,p->type);
		if(p->type == 1)
		{
			TotalMem += p->length;
		}

        PMMngr.e820_entrys[i].address =  p->address;
        PMMngr.e820_entrys[i].length = p->length;
        PMMngr.e820_entrys[i].type = p->type;
        PMMngr.e820_length = i;

		p++;
        if (p->type > 4 || p->type < 1)
            break;
    }

    debug_mm("OS Can Used Total RAM:%dMB\n",TotalMem>>20);

    // TotalMem = 0;
    // for (i = 0; i <= PMMngr.e820_length; i++)
    // {
    //     uint64_t start, end;
    //     if (PMMngr.e820_entrys[i].type != 1)
    //         continue;
    //     start = PAGE_2M_ALIGN(PMMngr.e820_entrys[i].address);
    //     end = ((PMMngr.e820_entrys[i].address + PMMngr.e820_entrys[i].length) >> PAGE_2M_SHIFT) << PAGE_2M_SHIFT;
    //     if (end <= start)
    //         continue;
    //     TotalMem += (end - start) >> PAGE_2M_SHIFT;
    // }
    // color_printk(ORANGE,BLACK,"OS Can Used Total 2M PAGEs:%#018lx=%018ld\n",TotalMem,TotalMem);

    //PMMngr.e820_length points to the last `valid` e820_entry
    // TotalMem = PMMngr.e820_entrys[PMMngr.e820_length].address + PMMngr.e820_entrys[PMMngr.e820_length].length;
    // for (i = 0; i < PMMngr.e820_length; i++)
    // {
    //     uint64_t end;
    //     if (PMMngr.e820_entrys[i].type != 1)
    //         continue;
    //     end = (PMMngr.e820_entrys[i].address + PMMngr.e820_entrys[i].length);
    //     // TotalMem is the max address of the last valid e820_entry
    //     if (end > TotalMem)
    //         TotalMem = end;
    // }

    //bits map construction init
    PMMngr.bits_map = (uint64_t *)((PMMngr.start_brk + PAGE_4K_SIZE - 1) & PAGE_4K_MASK);
    PMMngr.bits_size = TotalMem >> PAGE_2M_SHIFT;
    PMMngr.bits_length = (((uint64_t)(TotalMem >> PAGE_2M_SHIFT) + sizeof(long) * 8 - 1) / 8) & ( ~ (sizeof(long) - 1));
    // mem_dump(PMMngr.bits_map, PMMngr.bits_map + sizeof(uint64_t)*64);
    memset(PMMngr.bits_map, 0xff, PMMngr.bits_length);
    // mem_dump(PMMngr.bits_map, PMMngr.bits_map + sizeof(uint64_t)*64);

    //pages construction init
    PMMngr.pages_struct = (struct Page *)(((uint64_t)PMMngr.bits_map + PMMngr.bits_length + PAGE_4K_SIZE - 1) & PAGE_4K_MASK);
    PMMngr.pages_size = TotalMem >> PAGE_2M_SHIFT;
    PMMngr.pages_length = ((TotalMem >> PAGE_2M_SHIFT) * sizeof(struct Page) + sizeof(long) - 1) & ( ~ (sizeof(long) - 1));
    memset(PMMngr.pages_struct, 0x00, PMMngr.pages_length);

    //zones construction init
    PMMngr.zones_struct = (struct Zone *)(((uint64_t)PMMngr.pages_struct + PMMngr.pages_length + PAGE_4K_SIZE - 1) & PAGE_4K_MASK);
    PMMngr.zones_size = 0;
    PMMngr.zones_length = (5 * sizeof(struct Zone) + sizeof(long) - 1) & ( ~ (sizeof(long) - 1));
    memset(PMMngr.zones_struct, 0x00, PMMngr.zones_length);

    for (i = 0; i <= PMMngr.e820_length; i++)
    {
        uint64_t start, end;
        struct Zone * z;
        struct Page * p;

        if (PMMngr.e820_entrys[i].type != 1)
            continue;
        start = PAGE_2M_ALIGN(PMMngr.e820_entrys[i].address);
        end = ((PMMngr.e820_entrys[i].address + PMMngr.e820_entrys[i].length) >> PAGE_2M_SHIFT) << PAGE_2M_SHIFT;
        if(end <= start)
            continue;
        //zone init
        z = PMMngr.zones_struct + PMMngr.zones_size;
        PMMngr.zones_size++;

        z->zone_start_address = start;
        z->zone_end_address = end;
        z->zone_length = end - start;

        z->page_using_count = 0;
        z->page_free_count = (end - start) >> PAGE_2M_SHIFT;

        z->total_pages_link = 0;

        z->attribute = 0;
        z->manager_struct = &PMMngr;

        z->pages_length = (end - start) >> PAGE_2M_SHIFT;
        z->pages_group = (struct Page *)(PMMngr.pages_struct + (start >> PAGE_2M_SHIFT));

        //page init
        p = z->pages_group;
        for (j = 0; j < z->pages_length; j++, p++)
        {
            p->zone_struct = z;
            p->phy_address = start + PAGE_2M_SIZE * j;
            p->attribute = 0;

            p->reference_count = 0;
            p->age = 0;
            // bits_map array start at 0, so right shift 2^6
            *(PMMngr.bits_map + ((p->phy_address >> PAGE_2M_SHIFT) >> 6)) ^= 1UL << (p->phy_address >> PAGE_2M_SHIFT) % 64;
        }
        
    }

    //init address 0 to page struct 0; because the PMMngr.e820[0].type != 1
    PMMngr.pages_struct->zone_struct = PMMngr.zones_struct;
    PMMngr.pages_struct->phy_address = 0UL;
    set_page_attribute(PMMngr.pages_struct, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
    PMMngr.pages_struct->reference_count = 1;
    PMMngr.pages_struct->age = 0;

    PMMngr.zones_length = (PMMngr.zones_size * sizeof(struct Zone) + sizeof(long) - 1) & ( ~ (sizeof(long) - 1));

    debug_mm("bits_map:%#018lx,bits_size:%#018lx,bits_length:%#018lx\n",PMMngr.bits_map,PMMngr.bits_size,PMMngr.bits_length);
	debug_mm("pages_struct:%#018lx,pages_size:%#018lx,pages_length:%#018lx\n",PMMngr.pages_struct,PMMngr.pages_size,PMMngr.pages_length);
	debug_mm("zones_struct:%#018lx,zones_size:%#018lx,zones_length:%#018lx\n",PMMngr.zones_struct,PMMngr.zones_size,PMMngr.zones_length);

    ZONE_DMA_INDEX = 0;
	ZONE_NORMAL_INDEX = PMMngr.zones_size - 1;  // all mapped zones
	ZONE_UNMAPPED_INDEX = 0;

    for (i = 0; i < PMMngr.zones_size; i++)
    {
        struct Zone * z = PMMngr.zones_struct + i;
        debug_mm("zone_start_address:%#018lx,zone_end_address:%#018lx,zone_length:%#018lx,pages_group:%#018lx,pages_length:%#018lx\n",z->zone_start_address,z->zone_end_address,z->zone_length,z->pages_group,z->pages_length);

        if (z->zone_start_address >= 0x100000000 && !ZONE_UNMAPPED_INDEX) {
            ZONE_UNMAPPED_INDEX = i;
            ZONE_NORMAL_INDEX = i - 1;  // last zone below 4GB
        }
    }

    debug_mm("ZONE_DMA_INDEX:%d\tZONE_NORMAL_INDEX:%d\tZONE_UNMAPED_INDEX:%d\n",ZONE_DMA_INDEX,ZONE_NORMAL_INDEX,ZONE_UNMAPPED_INDEX);

    PMMngr.end_of_struct = (uint64_t)((uint64_t)PMMngr.zones_struct + PMMngr.zones_length + sizeof(long) * 32) & ( ~ (sizeof(long) - 1)); //leave some blank after PMMngr
    debug_mm("start_code:%#018lx,end_code:%#018lx,end_data:%#018lx,start_brk:%#018lx,end_of_struct:%#018lx\n",PMMngr.start_code,PMMngr.end_code,PMMngr.end_data,PMMngr.start_brk, PMMngr.end_of_struct);

    // page from 0 to PMMngr.end_of_struct are all ready used
    i = Virt_To_Phy(PMMngr.end_of_struct) >> PAGE_2M_SHIFT;

    for (j = 1; j <= i; j++)
    {
        struct Page * tmp_page = PMMngr.pages_struct + j;
        page_init(tmp_page, PG_PTable_Mapped | PG_Kernel_Init | PG_Kernel);
        *(PMMngr.bits_map + ((tmp_page->phy_address >> PAGE_2M_SHIFT) >> 6)) |= 1UL << (tmp_page->phy_address >> PAGE_2M_SHIFT) % 64;
        tmp_page->zone_struct->page_using_count++;
        tmp_page->zone_struct->page_free_count--;
    }
    debug_mm("1.PMMngr.bits_map:%#018lx\tzone_struct->page_using_count:%d\tzone_struct->page_free_count:%d\n",*PMMngr.bits_map,PMMngr.zones_struct->page_using_count,PMMngr.zones_struct->page_free_count);

    slab_init();
    list_init(&subpage_pools);
}

/*
    number: pages to alloc, must < 64
    zone_select: zone select from DMA, Mapped in Pagetable, Unmapped in Pagetable
    page_flags: struct Page flags
*/
struct Page * alloc_pages(int32_t zone_select, int32_t number, uint64_t page_flags)
{
    if (number > 64 || number < 0)
    {
        color_printk(RED, BLACK, "alloc_pages() ERROR: number is invalid\n");
        return NULL;
    }

    int32_t zone_start = 0;
    int32_t zone_end = 0;
    uint64_t attribute = 0;
    uint64_t page = 0;
    
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
    return NULL;

find_free_pages:
    return (struct Page *)(PMMngr.pages_struct + page);
}

/*
	page: free page start from this pointer
	number: number < 64
*/

void free_pages(struct Page * page,int32_t number)
{	
	int i = 0;
	
	if(page == NULL)
	{
		color_printk(RED,BLACK,"free_pages() ERROR: page is invalid\n");
		return ;
	}	

	if(number >= 64 || number <= 0)
	{
		color_printk(RED,BLACK,"free_pages() ERROR: number is invalid\n");
		return ;	
	}
	
	for(i = 0;i<number;i++,page++)
	{
		*(PMMngr.bits_map + ((page->phy_address >> PAGE_2M_SHIFT) >> 6)) &= ~(1UL << (page->phy_address >> PAGE_2M_SHIFT) % 64);
		page->zone_struct->page_using_count--;
		page->zone_struct->page_free_count++;
		page->attribute = 0;
	}
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
        int slot = (int)(offset / PAGE_4K_SIZE);
        if (slot >= 0 && slot < SUBPAGE_4K_COUNT) {
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