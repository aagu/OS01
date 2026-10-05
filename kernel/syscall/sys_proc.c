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

#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

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
//   * array element pointer < USER_MIN_ADDR or >= addr_limit → -EFAULT
//   * strnlen_user fault → -EFAULT
//   * strnlen_user returns MAX_ARG_STRLEN (no NUL within cap) → -E2BIG
//   * element count > MAX_ARGV → -E2BIG
//   * total bytes > MAX_ARG_TOTAL → -E2BIG
//   * kmalloc failure → -ENOMEM
//
// On failure all already-allocated strings + the partial array are freed.
// On success *out_arr is a NULL-terminated kmalloc'd array of kmalloc'd
// strings; caller frees with free_deep_argv().
static void free_deep_argv(char **kargv, char **kenvp)
{
    if (kargv) {
        for (size_t i = 0; kargv[i] != NULL; i++) kfree(kargv[i]);
        kfree(kargv);
    }
    if (kenvp) {
        for (size_t i = 0; kenvp[i] != NULL; i++) kfree(kenvp[i]);
        kfree(kenvp);
    }
}

// Internal helper: free all strings + the array (allocated so far).
// Used on the failure paths before the array is fully populated.
static void free_partial_argv(char **arr, size_t filled)
{
    if (!arr) return;
    for (size_t i = 0; i < filled; i++) kfree(arr[i]);
    kfree(arr);
}

// Test-only export: deep_copy_argv is normally static.  Under
// OS01_SELFTEST the storage class is dropped so the regression test in
// kernel/selftest/test_deep_copy_argv.c can call it directly without
// going through syscall dispatch.  No other callers exist outside this
// translation unit.
#ifdef OS01_SELFTEST
int64_t deep_copy_argv(const char *const *user_arr, char ***out_arr)
#else
static int64_t deep_copy_argv(const char *const *user_arr, char ***out_arr)
#endif
{
    *out_arr = NULL;
    if (user_arr == NULL) return 0;

    struct argv_scan {
        const char *ptrs[MAX_ARGV + 1];
        size_t lens[MAX_ARGV];
    };
    struct argv_scan *scan = (struct argv_scan *)kmalloc(sizeof(struct argv_scan));
    if (!scan) return -ENOMEM;

    // Phase 1: scan the array (fault-tolerant per pointer) to count
    // entries and validate every element pointer.  Bound the loop by
    // MAX_ARGV so a hostile unbounded array cannot loop forever.
    size_t count = 0;
    bool null_found = false;
    uint64_t addr_limit = current->addr_limit;
    int64_t ret = 0;

    for (size_t i = 0; i <= MAX_ARGV; i++) {
        uint64_t p = 0;
        if (copy_from_user_ft(&p, &user_arr[i], sizeof(p)) < 0) {
            ret = -EFAULT;
            goto out;
        }
        if (p == 0) {                       // NULL terminator
            count = i;
            null_found = true;
            break;
        }
        // Bad element pointer: kernel address or below USER_MIN_ADDR.
        if (p < USER_MIN_ADDR || p >= addr_limit) {
            ret = -EFAULT;
            goto out;
        }
        scan->ptrs[i] = (const char *)p;
    }
    // If the loop ran to MAX_ARGV+1 without seeing NULL, either the
    // array has more than MAX_ARGV entries (over cap) or it's not
    // NUL-terminated within MAX_ARGV+1 (treated the same).  Reject
    // with -E2BIG per the deep_copy_argv contract (line above).
    //
    // Distinguish this from the legitimate empty case (argv={NULL},
    // NULL found at i=0, null_found=true) which Phase 2/3 handles
    // naturally: zero strnlen iterations, kmalloc(8) for the array,
    // zero copies, arr[0]=NULL terminator.  setup_user_stack (task.c)
    // accepts both argv=NULL and argv={NULL}.
    if (!null_found) {
        ret = -E2BIG;
        goto out;
    }

    // Phase 2: for each element, strnlen + bounded total accumulator.
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        int n = strnlen_user(scan->ptrs[i], MAX_ARG_STRLEN);
        if (n < 0) { ret = -EFAULT; goto out; }
        if (n >= MAX_ARG_STRLEN) { ret = -E2BIG; goto out; }     // no NUL within cap
        scan->lens[i] = (size_t)n + 1;                    // incl. NUL
        total += scan->lens[i];
        if (total > MAX_ARG_TOTAL) { ret = -E2BIG; goto out; }
    }

    // Phase 3: allocate the kernel array (NULL-terminated) and copy
    // every string.  Any failure mid-way frees everything we already
    // allocated.
    char **arr = (char **)kmalloc((count + 1) * sizeof(char *));
    if (!arr) { ret = -ENOMEM; goto out; }
    size_t filled = 0;
    for (size_t i = 0; i < count; i++) {
        char *kstr = (char *)kmalloc(scan->lens[i]);
        if (!kstr) {
            free_partial_argv(arr, filled);
            ret = -ENOMEM;
            goto out;
        }
        if (copy_from_user_ft(kstr, scan->ptrs[i], scan->lens[i]) < 0) {
            kfree(kstr);
            free_partial_argv(arr, filled);
            ret = -EFAULT;
            goto out;
        }
        arr[filled++] = kstr;
    }
    arr[count] = NULL;
    *out_arr = arr;
    ret = 0;

