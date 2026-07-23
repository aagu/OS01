#include <sys/select.h>
#include <errno.h>
#include <stddef.h>
#include <sys/syscall.h>

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout)
{
    int64_t ret = syscall6(SYS_select,
                           (uint64_t)(int64_t)nfds,
                           (uint64_t)readfds,
                           (uint64_t)writefds,
                           (uint64_t)exceptfds,
                           (uint64_t)timeout,
                           0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask)
{
    /* Linux ABI: pselect6 packs {sigmask, sizeof(sigset_t)} as
     * a single struct in the 6th syscall argument.  Inline here
     * because libc cannot include kernel headers. */
    struct pselect6_sigmask {
        const sigset_t *ss;
        size_t          ss_len;
    };

    struct pselect6_sigmask packed = {
        .ss     = sigmask,
        .ss_len = sizeof(sigset_t),
    };

    int64_t ret = syscall6(SYS_pselect6,
                           (uint64_t)(int64_t)nfds,
                           (uint64_t)readfds,
                           (uint64_t)writefds,
                           (uint64_t)exceptfds,
                           (uint64_t)timeout,
                           (uint64_t)&packed);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}
