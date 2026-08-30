#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/bootinfo.h>
#include <kernel/arch/mmu.h>
// #include <kernel/pmm.h>

#define PAGE_OFFSET ARCH_PAGE_OFFSET

#define PAGE_GDT_SHIFT 39

#define Virt_To_Phy(addr) ((unsigned long)(addr) - PAGE_OFFSET)
#define Phy_To_Virt(addr) ((unsigned long *)((unsigned long)(addr) + PAGE_OFFSET))

#define Virt_To_2M_Page(kaddr) (PMMngr.pages_struct + (Virt_To_Phy(kaddr) >> PAGE_2M_SHIFT))
#define Phy_to_2M_Page(kaddr) (PMMngr.pages_struct + ((unsigned long)(kaddr) >> PAGE_2M_SHIFT))

extern struct Physical_Memory_Manager PMMngr;

// Backward-compatible alias — returns current page table base.
// New code should use arch_get_page_table() directly.
#define get_cr3() arch_get_page_table()

void pmm_init(const struct BOOT_MEMORY_MAP *map);
// void free_pages(struct Page * page,int32_t number);
// struct Page * alloc_pages(int32_t zone_select, int32_t number, uint64_t page_flags);
void vmm_init();
void mem_dump(const void * start, const void * end);

#endif