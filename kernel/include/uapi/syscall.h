#ifndef _UAPI_SYSCALL_H
#define _UAPI_SYSCALL_H

// ── Original (preserved for compatibility) ────────────────
#define SYS_putchar  0
#define SYS_write    1    // write(fd, buf, len)  — fd-based since Phase 1
#define SYS_exit     2
#define SYS_brk      3
#define SYS_getpid   4
#define SYS_exec     5
#define SYS_read     6    // read(fd, buf, len)   — fd-based since Phase 1

// ── Phase 1: file descriptor infrastructure ───────────────
#define SYS_open     7
#define SYS_close    8
#define SYS_dup      9
#define SYS_dup2    10

// ── Phase 2: process management ───────────────────────────
#define SYS_fork    11
#define SYS_waitpid 12

// ── Phase 3: pipe + directory ─────────────────────────────
#define SYS_pipe    13
#define SYS_chdir   14
#define SYS_getcwd  15

// ── Phase 4: filesystem metadata ─────────────────────────────
#define SYS_stat         16   // stat(path, buf)
#define SYS_fstat        17   // fstat(fd, buf)
#define SYS_lseek        18   // lseek(fd, offset, whence)
#define SYS_fcntl        19   // fcntl(fd, cmd, arg)
#define SYS_ioctl        20   // ioctl(fd, request, arg)
#define SYS_getdents64   21   // getdents64(fd, buf, count)
#define SYS_access       22   // access(path, mode)

// ── Phase 5: filesystem write operations ─────────────────────
#define SYS_unlink      23
#define SYS_mkdir       24
#define SYS_rmdir       25
#define SYS_rename      26
#define SYS_truncate    27
#define SYS_ftruncate   28

// ── Phase 6: remaining syscalls ──────────────────────────────
#define SYS_time            29
#define SYS_gettimeofday    30
#define SYS_nanosleep       31
#define SYS_chmod           32
#define SYS_fchmod          33
#define SYS_times           34
#define SYS_uname           35
#define SYS_getppid         36
#define SYS_umask           37
#define SYS_kill            38
#define SYS_signal          39
#define SYS_sync            40
#define SYS_reboot          41

// ── reboot(2) commands ──────────────────────────────────────
#define RB_AUTOBOOT    0x01234567
#define RB_POWER_OFF   0x4321FEDC
#define RB_HALT_SYSTEM 0xCDEF0123

#endif
