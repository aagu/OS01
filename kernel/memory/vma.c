// kernel/memory/vma.c — VMA linked-list management
#include <kernel/vma.h>
#include <kernel/task.h>
#include <kernel.h>
#include <kernel/slab.h>
#include <kernel/file.h>
#include <kernel/pmm.h>
#include <kernel/memory.h>
#include <string.h>
#include <errno.h>

// Find the VMA containing addr, or NULL
vma_t *vma_find(mm_t *mm, uint64_t addr)
{
    if (!mm) return NULL;
    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (addr >= v->vm_start && addr < v->vm_end)
            return v;
        if (addr < v->vm_start)
            break; // sorted — addr falls in a gap
        pos = pos->next;
    }
    return NULL;
}

// Insert vma into mm->vma_list sorted by vm_start.
// Does NOT merge adjacent VMAs (simpler; VMA count < 20 for busybox).
// Returns 0 on success.
int vma_insert(mm_t *mm, vma_t *vma)
{
    if (!mm || !vma) return -EINVAL;

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

// Remove and free a single VMA node (does NOT free pages).
void vma_remove(mm_t *mm, vma_t *vma)
{
    (void)mm;
    if (!vma) return;
    list_del(&vma->list);
    if (vma->vm_file)
        vfs_node_put(vma->vm_file);
    kfree(vma);
}

// Free ALL VMAs and their physical pages.  Called by exec/exit.
// Does NOT touch 2MB ELF pages (those are tracked outside VMA).
// Both anonymous and file-backed pages are freed via free_4k_page —
// in V1, file-backed pages also allocate from the subpage pool,
// so the slot is returned to the pool for reuse.
void vma_free_all(mm_t *mm)
{
    if (!mm) return;

    uint64_t *user_pml4 = NULL;
    if (mm->pml4)
        user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)mm->pml4);

    while (mm->vma_list.next != &mm->vma_list) {
        vma_t *v = container_of(mm->vma_list.next, vma_t, list);

        // If we have a valid pml4, unmap the physical pages.
        // If pml4 is NULL (shouldn't happen), skip unmap but still
        // free the VMA node + vfs_node_put to avoid leaks.
        if (user_pml4) {
            for (uint64_t va = v->vm_start; va < v->vm_end;
                 va += PAGE_4K_SIZE) {
                vmm_unmap_4k_page(user_pml4, va);
            }
        }

        vma_remove(mm, v);
    }

    list_init(&mm->vma_list);
}

// Deep-copy parent's VMA list to child.
vma_t *fork_vma_copy(mm_t *child_mm, mm_t *parent_mm)
{
    if (!child_mm || !parent_mm) return NULL;

    list_t *pos = parent_mm->vma_list.next;
    while (pos != &parent_mm->vma_list) {
        vma_t *pv = container_of(pos, vma_t, list);
        vma_t *cv = (vma_t *)kmalloc(sizeof(vma_t));
        if (!cv) { pos = pos->next; continue; }

        memcpy(cv, pv, sizeof(vma_t));
        list_init(&cv->list);
        if (cv->vm_file)
            vfs_node_get(cv->vm_file);
        vma_insert(child_mm, cv);

        pos = pos->next;
    }
    return NULL; // caller doesn't use return value
}
