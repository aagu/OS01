#ifndef _UAPI_TIME_H
#define _UAPI_TIME_H

#include <stdint.h>

/* Linux x86_64 time structures — must match userspace ABI exactly */
struct timeval {
    uint64_t tv_sec;    /* seconds */
    uint64_t tv_usec;   /* microseconds */
};

struct timespec {
    uint64_t tv_sec;    /* seconds */
    uint64_t tv_nsec;   /* nanoseconds */
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/* sys/utsname.h constants */
#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
};

/* sys/times.h */
struct tms {
    uint64_t tms_utime;
    uint64_t tms_stime;
    uint64_t tms_cutime;
    uint64_t tms_cstime;
};

/* Signal numbers */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGPOLL  29
#define SIGPWR   30
#define SIGSYS   31

#define NSIG      32

/* Special signal handler values */
#define SIG_DFL  ((void *)0)
#define SIG_IGN  ((void *)1)

/* sigaction structure */
struct sigaction {
    void     (*sa_handler)(int);
    uint64_t   sa_flags;
    void     (*sa_restorer)(void);
    uint64_t   sa_mask;
};

/* sigframe — saved on user stack by do_signal_delivery,
 * restored by SYS_sigreturn.  Matches pt_regs_t layout for the
 * GPR + iretq-frame portion; blocked is extra. */
struct sigframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  /* 0x00–0x38 */
    uint64_t rbx, rcx, rdx, rsi, rdi;               /* 0x40–0x60 */
    uint64_t rbp;                                     /* 0x68 */
    uint64_t ds, es;                                  /* 0x70, 0x78 */
    uint64_t rax;                                     /* 0x80 */
    uint64_t func, errcode;                           /* 0x88, 0x90 (padding) */
    uint64_t rip, cs, rflags;                        /* 0x98, 0xa0, 0xa8 */
    uint64_t rsp, ss;                                 /* 0xb0, 0xb8 */
    uint64_t blocked;                                 /* 0xc0 — saved mask */
};
/* sizeof(struct sigframe) == 200 bytes */

#endif
