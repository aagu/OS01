#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <core/bootinfo.h>
#include <arch/mmu.h>
// #include <memory/pmm.h>

#define PAGE_OFFSET ARCH_PAGE_OFFSET

#define PAGE_PGD_SHIFT 39   // top-level page-table index shift (39 on x86_64, etc.)

#define Virt_To_Phy(addr) ((unsigned long)(addr) - PAGE_OFFSET)
#define Phy_To_Virt(addr) ((unsigned long *)((unsigned long)(addr) + PAGE_OFFSET))

/* pmm_init indexes descriptors relative to the first RAM frame. Its
 * physical address is recorded in slot zero, including for nonzero RAM
 * bases. Callers pass addresses belonging to represented RAM frames. */
#define Phy_to_2M_Page(paddr) (PMMngr.pages_struct + \
    (((unsigned long)(paddr) - PMMngr.pages_struct[0].phy_address) >> PAGE_2M_SHIFT))
#define Virt_To_2M_Page(kaddr) Phy_to_2M_Page(Virt_To_Phy(kaddr))

extern struct Physical_Memory_Manager PMMngr;

// Backward-compatible alias — returns current page table base.
// New code should use arch_get_page_table() directly.
#define get_cr3() arch_get_page_table()

void pmm_init(const struct boot_context *ctx);
// void free_pages(struct Page * page,int32_t number);
// struct Page * alloc_pages(int32_t zone_select, int32_t number, uint64_t page_flags);
void vmm_init();
void mem_dump(const void * start, const void * end);

#endif
