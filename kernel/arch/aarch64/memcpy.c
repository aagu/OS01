/* kernel/arch/aarch64/memcpy.c — freestanding memcpy.
 *
 * AArch64 kernel links with -nostdlib, so provide the byte-copy helper
 * in-tree (mirror of kernel/arch/aarch64/memset.c).
 *
 * Overlapping regions: behaviour is unspecified per ISO C — we go
 * forwards, matching libc; callers that need overlap semantics must
 * use memmove instead. Currently no kernel caller overlaps.
 */

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}
