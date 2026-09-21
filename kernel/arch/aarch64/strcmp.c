/* kernel/arch/aarch64/strcmp.c — freestanding strcmp.
 *
 * kernel/subsys/subsys.c:subsys_status() calls strcmp(name, name) to
 * match a registered subsystem by name. On x86_64 this resolves via
 * the libc sysroot (-isystem .../usr/include provides the prototype
 * in <string.h> and libc/string/strcmp.c provides the symbol). On
 * aarch64 the kernel links -nostdlib with no libc sysroot, so we
 * provide a minimal in-tree implementation — same pattern as
 * kernel/arch/aarch64/memset.c.
 *
 * Byte-by-byte unsigned compare; stops at the first mismatch or at
 * '\0' on both sides (POSIX strcmp contract).
 */

#include <stddef.h>

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
