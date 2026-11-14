#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/spinlock.h>
#include <arch/irq.h>
#include <arch/mmu.h>
#include <core/debug.h>
#include <core/panic.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
#include <memory/vma.h>
#include <memory/slab.h>
#include <memory/uaccess.h>
#include <fs/file.h>
#include <stdlib.h>
#include <errno.h>
#include <kernel.h>
#include <arch/x86_64/vmm.h>
#include <arch/x86_64/vma.h>

// do_exit() frees user page tables and physical pages, then
// marks the task ZOMBIE. thread_t and task_union are freed by
// the waiter (do_waitpid) or, for kthreads, by __switch_to's
// PF_SELF_REAP epilogue (deferred because __switch_to dereferences
// current->thread, and we're running on the kernel stack inside
// task_union).
//
uint64_t do_exit(uint64_t exit_code)
{
    // ── Kernel-thread fast path ─────────────────────────────
    // A PF_KTHREAD task has NO user resources: no VMAs, no private page
    // tables, no waitpid consumer.  It delegates to kthread_exit()
    // (kernel/sched/kthread.c) IMMEDIATELY — before init-protection,
    // reparenting/SIGCHLD, VMA/MM and files cleanup — which releases
    // its copied file-table reference, removes it from the global list
    // and switches out permanently (spec §8.3).  The x86 first-entry
    // trampoline calls do_exit() when fn returns, so a kernel thread
    // ends up here too.
    if (current->flags & PF_KTHREAD)
        kthread_exit(exit_code);   // noreturn

    debug_task("task %d exiting with code %#018lx\n", current->pid, exit_code);

    // ── Init process protection ──────────────────────────
    // The user-space init process (PID 1) must never exit.
    // Check by pid rather than pointer — after fork, child
    // task structs are copies and pointer comparison fails.
    if (current->pid == user_init_pid) {
        debug_task("PANIC: init (pid=%d) attempted to exit with code %#018lx\n",
                      (int)current->pid, exit_code);
        while (1) { __asm__ __volatile__("hlt"); }
    }

    // ── Reparent children to init ────────────────────────
    if (user_init_task && current->pid != user_init_pid) {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        list_t *cpos = init_task_union.task.list.next;
        while (cpos != &init_task_union.task.list) {
            task_t *child = container_of(cpos, task_t, list);
            cpos = cpos->next;
            if (child->parent == current) {
                child->parent = user_init_task;
                debug_task("reparent: child %d → init (pid=%d)\n",
                              (int)child->pid, (int)user_init_task->pid);
            }
        }
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }

    // Defensive self-reparent: a USER task whose own parent is a kthread
    // or NULL (shouldn't happen in normal flow, but closes the
    // "parent==NULL zombie leak" the old reaper's parent==NULL branch
    // used to handle) becomes init's child so init can reap it.
    //
    // The current->parent read + write is done under task_list_lock, like
    // the child-reparent block above: waitpid_should_unblock and
    // sched_unblock_blocked read t->parent from unlocked contexts, so a
    // lockless write here is a data race with those scans on another CPU.
    // (Note: after reparenting to init, the SIGCHLD block below will
    // deliver to init instead of being skipped — harmless; init ignores it.)
    if (user_init_task && !(current->flags & PF_KTHREAD)) {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        task_t *p = current->parent;
        if (p == NULL || (p->flags & PF_KTHREAD)) {
            current->parent = user_init_task;
            debug_task("reparent: self %d → init (orphan)\n", (int)current->pid);
        }
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }

    /* Reparenting and reaping may replace/free the old parent concurrently.
     * Resolve and notify it inside one task-list lifetime transaction. */
    task_signal_parent(current, 1ULL << SIGCHLD);

    // Retire the old user root before any teardown (unmap/TLB
    // ownership plan Task 1).  One IRQ-off critical section — with
    // the existing task_list_lock publication order — saves the old
    // mm, publishes the detachment, and switches BOTH the saved CR3
    // and the hardware CR3 to init_mm.  User roots are single-task
    // owned, so once this task has switched away nothing can ever
    // install the old root again; only then is it destroyed.
    if (!(current->flags & PF_KTHREAD) && current->mm) {
        mm_t *dead_mm = current->mm;
        uint64_t flags = spin_lock_irqsave(&task_list_lock);
        current->mm = NULL;   /* publish detachment first */
        current->thread->cr3 = (uint64_t)init_mm.pgdir;
        arch_switch_mm(init_mm.pgdir);
        spin_unlock_irqrestore(&task_list_lock, flags);

        // Free VMA-managed pages (anon + file-backed unmaps, not 2MB
        // ELF pages), then the page tables themselves.
        vma_free_all(dead_mm);
        if (dead_mm->pgdir)
            vmm_free_user_map(
                (uint64_t *)Phy_To_Virt((uint64_t)dead_mm->pgdir));
        kfree(dead_mm);
    }

    // Detach our fd-table under task_list_lock (serializes against
    // task_files_pin_by_pid), then drop the reference outside the lock
    // (files_unpin drop-to-zero now calls files_free synchronously).
    files_t *fs = NULL;
    {
        uint64_t fl = spin_lock_irqsave(&task_list_lock);
        fs = current->files;
        current->files = NULL;
        spin_unlock_irqrestore(&task_list_lock, fl);
    }
    if (fs)
        files_unpin(fs);

    // On-CPU: our kernel stack is in use until __switch_to clears
    // on_cpu.  A waiter must not free us after we set ZOMBIE below
    // (final schedule() still runs on this stack).
    __atomic_store_n(&current->on_cpu, 1, __ATOMIC_RELEASE);

    current->exit_code = exit_code;

    // (The old PF_KTHREAD self-reap branch lived here; it moved to
    // kthread_exit() in kernel/sched/kthread.c and is reached via the
    // delegation at the top of do_exit, so it cannot double-clean.)

    // NOTE: we stay TASK_RUNNING through the cleanup below and only
    // become TASK_ZOMBIE immediately before the final schedule().
    // Setting ZOMBIE earlier lets a waiter reap (free) this task's
    // kernel stack while do_exit is still running on it — the
    // SMP-only intermittent #PF.

    /* No parent pointer survives its lifetime lock, even for diagnostics. */
    task_wake_parent(current);
    task_exit_zombie();
    debug_task("task %d now ZOMBIE\n", current->pid);
    schedule();
    return 0;  // unreachable
}

// ── task_files_pin_by_pid ─────────────────────────────────
// Locate a task by pid under task_list_lock and pin its fd table so a
// /proc reader can inspect it without racing do_exit.
files_t *task_files_pin_by_pid(int pid)
{
    files_t *fs = NULL;
    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid) {
            if (t->files) {
                fs = t->files;
                files_pin(fs);   // atomic, safe under task_list_lock
            }
            break;
        }
    }
    spin_unlock_irqrestore(&task_list_lock, tl_flags);
    return fs;
}
