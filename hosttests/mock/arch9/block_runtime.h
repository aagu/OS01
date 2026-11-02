/* hosttests/mock/arch9/block_runtime.h — mock environment for block tests */
#ifndef ARCH9_BLOCK_RUNTIME_H
#define ARCH9_BLOCK_RUNTIME_H

#include "test_platform.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
#endif

#ifndef debug_block
#define debug_block(...) do {} while (0)
#endif

/* The production block registry is a dynamically allocated, lock
 * protected list.  test_platform.h already provides the canonical host
 * spinlock_T surface, so the architecture spinlock header has to stay
 * out of the way (it would redefine spinlock_T). */
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H 1
#endif

/* The block registry allocates its objects through the slab.  The host
 * harness does not link the slab, so the production slab header is
 * skipped and the allocator entry points are declared here — the same
 * pattern the poll harness uses.  Every binary that links a
 * kernel/block/blockdev.c object must provide kmalloc/kfree. */
#ifndef _KERNEL_SLAB_H
#define _KERNEL_SLAB_H 1
#endif

void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* ARCH9_BLOCK_RUNTIME_H */
