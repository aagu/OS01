// kernel/arch/x86_64/runtime/printk_fb.c — x86_64 framebuffer mapping
// implementations, extracted from kernel/core/printk.c as part of the
// aarch64 color_printk enable (spec 2026-10-06). Bodies are byte-identical
// to the original; this file's only purpose is to host the arch-specific
// MMIO mapping code so the aarch64 build can supply its own variant.

#include <core/printk.h>            /* Pos, VIRT_FRAMEBUFFER_EARLY,
                                     VIRT_FRAMEBUFFER_OFFSET               */
#include <driver/fb_state.h>       /* fb_publish_initial_mapping             */
#include <driver/fb_cache.h>       /* fb_cache_policy_get                    */
#include <memory/memory.h>         /* flush_tlb                              */
#include <memory/vmm.h>            /* vmm_map_page, kernel_map, tlb_shootdown */
#include <memory/pmm.h>            /* PAGE_2M_SIZE / PAGE_2M_SHIFT / PAGE_2M_MASK */
#include <arch/x86_64/pte.h>       /* PAGE_* x86 hardware PTE bits           */
#include <arch/x86_64/fb_map.h>    /* fb_x86_revoke_early_alias              */
#include <log/log.h>               /* log_info                               */

/* The boot-time PMD table that holds the early framebuffer alias
 * (PUD[0]'s PMD table, physical 0x103000).  vmm_init() adopts the boot
 * PGD (physical 0x101000) as the kernel map, so this table stays live:
 * the early alias has to be revoked explicitly, see
 * fb_x86_revoke_early_alias(). */
#define FB_EARLY_PMD_TABLE ((uint64_t *)0xffff800000103000)

// Early framebuffer map via direct PDE writes, before PMM/VMM are available.
// Uses VIRT_FRAMEBUFFER_EARLY (within PUD[0]) for simple setup.
void frame_buffer_early_init()
{
    uint64_t *pmd = FB_EARLY_PMD_TABLE;
    /* Same policy as every other framebuffer mapping.  At this point no
     * backend has been identified yet, so the policy is still the
     * conservative UNCACHEABLE default. */
    uint64_t cache_bits = fb_x86_cache_bits(fb_cache_policy_get());
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        size_t level2 = (size_t)((VIRT_FRAMEBUFFER_EARLY + i) >> PAGE_2M_SHIFT) & 0x1FF;
        pmd[level2] = (((uint64_t)Pos.Phy_addr + i) & PAGE_2M_MASK)
            | (PAGE_KERNEL_PMD | cache_bits);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_EARLY;
    flush_tlb();
    fb_publish_initial_mapping(Pos.FB_addr, Pos.FB_length);
}

// Permanent framebuffer map via vmm_map_page, after PMM/VMM are available.
// Remaps to VIRT_FRAMEBUFFER_OFFSET in a separate PGD entry that never
// overlaps with the physical RAM direct mapping regardless of QEMU -m size.
void frame_buffer_init()
{
    uint64_t cache_bits = fb_x86_cache_bits(fb_cache_policy_get());
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        vmm_map_page(kernel_map, i + (uint64_t)Pos.Phy_addr,
            VIRT_FRAMEBUFFER_OFFSET + i,
            PAGE_KERNEL_PMD | cache_bits);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_OFFSET;
    tlb_shootdown();

    /* Revoke the early alias.  From here on the framebuffer physical
     * pages have exactly ONE mapping (the permanent one above), so the
     * kernel mapping, the /dev/fb user alias and the early boot range
     * can never access the same page under two different memory types
     * (IA32_PAT forbids memory-type aliasing).  The permanent mapping
     * is already installed and Pos.FB_addr already points at it, so no
     * console write can reach the revoked range. */
    int revoked = fb_x86_revoke_early_alias(FB_EARLY_PMD_TABLE,
                                            VIRT_FRAMEBUFFER_EARLY,
                                            Pos.FB_length);
    if (revoked > 0) {
        log_info("fb: revoked %d early alias entries\n", revoked);
    }
    fb_publish_initial_mapping(Pos.FB_addr, Pos.FB_length);
}
