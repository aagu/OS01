#ifndef _ARCH_X86_64_MMU_H
#define _ARCH_X86_64_MMU_H

/*
 * Non-privileged host-test mock for <arch/x86_64/mmu.h>.
 * Records arch_switch_mm(pgdir) without executing privileged movq %cr3.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void aslr_exec_record_switch_mm(uint64_t *pgdir);

static inline uint64_t *arch_get_page_table(void)
{
    return NULL;
}

static inline void arch_switch_mm(uint64_t *pgdir)
{
    aslr_exec_record_switch_mm(pgdir);
}

static inline void arch_flush_tlb_page(uintptr_t vaddr)
{
    (void)vaddr;
}

static inline uintptr_t arch_virt_to_phys(void *pgtbl, uintptr_t va)
{
    (void)pgtbl;
    return va;
}

static inline bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                              uint64_t len, bool writable)
{
    (void)pgtbl;
    (void)addr;
    (void)len;
    (void)writable;
    return true;
}

#endif /* _ARCH_X86_64_MMU_H */
