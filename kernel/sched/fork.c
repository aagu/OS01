#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/cpu.h>
#include <arch/spinlock.h>
#include <arch/irq.h>
#include <arch/segment.h>
#include <arch/mmu.h>
#include <core/debug.h>
#include <core/panic.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vma.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
#include <memory/slab.h>
#include <fs/file.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <kernel.h>

extern void arch_kernel_thread_entry(void);

// ── fork_mm_copy — create private address space for fork child ─
// Builds a new PGD with private copies of all user pages.
//
// Task 6 contract (docs/.../2026-10-01-user-heap-elf-isolation-
// design.md §6):
//   - Child 4 KiB ELF leaves and read-only non-VM_IO leaves
//     receive PRIVATE physical pages (alloc + memcpy).
//   - Writable VMA leaves use COW (parent PTE → R/O+COW; both
//     parent and child hold a ref on the shared phys).
//   - The stack (USER_STACK_BASE 2 MiB huge page) retains its
//     eager huge-page copy.
//   - All child page-table and leaf-phys allocations are STAGED
//     BEFORE any parent PTE mutation.  Parent mutations happen
//     in a bounded no-failure phase (pass 2).  If staging
//     fails, roll back and return NULL — do NOT mutate parent.
//   - On success, tlb_shootdown() flushes affected TLBs.
//
// The placeholder convention is used to mark writable leaves
// pending pass-2 mutation: child_pte = PAGE_VALID (= 1) means
// "writable leaf, mutating parent + adding COW refs in pass 2".
// A real RO leaf has phys bits set; a VM_IO shared leaf has the
// parent's full PTE (with the MMIO phys); a fork-of-fork COW
// leaf has the parent's full PTE.  Only the placeholder is
// == PAGE_VALID, so pass 2 walks the child pgd, finds these
// placeholders, and mutates the corresponding parent PTE +
// bumps the COW refcount.
static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)
{
    mm_t *child_mm = mm_alloc();
    uint64_t *child_pgd = (uint64_t *)vmm_alloc_map();
    if (!child_mm || !child_pgd) {
        if (child_mm) kfree(child_mm);
        if (child_pgd) kfree(child_pgd);
        if (cr3_out) *cr3_out = 0;
        return NULL;
    }

    uint64_t *parent_pgd = (uint64_t *)Phy_To_Virt((uint64_t)parent_mm->pgdir);
    uint64_t *kernel_pgd = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pgdir);

    memcpy(&child_pgd[256], &kernel_pgd[256], 256 * sizeof(uint64_t));

    /* ── Pass 1 (fail-able): stage child page tables, leaf phys
     * for RO leaves, and huge copies.  No parent PTE mutation. */
    for (int l4 = 0; l4 < 256; l4++) {
        uint64_t pgde = parent_pgd[l4];
        if (!(pgde & PAGE_VALID)) continue;

        uint64_t *parent_pud = (uint64_t *)Phy_To_Virt(pgde & PAGE_4K_MASK);
        uint64_t *child_pud  = (uint64_t *)calloc(1, PAGE_4K_SIZE);
        if (!child_pud) goto fail;
        child_pgd[l4] = Virt_To_Phy((uint64_t)child_pud) | PAGE_USER_PGD;

        for (int l3 = 0; l3 < 512; l3++) {
            uint64_t pude = parent_pud[l3];
            if (!(pude & PAGE_VALID)) continue;

            uint64_t *parent_pmd = (uint64_t *)Phy_To_Virt(pude & PAGE_4K_MASK);
            uint64_t *child_pmd  = (uint64_t *)calloc(1, PAGE_4K_SIZE);
            if (!child_pmd) goto fail;
            child_pud[l3] = Virt_To_Phy((uint64_t)child_pmd) | PAGE_USER_PUD;

            for (int l2 = 0; l2 < 512; l2++) {
                uint64_t pmde = parent_pmd[l2];
                if (!(pmde & PAGE_VALID)) continue;

                if (pmde & PAGE_HUGE) {
                    /* Huge page: eager 2 MiB copy.  Inline asm is
                     * used instead of memcpy because the kernel's
                     * libk memcpy has a bug with 2MB copies
                     * (CR2=0x8).  VM_IO huge pages are shared
                     * directly (no copy). */
                    uint64_t vaddr_2m = ((uint64_t)l4 << 39)
                                       | ((uint64_t)l3 << 30)
                                       | ((uint64_t)l2 << 21);
                    vma_t *vm = vma_find(parent_mm, vaddr_2m);
                    if (vm && (vm->vm_flags & VM_IO)) {
                        child_pmd[l2] = pmde;
                        continue;
                    }
                    struct Page *s = alloc_pages(ZONE_NORMAL, 1, 0);
                    if (!s) goto fail;
                    uint64_t dst = (uint64_t)Phy_To_Virt(s->phy_address);
                    /* Strip PAGE_NO_EXEC (bit 63) before Phy_To_Virt —
                     * PAGE_NO_EXEC lives in the high bit and PAGE_2M_MASK
                     * preserves it, which would corrupt the kernel VA
                     * (non-canonical addr → GP on rep movsb). */
                    uint64_t src = (uint64_t)Phy_To_Virt(
                        (pmde & PAGE_2M_MASK) & ~PAGE_NO_EXEC);
                    uint64_t sz  = PAGE_2M_SIZE;
                    __asm__ __volatile__(
                        "cld\n\t"
                        "rep movsb\n\t"
                        : "+S"(src), "+D"(dst), "+c"(sz)
                        :
                        : "memory"
                    );
                    child_pmd[l2] = s->phy_address
                                   | (pmde & ~PAGE_2M_MASK);
                    continue;
                }

                /* 4 KiB PTE table path. */
                uint64_t *parent_pte =
                    (uint64_t *)Phy_To_Virt(pmde & PAGE_4K_MASK);
                uint64_t *child_pte =
                    (uint64_t *)calloc(1, PAGE_4K_SIZE);
                if (!child_pte) goto fail;
                child_pmd[l2] = Virt_To_Phy((uint64_t)child_pte)
                               | (pmde & 0xfff);

                for (int l1 = 0; l1 < 512; l1++) {
                    uint64_t pte = parent_pte[l1];
                    if (!(pte & (PAGE_VALID | PAGE_PROTNONE)))
                        continue;

                    uint64_t vaddr = ((uint64_t)l4 << 39)
                                   | ((uint64_t)l3 << 30)
                                   | ((uint64_t)l2 << 21)
                                   | ((uint64_t)l1 << 12);
                    vma_t *vma = vma_find(parent_mm, vaddr);
                    if (vma && (vma->vm_flags & VM_IO)) {
                        /* MMIO: share parent's phys (no COW, no
                         * copy).  Pass 2 must not touch this leaf. */
                        child_pte[l1] = pte;
                        continue;
                    }

                    /* Check PAGE_COW before PAGE_WRITE — a COW
                     * page has R/W=0 and must not be misclassified
                     * as plain read-only. */
                    if (vma && (pte & PAGE_COW)) {
                        /* Already COW-shared (fork-of-fork):
                         * add a ref for the child, share the PTE. */
                        page_cow_get(pte & PAGE_4K_MASK);
                        child_pte[l1] = pte;
                    } else if (vma && (vma->vm_flags & VM_WRITE) &&
                               (pte & PAGE_WRITE)) {
                        /* Writable VMA: stage COW; commit in pass 2.
                         * The placeholder (PAGE_VALID only) is
                         * unique to writable leaves awaiting
                         * pass-2 mutation — pass 2 finds it and
                         * bumps the parent's refcount. */
                        child_pte[l1] = PAGE_VALID;
                    } else {
                        /* All non-VMA ELF leaves, plus read-only /
                         * PROT_NONE VMA leaves:
                         * alloc a fresh 4 KiB leaf and copy the
                         * parent's contents.  No COW ref — the
                         * child owns its phys outright. */
                        uint64_t new_phys = alloc_4k_page();
                        if (!new_phys) goto fail;
                        memcpy((void *)Phy_To_Virt(new_phys),
                               (void *)Phy_To_Virt(pte & PAGE_4K_MASK),
                               PAGE_4K_SIZE);
                        /* Preserve flags except PAGE_COW (the
                         * new phys has no COW reference). */
                        child_pte[l1] = new_phys | (pte & 0xfff & ~PAGE_COW);
                    }
                }
            }
        }
    }

    /* ── Pass 2 (no-fail): commit parent PTE changes for
     * writable leaves.  Bounded: no allocation, no copy. */
    for (int l4 = 0; l4 < 256; l4++) {
        uint64_t cpgde = child_pgd[l4];
        if (!(cpgde & PAGE_VALID)) continue;
        uint64_t *child_pud = (uint64_t *)Phy_To_Virt(cpgde & PAGE_4K_MASK);
        uint64_t *parent_pud =
            (uint64_t *)Phy_To_Virt(parent_pgd[l4] & PAGE_4K_MASK);

        for (int l3 = 0; l3 < 512; l3++) {
            uint64_t cpude = child_pud[l3];
            if (!(cpude & PAGE_VALID)) continue;
            uint64_t *child_pmd = (uint64_t *)Phy_To_Virt(cpude & PAGE_4K_MASK);
            uint64_t *parent_pmd =
                (uint64_t *)Phy_To_Virt(parent_pud[l3] & PAGE_4K_MASK);

            for (int l2 = 0; l2 < 512; l2++) {
                uint64_t cpmde = child_pmd[l2];
                if (!(cpmde & PAGE_VALID)) continue;
                if (cpmde & PAGE_HUGE) continue;

                uint64_t *child_pte =
                    (uint64_t *)Phy_To_Virt(cpmde & PAGE_4K_MASK);
                uint64_t *parent_pte =
                    (uint64_t *)Phy_To_Virt(parent_pmd[l2] & PAGE_4K_MASK);

                for (int l1 = 0; l1 < 512; l1++) {
                    /* Placeholder = PAGE_VALID only.  Any other
                     * PTE (real RO/COW/VMIO fork-of-fork) is
                     * already settled by pass 1. */
                    if (child_pte[l1] != PAGE_VALID) continue;
                    uint64_t ppte = parent_pte[l1];
                    uint64_t paddr = ppte & PAGE_4K_MASK;
                    /* Bump refcount twice: parent keeps one ref
                     * (its PTE still owns the phys) and child
                     * gets one ref (its PTE will own it too). */
                    page_cow_get(paddr);
                    page_cow_get(paddr);
                    ppte &= ~PAGE_WRITE;
                    ppte |= PAGE_COW;
                    parent_pte[l1] = ppte;
                    child_pte[l1] = ppte;
                }
            }
        }
    }

    memcpy(child_mm, parent_mm, sizeof(mm_t));
    /* vma_list must NOT be shared — fork_vma_copy fills the
     * child's own list (called by do_fork). */
    list_init(&child_mm->vma_list);
    spin_init(&child_mm->lock);   /* memcpy copied parent's lock value */
    child_mm->pgdir = (uint64_t *)Virt_To_Phy((uint64_t)child_pgd);
    *cr3_out = (uint64_t)child_mm->pgdir;

    /* TLB shootdown: parent's in-memory PTEs were modified (R/W →
     * R/O+COW).  With SMP load balancing the parent may run on any
     * CPU — must invalidate ALL cores' TLBs, not just the local
     * one.  Called from the END so the parent's commit phase is
     * fully visible before any CPU re-tlbs. */
    tlb_shootdown();

    return child_mm;

