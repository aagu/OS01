// kernel/arch/x86_64/memory/vma.c — VMA node lifecycle + fork metadata.
//
// Owns VMA node destruction (x86_vma_destroy_node), the exec/exit
// teardown (vma_free_all, including retired / never-installed roots)
// and the fork metadata copy (fork_vma_copy).  The heap/brk commit
// lives in brk.c; the mmap/munmap/mprotect transactions live in
// mmap.c; the kernel→user write lock pair lives in uaccess.c.
#include <memory/vma.h>        // VMA_* logical flags
#include <memory/vm_area.h>    // shared list primitives
#include <arch/x86_64/vma.h>   // owner surface (declares this TU's API)
#include <kernel.h>            // container_of
#include <sched/task.h>        // complete mm_t (vma_list)
#include <memory/slab.h>       // kmalloc / kfree
#include <fs/file.h>           // vfs_node_get / vfs_node_put
#include <memory/pmm.h>        // PAGE_2M_SIZE / PAGE_4K_SIZE
#include <memory/memory.h>     // Phy_To_Virt
#include <errno.h>             // -EINVAL / -ENOMEM
#include <string.h>            // memcpy
#include <arch/x86_64/vmm.h>   // x86_vmm_has_2m / take_* / release_owned_4k

// Destroy a single VMA node: unlink it from whatever list it is on,
// drop its file reference and free the node.  Does NOT free pages.
// Safe on NULL.  Used by the live-root teardown, the fork rollback and
// the retired-node drain alike (callers hold no mm->lock here).
void x86_vma_destroy_node(vma_t *vma)
{
    if (!vma) return;
    vm_area_unlink(vma);
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

    uint64_t *user_pgd = NULL;
    if (mm->pgdir)
        user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);

    while (mm->vma_list.next != &mm->vma_list) {
        vma_t *v = container_of(mm->vma_list.next, vma_t, list);

        /* Retired or never-installed root: no TLB sync is needed
         * (nothing can install this root again — user leaves are
         * non-global and no PCID is used, so stale entries on other
         * CPUs are unreachable).  Revoke every leaf through the
         * allocation-free take; borrowed (VMA_IO) leaves — 4 KiB
         * and full 2 MiB PMD blocks alike — are cleared but NEVER
         * released: the device keeps owning their pages. */
        if (user_pgd) {
            bool borrowed = (v->vm_flags & VMA_IO) != 0;
            uint64_t va = v->vm_start;
            while (va < v->vm_end) {
                if ((va & (PAGE_2M_SIZE - 1)) == 0 &&
                    va + PAGE_2M_SIZE <= v->vm_end &&
                    x86_vmm_has_2m(user_pgd, va)) {
                    x86_vmm_take_2m(user_pgd, va);   /* clear only */
                    va += PAGE_2M_SIZE;
                    continue;
                }
                uint64_t old = x86_vmm_take_4k(user_pgd, va);
                if (old && !borrowed)
                    x86_vmm_release_owned_4k(old);
                va += PAGE_4K_SIZE;
            }
        }

        x86_vma_destroy_node(v);
    }

    list_init(&mm->vma_list);
}

// Deep-copy parent's VMA list to child (unmap/TLB ownership plan
// Task 1).  Every node must copy for the caller to proceed to the
// child page-table build; on the first allocation failure the
// partial child metadata is destroyed here and -ENOMEM returned.
// The parent's nodes and references are never touched. */
int fork_vma_copy(mm_t *child_mm, mm_t *parent_mm)
{
    if (!child_mm || !parent_mm) return -EINVAL;

    list_t *pos = parent_mm->vma_list.next;
    while (pos != &parent_mm->vma_list) {
        vma_t *pv = container_of(pos, vma_t, list);
        vma_t *cv = (vma_t *)kmalloc(sizeof(vma_t));
        if (!cv) goto fail;

        memcpy(cv, pv, sizeof(vma_t));
        list_init(&cv->list);
        if (cv->vm_file)
            vfs_node_get(cv->vm_file);
        vm_area_insert_sorted(child_mm, cv);

        pos = pos->next;
    }
    return 0;

fail:
    /* Destroy the partial child metadata: unlink each node, drop
     * the file reference this call took, free the node.  The net
     * vfs_node_get/put balance returns to zero. */
    while (child_mm->vma_list.next != &child_mm->vma_list) {
        vma_t *v = container_of(child_mm->vma_list.next, vma_t, list);
        x86_vma_destroy_node(v);
    }
    return -ENOMEM;
}
