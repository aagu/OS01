// kernel/memory/vma.c — VMA linked-list management
#include <kernel/vma.h>
#include <kernel/task.h>
#include <kernel.h>
#include <kernel/slab.h>
#include <kernel/file.h>
#include <kernel/pmm.h>
#include <kernel/memory.h>
#include <kernel/arch/spinlock.h>   // mm->lock: guards munmap/MAP_FIXED/mprotect
#include <string.h>
#include <stdlib.h>                 // calloc (used by mm_alloc)
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

        if (v->vm_flags & VM_IO) {
            vma_remove(mm, v);
            continue;
        }

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

// mm_alloc — allocate + initialize an mm_t.  Centralizes the "lock = 1L"
// invariant so no call site can forget it (lock == 0 is the LOCKED state
// and would deadlock the first taker).
mm_t *mm_alloc(void)
{
    mm_t *mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (!mm) return NULL;
    list_init(&mm->vma_list);
    mm->mmap_base = 0x40000000;
    spin_init(&mm->lock);
    return mm;
}

// ── Helper: convert prot/flags to vm_page_prot flags ──────────
static int prot_to_page_flags(int prot, uint64_t *page_prot, uint64_t *vm_flags)
{
    *vm_flags = 0;

    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;

    // x86: reject pure PROT_WRITE first (hardware can't do write-only)
    if ((prot & PROT_WRITE) && !(prot & PROT_READ))
        return -EINVAL;

    // x86: PROT_EXEC or PROT_EXEC|PROT_WRITE → implicit PROT_READ
    if (prot & PROT_EXEC)
        prot |= PROT_READ;

    if (prot == PROT_NONE) {
        *page_prot = PAGE_U_S;
        *vm_flags = 0;
    } else if (prot == PROT_READ) {
        *page_prot = PAGE_USER_4K_RO;
        *vm_flags = VM_READ;
    } else if (prot == (PROT_READ | PROT_WRITE)) {
        *page_prot = PAGE_USER_4K;
        *vm_flags = VM_READ | VM_WRITE;
    } else if (prot == (PROT_READ | PROT_EXEC) ||
               prot == (PROT_READ | PROT_WRITE | PROT_EXEC)) {
        *page_prot = PAGE_USER_4K;  // no NX support yet
        *vm_flags = VM_READ | VM_EXEC
                  | ((prot & PROT_WRITE) ? VM_WRITE : 0);
    } else {
        return -EINVAL;
    }
    return 0;
}

// ── Helper: convert mmap flags to vm_flags ────────────────────
static void map_flags_to_vm(int flags, uint64_t *vm_flags)
{
    if (flags & MAP_SHARED)  *vm_flags |= VM_SHARED;
    if (flags & MAP_ANONYMOUS) *vm_flags |= VM_ANON;
}

// ── do_munmap ─────────────────────────────────────────────────
// do_munmap_locked — unmaps WITHOUT taking mm->lock (caller holds it).
// Defined before do_mmap because do_mmap(MAP_FIXED) calls it.
static int64_t do_munmap_locked(uint64_t addr, uint64_t length)
{
    addr   = PAGE_4K_ALIGN(addr);
    length = PAGE_4K_ALIGN(length);
    if (length == 0)
        return -EINVAL;

    uint64_t end = addr + length;
    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);

    list_t *pos = current->mm->vma_list.next;
    while (pos != &current->mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        pos = pos->next;

        if (v->vm_end <= addr)   continue;
        if (v->vm_start >= end)  break;

        uint64_t u_start = (addr > v->vm_start) ? addr : v->vm_start;
        uint64_t u_end   = (end  < v->vm_end)   ? end  : v->vm_end;
        for (uint64_t va = u_start; va < u_end; va += PAGE_4K_SIZE)
            vmm_unmap_4k_page(user_pml4, va);

        uint64_t orig_start = v->vm_start;
        uint64_t orig_end   = v->vm_end;

        if (u_start <= orig_start && u_end >= orig_end) {
            vma_remove(current->mm, v);
            continue;
        }

        if (u_start > orig_start && u_end < orig_end) {
            // Split: left + right
            v->vm_end = u_start;

            vma_t *right = (vma_t *)kmalloc(sizeof(vma_t));
            if (right) {
                memcpy(right, v, sizeof(vma_t));
                list_init(&right->list);
                right->vm_start = u_end;
                right->vm_end   = orig_end;
                if (right->vm_file)
                    vfs_node_get(right->vm_file);
                vma_insert(current->mm, right);
            }
        } else if (u_start > orig_start) {
            // Truncate right side
            v->vm_end = u_start;
        } else {
            // Truncate left side
            v->vm_start = u_end;
        }
    }

    flush_tlb();
    return 0;
}

