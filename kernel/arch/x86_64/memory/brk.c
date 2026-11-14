// kernel/arch/x86_64/memory/brk.c — user heap VMA + program break.
//
// Owns the initial zero-length heap VMA (mm_init_user_heap) and the
// brk commit/shrink/rollback transaction (mm_set_brk).  All committed
// 4 KiB leaves belong to this call: the grow path records the VAs of
// the leaves it installs in ONE log array; any failure withdraws
// exactly those leaves (two rounds: take old descriptors in place,
// local TLB sync, then release) and frees the log, leaving old break,
// heap VMA end, PTEs and *result untouched.  A successful grow frees
// the log as well.
#include <errno.h>
#include <string.h>
#include <kernel.h>             // container_of
#include <list.h>
#include <memory/memory.h>      // Phy_To_Virt
#include <memory/pmm.h>         // alloc_4k_page / free_4k_page / PAGE_4K_*
#include <memory/slab.h>        // kmalloc / kfree
#include <sched/task.h>         // complete mm_t (vma_list / start_brk)
#include <memory/vma.h>         // VMA_* logical flags
#include <memory/vm_area.h>     // vm_area_find_heap / vm_area_insert_sorted
#include <memory/vmm.h>         // vmm_pt_walk
#include <arch/spinlock.h>      // mm->lock
#include <arch/x86_64/pte.h>    // PAGE_* hardware bits, x86_pte_4k_*
#include <arch/x86_64/user_layout.h>  // X86_USER_HEAP_LIMIT
#include <arch/x86_64/vmm.h>    // x86_vmm_take_4k / x86_vmm_release_owned_4k
#include <arch/x86_64/vma.h>    // this TU's API + x86_vma_pte_flags (inline)


