#ifndef _ARCH_X86_64_FB_MAP_H
#define _ARCH_X86_64_FB_MAP_H

#include <stdint.h>

/*
 * fb_x86_map_checked:
 * Maps physical framebuffer memory into VIRT_FRAMEBUFFER_OFFSET using checked
 * 2MiB pages. Verifies alignment, checks range overflow, verifies each PMD
 * entry physical address and cache flags, flushes TLB, and leaves no partial
 * mapping on error.
 */
int fb_x86_map_checked(uint64_t phys, uint64_t size, uint32_t **out_addr);

/*
 * fb_x86_revoke_early_alias:
 * Removes the boot-time early framebuffer alias.  frame_buffer_early_init()
 * maps the framebuffer physical pages a second time at VIRT_FRAMEBUFFER_EARLY
 * inside the boot page table -- and that boot page table IS the kernel page
 * table (vmm_init() adopts the boot PGD), so the alias stays reachable after
 * the permanent mapping is installed.  Leaving it in place would let one
 * physical page be accessed under two different cache attributes.
 *
 * pmd_table is the PMD table that holds the early alias entries (the boot
 * PUD[0] PMD table); va_base/size describe the aliased range.  Returns the
 * number of entries that were cleared, or -EINVAL on a bad argument.
 */
int fb_x86_revoke_early_alias(uint64_t *pmd_table, uint64_t va_base, uint64_t size);

#endif /* _ARCH_X86_64_FB_MAP_H */