// do_munmap — public entry: take mm->lock, then unmap.
int64_t do_munmap(uint64_t addr, uint64_t length)
{
    spin_lock(&current->mm->lock);
    int64_t rc = do_munmap_locked(addr, length);
    spin_unlock(&current->mm->lock);
    return rc;
}

// ── do_mmap ───────────────────────────────────────────────────
int64_t do_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                uint64_t flags, uint64_t fd, uint64_t offset)
{
    // ── 1. Argument validation ──────────────────────────
    if (length == 0)
        return -EINVAL;

    uint64_t end = PAGE_4K_ALIGN(addr + length);
    if (end < addr)  // overflow
        return -EINVAL;

    if (offset & (PAGE_4K_SIZE - 1))
        return -EINVAL;

    if (flags & MAP_ANONYMOUS) {
        if ((int64_t)fd != -1)
            return -EINVAL;
    } else {
        file_t *file = NULL;
        if (fd < NOFILE && current->files)
            file = current->files->fd[fd];
        if (!file || !file->node)
            return -EBADF;
    }

    if (flags & MAP_FIXED) {
        if (addr & (PAGE_4K_SIZE - 1))
            return -EINVAL;
        if (addr >= current->addr_limit)
            return -ENOMEM;
    }

    // ── 2. Address computation ──────────────────────────
    length = PAGE_4K_ALIGN(length);
    if (!length) return -EINVAL;

    uint64_t search = current->mm->mmap_base;
    vma_t *prev = NULL;
    list_t *pos = current->mm->vma_list.next;
    addr = 0;

    if (!(flags & MAP_FIXED)) {
        while (pos != &current->mm->vma_list) {
            vma_t *v = container_of(pos, vma_t, list);
            uint64_t gap_start = prev ? prev->vm_end : search;
            if (gap_start < search) gap_start = search;
            if (gap_start + length <= v->vm_start) {
                addr = gap_start;
                break;
            }
            prev = v;
            pos = pos->next;
        }
        if (!addr) addr = prev ? prev->vm_end : search;
        if (addr < search) addr = search;
    } else {
        spin_lock(&current->mm->lock);
        do_munmap_locked(addr, length);
        spin_unlock(&current->mm->lock);
    }

    if (addr + length > current->addr_limit)
        return -ENOMEM;
    if (addr >= current->addr_limit)
        return -ENOMEM;

    // ── 3. prot → page flags ───────────────────────────
    uint64_t page_prot, vm_flags_base;
    int rc = prot_to_page_flags((int)prot, &page_prot, &vm_flags_base);
    if (rc) return rc;
    map_flags_to_vm((int)flags, &vm_flags_base);

    // ── 4. Check for device/file mmap ──────────────────
    vfs_node_t *file_node = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        file_t *file = current->files->fd[fd];
        if (!file || !file->node) {
            if ((int64_t)fd != -1) return -EBADF;
            return -EINVAL;
        }
        file_node = vfs_node_get(file->node);

        // Device node with mmap handler → device path.
        // The mmap macro (uint64_t*) conflicts with ops->mmap field name.
        // Save the callback pointer before undefining, then restore macro.
        #undef mmap
        int (*_dev_mmap)(struct vfs_node *, struct vma *) =
            (file_node && file_node->ops &&
             (uint64_t)file_node->ops >= 0xffff800000000000ULL &&
             file_node->ops->mmap &&
             (uint64_t)file_node->ops->mmap >= 0xffff800000000000ULL)
            ? file_node->ops->mmap : NULL;
        #define mmap uint64_t*

        if (_dev_mmap) {
            if (!(flags & MAP_SHARED))
                { vfs_node_put(file_node); return -EINVAL; }

            // Pre-allocate VMA for the device handler to fill PTEs
            vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
            if (!vma) { vfs_node_put(file_node); return -ENOMEM; }
            list_init(&vma->list);
            vma->vm_start     = addr;
            vma->vm_end       = addr + length;
            vma->vm_flags     = vm_flags_base | VM_IO;
            vma->vm_page_prot = page_prot;
            vma->vm_pgoff     = offset >> PAGE_4K_SHIFT;
            vma->vm_file      = file_node;  // handler may clear this

            int mmap_rc = _dev_mmap(file_node, (struct vma *)vma);

            if (mmap_rc < 0) {
                // Handler failed — vma not inserted, clean up
                vfs_node_put(file_node);
                kfree(vma);
                return mmap_rc;
            }
            // Handler filled PTEs (e.g. fb_mmap eager-fills + flush_tlb +
            //   vfs_node_put(vma->vm_file); vma->vm_file = NULL)
            vma_insert(current->mm, vma);
            return (int64_t)vma->vm_start;
        }
        // Normal file mapping → fall through to step 5
    }

    // ── 5. Allocate VMA ────────────────────────────────
    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
    if (!vma) {
        if (file_node) vfs_node_put(file_node);
        return -ENOMEM;
    }
    list_init(&vma->list);
    vma->vm_start     = addr;
    vma->vm_end       = addr + length;
    vma->vm_flags     = vm_flags_base;
    vma->vm_page_prot = page_prot;
    vma->vm_pgoff     = offset >> PAGE_4K_SHIFT;
    vma->vm_file      = file_node;

    vma_insert(current->mm, vma);

    return (int64_t)addr;
}

