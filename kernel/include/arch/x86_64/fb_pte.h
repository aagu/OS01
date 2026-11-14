#ifndef _ARCH_X86_64_FB_PTE_H
#define _ARCH_X86_64_FB_PTE_H

/*
 * x86_64 framebuffer cache encoding (mm-architecture-boundaries Task 1).
 *
 * The ONLY place where a framebuffer cache request becomes x86 PTE
 * cache bits, so the kernel mapping, the /dev/fb user alias and the
 * early boot alias can never drift apart.
 *
 * Callers normalize the display policy (fb_cache_policy_t) into the
 * semantic enum BEFORE calling: only an explicit WRITEBACK becomes WB.
 *
 *   UC (uncacheable): PCD=1, PWT=1  -- device registers, and every
 *                                      framebuffer whose backend has
 *                                      not identified ordinary RAM.
 *   WB (write-back):  PCD=0, PWT=0  -- ordinary RAM (QEMU Standard VGA
 *                                      RAM backend).
 */

#include <stdint.h>
#include <memory/vm_device.h>
#include <arch/x86_64/pte.h>

#define PAGE_FB_CACHE_UC   (PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH)
#define PAGE_FB_CACHE_WB   (0UL)

static inline uint64_t x86_fb_cache_bits(enum vm_device_cache cache)
{
    return (cache == VM_DEVICE_CACHE_WB) ? PAGE_FB_CACHE_WB
                                         : PAGE_FB_CACHE_UC;
}

/* Kernel-side 2MiB framebuffer leaf entry. */
static inline uint64_t x86_fb_kernel_pmd_flags(enum vm_device_cache cache)
{
    return PAGE_KERNEL_PMD | x86_fb_cache_bits(cache);
}

#endif /* _ARCH_X86_64_FB_PTE_H */
