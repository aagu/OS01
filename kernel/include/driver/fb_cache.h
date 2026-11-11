#ifndef _DRIVER_FB_CACHE_H
#define _DRIVER_FB_CACHE_H

#include <stdint.h>

/* ── Framebuffer cache policy ─────────────────────────────────────
 *
 * The physical pages behind a framebuffer can be two very different
 * kinds of hardware:
 *
 *   - device registers (MMIO).  These MUST stay uncacheable: a
 *     cached access to a register would break the device protocol and
 *     could hang the machine.
 *   - ordinary RAM behind a device BAR.  The QEMU Standard VGA RAM
 *     backend is the case OS01 supports: BAR0 is plain host RAM (QEMU
 *     creates it with memory_region_init_ram_flags_nomigrate() and the
 *     host scanout reads the very same bytes), so the framebuffer
 *     pages are cacheable memory and must NOT be mapped uncacheable.
 *
 * Which one it is depends on the BACKEND, so the backend publishes the
 * policy and every mapping path consumes it:
 *
 *   - the kernel's permanent framebuffer mapping
 *     (fb_x86_map_checked(), frame_buffer_init()),
 *   - the /dev/fb user alias (fb_mmap()),
 *   - the early boot alias (frame_buffer_early_init(), later revoked).
 *
 * One policy for all of them is not cosmetic: x86 forbids accessing
 * the same physical page under two different memory types (IA32_PAT
 * "avoid memory-type aliasing"), and a mixed UC/WB pair on the same
 * page is exactly that violation.
 *
 * The default is UNCACHEABLE: a platform that has not identified its
 * framebuffer backend (the generic GOP fallback) keeps the
 * conservative, always-correct-for-MMIO choice.  Only a backend that
 * positively identified ordinary RAM may publish WRITEBACK.
 */
typedef enum {
    FB_CACHE_POLICY_UNCACHEABLE = 0, /* UC: PCD=1, PWT=1 */
    FB_CACHE_POLICY_WRITEBACK  = 1,  /* WB: PCD=0, PWT=0 */
} fb_cache_policy_t;

/* Current policy.  Published once during boot by the identified
 * framebuffer backend, read afterwards by the mapping paths. */
fb_cache_policy_t fb_cache_policy_get(void);
void fb_cache_policy_set(fb_cache_policy_t policy);

#endif /* _DRIVER_FB_CACHE_H */
