#ifndef _ARCH_AARCH64_MMU_H
#define _ARCH_AARCH64_MMU_H
/* Host shadow of the production <arch/aarch64/mmu.h> for the ARM
 * page-table fixture (compiles aarch64 production code on the x86
 * host).  Uses the PRODUCTION include guard so the real header is a
 * no-op when both are included; ARCH_PAGE_OFFSET must match the old
 * <arch/mmu.h> shadow exactly (identical macro redefinition is
 * benign, but one value keeps the harness sane). */
#include <stdint.h>
#include <stddef.h>

#ifndef ARCH_PAGE_OFFSET
#define ARCH_PAGE_OFFSET UINT64_C(0x100000000)
#endif

#define AARCH64_USER_MIN   UINT64_C(0x1000)
#define AARCH64_USER_LIMIT (UINT64_C(1) << 48)

static inline uint64_t *arch_get_page_table(void) { return NULL; }
static inline void arch_switch_mm(uint64_t *pgtbl)
{
    (void)pgtbl;
}
static inline void arch_flush_tlb_page(uintptr_t vaddr) { (void)vaddr; }
bool arch_user_range_accessible(void *pgtbl, uint64_t addr,
                                uint64_t len, bool writable);
#endif
