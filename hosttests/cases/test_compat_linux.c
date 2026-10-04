#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syscall/compat.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>

/* Mock families to intercept dispatch calls from production compatibility layer */
static syscall_ctx_t *last_dispatched_ctx;
static int64_t mock_dispatch_return = 0;

int64_t sys_fs_dispatch(syscall_ctx_t *ctx)   { last_dispatched_ctx = ctx; return mock_dispatch_return; }
int64_t sys_mm_dispatch(syscall_ctx_t *ctx)   { last_dispatched_ctx = ctx; return mock_dispatch_return; }
int64_t sys_time_dispatch(syscall_ctx_t *ctx) { last_dispatched_ctx = ctx; return mock_dispatch_return; }
int64_t sys_misc_dispatch(syscall_ctx_t *ctx) { last_dispatched_ctx = ctx; return mock_dispatch_return; }
int64_t sys_net_dispatch(syscall_ctx_t *ctx)  { last_dispatched_ctx = ctx; return mock_dispatch_return; }
int64_t sys_proc_dispatch(syscall_ctx_t *ctx) { last_dispatched_ctx = ctx; return mock_dispatch_return; }

int main(void)
{
    /* 1. Compile-time & type sanity */
    _Static_assert(sizeof(compat_syscall_nr_t) == 2, "compat_syscall_nr_t must be 2 bytes");

    /* 2. Null context guard */
    if (compat_linux_dispatch(NULL) != -EINVAL) {
        fprintf(stderr, "FAIL: compat_linux_dispatch(NULL) must return -EINVAL\n");
        return 1;
    }

    /* 3. Out of bounds & unmapped / unsupported return -ENOSYS */
    syscall_ctx_t ctx;

    // Out of bounds
    ctx = (syscall_ctx_t){ .nr = LINUX_X86_64_NR_MAX };
    if (compat_linux_dispatch(&ctx) != -ENOSYS || compat_linux_has_syscall(ctx.nr)) {
        fprintf(stderr, "FAIL: out-of-bounds linux nr %lu must return -ENOSYS\n", ctx.nr);
        return 1;
    }

    ctx = (syscall_ctx_t){ .nr = 999 };
    if (compat_linux_dispatch(&ctx) != -ENOSYS || compat_linux_has_syscall(ctx.nr)) {
        fprintf(stderr, "FAIL: linux nr 999 must return -ENOSYS\n", ctx.nr);
        return 1;
    }

    // Unmapped entry (e.g. Linux nr 7 = poll)
    ctx = (syscall_ctx_t){ .nr = 7 };
    if (compat_linux_dispatch(&ctx) != -ENOSYS || compat_linux_has_syscall(ctx.nr)) {
        fprintf(stderr, "FAIL: unmapped linux nr 7 must return -ENOSYS\n");
        return 1;
    }

    // Explicit unsupported sentinel (Linux nr 25 = mremap)
    ctx = (syscall_ctx_t){ .nr = 25 };
    if (compat_linux_dispatch(&ctx) != -ENOSYS || compat_linux_has_syscall(ctx.nr)) {
        fprintf(stderr, "FAIL: unsupported linux nr 25 must return -ENOSYS\n");
        return 1;
    }
    if (compat_linux_lookup_nr(25) != COMPAT_UNSUPPORTED) {
        fprintf(stderr, "FAIL: lookup_nr(25) must return COMPAT_UNSUPPORTED\n");
        return 1;
    }

    /* 4. Validate key iconic Linux x86_64 mappings */
    struct {
        uint64_t linux_nr;
        compat_syscall_nr_t expected_os_nr;
    } test_mappings[] = {
        { 0,   SYS_read },
        { 1,   SYS_write },
        { 2,   SYS_open },
        { 3,   SYS_close },
        { 4,   SYS_stat },
        { 5,   SYS_fstat },
        { 6,   SYS_lstat },
        { 8,   SYS_lseek },
        { 9,   SYS_mmap },
        { 10,  SYS_mprotect },
        { 11,  SYS_munmap },
        { 12,  SYS_brk },
        { 14,  SYS_sigprocmask },
        { 15,  SYS_sigreturn },
        { 16,  SYS_ioctl },
        { 21,  SYS_access },
        { 32,  SYS_dup },
        { 33,  SYS_dup2 },
        { 35,  SYS_nanosleep },
        { 39,  SYS_getpid },
        { 41,  SYS_socket },
        { 42,  SYS_connect },
        { 43,  SYS_accept },
        { 44,  SYS_sendto },
        { 45,  SYS_recvfrom },
        { 48,  SYS_shutdown },
        { 49,  SYS_bind },
        { 50,  SYS_listen },
        { 51,  SYS_getsockname },
        { 54,  SYS_setsockopt },
        { 55,  SYS_getsockopt },
        { 56,  SYS_fork },
        { 57,  SYS_fork },
        { 59,  SYS_exec },
        { 60,  SYS_exit },
        { 62,  SYS_kill },
        { 63,  SYS_uname },
        { 79,  SYS_getcwd },
        { 80,  SYS_chdir },
        { 83,  SYS_mkdir },
        { 84,  SYS_rmdir },
        { 87,  SYS_unlink },
        { 88,  SYS_symlink },
        { 89,  SYS_readlink },
        { 110, SYS_getppid },
        { 162, SYS_nanosleep },
        { 164, SYS_getifaddr },
        { 201, SYS_times },
        { 217, SYS_getdents64 },
        { 228, SYS_clock_gettime },
        { 231, SYS_exit },
        { 262, SYS_fstatat },
        { 318, SYS_getrandom },
    };

    mock_dispatch_return = 12345;
    for (unsigned i = 0; i < sizeof(test_mappings) / sizeof(test_mappings[0]); i++) {
        uint64_t l_nr = test_mappings[i].linux_nr;
        compat_syscall_nr_t exp_nr = test_mappings[i].expected_os_nr;

        if (!compat_linux_has_syscall(l_nr)) {
            fprintf(stderr, "FAIL: compat_linux_has_syscall(%lu) returned false\n", l_nr);
            return 1;
        }
        if (compat_linux_lookup_nr(l_nr) != exp_nr) {
            fprintf(stderr, "FAIL: compat_linux_lookup_nr(%lu) = %d, expected %d\n",
                    l_nr, compat_linux_lookup_nr(l_nr), exp_nr);
            return 1;
        }

        ctx = (syscall_ctx_t){
            .nr = l_nr,
            .args = { 1, 2, 3, 4, 5, 6 },
        };
        last_dispatched_ctx = NULL;
        int64_t ret = compat_linux_dispatch(&ctx);
        if (ret != 12345 || last_dispatched_ctx != &ctx || ctx.nr != (uint64_t)exp_nr) {
            fprintf(stderr, "FAIL: compat_linux_dispatch(%lu) did not remap to %d correctly (got ret=%ld, ctx.nr=%lu)\n",
                    l_nr, exp_nr, (long)ret, (unsigned long)ctx.nr);
            return 1;
        }
    }

    /* 5. Test domain adapter: rt_sigaction (nr 13) */
    // sigsetsize != 8 must return -EINVAL
    ctx = (syscall_ctx_t){
        .nr = 13,
        .args = { 2, 0x1000, 0x2000, 4 }, // arg[3] = 4 (invalid)
    };
    if (compat_linux_dispatch(&ctx) != -EINVAL) {
        fprintf(stderr, "FAIL: rt_sigaction with sigsetsize != 8 must fail with -EINVAL\n");
        return 1;
    }

    // sigsetsize == 8 must succeed and remap to SYS_signal
    ctx = (syscall_ctx_t){
        .nr = 13,
        .args = { 2, 0x1000, 0x2000, 8 }, // arg[3] = 8 (valid)
    };
    last_dispatched_ctx = NULL;
    if (compat_linux_dispatch(&ctx) != 12345 || ctx.nr != SYS_signal) {
        fprintf(stderr, "FAIL: rt_sigaction with sigsetsize == 8 must remap to SYS_signal\n");
        return 1;
    }

    /* 6. Test domain adapter: wait4 (nr 61) */
    ctx = (syscall_ctx_t){
        .nr = 61,
        .args = { 100, 0x2000, 1, 0x3000 }, // pid=100, status, WNOHANG, rusage
    };
    last_dispatched_ctx = NULL;
    if (compat_linux_dispatch(&ctx) != 12345 || ctx.nr != SYS_waitpid) {
        fprintf(stderr, "FAIL: wait4 must remap to SYS_waitpid\n");
        return 1;
    }

    puts("test_compat_linux: PASS");
    return 0;
}
