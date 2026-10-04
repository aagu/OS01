#include <syscall/compat.h>
#include <uapi/syscall.h>
#include "internal.h"

_Static_assert(sizeof(compat_syscall_nr_t) == 2, "compat_syscall_nr_t must be 16-bit");
_Static_assert(SYS_fstatat <= INT16_MAX, "OS01 syscall numbers must fit within int16_t");
_Static_assert(LINUX_X86_64_NR_MAX <= 1024, "Linux nr table size sanity check");

const compat_syscall_nr_t linux_x86_64_table[LINUX_X86_64_NR_MAX] = {
    [0]   = SYS_read,
    [1]   = SYS_write,
    [2]   = SYS_open,
    [3]   = SYS_close,
    [4]   = SYS_stat,
    [5]   = SYS_fstat,
    [6]   = SYS_lstat,
    [8]   = SYS_lseek,
    [9]   = SYS_mmap,
    [10]  = SYS_mprotect,
    [11]  = SYS_munmap,
    [12]  = SYS_brk,
    [13]  = SYS_signal,        // rt_sigaction
    [14]  = SYS_sigprocmask,   // rt_sigprocmask
    [15]  = SYS_sigreturn,     // rt_sigreturn
    [16]  = SYS_ioctl,
    [21]  = SYS_access,
    [25]  = COMPAT_UNSUPPORTED,// mremap -> unsupported
    [32]  = SYS_dup,
    [33]  = SYS_dup2,
    [35]  = SYS_nanosleep,
    [39]  = SYS_getpid,
    [41]  = SYS_socket,
    [42]  = SYS_connect,
    [43]  = SYS_accept,
    [44]  = SYS_sendto,
    [45]  = SYS_recvfrom,
    [48]  = SYS_shutdown,
    [49]  = SYS_bind,
    [50]  = SYS_listen,
    [51]  = SYS_getsockname,
    [54]  = SYS_setsockopt,
    [55]  = SYS_getsockopt,
    [56]  = SYS_fork,          // clone
    [57]  = SYS_fork,          // fork
    [59]  = SYS_exec,          // execve
    [60]  = SYS_exit,          // _exit
    [61]  = SYS_waitpid,       // wait4
    [62]  = SYS_kill,
    [63]  = SYS_uname,
    [79]  = SYS_getcwd,
    [80]  = SYS_chdir,
    [83]  = SYS_mkdir,         // mkdir (Linux 83)
    [84]  = SYS_rmdir,         // rmdir (Linux 84)
    [85]  = SYS_unlink,        // historical alias
    [86]  = SYS_rmdir,         // historical alias
    [87]  = SYS_unlink,        // unlink (Linux 87)
    [88]  = SYS_symlink,
    [89]  = SYS_readlink,
    [102] = SYS_getppid,       // historical alias
    [110] = SYS_getppid,       // getppid
    [162] = SYS_nanosleep,
    [164] = SYS_getifaddr,
    [201] = SYS_times,
    [217] = SYS_getdents64,
    [228] = SYS_clock_gettime,
    [231] = SYS_exit,          // exit_group
    [262] = SYS_fstatat,       // newfstatat
    [318] = SYS_getrandom,
};
