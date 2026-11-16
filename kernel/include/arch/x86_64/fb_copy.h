#ifndef _ARCH_X86_64_FB_COPY_H
#define _ARCH_X86_64_FB_COPY_H

// kernel/include/arch/x86_64/fb_copy.h — the x86_64 strong override of
// the arch-neutral framebuffer copy facade (kernel/include/arch/fb_copy.h).
//
// The x86 wide write: a sequential 32-bit store loop instead of a byte
// copy.  On real hardware the byte copy of a full frame costs ~550 ms,
// the wide write ~9 ms (see
// docs/superpowers/plans/2026-11-15-x86-framebuffer-wide-write.md).
//
// This header carries the PRIMITIVE only; the arch-neutral geometry
// helpers (row offsets, packed write-back, in-place scroll) live in
// kernel/include/arch/fb_copy.h and call fb_copy_rgb32().
//
// It is NOT a general-purpose memcpy replacement and it does not
// introduce kernel SIMD.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* x86_fb_copy_rgb32 — the wide write.
 *
 * Copy `bytes` of RGB32 pixel data from `src` to `dst`.
 *
 * Preconditions the CALLER must have established:
 *   * `dst` points into a framebuffer pixel region whose format and
 *     bounds have already been validated;
 *   * `src` and `dst` MUST NOT overlap.
 *
 * When both addresses and the length are 4-byte aligned the copy uses
 * sequential `volatile uint32_t` stores — the wide write verified on
 * real hardware.  Anything else falls back to a conservative byte copy.
 */
static inline void x86_fb_copy_rgb32(void *dst, const void *src, size_t bytes)
{
    if (bytes == 0)
        return;

    if ((((uintptr_t)dst | (uintptr_t)src) & 3u) != 0 || (bytes & 3u) != 0) {
        memcpy(dst, src, bytes);
        return;
    }

    volatile uint32_t *out = (volatile uint32_t *)dst;
    const uint32_t *in = (const uint32_t *)src;
    size_t n = bytes / 4u;
    for (size_t i = 0; i < n; ++i)
        out[i] = in[i];
}

#endif /* _ARCH_X86_64_FB_COPY_H */
