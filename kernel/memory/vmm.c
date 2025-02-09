#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <stdint.h>
#include <stdlib.h>

// kernel map
mmap kernel_map;

// get next Level of map
mmap get_next_level(uint64_t *current_level, size_t entry, uint64_t flags)
{
    if (!current_level[entry] & 1)
    {
        current_level[entry] = Virt_To_Phy((uint64_t)calloc(PAGE_4K_SIZE));
        current_level[entry] |= flags;
    }
    return (uint64_t *) (current_level[entry] & ~(0x1ff));
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
    pml3 = get_next_level(pml4, level4, PAGE_KERNEL_GDT);
    pml2 = get_next_level(pml3, level3, PAGE_KERNEL_Dir);
    pml2[level2] = (physical_address & PAGE_2M_MASK) | flags;
}

// unmap virtual page to physical address
void vmm_unmap_page(uint64_t *pagemap, uintptr_t virtual_address)
{
    uint64_t *pml4, *pml3, *pml2;
    size_t level4, level3, level2;

    level4 = (size_t) (virtual_address >> PAGE_GDT_SHIFT) & 0x1ff;
    level3 = (size_t) (virtual_address >> PAGE_1G_SHIFT) & 0x1ff;
    level2 = (size_t) (virtual_address >> PAGE_2M_SHIFT) & 0x1ff;

    pml4 = pagemap;
    pml3 = get_next_level(pml4, level4, PAGE_KERNEL_GDT);
    pml2 = get_next_level(pml3, level3, PAGE_KERNEL_Dir);

    pml2[level2] = 0;
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
    
    flush_tlb();
}

mmap vmm_alloc_map() {
    return (mmap)calloc(sizeof(mmap));
}