#include <syscall/dispatch.h>
#include <arch/syscall.h>
#include <uapi/syscall.h>
#include <sched/task.h>
#include <memory/slab.h>
#include <memory/uaccess.h>
#include <fs/file.h>
#include <fs/select.h>
#include <tty/tty.h>
#include <errno.h>
#include <log/log.h>
#include <kernel.h>
#include <arch/x86_64/uaccess.h>
#include <fs/vfs.h>

// ── exec argv/envp bounded deep-copy (Task 5, Cat A') ───────
// Allocates a kernel-heap copy of a user-space NULL-terminated array
// of string pointers, including a kernel copy of every string.
//
// Bounds:
//   * at most MAX_ARGV pointers in the array
//   * each string is at most MAX_ARG_STRLEN bytes (incl. NUL)
//   * total bytes across all strings <= MAX_ARG_TOTAL
//
// On any fault or bound violation:
//   * array element pointer < X86_USER_MIN_ADDR or >= addr_limit → -EFAULT
//   * strnlen_user fault → -EFAULT
//   * strnlen_user returns MAX_ARG_STRLEN (no NUL within cap) → -E2BIG
//   * element count > MAX_ARGV → -E2BIG
//   * total bytes > MAX_ARG_TOTAL → -E2BIG
//   * kmalloc failure → -ENOMEM
//
// On failure all already-allocated strings + the partial array are freed.
// On success *out_arr is a NULL-terminated kmalloc'd array of kmalloc'd
// strings; caller frees with free_deep_argv().
int64_t sys_proc_dispatch(syscall_ctx_t *ctx)
{
    int64_t ret = -EINVAL;
    switch (ctx->nr) {
    case SYS_setpgid: {
        int pid = (int)(int64_t)ctx->args[0];
        int pgid = (int)(int64_t)ctx->args[1];
        if (pid == 0) pid = current->pid;
        if (pgid == 0) pgid = pid;
        if (pid < 0 || pgid < 0 || pid == 1) {
            ret = -EINVAL; break;
        }
        uint64_t f = spin_lock_irqsave(&task_list_lock);
        task_t *target = NULL;
        list_t *pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            pos = task_list_next(pos);
            if (t->pid == pid && !(t->flags & PF_KTHREAD)) {
                target = t; break;
            }
        }
        if (!target) {
            spin_unlock_irqrestore(&task_list_lock, f);
            ret = -ESRCH; break;
        }
        if (current->pid != target->pid && current->session != target->session) {
            spin_unlock_irqrestore(&task_list_lock, f);
            ret = -EPERM; break;
        }
        // v4: pgid == pid OR pgid exists in caller's session
        int pgid_ok = (pgid == pid);
        if (!pgid_ok) {
            list_t *pos2 = init_task_union.task.list.next;
            while (pos2 != &init_task_union.task.list) {
                task_t *t2 = container_of(pos2, task_t, list);
                pos2 = task_list_next(pos2);
                if (t2->pgrp == pgid && t2->session == current->session) {
                    pgid_ok = 1; break;
                }
            }
        }
        if (!pgid_ok) {
            spin_unlock_irqrestore(&task_list_lock, f);
            ret = -EPERM; break;
        }
        target->pgrp = pgid;
        // ── v3 自动 fg_pgrp 更新──────────────────
        // 任一成功 setpgid（含 join 现有 pgrp）且 fd 0 指向控制台 TTY 时
        // （file_t->tty == get_dev_tty()，由 §4.1.1 在 open 路径置位），
        // 把 dev_tty.fg_pgrp 同步到新 pgid——替代 POSIX 要求的"shell 调 tcsetpgrp"
        tty_t *dev_tty = get_dev_tty();
        if (dev_tty && current->files && current->files->fd[0]) {
            file_t *f0 = current->files->fd[0];
            if (f0->tty == dev_tty) {
                uint64_t ftf = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
                dev_tty->fg_pgrp = pgid;
                spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, ftf);
            }
        }
        spin_unlock_irqrestore(&task_list_lock, f);
        ret = 0;
        break;
    }
    case SYS_getpgid: {
        int pid = (int)(int64_t)ctx->args[0];
        if (pid == 0) pid = current->pid;
        uint64_t f = spin_lock_irqsave(&task_list_lock);
        ret = -ESRCH;
        list_t *pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            pos = task_list_next(pos);
            if (t->pid == pid) { ret = t->pgrp; break; }
        }
        spin_unlock_irqrestore(&task_list_lock, f);
        break;
    }
    case SYS_setsid: {
        uint64_t f = spin_lock_irqsave(&task_list_lock);
        if (current->pgrp == current->pid) {
            spin_unlock_irqrestore(&task_list_lock, f);
            ret = -EBUSY; break;
        }
        current->session = current->pid;
        current->pgrp = current->pid;
        spin_unlock_irqrestore(&task_list_lock, f);
        ret = current->pid;
        break;
    }
    case SYS_getsid: {
        ret = current->session;
        break;
    }
    case SYS_umask: {
        // umask(mode_t mode) — stub: always return 0
        ret = 0;
        break;
    }
    default:
        break;
    }
    return ret;
}
