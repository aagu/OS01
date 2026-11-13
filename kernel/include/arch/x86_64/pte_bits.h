#ifndef _ARCH_X86_64_PTE_BITS_H
#define _ARCH_X86_64_PTE_BITS_H

/*
 * x86_64 page-descriptor codecs (x86 PTE permissions plan, Task 1).
 *
 * Every read/write of the ADDRESS and ATTRIBUTE halves of a hardware
 * page descriptor goes through these helpers.  The physical address
 * field is extracted per granularity so NX (bit 63), the software
 * bits (PAGE_COW bit 10, PAGE_PROTNONE bit 9), A/D and the PAT bits
 * never mix into a PA and never get lost on the way back in:
 *
 *   4 KiB leaf:  PA = bits 51:12   (PAT is bit 7)
 *   2 MiB leaf:  PA = bits 51:21   (PS is bit 7, PAT is bit 12)
 *   1 GiB leaf:  PA = bits 51:30   (query/decode only; OS01 creates
 *                                   no 1 GiB leaves)
 *
 * Pure VIRTUAL address alignment and PHYSICAL block alignment keep
 * using PAGE_4K_MASK / PAGE_2M_MASK from <memory/pmm.h> — those
 * masks are NOT descriptor masks (they intentionally contain bit 63
 * and friends, which is exactly why descriptor arithmetic must not
 * use them).
 */

#include <stdint.h>

/* ── Named PAT bits ────────────────────────────────────────────
 * Bit 7 is dual-purpose on x86_64: PS ("this entry is a leaf") in
 * parent entries — PAGE_HUGE — and PAT in a 4 KiB leaf.  Bit 12 is
 * PAT in a large (2 MiB / 1 GiB) leaf.  PAGE_PAT_LARGE replaces the
 * ambiguous historical PAGE_PAT name. */
#define PAGE_PAT_4K     (1UL << 7)    /* PAT in a 4 KiB leaf == PAGE_HUGE */
#define PAGE_PAT_LARGE  (1UL << 12)   /* PAT in a 2 MiB / 1 GiB leaf      */

/* Descriptor address fields (x86_64 MAXPHYADDR up to 52 bits). */
#define X86_PTE_4K_PA_MASK   0x000FFFFFFFFFF000ULL
#define X86_PTE_2M_PA_MASK   0x000FFFFFFFE00000ULL
#define X86_PTE_1G_PA_MASK   0x000FFFFFC0000000ULL

/* ── 4 KiB leaf ───────────────────────────────────────────────── */
static inline uint64_t x86_pte_4k_pa(uint64_t entry)
{
    return entry & X86_PTE_4K_PA_MASK;
}

static inline uint64_t x86_pte_4k_flags(uint64_t entry)
{
    return entry & ~X86_PTE_4K_PA_MASK;
}

static inline uint64_t x86_pte_4k_make(uint64_t pa, uint64_t flags)
{
    return (pa & X86_PTE_4K_PA_MASK) | (flags & ~X86_PTE_4K_PA_MASK);
}

/* ── 2 MiB leaf ───────────────────────────────────────────────── */
static inline uint64_t x86_pte_2m_pa(uint64_t entry)
{
    return entry & X86_PTE_2M_PA_MASK;
}

static inline uint64_t x86_pte_2m_flags(uint64_t entry)
{
    return entry & ~X86_PTE_2M_PA_MASK;
}

static inline uint64_t x86_pte_2m_make(uint64_t pa, uint64_t flags)
{
    return (pa & X86_PTE_2M_PA_MASK) | (flags & ~X86_PTE_2M_PA_MASK);
}

/* ── 1 GiB leaf (decode/query only) ───────────────────────────── */
static inline uint64_t x86_pte_1g_pa(uint64_t entry)
{
    return entry & X86_PTE_1G_PA_MASK;
}

#endif /* _ARCH_X86_64_PTE_BITS_H */
