#include <syscall/compat.h>
#include <syscall/resource.h>
#include <uapi/syscall.h>
#include <uapi/resource.h>
#include <sched/task.h>
#include <sched/resource.h>
#include <memory/uaccess.h>
#include <errno.h>
#include "internal.h"

int64_t compat_sys_rt_sigaction(syscall_ctx_t *ctx)
{
    /* Linux rt_sigaction expects sigsetsize == sizeof(sigset_t) == 8 on x86_64 */
    if (ctx->args[3] != 8)
        return -EINVAL;

    ctx->nr = SYS_signal;
    return syscall_dispatch(ctx);
}

int64_t compat_sys_wait4(syscall_ctx_t *ctx)
{
    /* Linux wait4(pid, status, options, rusage) -> SYS_waitpid(pid, status, options) */
    ctx->nr = SYS_waitpid;
    return syscall_dispatch(ctx);
}

int64_t compat_sys_prlimit64(syscall_ctx_t *ctx)
{
    if (!ctx)
        return -EINVAL;

    task_t *cur = current;
    if (!cur)
        return -ESRCH;

    int64_t pid = (int64_t)ctx->args[0];
    if (pid != 0 && pid != cur->pid)
        return -ESRCH;

    uint64_t resource = ctx->args[1];
    if (resource != OS01_RLIMIT_NOFILE)
        return -EINVAL;

    uint64_t new_uptr = ctx->args[2];
    uint64_t old_uptr = ctx->args[3];

    struct os01_rlimit64 new_lim;
    const struct os01_rlimit64 *p_new = NULL;

    if (new_uptr) {
        if (!syscall_check_user_range(new_uptr, sizeof(new_lim), false))
            return -EFAULT;
        if (copy_from_user_ft(&new_lim, (const void *)new_uptr, sizeof(new_lim)) < 0)
            return -EFAULT;
        p_new = &new_lim;
    }

    struct os01_rlimit64 old_lim;
    int rc = task_nofile_update(cur, p_new, old_uptr ? &old_lim : NULL);
    if (rc < 0)
        return rc;

    if (old_uptr) {
        if (!syscall_check_user_range(old_uptr, sizeof(old_lim), true))
            return -EFAULT;
        if (copy_to_user_ft((void *)old_uptr, &old_lim, sizeof(old_lim)) < 0)
            return -EFAULT;
    }

    return 0;
}
