/* kernel/memory/vm_area.c — arch-neutral VMA list primitives.
 *
 * Extracted from kernel/memory/vma.c so the AArch64 anonymous-VM
 * policy can share the sorted-list rules without dragging in the x86
 * file-mapping, COW and PTE code.  The legacy names in vma.c are
 * wrappers over these three functions.
 */

#include <errno.h>
#include <stddef.h>
#include <kernel.h>             /* container_of */
#include <list.h>
#include <memory/vm_area.h>
#include <memory/vma.h>
#include <sched/task.h>         /* complete mm_t (vma_list) */

vma_t *vm_area_lookup(mm_t *mm, uint64_t va)
{
    if (!mm)
        return NULL;
    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (va >= v->vm_start && va < v->vm_end)
            return v;
        if (va < v->vm_start)
            break;              /* sorted — va falls in a gap */
        pos = pos->next;
    }
    return NULL;
}

int vm_area_insert_sorted(mm_t *mm, vma_t *vma)
{
    if (!mm || !vma)
        return -EINVAL;

    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (vma->vm_start < v->vm_start)
            break;
        pos = pos->next;
    }
    list_add_to_before(pos, &vma->list);
    return 0;
}

void vm_area_unlink(vma_t *vma)
{
    if (vma)
        list_del(&vma->list);
}
