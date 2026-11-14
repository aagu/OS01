#ifndef _ARCH_MMU_H
#define _ARCH_MMU_H

/*
 * Arch MMU facade (mm-architecture-boundaries Task 2).
 *
 * Only two things are shared across architectures through this header:
 *   - ARCH_PAGE_OFFSET: the direct physical-map base of each port;
 *   - arch_flush_tlb_all(): each port's local full-TLB reload.
 *
 * Root/single-page primitives (arch_get_page_table, arch_switch_mm,
 * arch_flush_tlb_page) and the permission walkers live in the per-arch
 * <arch/<arch>/mmu.h>; direct callers include those.  This facade must
 * not leak descriptor codecs, raw roots or page-table implementation
 * details.
 */

#include <stdint.h>

#ifdef __x86_64__

// Higher-half base address for direct physical memory mapping.
// All physical RAM is mapped at this offset (256th PGD entry).
// Host backends compile the real walker with -DARCH_PAGE_OFFSET=0
// (identity mapping), so the default only applies when not provided.
#ifndef ARCH_PAGE_OFFSET
#define ARCH_PAGE_OFFSET 0xffff800000000000ULL
#endif

// Reload CR3 to flush entire TLB
static inline void arch_flush_tlb_all(void) {
    uint64_t cr3;
    __asm__ __volatile__("movq %%cr3, %0; movq %0, %%cr3" : "=r"(cr3) :: "memory");
}

#elif defined(__aarch64__)

#ifndef ARCH_PAGE_OFFSET
#define ARCH_PAGE_OFFSET 0xffff000000000000ULL
#endif

static inline void arch_flush_tlb_all(void)
{
    __asm__ __volatile__("tlbi vmalle1 \n\t dsb sy \n\t isb" ::: "memory");
}

#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_MMU_H */
