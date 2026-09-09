/* kernel/arch/aarch64/memset.c — freestanding memset.
 *
 * AArch64 kernel links with -nostdlib, so provide the byte-fill helper
 * in-tree.
 */

#include <stddef.h>

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}
