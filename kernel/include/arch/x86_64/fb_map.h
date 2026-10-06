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

#endif /* _ARCH_X86_64_FB_MAP_H */
