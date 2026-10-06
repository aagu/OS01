// kernel/arch/x86_64/runtime/printk_fb.c — x86_64 framebuffer mapping
// implementations, extracted from kernel/core/printk.c as part of the
// aarch64 color_printk enable (spec 2026-10-06). Bodies are byte-identical
// to the original; this file's only purpose is to host the arch-specific
// MMIO mapping code so the aarch64 build can supply its own variant.

#include <core/printk.h>            /* Pos, VIRT_FRAMEBUFFER_EARLY,
                                     VIRT_FRAMEBUFFER_OFFSET               */
#include <memory/memory.h>         /* flush_tlb                              */
#include <memory/vmm.h>            /* vmm_map_page, kernel_map, tlb_shootdown */
#include <memory/pmm.h>            /* PAGE_2M_SIZE / PAGE_2M_SHIFT / PAGE_2M_MASK */
#include <arch/x86_64/pte.h>       /* PAGE_* x86 hardware PTE bits           */

// Early framebuffer map via direct PDE writes, before PMM/VMM are available.
// Uses VIRT_FRAMEBUFFER_EARLY (within PUD[0]) for simple setup.
void frame_buffer_early_init()
{
    uint64_t *pmd = (uint64_t *)0xffff800000103000;
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        size_t level2 = (size_t)((VIRT_FRAMEBUFFER_EARLY + i) >> PAGE_2M_SHIFT) & 0x1FF;
        pmd[level2] = (((uint64_t)Pos.Phy_addr + i) & PAGE_2M_MASK)
            | (PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_EARLY;
    flush_tlb();
}

// Permanent framebuffer map via vmm_map_page, after PMM/VMM are available.
// Remaps to VIRT_FRAMEBUFFER_OFFSET in a separate PGD entry that never
// overlaps with the physical RAM direct mapping regardless of QEMU -m size.
void frame_buffer_init()
{
    for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
    {
        vmm_map_page(kernel_map, i + (uint64_t)Pos.Phy_addr,
            VIRT_FRAMEBUFFER_OFFSET + i,
            PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE);
    }
    Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_OFFSET;
    tlb_shootdown();
}