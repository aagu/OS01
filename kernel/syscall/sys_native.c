#include <errno.h>
#include <limits.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#include <uapi/time.h>
#include <sched/task.h>
#include <memory/slab.h>
#include <memory/uaccess.h>
#include <arch/clocksource.h>
#include <arch/syscall.h>
#include <log/log.h>

/* ── Shared native syscall handlers ────────────────────────────
 *
 * These four entries are the ones the AArch64 port serves as well,
 * so they live in their own translation unit instead of inside the
 * architecture-neutral dispatch families.  The unit stays free of any
 * per-architecture register access: the syscall context carries the
 * arguments, and the architecture-specific effect of exit() is
 * delegated to <arch/syscall.h>.
 */

int64_t sys_native_exit(syscall_ctx_t *ctx)
{
    // exit(int code) — terminate the current process.
    // Encode as Linux does: the exit code lives in the high byte
    // (code << 8) so waitpid() can tell a normal exit (WIFEXITED)
    // from a signal death (low byte = signal → WIFSIGNALED).
    uint64_t status = (ctx->args[0] & 0xff) << 8;
    arch_syscall_exit(status);
    __builtin_unreachable();
}

int64_t sys_native_fork(syscall_ctx_t *ctx)
{
    // fork() → child PID in the parent, 0 in the child's saved frame.
    // The architecture owns the frame fix-up; this handler keeps the
    // historical x86 trace line and return value.
    int64_t pid = arch_syscall_fork(ctx ? ctx->arch_frame : NULL);
    log_info("fork: pid=%d returned %d\n", (int)current->pid, (int)pid);
    return pid;
}

int64_t sys_native_getpid(syscall_ctx_t *ctx)
{
    (void)ctx;
    return current->pid;
}

int64_t sys_native_getppid(syscall_ctx_t *ctx)
{
    (void)ctx;
    // getppid() → parent PID (or 0 for init)
    return current->parent ? current->parent->pid : 0;
}

int64_t sys_native_clock_gettime(syscall_ctx_t *ctx)
{
    // clock_gettime(clockid_t clk_id, struct timespec *tp)
    // OS01 has no real RTC wall clock yet (gettimeofday returns 0),
    // so both CLOCK_REALTIME and CLOCK_MONOTONIC report the same
    // monotonic clocksource time (clocksource_read_ns, ns).
    uint64_t clk_id = ctx->args[0];
    struct timespec *tp = (struct timespec *)ctx->args[1];
    if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC)
        return -EINVAL;
    if (!tp)
        return -EFAULT;
    uint64_t ns = arch_clocksource_read_ns();
    struct timespec kts = {
        .tv_sec  = ns / 1000000000ULL,
        .tv_nsec = ns % 1000000000ULL,
    };
    ssize_t r = copy_to_user_ft(tp, &kts, sizeof(kts));
    if (r < 0)
        return r;
    return 0;
}

int64_t sys_native_waitpid(syscall_ctx_t *ctx)
{
    int64_t pid = (int64_t)(int)ctx->args[0];
    uint64_t status_addr = ctx->args[1];
    int options = (int)ctx->args[2];

    if (status_addr) {
        int rc = syscall_prepare_user_write(status_addr, sizeof(int));
        if (rc < 0)
            return rc;
    }
    return do_waitpid(pid, (int *)(uintptr_t)status_addr, options);
}

static void free_deep_argv(char **kargv, char **kenvp)
{
    if (kargv) {
        for (size_t i = 0; kargv[i] != NULL; i++)
            kfree(kargv[i]);
        kfree(kargv);
    }
    if (kenvp) {
        for (size_t i = 0; kenvp[i] != NULL; i++)
            kfree(kenvp[i]);
        kfree(kenvp);
    }
}

static void free_partial_argv(char **arr, size_t filled)
{
    if (!arr)
        return;
    for (size_t i = 0; i < filled; i++)
        kfree(arr[i]);
    kfree(arr);
}

