#include <syscall/resource.h>
#include <sched/resource.h>
#include <sched/task.h>
#include <memory/uaccess.h>
#include <errno.h>
#include <uapi/resource.h>
#include <uapi/syscall.h>

int64_t sys_resource_dispatch(syscall_ctx_t *ctx)
{
    if (!ctx)
        return -EINVAL;

    task_t *cur = current;
    if (!cur)
        return -ESRCH;

    uint64_t resource = ctx->args[0];
    if (resource != OS01_RLIMIT_NOFILE)
        return -EINVAL;

    switch (ctx->nr) {
    case SYS_getrlimit: {
        uint64_t uptr = ctx->args[1];
        if (!uptr || !syscall_check_user_range(uptr, sizeof(struct os01_rlimit64), true))
            return -EFAULT;

        struct os01_rlimit64 old_lim;
        int rc = task_nofile_update(cur, NULL, &old_lim);
        if (rc < 0)
            return rc;

        if (copy_to_user_ft((void *)uptr, &old_lim, sizeof(old_lim)) < 0)
            return -EFAULT;
        return 0;
    }
    case SYS_setrlimit: {
        uint64_t uptr = ctx->args[1];
        if (!uptr || !syscall_check_user_range(uptr, sizeof(struct os01_rlimit64), false))
            return -EFAULT;

        struct os01_rlimit64 new_lim;
        if (copy_from_user_ft(&new_lim, (const void *)uptr, sizeof(new_lim)) < 0)
            return -EFAULT;

        return task_nofile_update(cur, &new_lim, NULL);
    }
    default:
        return -EINVAL;
    }
}
