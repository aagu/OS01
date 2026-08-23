#ifndef _ARCH_MMU_H
#define _ARCH_MMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __x86_64__

// Higher-half base address for direct physical memory mapping.
// All physical RAM is mapped at this offset (256th PML4 entry).
#define ARCH_PAGE_OFFSET 0xffff800000000000ULL

// Return the current page table base (CR3 on x86_64, TTBR0_EL1 on aarch64).
static inline uint64_t *arch_get_page_table(void) {
    uint64_t cr3;
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)cr3;
}

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
    uint64_t *pml3 = (uint64_t *)((pml4[l4] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l3 = (va >> 30) & 0x1FF;
    if (!(pml3[l3] & 1)) return 0;
    if (pml3[l3] & 0x80)  // 1GB huge page
        return (pml3[l3] & 0xFFFFFC0000000ULL) | (va & 0x3FFFFFFF);
    uint64_t *pml2 = (uint64_t *)((pml3[l3] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l2 = (va >> 21) & 0x1FF;
    if (!(pml2[l2] & 1)) return 0;
    if (pml2[l2] & 0x80)  // 2MB huge page
        return (pml2[l2] & 0xFFFFFFFE00000ULL) | (va & 0x1FFFFF);
    uint64_t *pml1 = (uint64_t *)((pml2[l2] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l1 = (va >> 12) & 0x1FF;
    if (!(pml1[l1] & 1)) return 0;
    return (pml1[l1] & 0xFFFFFFFFFFFFF000ULL) | (va & 0xFFF);
}

// Cross-level effective-permission walk: returns true iff every page in
// [addr, addr+len) is present + user-accessible + (writable ? RW set),
// ANDing perms across pml4→pdp→pd→pt (x86 semantics: any level U/S=0 →
// supervisor page, any level RW=0 → read-only).  Handles 4KB + 2MB pages
// (OS01 creates no 1GB pages, defensive false on 1GB PDP entry).
//
// addr+len overflow is self-guarded here — callers may legitimately pass
// (addr=2, len=UINT64_MAX) for hostile-input filtering, so we cannot rely
// on the caller to pre-check.
//
// `pgtbl` is parameterized (not read from CR3) so the selftest (Task 3)
// can exercise the walker against synthetic page tables.
static inline bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                              uint64_t len, bool writable)
{
    uint64_t *pml4 = (uint64_t *)pgtbl;
    if (len == 0) return true;                       // empty range → trivially OK
    if (addr + len < addr) return false;              // addr+len overflow
    uint64_t end = addr + len;
    for (uint64_t va = addr & ~(uint64_t)0xFFF; va < end; ) {
        uint64_t l4 = (va >> 39) & 0x1FF;
        if (!(pml4[l4] & 1)) return false;
        bool user = !!(pml4[l4] & 4), rw = !!(pml4[l4] & 2);

        uint64_t *pml3 = (uint64_t *)((pml4[l4] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
        uint64_t l3 = (va >> 30) & 0x1FF;
        if (!(pml3[l3] & 1)) return false;
        user = user && !!(pml3[l3] & 4);
        rw   = rw   && !!(pml3[l3] & 2);
        if (pml3[l3] & 0x80) return false;           // 1GB: defensive (not created)

        uint64_t *pml2 = (uint64_t *)((pml3[l3] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
        uint64_t l2 = (va >> 21) & 0x1FF;
        if (!(pml2[l2] & 1)) return false;
        user = user && !!(pml2[l2] & 4);
        rw   = rw   && !!(pml2[l2] & 2);
        if (pml2[l2] & 0x80) {                       // 2MB huge page (PDE is leaf)
            if (!user || (writable && !rw)) return false;
            va = (va & ~(uint64_t)0x1FFFFF) + 0x200000ULL;
            continue;
        }

        uint64_t *pml1 = (uint64_t *)((pml2[l2] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
        uint64_t l1 = (va >> 12) & 0x1FF;
        if (!(pml1[l1] & 1)) return false;
        user = user && !!(pml1[l1] & 4);
        rw   = rw   && !!(pml1[l1] & 2);
        if (!user || (writable && !rw)) return false;
        va += 0x1000ULL;
    }
    return true;
}

#elif defined(__aarch64__)

#define ARCH_PAGE_OFFSET 0xffff000000000000ULL

static inline uint64_t *arch_get_page_table(void)
{
    uint64_t ttbr0;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    return (uint64_t *)ttbr0;
}

static inline void arch_flush_tlb_all(void)
{
    __asm__ __volatile__("tlbi vmalle1 \n\t dsb sy \n\t isb" ::: "memory");
}

static inline void arch_flush_tlb_page(uintptr_t vaddr)
{
    __asm__ __volatile__("tlbi vae1, %0 \n\t dsb sy \n\t isb" :: "r"(vaddr >> 12) : "memory");
}

static inline void arch_switch_mm(uint64_t *pgtbl)
{
    __asm__ __volatile__("msr ttbr0_el1, %0 \n\t isb" :: "r"(pgtbl) : "memory");
}

// Walk 4-level (48-bit VA) page table: PGD->PUD->PMD->PTE.
// Uses ARCH_PAGE_OFFSET to convert physical entries to direct-mapped virtual.
static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va)
{
    uint64_t *pgd = (uint64_t *)pgtbl;
    uint64_t l0 = (va >> 39) & 0x1FF;
    if (!(pgd[l0] & 1)) return 0;
    uint64_t *pud = (uint64_t *)((pgd[l0] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l1 = (va >> 30) & 0x1FF;
    if (!(pud[l1] & 1)) return 0;
    // 1GB block (table entry bit 1 set)
    if ((pud[l1] & 2) == 0)
        return (pud[l1] & 0xFFFFFC0000000ULL) | (va & 0x3FFFFFFF);
    uint64_t *pmd = (uint64_t *)((pud[l1] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l2 = (va >> 21) & 0x1FF;
    if (!(pmd[l2] & 1)) return 0;
    // 2MB block
    if ((pmd[l2] & 2) == 0)
        return (pmd[l2] & 0xFFFFFFFE00000ULL) | (va & 0x1FFFFF);
    uint64_t *pte = (uint64_t *)((pmd[l2] & ~(uint64_t)0xFFF) + ARCH_PAGE_OFFSET);
    uint64_t l3 = (va >> 12) & 0x1FF;
    if (!(pte[l3] & 1)) return 0;
    return (pte[l3] & 0xFFFFFFFFFFFFF000ULL) | (va & 0xFFF);
}

// aarch64 stub: fail-closed.  No uaccess support on this arch yet (OS01
// runs x86_64 only); a false return forces syscall_check_user_range to
// -EFAULT every user pointer, matching the "no SMAP/SMEP / no get_user_pages"
// scope (see design §10).
static inline bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                              uint64_t len, bool writable)
{
    (void)pgtbl; (void)addr; (void)len; (void)writable;
    return false;
}

#else
#error "Unsupported architecture"
#endif

#endif
