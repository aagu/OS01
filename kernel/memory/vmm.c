#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <kernel/printk.h>
#include <stdint.h>
#include <stdlib.h>
#include <driver/serial.h>

// kernel map
mmap kernel_map;

// get next Level of map
mmap get_next_level(uint64_t *current_level, size_t entry, uint64_t flags)
{
    if (!(current_level[entry] & 1))
    {
        current_level[entry] = Virt_To_Phy((uint64_t)calloc(PAGE_4K_SIZE));
        current_level[entry] |= flags;
    }
    return (uint64_t *) (current_level[entry] & PAGE_4K_MASK);
}

// map virtual page to physical address
void vmm_map_page(uint64_t *pagemap, uintptr_t physical_address, uintptr_t virtual_address, uint64_t flags)
{
    uint64_t *pml4, *pml3, *pml2;
    size_t level4, level3, level2;

    level4 = (size_t) (virtual_address >> PAGE_GDT_SHIFT) & 0x1ff;
    level3 = (size_t) (virtual_address >> PAGE_1G_SHIFT) & 0x1ff;
    level2 = (size_t) (virtual_address >> PAGE_2M_SHIFT) & 0x1ff;

    pml4 = pagemap;
    // Use user-accessible intermediate levels when mapping a user page
    uint64_t gdt_flags = (flags & PAGE_U_S) ? PAGE_USER_GDT : PAGE_KERNEL_GDT;
    uint64_t dir_flags = (flags & PAGE_U_S) ? PAGE_USER_Dir : PAGE_KERNEL_Dir;
    pml3 = get_next_level(pml4, level4, gdt_flags);
    pml2 = get_next_level(pml3, level3, dir_flags);
    pml2[level2] = (physical_address & PAGE_2M_MASK) | flags;

    // If modifying the shared kernel page table after APs are online,
    // broadcast TLB invalidation so other cores drop stale PDE/TLB entries.
    if (pagemap == kernel_map && num_cpus > 1)
        tlb_shootdown();
}

// unmap virtual page to physical address, return the physical address
uintptr_t vmm_unmap_page(uint64_t *pagemap, uintptr_t virtual_address)
{
    uint64_t *pml4, *pml3, *pml2;
    size_t level4, level3, level2;

    level4 = (size_t) (virtual_address >> PAGE_GDT_SHIFT) & 0x1ff;
    level3 = (size_t) (virtual_address >> PAGE_1G_SHIFT) & 0x1ff;
    level2 = (size_t) (virtual_address >> PAGE_2M_SHIFT) & 0x1ff;

    pml4 = pagemap;
    if (!(pml4[level4] & PAGE_Present))
        return 0;
    pml3 = (uint64_t *)Phy_To_Virt(pml4[level4] & PAGE_4K_MASK);
    if (!(pml3[level3] & PAGE_Present))
        return 0;
    pml2 = (uint64_t *)Phy_To_Virt(pml3[level3] & PAGE_4K_MASK);

    uintptr_t phys = pml2[level2] & PAGE_2M_MASK;
    pml2[level2] = 0;
    return phys;
}

static void dump_memory_map()
{
    mem_dump(kernel_map, kernel_map+512);
    // mem_dump(kernel_map+PAGE_4K_SIZE, kernel_map+512+PAGE_4K_SIZE);
}

void vmm_init()
{
    kernel_map = (uint64_t *)Phy_To_Virt(0x101000);
    uint64_t i, j;
    #ifdef DEBUG
    unsigned long * tmp = NULL;
    tmp = (unsigned long *)(((unsigned long)Phy_To_Virt((unsigned long)get_cr3() & (~ 0xfffUL))) + 8 * 256);
		
	color_printk(YELLOW,BLACK,"1:%#018lx,%#018lx\t\t\n",(unsigned long)tmp,*tmp);
	tmp = Phy_To_Virt(*tmp & (~0xfffUL));

	color_printk(YELLOW,BLACK,"2:%#018lx,%#018lx\t\t\n",(unsigned long)tmp,*tmp);
	tmp = Phy_To_Virt(*tmp & (~0xfffUL));
	color_printk(YELLOW,BLACK,"3:%#018lx,%#018lx\t\t\n",(unsigned long)tmp,*tmp);
    #endif

    for (i = 0; i < PMMngr.zones_size; i++)
    {
        struct Zone * z = PMMngr.zones_struct + i;
        struct Page * p = z->pages_group;

        if (ZONE_UNMAPPED_INDEX && i == ZONE_UNMAPPED_INDEX)
            break;
        
        for (j = 0; j < z->pages_length; j++, p++)
        {
            vmm_map_page(kernel_map, p->phy_address, (uintptr_t)Phy_To_Virt(p->phy_address), PAGE_KERNEL_Page);
            #ifdef DEBUG
            if(j % 50 == 0)
            {
                uint64_t *pml4, *pml3, *pml2;
                size_t level4, level3, level2;

                level4 = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_GDT_SHIFT) & 0x1ff;
                level3 = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_1G_SHIFT) & 0x1ff;
                level2 = (size_t) ((uintptr_t)Phy_To_Virt(p->phy_address) >> PAGE_2M_SHIFT) & 0x1ff;

                pml4 = kernel_map;
                pml3 = get_next_level(pml4, level4, PAGE_KERNEL_GDT);
                pml2 = get_next_level(pml3, level3, PAGE_KERNEL_Dir);

                // pml2[level2] = 0;
                color_printk(GREEN,BLACK,"-----\t\n");
                color_printk(GREEN,BLACK,"pml3:%#018lx,%#018lx\t\n",(unsigned long)pml3,pml3[level3]);
                color_printk(GREEN,BLACK,"pml2:%#018lx,%#018lx\t\n",(unsigned long)pml2,pml2[level2]);
            }
            #endif
        }
    }
    
    tlb_shootdown();
}

mmap vmm_alloc_map() {
    return (mmap)calloc(PAGE_4K_SIZE);
}

void vmm_free_user_map(mmap pagemap)
{
    if (!pagemap)
        return;

    // Walk PML4 entries 0–255 (user half). Entries 256–511 are
    // shared kernel entries and must not be freed.
    for (int l4 = 0; l4 < 256; l4++) {
        uint64_t pml4e = pagemap[l4];
        if (!(pml4e & PAGE_Present))
            continue;

        uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4e & PAGE_4K_MASK);

        // Walk PDPT entries (pointers to PDE pages)
        for (int l3 = 0; l3 < 512; l3++) {
            uint64_t pml3e = pml3[l3];
            if (!(pml3e & PAGE_Present))
                continue;

            uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3e & PAGE_4K_MASK);

            // Walk PDE entries (2MB pages via PAGE_PS)
            for (int l2 = 0; l2 < 512; l2++) {
                uint64_t pml2e = pml2[l2];
                if (!(pml2e & PAGE_Present))
                    continue;

                // Only 2MB pages (PAGE_PS set) are supported
                if (pml2e & PAGE_PS) {
                    uintptr_t phys = pml2e & PAGE_2M_MASK;
                    struct Page *page = Phy_to_2M_Page(phys);
                    page_clean(page);
                    free_pages(page, 1);
                }
            }

            kfree(pml2);
        }

        kfree(pml3);
    }

    kfree(pagemap);
}