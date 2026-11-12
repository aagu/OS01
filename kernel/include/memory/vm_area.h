#ifndef _KERNEL_MEMORY_VM_AREA_H
#define _KERNEL_MEMORY_VM_AREA_H

/* Shared VMA linked-list primitives (x86_64 + aarch64).
 *
 * The list and the vma_t layout are arch-neutral (see <memory/vma.h>),
 * so the three operations the anonymous-VM policy needs live here and
 * are compiled on both architectures.  The legacy x86 vma_find /
 * vma_insert / vma_remove entry points remain in kernel/memory/vma.c
 * as thin wrappers; vma_remove keeps the x86-only file-reference drop
 * and node free on top of vm_area_unlink().
 */

#include <stdint.h>

struct mm_struct;
typedef struct mm_struct mm_t;

struct vm_area_struct;
typedef struct vm_area_struct vma_t;

/* Find the VMA containing addr, or NULL.  The list is sorted by
 * vm_start, so the walk stops at the first VMA that starts above it.
 * A zero-length VMA never matches. */
vma_t *vm_area_lookup(mm_t *mm, uint64_t va);

/* Insert vma into mm's list sorted by vm_start.  Does not merge
 * adjacent VMAs (same rule the x86 implementation documents).
 * Returns 0, -EINVAL for a NULL mm/vma. */
int vm_area_insert_sorted(mm_t *mm, vma_t *vma);

/* Unlink vma from whatever list it is on.  Does not free the node and
 * does not drop vm_file — both stay with the owning implementation. */
void vm_area_unlink(vma_t *vma);

#endif /* _KERNEL_MEMORY_VM_AREA_H */
