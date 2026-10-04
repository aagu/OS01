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
#include <memory/vma.h>
#include <memory/slab.h>
#include <memory/uaccess.h>
#include <fs/file.h>
#include <stdlib.h>
#include <errno.h>
#include <kernel.h>

// do_exit() frees user page tables and physical pages, then
// marks the task ZOMBIE. thread_t and task_union are freed by
// the waiter (do_waitpid) or, for kthreads, by __switch_to's
// PF_SELF_REAP epilogue (deferred because __switch_to dereferences
// current->thread, and we're running on the kernel stack inside
// task_union).
//
uint64_t do_exit(uint64_t exit_code)
{
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

    // ── Send SIGCHLD to parent ───────────────────────────
    // NOTE: we write-protect parent->state here because we are
    // about to become ZOMBIE.  After this point, the parent may
    // run and reap us via do_waitpid (wait-driven).  A kthread's
    // ZOMBIE is freed by __switch_to's PF_SELF_REAP epilogue.
    if (current->parent && !(current->parent->flags & PF_KTHREAD)) {
        __sync_fetch_and_or(&current->parent->signal, (1ULL << SIGCHLD));
        // Use blocker_wake which checks condition callback before waking.
        // This is the explicit fast path; sched_unblock_blocked() in
        // schedule() is the reliable fallback.
        if (current->parent->blocker.type != BLOCKER_NONE)
            blocker_wake(current->parent);
    }

    // Stop using this address space before any of its pages can be reused.
    // Keep the saved CR3 in sync with hardware while preemption is disabled:
    // otherwise a later switch-in could reload the freed user PGD.
    if (!(current->flags & PF_KTHREAD) && current->mm) {
        arch_irq_state_t irq_flags = arch_local_irq_save();
        current->thread->cr3 = (uint64_t)init_mm.pgdir;
        arch_switch_mm(init_mm.pgdir);
        arch_local_irq_restore(irq_flags);
    }

    // Free VMA-managed pages (anon + file-backed unmaps, not 2MB ELF pages)
    vma_free_all(current->mm);

    if (!(current->flags & PF_KTHREAD) && current->mm) {
        uint64_t *pgd_virt = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pgdir);
        bool mm_is_shared = (current->parent != NULL &&
                             current->parent->mm == current->mm);
        if (!mm_is_shared && current->mm->pgdir)
            vmm_free_user_map(pgd_virt);
        kfree(current->mm);
        current->mm = NULL;
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

    // ── kthread self-reap ──────────────────────────────────
    // Kernel threads have no waitpid consumer. Reclaim them here by
    // removing them from the global list and marking PF_SELF_REAP so
    // __switch_to's epilogue frees thread/fpu_save/stack after the
    // final switch. The list_del + PF_SELF_REAP + ZOMBIE transition is
    // atomic under task_list_lock (IRQs off): a tick firing between any
    // two steps could schedule() and either re-enqueue a still-RUNNING
    // task whose stack __switch_to is about to free (UAF) or switch
    // away without freeing (leak).
    //
    // Note: this branch runs AFTER do_exit's earlier vma_free_all(mm)
    // and files_unpin(fs). A kthread's mm is shared (do_fork: tsk->mm =
    // current->mm), so vma_free_all here is a no-op on the shared
    // (empty-vma) init_mm — pre-existing behavior, not introduced by
    // this change. The self-reap epilogue below still only kfree's the
    // three standalone slabs (thread/fpu_save/stack); files/mm were
    // already handled above.
    if (current->flags & PF_KTHREAD) {
        uint64_t fl = spin_lock_irqsave(&task_list_lock);
        list_del(&current->list);
        current->list.next = NULL;
        current->list.prev = NULL;
        current->flags |= PF_SELF_REAP;
        current->state = TASK_ZOMBIE;
        spin_unlock_irqrestore(&task_list_lock, fl);
        schedule();   // switch_to → __switch_to epilogue frees this task
        // Defensive: a ZOMBIE task is never re-enqueued, so schedule()
        // always switches away. But if a future early-return path
        // (scheduler_ok==0, or in_schedule) ever let us fall through, we
        // are off-list + ZOMBIE + PF_SELF_REAP — halt rather than return
        // into arch_kernel_thread_entry's epilogue, which would do_exit() again
        // and double-list_del our already-NULL'd list node.
        for (;;) __asm__ __volatile__("hlt");
    }

    // NOTE: we stay TASK_RUNNING through the cleanup below and only
    // become TASK_ZOMBIE immediately before the final schedule().
    // Setting ZOMBIE earlier lets a waiter reap (free) this task's
    // kernel stack while do_exit is still running on it — the
    // SMP-only intermittent #PF.

    // ── Direct switch to parent ─────────────────────────────
    // By the time we reach ZOMBIE the parent is either already
    // RUNNING (SIGCHLD woke it) or still INTERRUPTIBLE in
    // do_waitpid.  In either case we want to switch directly to
    // avoid schedule()'s round-robin scan which may pick the
    // idle task instead (task list order changes after zombie
    // reaping inside schedule() can cause this).
    task_t *parent = current->parent;
    int parent_woken = 0;

    if (parent) {
        uint64_t ps = parent->state;
        if (ps == TASK_INTERRUPTIBLE) {
            // EEVDF: don't set RUNNING here — task_wake handles it at end of do_exit
            parent_woken = 1;
        } else if (ps == TASK_RUNNING) {
            // SIGCHLD already woke the parent.
            parent_woken = 1;
        } else if (ps == TASK_UNINTERRUPTIBLE) {
            // Parent is in an unkillable sleep — unlikely for
            // waitpid but handle gracefully: leave it; when the
            // parent finally wakes, do_waitpid reaps us.
            debug_task("exit: p%d parent p%d UNINTERRUPTIBLE (%ld), "
                          "skipping direct switch\n",
                          current->pid, parent->pid, ps);
        }
    }

    debug_task("task %d now ZOMBIE (parent=%d w=%d ps=%ld)\n",
                  current->pid,
                  parent ? (int)parent->pid : -1,
                  parent_woken,
                  parent ? (long)parent->state : -1);

    // Transfer directly to the woken parent to avoid scheduler
    // scan races.  parent_woken is always 1 for normal exit
    // (parent in waitpid = INTERRUPTIBLE or RUNNING).
    if (parent_woken) {
        task_wake(parent);
    }
    current->state = TASK_ZOMBIE;   // now reaped-able: do_exit is done
    schedule();
    return 0;  // unreachable
}

