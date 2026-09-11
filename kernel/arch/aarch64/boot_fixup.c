/* kernel/arch/aarch64/boot_fixup.c
 *
 * C-side one-shot fixup for a latent head.S bug: `build_pagetables`
 * fills ALL 512 slots of `PMD_low0` (covers physical 0..1 GiB) but
 * only PMD_low1[0] (the 2 MiB region holding the kernel image at LMA
 * 0x40000000..0x40200000). PMD_low1 slots 1..511 are left zero, so
 * physical 0x40200000..0x7FFFFFFF has no high-half direct-map alias.
 *
 * The first runtime caller to feel the gap is pmm.c's `alloc_4k_page`
 * which, after carving a fresh 2 MiB pool, writes a subpage_pool header
 * to the new page's direct-map alias (`Phy_To_Virt(0x40200000+)`). The
 * resulting access faults; the sync-EL1h handler at entry.S:59 is
 * `b .`, so the BSP spins forever and the QEMU harness kills at 90 s
 * with `complete=false`. The earlier `alloc_pages`/`free_pages` PMM
 * smoke does not exhibit the hang because it only mutates bitmaps
 * that already live inside the kernel-image range (which IS mapped).
 *
 * The fixup walks the installed PGD -> PUD -> PMD_low1 and writes
 * slots 1..511 as 2 MiB Normal kernel-RW non-exec block descriptors
 * (V=1, TYPE=0 [block], AP=00, SH=IS, AttrIdx=1=Normal, AF=1,
 * PXN=UXN=1) covering physical 0x40200000..0x80000000. Each write is
 * followed by `dsb ishst`; a single `dsb ish; isb` after the loop
 * publishes the range to the active root (BSP pre-SMP; TTBR0_EL1 ==
 * TTBR1_EL1 == boot_page_tables). The first mapping that previously
 * faulted is now a fresh, never-cached translation, so no explicit
 * TLBI is required (the ARM ARM does not cache faulting walks).
 *
 * Future work: migrate the fill into head.S `build_pagetables` so the
 * direct map is correct from the first instruction and this C-side
 * fixup can be deleted. The plan for this increment forbids touching
 * head.S, hence the C-side workaround.
 */

#include <stdint.h>
#include <stddef.h>
#include <kernel/arch/mmu.h>        /* ARCH_PAGE_OFFSET */
#include "aarch64_percpu.h"         /* aarch64_boot_page_tables_addr */

/* ── Block-descriptor flag bits ─────────────────────────────────── */

/* PA mask for a 2 MiB block: bits [47:21] of the descriptor. The
 * caller's PA is 2 MiB-aligned so the mask is a no-op, but applying
 * it explicitly makes any future miscalculation benign. */
#define BOOT_FIXUP_PA_MASK  UINT64_C(0xFFFFFFFFFFE00000)

/* Per-slot block-descriptor flag bits (no PA bits yet). Layout
 * exactly matches head.S's `make_2m` assembly path for PMD_low0
 * (head.S:426-434 + the PXN|UXN OR at head.S:443), but with V=1 and
 * TYPE=0 (block) — the OPPOSITE of the intermediate-table descriptor
 * this file's helper uses for L0/L1/L2 in page_table.c. Don't confuse
 * them. Slot 0 of PMD_low1 is filled by head.S with PXN cleared for
 * the kernel image; we only write slots 1..511 here. */
#define BOOT_FIXUP_BLOCK_FLAGS \
    (UINT64_C(0x001)               | /* V=1                          */ \
     UINT64_C(0x004)               | /* AttrIdx 1 (bits [4:2]) =     */ \
                                    /* Normal WB/WA                 */ \
     UINT64_C(0x300)               | /* SH[1:0] = 0b11               */ \
                                    /* inner-shareable (bits [9:8]) */ \
     UINT64_C(0x400)               | /* AF = 1 (bit 10)              */ \
     UINT64_C(0x20000000000000)    | /* PXN (bit 53)                 */ \
     UINT64_C(0x40000000000000))      /* UXN (bit 54)                 */

/* bit 1 is 0 — TYPE = block, not table. Implicit: the mask above
 * does not set bit 1, which is correct for a 2 MiB block descriptor
 * at PMD level. Asserted at compile time below. */
_Static_assert((BOOT_FIXUP_BLOCK_FLAGS & UINT64_C(0x002)) == 0,
               "2 MiB block descriptor must leave bit 1 (TYPE) clear");
