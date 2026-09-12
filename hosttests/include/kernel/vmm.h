#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

#include <stdint.h>

//page table attribute

//bit 63 Execution Disable:
#define PAGE_NO_EXEC       (1UL << 63)
//bit 12 Page Attribute Table:
#define PAGE_PAT      (1UL << 12)
//bit 8 Global Page:1,global;0,part
#define PAGE_GLOBAL   (1UL << 8)
//bit 7 Page Size:1,big page;0,small page
#define PAGE_HUGE       (1UL << 7)
//bit 6 Dirty:1,dirty;0,clean
#define PAGE_Dirty    (1UL << 6)
//bit 5 Accessed:1,visited;0,unvisited
#define PAGE_Accessed (1UL << 5)
//bit 4 Page Level Cache Disable
#define PAGE_CACHE_DISABLE      (1UL << 4)
//bit 3 Page Level Write Through
#define PAGE_WRITE_THROUGH      (1UL << 3)
//bit 2 User Supervisor:1,user and supervisor;0,supervisor
#define PAGE_USER      (1UL << 2)
//bit 1 Read Write:1,read and write;0,read
#define PAGE_WRITE      (1UL << 1)
//bit 0 Present:1,present;0,not present
#define PAGE_VALID  (1UL << 0)

#define PAGE_KERNEL_PGD  (PAGE_WRITE | PAGE_VALID)
#define PAGE_KERNEL_PUD  (PAGE_WRITE | PAGE_VALID)
#define	PAGE_KERNEL_PMD (PAGE_HUGE  | PAGE_WRITE | PAGE_VALID)
// MMIO (uncacheable): PCD=1, PWT=1 for Strong Uncacheable (UC)
#define PAGE_KERNEL_MMIO (PAGE_HUGE | PAGE_WRITE | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH | PAGE_VALID)
#define PAGE_USER_PGD    (PAGE_USER | PAGE_WRITE | PAGE_VALID)
#define PAGE_USER_PUD    (PAGE_USER | PAGE_WRITE | PAGE_VALID)
#define	PAGE_USER_PMD   (PAGE_HUGE  | PAGE_USER | PAGE_WRITE | PAGE_VALID)

#define KERNEL_MEM_OFFSET 0xffffffff80000000
#define PHYS_MEM_OFFSET 0xffff800000000000

#define mmap uint64_t*

extern mmap kernel_map;

#define flush_tlb()				            \
do								            \
{								            \
	unsigned long	tmpreg;					\
	__asm__ __volatile__ 	(				\
				"movq	%%cr3,	%0	\n\t"	\
				"movq	%0,	%%cr3	\n\t"	\
				:"=r"(tmpreg)			    \
				:				            \
				:"memory"			        \
				);				            \
}while(0)

#define switch_tlb(tlb)								\
do													\
{													\
	__asm__ __volatile__("mov %0, %%cr3"::"r"(tlb));\
} while (0);										\

void vmm_map_page(uint64_t *pagemap, uintptr_t physical_address,
                  uintptr_t virtual_address, uint64_t flags);
uintptr_t vmm_unmap_page(uint64_t *pagemap, uintptr_t virtual_address);
mmap vmm_alloc_map(void);
void vmm_free_user_map(mmap pagemap);

// ── TLB shootdown (SMP) ─────────────────────────────
// When modifying shared kernel page tables (kernel_map), other CPUs
// may have stale TLB entries.  Call this instead of flush_tlb() to
// notify all online cores via IPI.  Internally falls through to
// local flush_tlb() when num_cpus ≤ 1.
void tlb_shootdown(void);

#endif