fail:
    /* Roll back.  Walk the partial child pgd and free every
     * allocation we made in pass 1:
     *   - 4 KiB child PTE tables (calloc'd)  → kfree
     *   - 4 KiB child leaves (alloc_4k_page'd) → free_4k_page
     *   - 2 MiB child huge copies (alloc_pages'd) → free_pages
     * Skip VM_IO shared leaves (parent's MMIO phys — not ours).
     * Skip placeholders (PAGE_VALID only — no phys).  No parent
     * PTE was mutated, so nothing to undo there. */
    if (child_pgd) {
        for (int l4 = 0; l4 < 256; l4++) {
            uint64_t pgde = child_pgd[l4];
            if (!(pgde & PAGE_VALID)) continue;
            uint64_t *pud = (uint64_t *)Phy_To_Virt(pgde & PAGE_4K_MASK);
            for (int l3 = 0; l3 < 512; l3++) {
                uint64_t pude = pud[l3];
                if (!(pude & PAGE_VALID)) continue;
                uint64_t *pmd = (uint64_t *)Phy_To_Virt(pude & PAGE_4K_MASK);
                for (int l2 = 0; l2 < 512; l2++) {
                    uint64_t pmde = pmd[l2];
                    if (!(pmde & PAGE_VALID)) continue;
                    if (pmde & PAGE_HUGE) {
                        /* Bug A1 fix: the huge-page branch may
                         * share the parent's MMIO PMD for VM_IO
                         * VAs (pass 1, task.c ~ line 2041) — in
                         * that case child_pmd[l2] == parent's PMD
                         * and the phys is the parent's MMIO phys,
                         * never ours to free.  Check the parent's
                         * 2 MiB VA against vma_find (same lookup
                         * pattern pass 1 uses) and skip the
                         * free_pages for VM_IO. */
                        uint64_t vaddr_2m = ((uint64_t)l4 << 39)
                                           | ((uint64_t)l3 << 30)
                                           | ((uint64_t)l2 << 21);
                        vma_t *vm = vma_find(parent_mm, vaddr_2m);
                        if (vm && (vm->vm_flags & VM_IO)) {
                            /* Shared MMIO PMD — not ours. */
                        } else {
                            uint64_t phys = pmde & PAGE_2M_MASK;
                            struct Page *p = Phy_to_2M_Page(phys);
                            free_pages(p, 1);
                        }
                    } else {
                        uint64_t *pt = (uint64_t *)Phy_To_Virt(pmde & PAGE_4K_MASK);
                        for (int l1 = 0; l1 < 512; l1++) {
                            uint64_t pte = pt[l1];
                            uint64_t vaddr = ((uint64_t)l4 << 39)
                                           | ((uint64_t)l3 << 30)
                                           | ((uint64_t)l2 << 21)
                                           | ((uint64_t)l1 << 12);
                            vma_t *vma = vma_find(parent_mm, vaddr);
                            int is_vmio = (vma &&
                                           (vma->vm_flags & VM_IO));
                            int is_placeholder = (pte == PAGE_VALID);
                            int is_cow = !!(pte & PAGE_COW);
                            /* Free / put only what we touched in
                             * pass 1:
                             *   - RO leaves (privet): free_4k_page
                             *     a fresh 4 KiB phys we allocated.
                             *   - VMIO shared: skip — parent's
                             *     MMIO phys, never ours.
                             *   - Placeholder: skip — pass 2 has
                             *     not run, no phys to free.
                             *   - Bug A2 fix: COW (fork-of-fork
                             *     already-shared) — pass 1 did
                             *     page_cow_get to add a child
                             *     ref; balance it with page_cow_put.
                             *     The shared phys stays alive for
                             *     parent + any other siblings. */
                            if ((pte & PAGE_VALID) &&
                                (pte & PAGE_4K_MASK) &&
                                !is_placeholder &&
                                !is_vmio) {
                                if (is_cow) {
                                    page_cow_put(pte & PAGE_4K_MASK);
                                } else {
                                    free_4k_page(pte & PAGE_4K_MASK);
                                }
                            }
                        }
                        kfree(pt);
                    }
                }
                kfree(pmd);
            }
            kfree(pud);
        }
        kfree(child_pgd);
    }
    if (child_mm) kfree(child_mm);
    if (cr3_out)  *cr3_out = 0;
    return NULL;
}

