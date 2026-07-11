#ifndef _ARCH_MMU_H
#define _ARCH_MMU_H

#include <stdint.h>

#ifdef __x86_64__

// Reload CR3 to flush entire TLB
static inline void arch_flush_tlb_all(void) {
    uint64_t cr3;
    __asm__ __volatile__("movq %%cr3, %0; movq %0, %%cr3" : "=r"(cr3) :: "memory");
}

// Invalidate a single 4KB page mapping
static inline void arch_flush_tlb_page(uintptr_t vaddr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

// Switch address space (load page table base)
static inline void arch_switch_mm(uint64_t *pml4) {
    __asm__ __volatile__("movq %0, %%cr3" : : "r"(pml4) : "memory");
}

// Walk 4-level page table (PML4→PDPT→PD→PT), return full physical
// address (page base + in-page offset), or 0 if unmapped.
// Does NOT interpret PTE flags — that stays in arch/x86_64/trap.c.
static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va) {
    uint64_t *pml4 = (uint64_t *)pgtbl;
    uint64_t l4 = (va >> 39) & 0x1FF;
    if (!(pml4[l4] & 1)) return 0;
    uint64_t *pml3 = (uint64_t *)((pml4[l4] & ~(uint64_t)0xFFF) + 0xffff800000000000ULL);
    uint64_t l3 = (va >> 30) & 0x1FF;
    if (!(pml3[l3] & 1)) return 0;
    if (pml3[l3] & 0x80)  // 1GB huge page
        return (pml3[l3] & 0xFFFFFC0000000ULL) | (va & 0x3FFFFFFF);
    uint64_t *pml2 = (uint64_t *)((pml3[l3] & ~(uint64_t)0xFFF) + 0xffff800000000000ULL);
    uint64_t l2 = (va >> 21) & 0x1FF;
    if (!(pml2[l2] & 1)) return 0;
    if (pml2[l2] & 0x80)  // 2MB huge page
        return (pml2[l2] & 0xFFFFFFFE00000ULL) | (va & 0x1FFFFF);
    uint64_t *pml1 = (uint64_t *)((pml2[l2] & ~(uint64_t)0xFFF) + 0xffff800000000000ULL);
    uint64_t l1 = (va >> 12) & 0x1FF;
    if (!(pml1[l1] & 1)) return 0;
    return (pml1[l1] & 0xFFFFFFFFFFFFF000ULL) | (va & 0xFFF);
}

#elif defined(__aarch64__)
#error "aarch64 mmu.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