/*
 * Blocker condition callback for do_waitpid.
 * Returns true when a waited-for child has become TASK_ZOMBIE and fully
 * left the CPU (on_cpu == 0), i.e. is immediately reapable.  The caller
 * (do_waitpid) re-scans the task list to actually reap it.
 */
static bool waitpid_should_unblock(task_t *waiter)
{
    int64_t target_pid = waiter->blocker_data.waited_pid;
    list_t *pos = init_task_union.task.list.next;

    while (pos != &init_task_union.task.list) {
        if ((uintptr_t)pos < 0x1000) {
            log_err("[sched] task-list corruption at %p, breaking\n", (void *)pos);
            break;
        }
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);

        if (t->parent != waiter)
            continue;
        if (target_pid != -1 && t->pid != target_pid)
            continue;
        // Reapable only once the child fully left the CPU. A ZOMBIE
        // child still on_cpu==1 is mid-final-schedule — skip it and
        // keep scanning (do NOT return false: another child may be
        // ready).
        if (t->state == TASK_ZOMBIE &&
            __atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) == 0) {
            return true;   // reapable — do_waitpid re-scans to reap it
        }
    }
    return false;
}

// ── do_waitpid ────────────────────────────────────────────
// Block until a child with <pid> exits, or return immediately
// if WNOHANG is set and no child is ready.
//
// pid > 0  → wait for specific child
// pid == -1 → wait for any child
// Returns child PID on success, -ECHILD if no such child, -EINTR if interrupted.
int64_t do_waitpid(int64_t pid, int *user_status, int options)
{
    for (;;) {
        task_t   *child      = NULL;
        int64_t   child_pid  = -1;
        int64_t   exit_code  = 0;

        // Pass 1: find a reapable ZOMBIE child (on_cpu==0) and detach it.
        {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                if ((uintptr_t)pos < 0x1000) {
                    log_err("[sched] task-list corruption at %p, breaking\n", (void *)pos);
                    break;
                }
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);

                if (t->parent != current)
                    continue;
                if (t->state != TASK_ZOMBIE)
                    continue;
                if (pid != -1 && t->pid != pid)
                    continue;
                // Gate on on_cpu==0: __switch_to's RELEASE store
                // guarantees the child's stack/thread/exit_code are no
                // longer in use. A ZOMBIE child still on_cpu==1 is not
                // reapable yet — treat it as "exists, keep waiting".
                if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) != 0)
                    continue;

                // Capture pid/exit_code BEFORE detaching and freeing
                // (kfree(stack_alloc_base) frees the task_union holding
                // the task_t, so t->pid is invalid after step 2 below).
                child      = t;
                child_pid  = t->pid;
                exit_code  = t->exit_code;
                list_del(&t->list);
                t->list.next = NULL;
                t->list.prev = NULL;
                break;
            }
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
        }

        if (child) {
            // Cat B write-back: copy status to user via _ft, then
            // UNCONDITIONALLY reclaim the child.  NEVER return inside
            // the _ft failure branch — that would leak the child's
            // thread/fpu_save/stack_alloc_base slabs.
            ssize_t status_rc = 0;
            if (user_status) {
                int status = (int)exit_code;
                status_rc = copy_to_user_ft(user_status, &status, sizeof(status));
            }

            // Synchronous reclamation (no more schedule() reaper).
            // Order: free the two standalone slabs first, then the
            // task_union (which contains the task_t itself).
            if (child->thread)           kfree(child->thread);
            if (child->fpu_save)         kfree(child->fpu_save);
            if (child->stack_alloc_base) kfree(child->stack_alloc_base);

            debug_task("waitpid: pid=%d reaped child %d (exit=%d)\n",
                          (int)current->pid, (int)child_pid, (int)exit_code);
            return (status_rc < 0) ? status_rc : child_pid;
        }

        // No reapable child — check existence for -ECHILD / WNOHANG.
        int child_exists = 0;
        {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                if ((uintptr_t)pos < 0x1000) {
                    log_err("[sched] task-list corruption at %p, breaking\n", (void *)pos);
                    break;
                }
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (t->parent == current && (pid == -1 || t->pid == pid)) {
                    child_exists = 1;
                    break;
                }
            }
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
        }

        if (!child_exists)
            return -ECHILD;

        if (options & WNOHANG)
            return 0;

        current->blocker_data.waited_pid = pid;

        int ret = blocker_wait(waitpid_should_unblock, BLOCKER_WAITPID, true);
        if (ret == -EINTR)
            continue;   // re-check for children before sleeping again
        // blocker_wait returned 0 → condition met; loop and reap in Pass 1.
        continue;
    }
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
