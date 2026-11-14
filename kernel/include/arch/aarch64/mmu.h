#ifndef _ARCH_AARCH64_MMU_H
#define _ARCH_AARCH64_MMU_H

/*
 * AArch64 MMU primitives (mm-architecture-boundaries Task 2).
 *
 * Root/single-page operations and the permission walker the ARM side
 * owns.  <arch/mmu.h> deliberately does NOT route these through the
 * arch facade — direct callers include this header.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <arch/mmu.h>   /* ARCH_PAGE_OFFSET (facade: per-port direct map) */

#define AARCH64_USER_MIN 0x1000ULL
#define AARCH64_USER_LIMIT (1ULL << 48)

static inline uint64_t *arch_get_page_table(void)
{
    uint64_t ttbr0;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    return (uint64_t *)ttbr0;
}

static inline void arch_switch_mm(uint64_t *pgtbl)
{
    __asm__ __volatile__("msr ttbr0_el1, %0 \n\t isb" :: "r"(pgtbl) : "memory");
}

static inline void arch_flush_tlb_page(uintptr_t vaddr)
{
    __asm__ __volatile__("tlbi vae1, %0 \n\t dsb sy \n\t isb" :: "r"(vaddr >> 12) : "memory");
}

/* Snapshot of a lower-address range in a direct-mapped AArch64 root.
 * Implemented by the ARM user-root owner
 * (kernel/arch/aarch64/memory/user_root.c). */
bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                uint64_t len, bool writable);

#endif /* _ARCH_AARCH64_MMU_H */