_Static_assert((BOOT_FIXUP_BLOCK_FLAGS & UINT64_C(0x001)) != 0,
               "2 MiB block descriptor must set bit 0 (V)");
_Static_assert((BOOT_FIXUP_BLOCK_FLAGS & UINT64_C(0x07F8)) == UINT64_C(0x0700),
               "block flags AF|SH must match head.S PMD_low0 (AttrIdx "
               "bit 2 is outside the mask; PXN|UXN are too high)");
_Static_assert(BOOT_FIXUP_BLOCK_FLAGS == UINT64_C(0x60000000000705),
               "block flags + PXN|UXN exact-value lock-in");

#define BOOT_FIXUP_PMD_SLOTS  UINT64_C(512)
#define BOOT_FIXUP_PMD_BASE   UINT64_C(0x40000000)   /* PMD_low1 starts here */
#define BOOT_FIXUP_BLOCK_SIZE UINT64_C(0x00200000)   /* 2 MiB */

/* ── Tiny barrier helpers (kept local — the page-table primitives
 * have their own copies in page_table.c; this file runs from C
 * before the primitives are guaranteed ready). ────────────────── */

static inline void dsb_ishst(void)
{
    __asm__ __volatile__("dsb ishst" ::: "memory");
}

static inline void dsb_ish(void)
{
    __asm__ __volatile__("dsb ish" ::: "memory");
}

static inline void isb(void)
{
    __asm__ __volatile__("isb" ::: "memory");
}

/* ── The fixup ─────────────────────────────────────────────────── */

void aarch64_extend_direct_map(void)
{
    /* Resolve the installed PGD's physical base via the ldr-literal
     * helper in head.S:731. Convert only after the validation in
     * parent_pa-style checks below; for now we trust head.S to have
     * populated a 4 KiB-aligned, non-zero PA below 1 TiB. */
    uint64_t pgd_pa = aarch64_boot_page_tables_addr();
    uint64_t *pgd = (uint64_t *)(pgd_pa + ARCH_PAGE_OFFSET);

    /* PGD[0] → PUD_low. Must be a valid table descriptor (V=1,
     * bit1=1). If not, head.S layout invariant is broken — return
     * silently rather than corrupt the tree. The caller (the BSP)
     * will hang at the next translation fault, which is the same
     * observable failure mode as the original bug, so this fixup
     * can't make things worse.
     *
     * Table-descriptor PA is at bits [47:12]; mask 0x0000_00ff_ffff_f000.
     * (Do NOT use BOOT_FIXUP_PA_MASK — that's the 2 MiB-block mask
     * 0xffff_ffff_ffe0_0000 and would zero the lower PA bits.) */
    uint64_t pgd_slot = pgd[0];
    if ((pgd_slot & UINT64_C(0x003)) != UINT64_C(0x003)) return;
    uint64_t pud_pa = pgd_slot & UINT64_C(0x000000fffffff000);
    if (pud_pa == 0) return;
    uint64_t *pud = (uint64_t *)(pud_pa + ARCH_PAGE_OFFSET);

    /* PUD[1] → PMD_low1. Same validity checks. */
    uint64_t pud_slot = pud[1];
    if ((pud_slot & UINT64_C(0x003)) != UINT64_C(0x003)) return;
    uint64_t pmd_pa = pud_slot & UINT64_C(0x000000fffffff000);
    if (pmd_pa == 0) return;
    uint64_t *pmd = (uint64_t *)(pmd_pa + ARCH_PAGE_OFFSET);

    /* Fill slots 1..511. Slot 0 is the head.S-installed kernel-image
     * block with PXN cleared — leave it alone. */
    for (uint64_t i = 1; i < BOOT_FIXUP_PMD_SLOTS; ++i) {
        uint64_t pa = BOOT_FIXUP_PMD_BASE + i * BOOT_FIXUP_BLOCK_SIZE;
        uint64_t desc = BOOT_FIXUP_BLOCK_FLAGS | (pa & BOOT_FIXUP_PA_MASK);
        pmd[i] = desc;
        dsb_ishst();
    }

    /* Completion fence: publish the writes to the active root. No
     * TLBI needed — these are new translations that never faulted
     * before (pmm.c faults on the phy's direct-map alias, which is
     * not a VA that the TLB caches as a successful translation). */
    dsb_ish();
    isb();
}