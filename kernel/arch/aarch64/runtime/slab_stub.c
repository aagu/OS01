/* kernel/arch/aarch64/slab_stub.c — slab and allocator stubs.
 *
 * The AArch64 build does not compile the x86-only slab implementation.
 * These no-op definitions provide the allocator symbols required by the
 * AArch64 kernel until a native slab allocator is available.
 *
 * Signatures mirror kernel/include/memory/slab.h exactly.
 */

#include <stddef.h>

size_t slab_init(void)
{
    return 0;
}

void *kmalloc(size_t size)
{
    (void)size;
    return NULL;
}

size_t kfree(void *address)
{
    (void)address;
    return 0;
}

void *kzalloc(size_t size)
{
    (void)size;
    return NULL;
}

size_t ksize(void *address)
{
    (void)address;
    return 0;
}