// ── do_fork ──────────────────────────────────────────────
uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags __attribute__((unused)),
                 uint64_t stack_start __attribute__((unused)),
                 uint64_t stack_size __attribute__((unused)))
{
    void *raw_alloc = malloc(sizeof(union task_union) + STACK_SIZE);
    task_t *tsk = (task_t *)(((uint64_t)raw_alloc + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)malloc(sizeof(thread_t));

    if (!raw_alloc || !thd) {
        if (raw_alloc) kfree(raw_alloc);
        if (thd) kfree(thd);
        return -ENOMEM;
    }

    memset(tsk, 0, sizeof(task_t));
    memset(thd, 0, sizeof(thread_t));

    // ── Selective field initialization (NOT *tsk = *current) ──
    // The shallow struct copy was the root cause of the CR2=0x8
    // page fault: it copied stale list nodes (wait_list, io_wait_node),
    // the parent's stack_alloc_base, and the parent's thread pointer
    // — all of which must belong to the CHILD, not the parent.
    tsk->stack_alloc_base = raw_alloc;

    // Inherit from parent (safe value-copy fields, not pointers)
    tsk->flags       = current->flags;
    tsk->addr_limit  = current->addr_limit;

    // EEVDF: fair starting vruntime — pick target CPU first, then use its min_vruntime
    uint32_t target_cpu = sched_pick_cpu();
    {
        uint64_t fair_start = percpu_data[target_cpu].min_vruntime;
        tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
    }

    // Inherit signal handlers (shallow copy of sighand array — values, not pointers)
    for (int sig = 0; sig < NSIG; sig++)
        tsk->sighand[sig] = current->sighand[sig];

    // ── Fresh fields (must NOT inherit from parent) ────────
    tsk->signal      = 0;   // child must not inherit parent's pending signals
    tsk->blocked     = current->blocked;  // do_fork inherits parent's blocked mask
    tsk->state       = TASK_UNINTERRUPTIBLE;
    tsk->priority    = 3;
    tsk->cpu         = target_cpu;
    tsk->pid         = alloc_pid();
    tsk->thread      = thd;
    tsk->parent      = current;
    tsk->exit_code   = 0;

    // Inherit controlling terminal from parent
    tsk->ctty_type = current->ctty_type;
    tsk->ctty      = current->ctty;

    // v2: inherit parent's pgrp/session (not pgrp leader: trap.c SYS_fork case
    // is just a thin wrapper around do_fork(); the real child struct is built here)
    tsk->pgrp = current->pgrp;
    tsk->session = current->session;

    list_init(&tsk->list);
    list_init(&tsk->wait_list);
    list_init(&tsk->io_wait_node);
    {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        list_add_to_before(&init_task_union.task.list, &tsk->list);
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }

    // Blocker starts clean — child inherits no blocker state
    tsk->blocker.type = BLOCKER_NONE;
    tsk->blocker.check = NULL;
    tsk->blocker.signal_can_wake = false;
    memset(&tsk->blocker_data, 0, sizeof(tsk->blocker_data));

    // FPU: child gets a fresh FPU save area.  The parent's FPU
    // registers are in hardware (not in the fpu_save buffer), so
    // copying the area would give stale data.  A fresh area is
    // correct for the common case (child execs immediately).
    tsk->fpu_save = fpu_area_alloc();

    // Inherit address space (shared initially, replaced for user tasks)
    tsk->mm = current->mm;

    // ── Inherit fd table (deep copy — refcount++) ──────────
    if (current->files)
        tsk->files = files_dup(current->files);

    thd->cr3 = current->thread->cr3;

    if ((regs->cs & 3) == 3) {
        tsk->flags &= ~PF_KTHREAD;
        tsk->addr_limit = 0x00007FFFFFFFFFFF;
        if (current->mm && current->mm->pgdir) {
            tsk->mm = fork_mm_copy(current->mm, &thd->cr3);
            if (!tsk->mm) {
                /* Task 6: hard OOM.  The pre-existing
                 *   tsk->mm = current->mm; thd->cr3 = current->thread->cr3;
                 * fallback used to "share the parent's mm" on
                 * fork_mm_copy failure — that exposed the parent's
                 * pgdir to the child (and the child's COW refs
                 * would silently leak into the parent).  Brief:
                 * "remove the fork: ... falling back to shared mm
                 *  fallback in do_fork" + "do_fork releases task
                 *  resources on fork_mm_copy failure and returns
                 *  -ENOMEM".
                 *
                 * Release every unpublished resource this function
                 * allocated (raw_alloc + thd + fpu_save + files +
                 * the not-yet-runnable task list link) and return
                 * -ENOMEM to the caller.  The child is never
                 * published, so init's waitpid loop never sees it. */
                debug_task("fork: pid=%d fork_mm_copy OOM, releasing task\n",
                    (int)current->pid);

                /* Remove the not-yet-runnable child from the
                 * global task list so init's waitpid loop cannot
                 * reach it. */
                {
                    uint64_t tl_flags2 =
                        spin_lock_irqsave(&task_list_lock);
                    list_del(&tsk->list);
                    spin_unlock_irqrestore(&task_list_lock, tl_flags2);
                }
                /* Files table reference (if any). */
                if (tsk->files) {
                    files_unpin(tsk->files);
                    tsk->files = NULL;
                }
                /* FPU save area. */
                if (tsk->fpu_save) {
                    kfree(tsk->fpu_save);
                    tsk->fpu_save = NULL;
                }
                /* Thread struct + kernel stack / task union. */
                kfree(thd);
                kfree(tsk->stack_alloc_base);
                return -ENOMEM;
            }
            if (tsk->mm)
                fork_vma_copy(tsk->mm, current->mm);
        } else if (current->mm) {
            debug_task("fork: pid=%d parent_mm->pgdir is NULL, sharing mm\n",
                (int)current->pid);
        }
    }

    memcpy((void *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t)),
           regs, sizeof(pt_regs_t));

    // Child process sees fork() return 0
    {
        pt_regs_t *child_regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
        child_regs->rax = 0;
    }

    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;

    thd->rip = regs->rip;
    if (!(tsk->flags & PF_KTHREAD))
        thd->rip = (uint64_t)ret_from_intr;  // child via softirq/preemption check
    // Parent: do NOT change regs->rip — returns via error_code → RESTORE_ALL

    tsk->state = TASK_RUNNING;
    {
        uint64_t flags = spin_lock_irqsave(&percpu_data[tsk->cpu].rq_lock);
        enqueue_task(tsk, &percpu_data[tsk->cpu]);
        spin_unlock_irqrestore(&percpu_data[tsk->cpu].rq_lock, flags);
    }
    sched_notify_remote(tsk);
    return tsk->pid;   // parent sees child PID
}

int kernel_thread(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t flags)
{
    pt_regs_t regs;
    memset(&regs, 0, sizeof(pt_regs_t));

    regs.rbx = (uint64_t)fn;
    regs.rdx = (uint64_t)arg;

    regs.ds = KERNEL_DS;
    regs.es = KERNEL_DS;
    regs.cs = KERNEL_CS;
    regs.ss = KERNEL_DS;
    regs.rflags = (1 << 9);
    regs.rip = (uint64_t)arch_kernel_thread_entry;

    return do_fork(&regs, flags, 0, 0);
}

struct task_struct *create_kthread(uint64_t (*fn)(uint64_t), uint64_t arg,
                                   const char *name)
{
    (void)name;
    int64_t pid = kernel_thread(fn, arg, PF_KTHREAD);
    if (pid < 0)
        return NULL;

    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        if (t->pid == pid)
            return t;
        pos = task_list_next(pos);
    }
    return NULL;
}

// ── task_send_signal ────────────────────────────────────────
// SMP-safe signal delivery: find task by pid under
// task_list_lock, check PF_KTHREAD / init protection,
// set signal bit, wake if interruptible.
