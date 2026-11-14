#ifndef _ARCH_X86_64_MMU_H
#define _ARCH_X86_64_MMU_H

/*
 * x86_64 MMU primitives (mm-architecture-boundaries Task 2).
 *
 * Root/single-page operations and the permission walkers the x86 side
 * owns.  <arch/mmu.h> deliberately does NOT route these through the
 * arch facade — direct callers include this header.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Return the current page table base (CR3, raw register value — the
// low bits are PCID flags, not a direct-map pointer).
static inline uint64_t *arch_get_page_table(void) {
    uint64_t cr3;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)cr3;
}

// Switch address space (load page table base register).
static inline void arch_switch_mm(uint64_t *pgdir) {
    __asm__ __volatile__("movq %0, %%cr3" : : "r"(pgdir) : "memory");
}

// Invalidate a single 4KB page mapping
static inline void arch_flush_tlb_page(uintptr_t vaddr) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}

// Walk 4-level page table (PGD→PUD→PMD→PT), return full physical
// address (page base + in-page offset), or 0 if unmapped.
// Does NOT interpret PTE flags -- that stays in arch/x86_64/trap.c.
// Supports 4 KiB / 2 MiB / 1 GiB translations; implemented in the x86
// VMM translation unit (kernel/arch/x86_64/memory/vmm.c) so host
// harnesses can link the real walker with -DARCH_PAGE_OFFSET=0.
uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va);

// Cross-level effective-permission walk: returns true iff every page in
// [addr, addr+len) is valid + user-accessible + (writable ? RW or COW set),
// ANDing perms across pgd→pud→pmd→pte (x86 semantics: any level
// USER=0 → supervisor page, any level WRITE=0 → read-only, COW bit
// 10 (x86_64 ignored) counts as a writable-eligible marker).
// Implemented in the x86 VMM translation unit.  See the contract
// comment next to the definition in kernel/arch/x86_64/memory/vmm.c.
bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                uint64_t len, bool writable);

#endif /* _ARCH_X86_64_MMU_H */
