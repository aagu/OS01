#ifndef M1_A64_HOST_MMU_H
#define M1_A64_HOST_MMU_H
#include <stdint.h>
#include <stddef.h>
/* Use a host-userspace-friendly address for the "direct map" base so
 * the test harness can mmap() a backing region here.  Real aarch64
 * uses 0xffff000000000000 (kernel half); x86_64 host treats addresses
 * above 0x00007fffffffffff as non-canonical and mmap refuses them. */
#define ARCH_PAGE_OFFSET UINT64_C(0x100000000)

/* Minimal host stubs for the few symbols page_table.c pulls in via
 * <arch/mmu.h>.  On real aarch64 the production header defines them
 * via inline asm; on x86 host (where this mock is used) the
 * production header's __x86_64__ branch would normally provide
 * arch_get_page_table / arch_flush_tlb_all — but that branch sets
 * ARCH_PAGE_OFFSET to 0xffff800000000000, not the aarch64 value
 * above.  We shadow the whole header so ARCH_PAGE_OFFSET stays
 * aarch64-correct, and define the two functions the production
 * x86 branch would have defined.  arch_get_page_table() returns
 * the host CR3 (which never matches a test root, so is_active_root
 * in page_table.c always returns false and the guarded TLBI path
 * is never reached). */
static inline uint64_t *arch_get_page_table(void)
{
    /* Mock: return a sentinel the test never uses.  Real aarch64 reads
     * TTBR0_EL1 (a kernel-mode MRS — not legal from userspace on
     * Linux); the x86 production branch would `mov %cr3, %r`, which
     * IS legal from userspace on x86 Linux but we'd rather not
     * touch CR3 in a unit test.  Returning NULL guarantees
     * is_active_root() returns false in page_table.c, so the
     * guarded TLBI path is never taken. */
    return NULL;
}
static inline void arch_flush_tlb_all(void)
{
    uint64_t cr3;
    __asm__ __volatile__("movq %%cr3, %0; movq %0, %%cr3" : "=r"(cr3) :: "memory");
}
static inline void arch_flush_tlb_page(uintptr_t vaddr)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr) : "memory");
}
static inline void arch_switch_mm(uint64_t *pgtbl)
{
    __asm__ __volatile__("movq %0, %%cr3" : : "r"(pgtbl) : "memory");
}
#endif