// ── do_mprotect ───────────────────────────────────────────────
int64_t do_mprotect(uint64_t addr, uint64_t length, uint64_t prot)
{
    addr   = PAGE_4K_ALIGN(addr);
    length = PAGE_4K_ALIGN(length);
    if (length == 0)
        return -EINVAL;
    if (addr + length < addr)  // overflow
        return -EINVAL;

    uint64_t end = addr + length;

    uint64_t new_page_prot, new_vm_flags;
    int rc = prot_to_page_flags((int)prot, &new_page_prot, &new_vm_flags);
    if (rc) return rc;

    spin_lock(&current->mm->lock);

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);

    list_t *pos = current->mm->vma_list.next;
    while (pos != &current->mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (v->vm_end <= addr)   { pos = pos->next; continue; }
        if (v->vm_start >= end)  break;

        // Hole check
        if (v->vm_start > addr) {
            spin_unlock(&current->mm->lock);
            return -ENOMEM;
        }

        // Update VMA
        v->vm_flags     &= ~(VM_READ | VM_WRITE | VM_EXEC);
        v->vm_flags     |= new_vm_flags;
        v->vm_page_prot  = new_page_prot;

        // Update existing PTEs
        uint64_t va_start = (addr > v->vm_start) ? addr : v->vm_start;
        uint64_t va_end   = (end < v->vm_end) ? end : v->vm_end;
        for (uint64_t va = va_start; va < va_end; va += PAGE_4K_SIZE) {
            uint64_t *pte = vmm_pt_walk(user_pml4, va, 0, 0);
            if (!pte) continue;
            if (!(*pte & (PAGE_Present | PAGE_PROTNONE))) continue;

            uint64_t phys = *pte & PAGE_4K_MASK;

            if (prot == PROT_NONE) {
                // Stash phys for later restore.
                // Preserve PAGE_COW if set — we keep our COW reference.
                uint64_t stash = phys | PAGE_U_S | PAGE_PROTNONE;
                if (*pte & PAGE_COW)
                    stash |= PAGE_COW;
                *pte = stash;
            } else {
                // Restoring from PROTNONE or changing existing mapping.
                // Check COW before blindly applying new_page_prot.
                if (*pte & PAGE_COW) {
                    if (page_cow_refs(phys) > 1) {
                        // Multiple sharers: allocate private copy
                        uint64_t new_phys = alloc_4k_page();
                        if (!new_phys) continue; // OOM: skip this page
                        memcpy((void *)Phy_To_Virt(new_phys),
                               (void *)Phy_To_Virt(phys), PAGE_4K_SIZE);
                        page_cow_put(phys);
                        *pte = new_phys | new_page_prot;
                    } else {
                        // Last sharer: promote in-place
                        (void)page_cow_put(phys);
                        *pte = phys | new_page_prot;
                    }
                } else if (*pte & PAGE_PROTNONE) {
                    // Non-COW PROTNONE restore
                    *pte = phys | new_page_prot;
                } else {
                    *pte = phys | new_page_prot;
                }
            }
        }

        addr = v->vm_end;
        if (addr >= end) break;
        pos = pos->next;
    }

    spin_unlock(&current->mm->lock);

    if (addr < end)
        return -ENOMEM;

    flush_tlb();
    return 0;
}

