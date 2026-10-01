// kernel/memory/vma.c — VMA linked-list management
#include <memory/vma.h>
#include <sched/task.h>
#include <kernel.h>
#include <memory/slab.h>
#include <fs/file.h>
#include <memory/pmm.h>
#include <memory/memory.h>
#include <memory/uaccess.h>          // USER_MIN_ADDR, arch_user_range_accessible
#include <arch/spinlock.h>   // mm->lock: guards munmap/MAP_FIXED/mprotect
#include <string.h>
#include <stdlib.h>                 // calloc (used by mm_alloc)
#include <errno.h>

// User-VA layout (must mirror kernel/arch/x86_64/intr/trap.c
// USER_CODE_ADDR/USER_PAGE_SIZE and kernel/include/sched/task.h
// USER_STACK_BASE).  HEAP_LIMIT keeps the heap one page below the
// user stack area so mm_brk() cannot grow into the stack window.
#define USER_CODE_ADDR  0x400000UL
#define USER_PAGE_SIZE  0x1000000UL
#define USER_STACK_BASE 0x1400000UL
#define HEAP_LIMIT      (USER_CODE_ADDR + USER_PAGE_SIZE - 0x1000UL)

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

    uint64_t *user_pgd = NULL;
    if (mm->pgdir)
        user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);

    while (mm->vma_list.next != &mm->vma_list) {
        vma_t *v = container_of(mm->vma_list.next, vma_t, list);

        if (v->vm_flags & VM_IO) {
            vma_remove(mm, v);
            continue;
        }

        // If we have a valid pgd, unmap the physical pages.
        // If pgd is NULL (shouldn't happen), skip unmap but still
        // free the VMA node + vfs_node_put to avoid leaks.
        if (user_pgd) {
            for (uint64_t va = v->vm_start; va < v->vm_end;
                 va += PAGE_4K_SIZE) {
                vmm_unmap_4k_page(user_pgd, va);
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

// mm_init_user_heap — install the unique heap VMA and set mm's break
// fields.  Must be called after a successful elf_load() so the new
// mm already has start_code/end_code set; elf_end is the high-water
// mark of the loaded image (== end_code aligned by elf_layout).
//
// Contract (docs/.../2026-10-01-user-heap-elf-isolation-design.md §4):
//   - mm->start_brk = mm->end_brk = ALIGN_UP(elf_end, 4096)
//   - ONE heap VMA inserted at [start_brk, start_brk) — zero-length
//   - vm_flags     = VM_READ | VM_WRITE | VM_ANON | VM_HEAP
//   - vm_page_prot = PAGE_USER  | PAGE_WRITE | PAGE_VALID
//   - vma_find() does NOT match this VMA (zero-length invariant)
//
// Failure handling: if the VMA allocation fails, mm is left
// unchanged.  The caller (spawn_user_task / sys_exec) destroys the
// unpublished mm via destroy_unpublished_user_mm — see
// kernel/sched/task.c — which calls vma_free_all() then
// vmm_free_user_map().  Nothing has been added to mm yet, so on the
// failure path vma_free_all() walks an empty list and the user-page
// tables are torn down as usual.
int mm_init_user_heap(mm_t *mm, uint64_t elf_end)
{
    if (!mm) return -EINVAL;

    uint64_t heap_base = (elf_end + (PAGE_4K_SIZE - 1)) & ~(PAGE_4K_SIZE - 1);

    vma_t *hv = (vma_t *)kmalloc(sizeof(vma_t));
    if (!hv) return -ENOMEM;

    list_init(&hv->list);
    hv->vm_start     = heap_base;
    hv->vm_end       = heap_base;          /* zero-length on purpose */
    hv->vm_flags     = VM_READ | VM_WRITE | VM_ANON | VM_HEAP;
    hv->vm_page_prot = PAGE_USER | PAGE_WRITE | PAGE_VALID;
    hv->vm_pgoff     = 0;
    hv->vm_file      = NULL;

    /* Commit the break fields only AFTER the VMA alloc succeeds —
     * a -ENOMEM return leaves mm unchanged (start_brk/end_brk
     * untouched, VMA list untouched). */
    mm->start_brk = heap_base;
    mm->end_brk   = heap_base;
    vma_insert(mm, hv);
    return 0;
}

// ── mm_user_range_protected — reserved-window predicate (Task 5) ──
//
// Returns true iff [start, end) intersects any of:
//   [0x400000,        mm->start_brk)               ELF reserve envelope
//   [mm->start_brk,   HEAP_LIMIT (0x13ff000))      heap reserve
//   [HEAP_LIMIT,      USER_STACK_BASE)             guard page
//   [USER_STACK_BASE, USER_STACK_BASE + 0x200000)  user stack
//
// See the full contract in kernel/include/memory/vma.h.  Summary:
// takes no lock; the caller validates page alignment / overflow /
// user bounds first; on true the caller must reject with -EINVAL
// and perform NO mutation (no PTE change, no VMA split, no
// do_munmap_locked, no device mmap callback).
bool mm_user_range_protected(const mm_t *mm, uint64_t start, uint64_t end)
{
    if (!mm || mm->start_brk == 0)
        return false;
    if (end <= start)
        return false;

    const uint64_t stack_end = USER_STACK_BASE + 0x200000UL;

    // ELF reserve envelope (image + inter-segment gaps).
    if (start < mm->start_brk && USER_CODE_ADDR < end)
        return true;
    // Heap reserve — the whole brk window, committed or not.  The
    // upper bound is HEAP_LIMIT, NOT end_brk (end_brk is Task 4's
    // committed-boundary bookkeeping).
    if (start < HEAP_LIMIT && mm->start_brk < end)
        return true;
    // Guard page between heap and stack.
    if (start < USER_STACK_BASE && HEAP_LIMIT < end)
        return true;
    // User stack (2 MiB).
    if (start < stack_end && USER_STACK_BASE < end)
        return true;
    return false;
}

// ── mm_set_brk — program-break owner (Task 4) ─────────────
//
// Sets the program break end_brk and the matching heap VMA endpoint.
// All committed 4 KiB leaves are owned by this call: each new leaf
// is staged (alloc_4k_page + memset 0 + vmm_map_4k_page) BEFORE the
// break/VMA endpoint is published, so any OOM leaves old break, old
// heap VMA end, and old PTEs intact.
//
// On shrink across a page boundary, the retained tail is
// privatized if COW with refs > 1 (alloc new phys, copy, replace
// PTE, drop cow_ref), then zeroed, then the released leaves are
// unmapped through the COW-aware vmm_unmap_4k_page path.  SMP
// TLBs are synchronized via tlb_shootdown().  On any failure
// during grow, only this call's leaves (and any empty intermediate
// tables) are rolled back; the old state is restored.
//
// Caller must NOT hold mm->lock; mm_set_brk takes it.
int mm_set_brk(mm_t *mm, uint64_t requested, uint64_t *result)
{
    if (!mm) return -EINVAL;

    /* Query path: requested == 0.  Per the brief, return 0 with
     * *result set to the current end_brk.  result may be NULL. */
    if (requested == 0) {
        if (result) *result = mm->end_brk;
        return 0;
    }

    /* Bounds check — preserved error codes from the original
     * SYS_brk: -EINVAL for below start_brk, -ENOMEM for above
     * heap_limit. */
    uint64_t start_brk = mm->start_brk;
    uint64_t end_brk   = mm->end_brk;
    if (start_brk == 0)               return -ENOMEM;
    if (requested < start_brk)        return -EINVAL;
    if (requested > HEAP_LIMIT)       return -ENOMEM;

    /* Fast path: requested == current end_brk.  No change. */
    if (requested == end_brk) {
        if (result) *result = end_brk;
        return 0;
    }

    spin_lock(&mm->lock);

    /* Re-read the live fields under the lock (another CPU may
     * have mutated them between the unlocked check and the lock).
     * A concurrent mm_set_brk on the same mm would now serialize
     * via the lock and produce the same outcome. */
    start_brk = mm->start_brk;
    end_brk   = mm->end_brk;

    /* The heap VMA tracks [start_brk, ALIGN_UP(end_brk, 4096)) —
     * locate it for the commit step.  There is exactly one
     * VM_HEAP VMA, inserted by mm_init_user_heap; its length
     * matches the page-aligned portion of end_brk. */
    vma_t *heap_vma = NULL;
    {
        list_t *pos;
        for (pos = mm->vma_list.next; pos != &mm->vma_list; pos = pos->next) {
            vma_t *v = container_of(pos, vma_t, list);
            if ((v->vm_flags & VM_HEAP) && v->vm_start == start_brk) {
                heap_vma = v;
                break;
            }
        }
    }
    if (!heap_vma) {
        spin_unlock(&mm->lock);
        return -ENOMEM;
    }

    /* Heap VMA's leaf-prot flags (user R/W present). */
    uint64_t page_prot = heap_vma->vm_page_prot;
    uint64_t *pgd      = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);

    int64_t rc = 0;

    if (requested > end_brk) {
        /* ── GROW ──────────────────────────────────────── */
        /* Brief §5.2: stage and map new zeroed 4 KiB leaves
         * across [ALIGN_UP(end_brk), ALIGN_UP(requested)); same-
         * page grow needs no new leaf.  vmm_pt_walk with
         * allocate=0 is used to skip pages that are already
         * committed (no overwrite — would lose the phys). */
        uint64_t aligned_old = (end_brk + (PAGE_4K_SIZE - 1)) & PAGE_4K_MASK;
        uint64_t aligned_new = (requested + (PAGE_4K_SIZE - 1)) & PAGE_4K_MASK;
        if (aligned_new > HEAP_LIMIT) aligned_new = HEAP_LIMIT;

        if (aligned_new <= aligned_old) {
            /* No new page needed (grow stays within the same
             * page).  Commit the new end_brk only. */
            mm->end_brk   = requested;
            heap_vma->vm_end = requested;
            if (result) *result = requested;
            spin_unlock(&mm->lock);
            return 0;
        }

        /* Stage and map each new 4 KiB leaf.  If any stage
         * fails, roll back every leaf staged in THIS call. */
        /* The staging table is heap-allocated (kmalloc), not a
         * fixed stack array: a single brk() grow may span up to
         * (HEAP_LIMIT - start_brk) / 4 KiB ≈ 4096 leaves, and a
         * fixed cap here would wrongly fail legitimate grows with
         * -ENOMEM (e.g. one large malloc).  Entries are recorded
         * only after vmm_map_4k_page succeeds, so every recorded
         * leaf is ours to roll back. */
        uint64_t grow_pages = (aligned_new - aligned_old) / PAGE_4K_SIZE;
        uint64_t *staged_va =
            (uint64_t *)kmalloc(grow_pages * sizeof(uint64_t));
        uint64_t *staged_phys =
            (uint64_t *)kmalloc(grow_pages * sizeof(uint64_t));
        int staged_count = 0;
        if (!staged_va || !staged_phys) {
            if (staged_phys) kfree(staged_phys);
            if (staged_va) kfree(staged_va);
            spin_unlock(&mm->lock);
            return -ENOMEM;
        }

        for (uint64_t va = aligned_old; va < aligned_new; va += PAGE_4K_SIZE) {
            /* Skip if a PTE is already committed (grow within an
             * already-mapped region). */
            uint64_t *existing = vmm_pt_walk(pgd, va, 0, 0);
            if (existing && (*existing & PAGE_VALID)) {
                /* This page is already committed from an earlier
                 * grow; do not remap (would overwrite a valid
                 * PTE and lose the previous phys). */
                continue;
            }
            uint64_t phys = alloc_4k_page();
            if (!phys) { rc = -ENOMEM; break; }
            /* Zero the leaf (production alloc_4k_page from the
             * subpage allocator returns zeroed pages; this memset
             * is defensive for paths that swap in a non-subpage
             * phys in tests). */
            memset((void *)Phy_To_Virt(phys), 0, PAGE_4K_SIZE);
            int map_rc = vmm_map_4k_page(pgd, phys, va, page_prot);
            if (map_rc != 0) {
                /* Map failed (intermediate-table OOM).  Free the
                 * phys we just allocated and roll back everything
                 * we staged so far. */
                free_4k_page(phys);
                rc = -ENOMEM;
                break;
            }
            staged_va[staged_count]   = va;
            staged_phys[staged_count] = phys;
            staged_count++;
        }

        if (rc != 0) {
            /* Roll back only THIS call's leaves.  vmm_unmap_4k_page
             * frees the phys (it does not own the COW reference
             * since we never set PAGE_COW here). */
            for (int i = 0; i < staged_count; i++)
                vmm_unmap_4k_page(pgd, staged_va[i]);
            kfree(staged_va);
            kfree(staged_phys);
            /* *result unchanged on failure (caller's contract). */
            spin_unlock(&mm->lock);
            return rc;
        }
        kfree(staged_va);
        kfree(staged_phys);

        /* Commit the new break + heap VMA end. */
        mm->end_brk       = requested;
        heap_vma->vm_end  = requested;
        /* Synchronize SMP TLBs (the brief requires this even
         * though the user page table is per-process — future
         * schedulers may run the same process on multiple CPUs). */
        tlb_shootdown();
        if (result) *result = requested;
        spin_unlock(&mm->lock);
        return 0;
    }

    /* ── SHRINK (requested < end_brk) ─────────────────────── */
    /* The page containing `requested` is retained (we may zero
     * its tail).  The pages strictly above its aligned-up
     * address are released. */
    uint64_t aligned_new = (requested + (PAGE_4K_SIZE - 1)) & PAGE_4K_MASK;
    uint64_t aligned_old = (end_brk   + (PAGE_4K_SIZE - 1)) & PAGE_4K_MASK;

    /* 1. Retained tail within [requested, aligned_new).  If
     *    requested is page-aligned, there is no retained tail. */
    if (requested < aligned_new) {
        /* The page containing 'requested' is the aligned_new page
         * (which lies at start_brk + k*PAGE_4K_SIZE for some k).
         * We must (a) privatize if the page is COW with refs > 1,
         * (b) zero the retained tail bytes. */
        uint64_t tail_va = aligned_new - PAGE_4K_SIZE;
        uint64_t *pte = vmm_pt_walk(pgd, tail_va, 0, 0);
        if (pte && (*pte & PAGE_VALID)) {
            uint64_t old_phys = *pte & PAGE_4K_MASK;
            if (*pte & PAGE_COW) {
                if (page_cow_refs(old_phys) > 1) {
                    uint64_t new_phys = alloc_4k_page();
                    if (!new_phys) {
                        spin_unlock(&mm->lock);
                        return -ENOMEM;
                    }
                    memcpy((void *)Phy_To_Virt(new_phys),
                           (void *)Phy_To_Virt(old_phys),
                           PAGE_4K_SIZE);
                    *pte = new_phys | page_prot;
                    (void)page_cow_put(old_phys);
                    old_phys = new_phys;
                } else {
                    (void)page_cow_put(old_phys);
                    *pte = old_phys | page_prot;
                }
            }
            /* Zero the retained-tail range [requested - tail_va,
             * PAGE_4K_SIZE).  The fresh phys is already zeroed
             * (from alloc_4k_page); only the partial-tail bytes
             * need explicit zeroing. */
            size_t off = (size_t)(requested - tail_va);
            memset((char *)Phy_To_Virt(old_phys) + off, 0,
                   PAGE_4K_SIZE - off);
        }
    }

    /* 2. Unmap released leaves [aligned_new, aligned_old). */
    for (uint64_t va = aligned_new; va < aligned_old; va += PAGE_4K_SIZE)
        vmm_unmap_4k_page(pgd, va);

    /* 3. Commit the smaller break + heap VMA end. */
    mm->end_brk       = requested;
    heap_vma->vm_end  = requested;
    tlb_shootdown();
    if (result) *result = requested;
    spin_unlock(&mm->lock);
    return 0;
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
        *page_prot = PAGE_USER;
        *vm_flags = 0;
    } else if (prot == PROT_READ) {
        *page_prot = PAGE_USER_PTE_RO;
        *vm_flags = VM_READ;
    } else if (prot == (PROT_READ | PROT_WRITE)) {
        *page_prot = PAGE_USER_PTE;
        *vm_flags = VM_READ | VM_WRITE;
    } else if (prot == (PROT_READ | PROT_EXEC) ||
               prot == (PROT_READ | PROT_WRITE | PROT_EXEC)) {
        *page_prot = PAGE_USER_PTE;  // no NX support yet
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

    // Task 5: reject any unmap that touches the reserved window
    // (ELF envelope / heap reserve / guard / stack) BEFORE any PTE
    // unmap or VMA split happens — a rejected request must leave
    // every existing mapping untouched.
    if (mm_user_range_protected(current->mm, addr, end))
        return -EINVAL;

    uint64_t *user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pgdir);

    list_t *pos = current->mm->vma_list.next;
    while (pos != &current->mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        pos = pos->next;

        if (v->vm_end <= addr)   continue;
        if (v->vm_start >= end)  break;

        uint64_t u_start = (addr > v->vm_start) ? addr : v->vm_start;
        uint64_t u_end   = (end  < v->vm_end)   ? end  : v->vm_end;
        for (uint64_t va = u_start; va < u_end; va += PAGE_4K_SIZE)
            vmm_unmap_4k_page(user_pgd, va);

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

    // Keep the "nothing below 0x400000 mapped" invariant: any explicit
    // address (MAP_FIXED or otherwise) below USER_MIN_ADDR is rejected
    // here.  Without this guard a buggy or hostile caller could map at
    // 0x1000 and later trigger kernel faults on what looks like a NULL
    // deref — see design §3 invariant.  addr == 0 (let the kernel pick)
    // is unaffected.
    if (addr != 0 && addr < USER_MIN_ADDR)
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

    // ── 1b. Reserved-window check (Task 5) ──────────────
    // A MAP_FIXED request that touches the ELF envelope, the heap
    // reserve, the guard page or the user stack is rejected BEFORE
    // any do_munmap_locked / VMA split / PTE change / device mmap
    // callback, so a rejected request leaves every existing
    // mapping byte-for-byte intact.  (Overflow and user bounds
    // were validated above, per the predicate's contract.)
    // The non-fixed path re-checks its computed candidate below.
    if ((flags & MAP_FIXED) &&
        mm_user_range_protected(current->mm, addr, end))
        return -EINVAL;

    // ── 2. Address computation ──────────────────────────
    length = PAGE_4K_ALIGN(length);
    if (!length) return -EINVAL;

    uint64_t search = current->mm->mmap_base;
    vma_t *prev = NULL;
    list_t *pos = current->mm->vma_list.next;

    // NOTE (Task 5): `addr` must keep its parameter value on the
    // MAP_FIXED path — the old unconditional `addr = 0;` here
    // clobbered the fixed address, so MAP_FIXED silently mapped at
    // 0 and ran do_munmap_locked over [0, length).  Zeroing now
    // happens only on the auto-search path, where 0 means "no
    // candidate found yet".
    if (!(flags & MAP_FIXED)) {
        addr = 0;
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

        // Task 5: the auto-search must never return a candidate
        // inside the reserved window.  mmap_base (0x40000000) is
        // already far above it, so this is a defensive skip: jump
        // the candidate past the ENTIRE window.
        if (mm_user_range_protected(current->mm, addr, addr + length))
            addr = USER_STACK_BASE + 0x200000UL;
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

            // Task 5: never hand a protected range to a device mmap
            // handler — the handler fills PTEs eagerly, so the check
            // MUST precede the callback (defence in depth on top of
            // the §1b / post-search checks above).
            if (mm_user_range_protected(current->mm, addr, addr + length)) {
                vfs_node_put(file_node);
                return -EINVAL;
            }

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

    // Task 5: reject before any VMA flag / prot / PTE change —
    // mprotect must never be able to strip protections from the
    // ELF envelope, heap reserve, guard page or user stack.
    if (mm_user_range_protected(current->mm, addr, end))
        return -EINVAL;

    uint64_t new_page_prot, new_vm_flags;
    int rc = prot_to_page_flags((int)prot, &new_page_prot, &new_vm_flags);
    if (rc) return rc;

    spin_lock(&current->mm->lock);

    uint64_t *user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pgdir);

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
            uint64_t *pte = vmm_pt_walk(user_pgd, va, 0, 0);
            if (!pte) continue;
            if (!(*pte & (PAGE_VALID | PAGE_PROTNONE))) continue;

            uint64_t phys = *pte & PAGE_4K_MASK;

            if (prot == PROT_NONE) {
                // Stash phys for later restore.
                // Preserve PAGE_COW if set — we keep our COW reference.
                uint64_t stash = phys | PAGE_USER | PAGE_PROTNONE;
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
// Permission check is delegated to arch_user_range_accessible (mmu.h) so
// that upper-level PGD/PDP/PD entries are ANDed into the effective
// permissions — a leaf PTE marked user+RW above a supervisor-only PGDE
// is still inaccessible from ring-3.  COW (PAGE_COW, RW=0) is rejected
// here, consistent with the design choice that the lock-and-write path
// never allocates a private copy.
static uint64_t *user_leaf_pte(uint64_t *pgd, uint64_t va)
{
    size_t l4 = (size_t)(va >> 39) & 0x1ff;
    size_t l3 = (size_t)(va >> 30) & 0x1ff;
    size_t l2 = (size_t)(va >> 21) & 0x1ff;
    size_t l1 = (size_t)(va >> 12) & 0x1ff;

    if (l4 >= 256) return NULL;                       // kernel half — out of scope

    if (!(pgd[l4] & PAGE_VALID)) return NULL;
    uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd[l4] & PAGE_4K_MASK);

    if (!(pud[l3] & PAGE_VALID)) return NULL;
    if (pud[l3] & PAGE_HUGE) return NULL;   // 1GB huge page: unsupported here (same gap as vmm_pt_walk)
    uint64_t *pmd = (uint64_t *)Phy_To_Virt(pud[l3] & PAGE_4K_MASK);

    if (!(pmd[l2] & PAGE_VALID)) return NULL;

    if (pmd[l2] & PAGE_HUGE)                            // 2MB huge page: PDE is the leaf
        return &pmd[l2];

    uint64_t *pte_table = (uint64_t *)Phy_To_Virt(pmd[l2] & PAGE_4K_MASK);
    return &pte_table[l1];
}

int user_write_range_begin(uint64_t addr, size_t len)
{
    if (current->mm == NULL)
        return 0;

    // USER_MIN_ADDR rejects NULL and any address below 0x400000 — together
    // with the existing >= addr_limit check this covers the whole low half.
    if (addr < USER_MIN_ADDR ||
        addr >= current->addr_limit ||
        len > current->addr_limit - addr)
        return -EFAULT;

    spin_lock(&current->mm->lock);

    uint64_t *user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pgdir);
    if (!arch_user_range_accessible(user_pgd, addr, len, true)) {
        spin_unlock(&current->mm->lock);
        return -EFAULT;
    }
    return 0;   // lock held
}

void user_write_range_end(void)
{
    if (current->mm != NULL)
        spin_unlock(&current->mm->lock);
}
