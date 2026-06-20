#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/cdefs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* sigset_t - signal set type */
typedef unsigned long sigset_t;

/* sigprocmask how values */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

/* Special signal handler values */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)
#define SIG_ERR  ((void (*)(int))-1)

/* sigaction flags */
#define SA_NOCLDSTOP  1
#define SA_NOCLDWAIT  2
#define SA_SIGINFO    4
#define SA_RESTART    0x10000000

/* sigaction structure */
struct sigaction {
    void     (*sa_handler)(int);
    uint64_t   sa_flags;
    void     (*sa_restorer)(void);
    uint64_t   sa_mask;
};

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int kill(int64_t pid, int sig);

/* BSD-style signal */
typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler);

/* Signal set manipulation */
#define sigemptyset(set) (*(set) = 0)
#define sigfillset(set)  (*(set) = ~0UL)
#define sigaddset(set, n) (*(set) |= (1UL << ((n)-1)))
#define sigdelset(set, n) (*(set) &= ~(1UL << ((n)-1)))
#define sigismember(set, n) (!!(*(set) & (1UL << ((n)-1))))

int raise(int sig);
int sigsuspend(const sigset_t *mask);
int sigpending(sigset_t *set);

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */
