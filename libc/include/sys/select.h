#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H 1

#include <sys/types.h>    /* fd_set */
#include <sys/time.h>     /* struct timeval, struct timespec */
#include <signal.h>       /* sigset_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── fd_set size ────────────────────────────────────────── */
#define FD_SETSIZE  1024

/* ── Bitmap index / position helpers ────────────────────── */
#define FD_IDX(fd)     ((fd) / (8 * sizeof(long)))
#define FD_BITPOS(fd)  ((fd) % (8 * sizeof(long)))

/* ── fd_set manipulation (static inline with bounds check) ─ */

static inline void FD_ZERO(fd_set *set)
{
    for (int __i = 0; __i < 16; __i++)
        set->__fds_bits[__i] = 0;
}

static inline void FD_SET(int fd, fd_set *set)
{
    if ((unsigned)fd < FD_SETSIZE)
        set->__fds_bits[FD_IDX(fd)] |= (1L << FD_BITPOS(fd));
}

static inline void FD_CLR(int fd, fd_set *set)
{
    if ((unsigned)fd < FD_SETSIZE)
        set->__fds_bits[FD_IDX(fd)] &= ~(1L << FD_BITPOS(fd));
}

static inline int FD_ISSET(int fd, fd_set *set)
{
    if ((unsigned)fd < FD_SETSIZE)
        return (set->__fds_bits[FD_IDX(fd)] >> FD_BITPOS(fd)) & 1;
    return 0;
}

/* ── select/pselect declarations ────────────────────────── */

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SELECT_H */
