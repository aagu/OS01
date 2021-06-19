#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/bootinfo.h>
// #include <kernel/pmm.h>

#define PAGE_OFFSET ((unsigned long)0xffff800000000000)

#define PAGE_GDT_SHIFT 39

#define Virt_To_Phy(addr) ((unsigned long)(addr) - PAGE_OFFSET)
#define Phy_To_Virt(addr) ((unsigned long *)((unsigned long)(addr) + PAGE_OFFSET))

#define Virt_To_2M_Page(kaddr) (PMMngr.pages_struct + (Virt_To_Phy(kaddr) >> PAGE_2M_SHIFT))
#define Phy_to_2M_Page(kaddr) (PMMngr.pages_struct + ((unsigned long)(kaddr) >> PAGE_2M_SHIFT))

extern struct Physical_Memory_Manager PMMngr;

uint64_t * __attribute__((always_inline)) get_cr3()
{
    uint64_t * tmp;
    __asm__ __volatile__ (
        "movq   %%cr3,  %0  \n\t"
        :"=r"(tmp)
        :
        :"memory"
    );
    return tmp;
}

void pmm_init(struct MEMORY_INFO E820_Info);
// void free_pages(struct Page * page,int32_t number);
// struct Page * alloc_pages(int32_t zone_select, int32_t number, uint64_t page_flags);
void vmm_init();
void mem_dump(const void * start, const void * end);

#endif