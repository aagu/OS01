#include <sched/task.h>
#include <sched/internal.h>
#include <arch/spinlock.h>
#include <memory/uaccess.h>
#include <core/debug.h>
#include <log/log.h>
#include <kernel.h>
#include <errno.h>

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
            sched_free_reaped_task(child);

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
