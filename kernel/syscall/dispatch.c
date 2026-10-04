#include <errno.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>
typedef struct syscall_entry {
    syscall_handler_t handler;
    const char *name;
} syscall_entry_t;
int64_t sys_fs_dispatch(syscall_ctx_t *ctx);
int64_t sys_mm_dispatch(syscall_ctx_t *ctx);
int64_t sys_time_dispatch(syscall_ctx_t *ctx);
int64_t sys_misc_dispatch(syscall_ctx_t *ctx);


/* Handlers are added here as syscall families migrate out of trap.c. */
static const syscall_entry_t syscall_table[SYS_fstatat + 1] = {
    [SYS_write] = { sys_fs_dispatch, "write" },
    [SYS_read] = { sys_fs_dispatch, "read" },
    [SYS_open] = { sys_fs_dispatch, "open" },
    [SYS_close] = { sys_fs_dispatch, "close" },
    [SYS_dup] = { sys_fs_dispatch, "dup" },
    [SYS_dup2] = { sys_fs_dispatch, "dup2" },
    [SYS_pipe] = { sys_fs_dispatch, "pipe" },
    [SYS_chdir] = { sys_fs_dispatch, "chdir" },
    [SYS_getcwd] = { sys_fs_dispatch, "getcwd" },
    [SYS_stat] = { sys_fs_dispatch, "stat" },
    [SYS_fstat] = { sys_fs_dispatch, "fstat" },
    [SYS_lseek] = { sys_fs_dispatch, "lseek" },
    [SYS_fcntl] = { sys_fs_dispatch, "fcntl" },
    [SYS_ioctl] = { sys_fs_dispatch, "ioctl" },
    [SYS_getdents64] = { sys_fs_dispatch, "getdents64" },
    [SYS_access] = { sys_fs_dispatch, "access" },
    [SYS_unlink] = { sys_fs_dispatch, "unlink" },
    [SYS_mkdir] = { sys_fs_dispatch, "mkdir" },
    [SYS_rmdir] = { sys_fs_dispatch, "rmdir" },
    [SYS_rename] = { sys_fs_dispatch, "rename" },
    [SYS_truncate] = { sys_fs_dispatch, "truncate" },
    [SYS_ftruncate] = { sys_fs_dispatch, "ftruncate" },
    [SYS_chmod] = { sys_fs_dispatch, "chmod" },
    [SYS_fchmod] = { sys_fs_dispatch, "fchmod" },
    [SYS_poll] = { sys_fs_dispatch, "poll" },
    [SYS_ppoll] = { sys_fs_dispatch, "ppoll" },
    [SYS_select] = { sys_fs_dispatch, "select" },
    [SYS_pselect6] = { sys_fs_dispatch, "pselect6" },
    [SYS_symlink] = { sys_fs_dispatch, "symlink" },
    [SYS_readlink] = { sys_fs_dispatch, "readlink" },
    [SYS_lstat] = { sys_fs_dispatch, "lstat" },
    [SYS_fstatat] = { sys_fs_dispatch, "fstatat" },
    [SYS_brk] = { sys_mm_dispatch, "brk" },
    [SYS_mmap] = { sys_mm_dispatch, "mmap" },
    [SYS_mprotect] = { sys_mm_dispatch, "mprotect" },
    [SYS_munmap] = { sys_mm_dispatch, "munmap" },
    [SYS_futex] = { sys_mm_dispatch, "futex" },
    [SYS_time] = { sys_time_dispatch, "time" },
    [SYS_gettimeofday] = { sys_time_dispatch, "gettimeofday" },
    [SYS_clock_gettime] = { sys_time_dispatch, "clock_gettime" },
    [SYS_nanosleep] = { sys_time_dispatch, "nanosleep" },
    [SYS_times] = { sys_time_dispatch, "times" },
    [SYS_putchar] = { sys_misc_dispatch, "putchar" },
    [SYS_getrandom] = { sys_misc_dispatch, "getrandom" },
    [SYS_sync] = { sys_misc_dispatch, "sync" },
    [SYS_reboot] = { sys_misc_dispatch, "reboot" },
    [SYS_uname] = { sys_misc_dispatch, "uname" },
};
bool syscall_has_handler(uint64_t nr)
{
    return nr < (uint64_t)(sizeof(syscall_table) / sizeof(syscall_table[0])) &&
           syscall_table[nr].handler != 0;
}
int64_t syscall_dispatch(syscall_ctx_t *ctx)
{
    if (!ctx || !syscall_has_handler(ctx->nr))
        return -EINVAL;
    return syscall_table[ctx->nr].handler(ctx);
}
