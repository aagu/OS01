/* test/mock/slab_include/arch/mmu.h — host shadow of
 * kernel/include/arch/mmu.h for compiling the REAL
 * kernel/memory/slab.c in the host test suite (test_slab_idempotent_reservation,
 * test_slab_basic_x86_count).
 *
 * The production <arch/mmu.h> defines ARCH_PAGE_OFFSET as a large
 * direct-map alias (0xffff800000000000 on x86_64) and pulls in
 * memory.h's `Virt_To_Phy(addr) = addr - PAGE_OFFSET`.  On the host,
 * our test fixture uses VA == PA (test_platform.h's identity
 * Phy_To_Virt / Virt_To_Phy), so the production offset would map
 * every slab_init PA computation into a huge unsigned value and
 * Phy_to_2M_Page's RAM-relative index would land far outside the
 * fixture's pages_struct[] array.
 *
 * Override ARCH_PAGE_OFFSET = 0 here so the production macros
 * collapse to the identity map. Only the minimum the slab test
 * transitively needs (the constant + a forward decl to break the
 * header's own dependency chain) is shadowed.
 */
#ifndef _ARCH_MMU_H
#define _ARCH_MMU_H

#define ARCH_PAGE_OFFSET 0ULL

#endif /* _ARCH_MMU_H */