out:
    kfree(scan);
    return ret;
}

int64_t sys_proc_dispatch(syscall_ctx_t *ctx)
{
    int64_t ret = -EINVAL;
    switch (ctx->nr) {
    case SYS_exit: {
        // exit(int code) — terminate current process.
        // Encode as Linux does: exit code in the high byte (code<<8),
        // so waitpid() status can distinguish a normal exit (WIFEXITED)
        // from a signal death (low byte = signal → WIFSIGNALED).
        uint64_t code = ctx->args[0] & 0xFF;
        current->exit_code = code << 8;
        do_exit(code << 8);
        // unreachable — do_exit calls schedule() which never returns
    }
    case SYS_getpid: {
        ret = current->pid;
        break;
    }
    case SYS_exec: {
        // exec(const char *path, char *const argv[], char *const envp[])
        // If argv == NULL: old behavior (no args)
        // If argv != NULL: deep-copy argv/envp arrays+strings to kernel
        // heap (Task 5) before calling sys_exec.  sys_exec then operates
        // ONLY on the kernel copies — no user-memory dereference.
        const char *path = (const char *)ctx->args[0];
        const char *const *argv = (const char *const *)ctx->args[1];
        const char *const *envp = (const char *const *)ctx->args[2];

        if ((uint64_t)path >= current->addr_limit) {
            ret = -EFAULT;
            break;
        }
        // Validate argv pointer if non-NULL
        if (argv != NULL && (uint64_t)argv >= current->addr_limit) {
            ret = -EFAULT;
            break;
        }
        // Validate envp pointer if non-NULL
        if (envp != NULL && (uint64_t)envp >= current->addr_limit) {
            ret = -EFAULT;
            break;
        }

        // Copy path to kernel heap (bounded + fault-tolerant) so that
        // user-mapped pages cannot fault us mid-VFS-walk.
        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { ret = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { ret = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { ret = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            ret = -EFAULT;
            break;
        }

        // Deep-copy argv/envp arrays+strings to kernel heap.  Done
        // BEFORE sys_exec builds the new pgd / frees the old space
        // (no UAF on the old address space).  sys_exec never touches
        // user memory after this point.
        char **kargv = NULL;
        char **kenvp = NULL;
        if (argv != NULL) {
            int64_t r = deep_copy_argv(argv, &kargv);
            if (r < 0) {
                kfree(path_copy);
                ret = r;
                break;
            }
        }
        if (envp != NULL) {
            int64_t r = deep_copy_argv(envp, &kenvp);
            if (r < 0) {
                free_deep_argv(kargv, NULL);
                kfree(path_copy);
                ret = r;
                break;
            }
        }

        // sys_exec builds argv/envp on the user stack using a fixed
        // str_offset[128] table (task.c) — the COMBINED argc+envc must
        // fit (each array alone is bounded to MAX_ARGV=128 by
        // deep_copy_argv, but both together could reach 256).  Enforce
        // the combined cap here so sys_exec's fixed table cannot be
        // overrun (kernel-stack corruption).
        if ((argv != NULL) && (envp != NULL)) {
            size_t ac = 0, ec = 0;
            while (kargv[ac]) ac++;
            while (kenvp[ec]) ec++;
            if (ac + ec > 128) {
                free_deep_argv(kargv, kenvp);
                kfree(path_copy);
                ret = -E2BIG;
                break;
            }
        }

        ret = arch_syscall_exec(ctx->arch_frame, path_copy,
                               (const char *const *)kargv,
                               (const char *const *)kenvp);
        free_deep_argv(kargv, kenvp);
        kfree(path_copy);
        break;
    }
    case SYS_fork: {
        // fork() → child PID in parent, 0 in child
        int64_t pid = arch_syscall_fork(ctx->arch_frame);
        // Parent path: ret = child PID
        // Child return value is zero in its saved frame.
        ret = pid;
        log_info("fork: pid=%d returned %d\n", (int)current->pid, (int)pid);
        break;
    }
    case SYS_waitpid: {
        // waitpid(pid, *status, options) → child PID or error
        int64_t pid = (int64_t)(int)ctx->args[0];
        int *status = (int *)ctx->args[1];
        int options = (int)ctx->args[2];

        // Validate status pointer (NULL legal → skip).
        // check_user_range is a fast reject; the actual _ft copy in
        // do_waitpid is the authority (handles racing munmap).
        if (status &&
            !syscall_check_user_range((uint64_t)status, sizeof(int), true)) {
            ret = -EFAULT;
            break;
        }

        ret = do_waitpid(pid, status, options);
        break;
    }
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
    case SYS_getppid: {
        // getppid() → parent PID (or 0 for init)
        if (current->parent)
            ret = current->parent->pid;
        else
            ret = 0;
        break;
    }
    case SYS_umask: {
        // umask(mode_t mode) — stub: always return 0
        ret = 0;
        break;
    }
    case SYS_kill: {
        // kill(pid, sig) — POSIX process-group semantics:
        //   pid > 0   → signal single task (pid)
        //   pid == 0  → signal caller's process group
        //   pid == -1 → broadcast: all non-init, non-kthread, non-self
        //   pid < -1  → signal process group (-pid)
        int pid = (int)(int64_t)ctx->args[0];
        int sig = (int)ctx->args[1];

        if (sig < 1 || sig >= NSIG) {
            ret = -EINVAL;
            break;
        }

        if (pid > 0) {
            ret = task_send_signal(pid, sig);
        } else if (pid == 0) {
            ret = signal_pgrp(current->pgrp, sig);
        } else if (pid == -1) {
            // POSIX pid==-1: signal to all tasks the caller may signal —
            // everyone except init (pid 1), kernel threads, and self.
            uint64_t f = spin_lock_irqsave(&task_list_lock);
            int matched = 0;
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (t == current) continue;
                if (t->flags & PF_KTHREAD) continue;
                if (t->pid == 1) continue;
                t->signal |= (1ULL << sig);
                if (t->state == TASK_INTERRUPTIBLE)
                    task_wake(t);
                matched++;
            }
            spin_unlock_irqrestore(&task_list_lock, f);
            ret = matched > 0 ? 0 : -ESRCH;
        } else { // pid < -1
            ret = signal_pgrp(-pid, sig);
        }
        break;
    }
    case SYS_signal: {
        // sigaction(signum, const struct sigaction *act,
        //           struct sigaction *oldact) → 0 / -errno
        // Cat B: read user act into kernel copy via _ft, validate, install;
        // write kernel → user oldact via _ft.
        int signum = (int)ctx->args[0];
        const struct sigaction *act = (const struct sigaction *)ctx->args[1];
        struct sigaction *oldact = (struct sigaction *)ctx->args[2];

        if (signum < 1 || signum >= NSIG) {
            ret = -EINVAL;
            break;
        }

        // Read user act into kernel copy (NULL legal → skip).
        struct sigaction kact;
        if (act) {
            if (!syscall_check_user_range((uint64_t)act, sizeof(kact), false)) {
                ret = -EFAULT;
                break;
            }
            if (copy_from_user_ft(&kact, act, sizeof(kact)) < 0) {
                ret = -EFAULT;
                break;
            }
        }

        // Return old action if requested.  Build kernel copy, then _ft
        // write to user.  No bare field writes into user.
        if (oldact) {
            if (!syscall_check_user_range((uint64_t)oldact, sizeof(kact), true)) {
                ret = -EFAULT;
                break;
            }
            struct sigaction kold = {
                .sa_handler  = current->sighand[signum].sa_handler,
                .sa_flags    = current->sighand[signum].sa_flags,
                .sa_restorer = current->sighand[signum].sa_restorer,
                .sa_mask     = current->sighand[signum].sa_mask,
            };
            {
                ssize_t user_copy_rc = copy_to_user_ft(oldact, &kold, sizeof(kold));
                if (user_copy_rc < 0) {
                    ret = user_copy_rc;
                    break;
                }
            }
        }

        // Install new handler (SIGKILL and SIGSTOP cannot be caught or ignored)
        if (act && signum != SIGKILL && signum != SIGSTOP) {
            // Validate user function pointers are not in kernel space
            if ((uint64_t)kact.sa_restorer >= current->addr_limit ||
                (uint64_t)kact.sa_handler  >= current->addr_limit) {
                ret = -EINVAL;
                break;
            }
            current->sighand[signum].sa_handler  = kact.sa_handler;
            current->sighand[signum].sa_flags    = kact.sa_flags;
            current->sighand[signum].sa_restorer = kact.sa_restorer;
            current->sighand[signum].sa_mask     = kact.sa_mask;
        }
        ret = 0;
        break;
    }
    case SYS_sigprocmask: {
        // sigprocmask(int how, const sigset_t *set, sigset_t *oldset).
        // NULL → skip; otherwise _ft bounce.
        int how = (int)ctx->args[0];
        const sigset_t *set = (const sigset_t *)ctx->args[1];
        sigset_t *oldset = (sigset_t *)ctx->args[2];

        // Return current mask if requested
        if (oldset) {
            if (!syscall_check_user_range((uint64_t)oldset,
                                          sizeof(sigset_t), true)) {
                ret = -EFAULT;
                break;
            }
            sigset_t kold = (sigset_t)current->blocked;
            {
                ssize_t user_copy_rc = copy_to_user_ft(oldset, &kold, sizeof(kold));
                if (user_copy_rc < 0) {
                    ret = user_copy_rc;
                    break;
                }
            }
        }

        // Update mask if set is provided
        if (set) {
            if (!syscall_check_user_range((uint64_t)set,
                                          sizeof(sigset_t), false)) {
                ret = -EFAULT;
                break;
            }
            sigset_t kset;
            if (copy_from_user_ft(&kset, set, sizeof(kset)) < 0) {
                ret = -EFAULT;
                break;
            }
            switch (how) {
            case SIG_BLOCK:
                current->blocked |= kset;
                break;
            case SIG_UNBLOCK:
                current->blocked &= ~kset;
                break;
            case SIG_SETMASK:
                current->blocked = (int64_t)kset;
                break;
            default:
                ret = -EINVAL;
                break;
            }
        }
        ret = 0;
        break;
    }
    case SYS_sigreturn: {
        int64_t result = arch_syscall_sigreturn(ctx->arch_frame);
        if (result == 0)
            ctx->suppress_writeback = true;
        return result;
    }
    default:
        break;
    }
    return ret;
}