// ── user_write_range_begin/end — validate + lock a kernel→user write ──
// Closes the TOCTOU between "check the pages are mapped+writable" and
// "write them": the caller holds current->mm->lock for the whole fill, and
// munmap/MAP_FIXED/mprotect take the same lock, so no concurrent call can
// tear down or narrow a page mid-write (which would fault into the
// do_page_fault hlt hang — there is no kernel-side demand paging).
//
// On success returns 0 with mm->lock HELD; on any failure returns -EFAULT
// and the lock is NOT held.  current->mm == NULL (kthread reading
// /dev/urandom) skips the check/lock entirely — its buffer is a trusted
// kernel buffer.
//
// Walk is huge-page aware: the user stack is a single 2MB page (PAGE_PS),
// so buffers on the stack must validate the PDE itself rather than a 4KB PTE.
static uint64_t *user_leaf_pte(uint64_t *pml4, uint64_t va)
{
    size_t l4 = (size_t)(va >> 39) & 0x1ff;
    size_t l3 = (size_t)(va >> 30) & 0x1ff;
    size_t l2 = (size_t)(va >> 21) & 0x1ff;
    size_t l1 = (size_t)(va >> 12) & 0x1ff;

    if (l4 >= 256) return NULL;                       // kernel half — out of scope

    if (!(pml4[l4] & PAGE_Present)) return NULL;
    uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);

    if (!(pml3[l3] & PAGE_Present)) return NULL;
    uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);

    if (!(pml2[l2] & PAGE_Present)) return NULL;

    if (pml2[l2] & PAGE_PS)                            // 2MB huge page: PDE is the leaf
        return &pml2[l2];

    uint64_t *pte_table = (uint64_t *)Phy_To_Virt(pml2[l2] & PAGE_4K_MASK);
    return &pte_table[l1];
}

int user_write_range_begin(uint64_t addr, size_t len)
{
    if (current->mm == NULL)
        return 0;

    if (addr == 0 || addr >= current->addr_limit ||
        len > current->addr_limit - addr)
        return -EFAULT;

    spin_lock(&current->mm->lock);

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    uint64_t end = addr + len;
    for (uint64_t va = addr & PAGE_4K_MASK; va < end; ) {
        uint64_t *leaf = user_leaf_pte(user_pml4, va);
        // Require present + writable.  A COW page (P=1, W=0, PAGE_COW set)
        // fails here — a documented deviation from Linux's COW-fault-on-write.
        if (!leaf || !(*leaf & (PAGE_Present | PAGE_R_W))) {
            spin_unlock(&current->mm->lock);
            return -EFAULT;
        }
        // Huge page: the single PDE covers 2 MB — validate it once, skip to
        // the next 2 MB boundary.  Otherwise advance one 4 KB page.
        va = (*leaf & PAGE_PS)
             ? (va & PAGE_2M_MASK) + PAGE_2M_SIZE
             : va + PAGE_4K_SIZE;
    }
    return 0;   // lock held
}

void user_write_range_end(void)
{
    if (current->mm != NULL)
        spin_unlock(&current->mm->lock);
}
