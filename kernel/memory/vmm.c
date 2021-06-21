#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <stdint.h>
#include <stdlib.h>

// kernel map
uint64_t *kernel_map = NULL;

// get next Level of map
uint64_t *get_next_level(uint64_t *current_level, size_t entry, uint64_t flags)
{
    if (!current_level[entry] & 1)
    {
        current_level[entry] = (uint64_t)calloc(PAGE_4K_SIZE);
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
    kernel_map = (uint64_t *)calloc(PAGE_4K_SIZE);

    // map 0~32M
    for (uintptr_t i = 0; i < (PAGE_2M_SIZE << 4); i += PAGE_2M_SIZE)
    {
        vmm_map_page(kernel_map, i, i, PAGE_KERNEL_Page);
    }

    // dump_memory_map();
    
    switch_tlb(kernel_map);
}