#ifdef OS01_SELFTEST
int64_t deep_copy_argv(const char *const *user_arr, char ***out_arr)
#else
static int64_t deep_copy_argv(const char *const *user_arr, char ***out_arr)
#endif
{
    *out_arr = NULL;
    if (user_arr == NULL)
        return 0;

    struct argv_scan {
        const char *ptrs[MAX_ARGV + 1];
        size_t lens[MAX_ARGV];
    };
    struct argv_scan *scan = (struct argv_scan *)kmalloc(sizeof(struct argv_scan));
    if (!scan)
        return -ENOMEM;

    size_t count = 0;
    bool null_found = false;
    int64_t ret = 0;

    for (size_t i = 0; i <= MAX_ARGV; i++) {
        uint64_t slot_addr = (uint64_t)(uintptr_t)&user_arr[i];
        if (syscall_strnlen_user(slot_addr, 0) < 0 ||
            syscall_strnlen_user(slot_addr + sizeof(uint64_t) - 1, 0) < 0) {
            ret = -EFAULT;
            goto out;
        }
        uint64_t p = 0;
        if (copy_from_user_ft(&p, &user_arr[i], sizeof(p)) < 0) {
            ret = -EFAULT;
            goto out;
        }
        if (p == 0) {
            count = i;
            null_found = true;
            break;
        }
        if (syscall_strnlen_user(p, 0) < 0) {
            ret = -EFAULT;
            goto out;
        }
        scan->ptrs[i] = (const char *)(uintptr_t)p;
    }
    if (!null_found) {
        ret = -E2BIG;
        goto out;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        int n = syscall_strnlen_user((uint64_t)(uintptr_t)scan->ptrs[i],
                                     MAX_ARG_STRLEN);
        if (n < 0) {
            ret = -EFAULT;
            goto out;
        }
        if (n >= MAX_ARG_STRLEN) {
            ret = -E2BIG;
            goto out;
        }
        scan->lens[i] = (size_t)n + 1;
        total += scan->lens[i];
        if (total > MAX_ARG_TOTAL) {
            ret = -E2BIG;
            goto out;
        }
    }

    char **arr = (char **)kmalloc((count + 1) * sizeof(char *));
    if (!arr) {
        ret = -ENOMEM;
        goto out;
    }
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

int64_t sys_native_exec(syscall_ctx_t *ctx)
{
    uint64_t u_path = ctx->args[0];
    const char *const *argv = (const char *const *)(uintptr_t)ctx->args[1];
    const char *const *envp = (const char *const *)(uintptr_t)ctx->args[2];

    if (argv != NULL && syscall_strnlen_user((uint64_t)(uintptr_t)argv, 0) < 0)
        return -EFAULT;
    if (envp != NULL && syscall_strnlen_user((uint64_t)(uintptr_t)envp, 0) < 0)
        return -EFAULT;

    int plen = syscall_strnlen_user(u_path, PATH_MAX);
    if (plen < 0)
        return -EFAULT;
    if (plen >= PATH_MAX)
        return -ENAMETOOLONG;

    char *path_copy = (char *)kmalloc((size_t)plen + 1);
    if (!path_copy)
        return -ENOMEM;
    if (copy_from_user_ft(path_copy, (const void *)(uintptr_t)u_path,
                          (size_t)plen + 1) < 0) {
        kfree(path_copy);
        return -EFAULT;
    }

    char **kargv = NULL;
    char **kenvp = NULL;
    if (argv != NULL) {
        int64_t r = deep_copy_argv(argv, &kargv);
        if (r < 0) {
            kfree(path_copy);
            return r;
        }
    }
    if (envp != NULL) {
        int64_t r = deep_copy_argv(envp, &kenvp);
        if (r < 0) {
            free_deep_argv(kargv, NULL);
            kfree(path_copy);
            return r;
        }
    }

    if (argv != NULL && envp != NULL) {
        size_t ac = 0;
        size_t ec = 0;
        while (kargv[ac])
            ac++;
        while (kenvp[ec])
            ec++;
        if (ac + ec > MAX_ARGV) {
            free_deep_argv(kargv, kenvp);
            kfree(path_copy);
            return -E2BIG;
        }
    }

    int64_t rc = arch_syscall_exec(ctx->arch_frame, path_copy,
                                   (const char *const *)kargv,
                                   (const char *const *)kenvp);
    free_deep_argv(kargv, kenvp);
    kfree(path_copy);

    if (rc == 0)
        ctx->suppress_writeback = true;
    return rc;
}