// mm_init_user_heap — install the unique heap VMA and set mm's break
// fields.  Must be called after a successful elf_load() so the new
// mm already has start_code/end_code set; elf_end is the high-water
// mark of the loaded image (== end_code aligned by elf_layout).
//
// Contract (docs/.../2026-10-01-user-heap-elf-isolation-design.md §4):
//   - mm->start_brk = mm->end_brk = ALIGN_UP(elf_end, 4096)
//   - ONE heap VMA inserted at [start_brk, start_brk) — zero-length
//   - vm_flags = VMA_PROT_READ | VMA_PROT_WRITE | VMA_ANON | VMA_HEAP
//     (heap data pages are never executable — the leaf word is derived
//     on demand via x86_vma_pte_flags)
//   - vm_area_lookup() does NOT match this VMA (zero-length invariant)
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
    hv->vm_flags     = VMA_PROT_READ | VMA_PROT_WRITE | VMA_ANON | VMA_HEAP;
    hv->vm_pgoff     = 0;
    hv->vm_file      = NULL;

    /* Commit the break fields only AFTER the VMA alloc succeeds —
     * a -ENOMEM return leaves mm unchanged (start_brk/end_brk
     * untouched, VMA list untouched). */
    mm->start_brk = heap_base;
    mm->end_brk   = heap_base;
    vm_area_insert_sorted(mm, hv);
    return 0;
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
// unmapped through the COW-aware withdraw + release path.  SMP
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
    if (requested > X86_USER_HEAP_LIMIT) return -ENOMEM;

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
     * VMA_HEAP VMA, inserted by mm_init_user_heap; its length
     * matches the page-aligned portion of end_brk. */
    vma_t *heap_vma = vm_area_find_heap(mm);
    if (!heap_vma) {
        spin_unlock(&mm->lock);
        return -ENOMEM;
    }

    /* Heap VMA's leaf permission word (user R/W present, NX). */
    uint64_t page_prot = x86_vma_pte_flags(heap_vma->vm_flags);
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
        if (aligned_new > X86_USER_HEAP_LIMIT) aligned_new = X86_USER_HEAP_LIMIT;

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
        /* The log is heap-allocated (kmalloc), not a fixed stack
         * array: a single brk() grow may span up to
         * (X86_USER_HEAP_LIMIT - start_brk) / 4 KiB ≈ 4096 leaves, and a
         * fixed cap here would wrongly fail legitimate grows with
         * -ENOMEM (e.g. one large malloc).  Only successfully
         * installed leaves are recorded — pages already committed
         * by an earlier grow are skipped and never enter the log,
         * and a map failure's just-allocated PA is returned
         * separately below, so the log is always exactly the set
         * of leaves this call owns. */
        uint64_t grow_pages = (aligned_new - aligned_old) / PAGE_4K_SIZE;
        uint64_t *log_va =
            (uint64_t *)kmalloc(grow_pages * sizeof(uint64_t));
        int log_count = 0;
        if (!log_va) {
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
                /* Map failed (intermediate-table OOM).  The PA is
                 * not installed anywhere, so it is returned here,
                 * separately from the two-round rollback below. */
                free_4k_page(phys);
                rc = -ENOMEM;
                break;
            }
            log_va[log_count] = va;
            log_count++;
        }

        if (rc != 0) {
            /* Roll back only THIS call's installed leaves, in two
             * rounds (unmap/TLB ownership plan): round 1 withdraws
             * each logged leaf and writes the taken old descriptor
             * back over its VA slot — no allocation, no release.
             * The local flush between the rounds is the full sync
             * of the active root.  Round 2 releases the withdrawn
             * descriptors and the log itself. */
            for (int i = 0; i < log_count; i++)
                log_va[i] = x86_vmm_take_4k(pgd, log_va[i]);
            flush_tlb();
            for (int i = 0; i < log_count; i++)
                x86_vmm_release_owned_4k(log_va[i]);
            kfree(log_va);
            /* *result unchanged on failure (caller's contract). */
            spin_unlock(&mm->lock);
            return rc;
        }
        /* Success frees the log too — the leaves are committed and
         * the bookkeeping has no life past this call. */
        kfree(log_va);

        /* Commit the new break + heap VMA end. */
        mm->end_brk       = requested;
        heap_vma->vm_end  = requested;
        /* The user root is single-task owned and active only on
         * this CPU — a local CR3 reload is the complete sync
         * (unmap/TLB ownership plan). */
        flush_tlb();
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
     *    requested is page-aligned, there is no retained tail.
     *    The COW puts are DEFERRED to after the flush (they are
     *    recorded below and run once the replaced leaf's stale
     *    translation is gone). */
    uint64_t free_old_phys = 0;
    uint64_t tail_put_phys = 0;
    bool tail_put_privatized = false;
    if (requested < aligned_new) {
        /* The page containing 'requested' is the aligned_new page
         * (which lies at start_brk + k*PAGE_4K_SIZE for some k).
         * We must (a) privatize if the page is COW with refs > 1,
         * (b) zero the retained tail bytes. */
        uint64_t tail_va = aligned_new - PAGE_4K_SIZE;
        uint64_t *pte = vmm_pt_walk(pgd, tail_va, 0, 0);
        if (pte && (*pte & PAGE_VALID)) {
            uint64_t old_phys = x86_pte_4k_pa(*pte);
            if (*pte & PAGE_COW) {
                /* Replace first; the put happens after the flush,
                 * so another owner's reference stays visible until
                 * the stale translation is gone (same protocol as
                 * uaccess/trap COW). */
                if (page_cow_refs(old_phys) > 1) {
                    uint64_t new_phys = alloc_4k_page();
                    if (!new_phys) {
                        spin_unlock(&mm->lock);
                        return -ENOMEM;
                    }
                    memcpy((void *)Phy_To_Virt(new_phys),
                           (void *)Phy_To_Virt(old_phys),
                           PAGE_4K_SIZE);
                    *pte = x86_pte_4k_make(new_phys, page_prot);
                    tail_put_phys = old_phys;
                    tail_put_privatized = true;
                    old_phys = new_phys;
                } else {
                    /* In-place upgrade: the leaf keeps this PA — the
                     * put drops the COW bookkeeping but must NEVER
                     * free the page. */
                    *pte = x86_pte_4k_make(old_phys, page_prot);
                    tail_put_phys = old_phys;
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

    /* 2. Unmap released leaves [aligned_new, aligned_old): fixed
     * batches of withdrawn descriptors — clear the batch, sync the
     * active root, then release (unmap/TLB ownership plan). */
    {
        uint64_t batch[32];
        uint64_t va = aligned_new;
        while (va < aligned_old) {
            int n = 0;
            for (; va < aligned_old && n < 32; va += PAGE_4K_SIZE) {
                uint64_t old = x86_vmm_take_4k(pgd, va);
                if (old)
                    batch[n++] = old;
            }
            flush_tlb();
            for (int i = 0; i < n; i++)
                x86_vmm_release_owned_4k(batch[i]);
        }
    }

    /* 3. Commit the smaller break + heap VMA end. */
    mm->end_brk       = requested;
    heap_vma->vm_end  = requested;
    flush_tlb();
    /* The deferred tail put runs only after the flush (review fix:
     * replace → sync → release, matching uaccess/trap COW).  Only
     * the privatized leaf's OLD PA is releasable — the in-place
     * upgrade keeps mapping it. */
    if (tail_put_phys) {
        bool last = page_cow_put(tail_put_phys);
        if (tail_put_privatized && last)
            free_old_phys = tail_put_phys;
    }
    if (free_old_phys)
        free_4k_page(free_old_phys);
    if (result) *result = requested;
    spin_unlock(&mm->lock);
    return 0;
}
