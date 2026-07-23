// kernel/fs/select.c — select/pselect6 system call implementation
//
// Provides: do_select(), do_pselect6()
// Shared helpers (static): do_select_common(), do_select_nofds()
//
// Architecture:
//   Both do_select and do_pselect6 share ~70% logic (nfds validation,
//   fd_set copy, pollfd conversion, do_poll_core, reverse mapping,
//   fd_set write-back).  The shared core is do_select_common().
//   do_select_nofds() handles the degenerate nfds==0 case for both.

#include <kernel/select.h>
#include <kernel/poll.h>
#include <kernel/task.h>
#include <kernel/slab.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// ── do_select_nofds — nfds==0 shared path ───────────────────
// Skips fd_set copy, pollfd allocation, and reverse mapping.
// Allocates a minimal poll_table (1 entry) for do_poll_core.
// Returns 0 on success (timeout), -errno on error.

static int64_t do_select_nofds(int64_t ms)
{
    poll_table_t pt;
    if (poll_table_setup(&pt, 1) != 0)
        return -ENOMEM;
    int64_t ret = do_poll_core(NULL, 0, ms, &pt);
    poll_table_destroy(&pt);
    return (ret < 0) ? ret : 0;
}

// ── do_select_common — shared core for do_select / do_pselect6 ──
// Precondition:
//   kr/kw/ke are valid caller-stack variables (never NULL).
//   pfds is a kmalloc'd pollfd array of length nfds.
//   pt has been poll_table_setup'd.
// Does:
//   do_poll_core → reverse mapping → memcpy write-back to user
//   → kfree pfds + poll_table_destroy.
// Returns: ready count (>=0), or -errno (<0).
// On error (<0), fd_sets in user space are NOT modified.

static int64_t do_select_common(int nfds,
    kernel_fd_set *kr, kernel_fd_set *kw, kernel_fd_set *ke,
    struct pollfd *pfds, poll_table_t *pt, int64_t ms,
    void *ur, void *uw, void *ue)
{
    int64_t count = 0;
    int64_t ret = do_poll_core(pfds, nfds, ms, pt);

    if (ret < 0)
        goto out;

    // ── Reverse mapping: pollfd revents → kernel_fd_set ─────
    memset(kr, 0, sizeof(kernel_fd_set));
    memset(kw, 0, sizeof(kernel_fd_set));
    memset(ke, 0, sizeof(kernel_fd_set));
    count = 0;

    for (uint32_t i = 0; i < (uint32_t)nfds; i++) {
        int fd = pfds[i].fd;
        if (fd < 0)
            continue;
        uint32_t r = pfds[i].revents;
        if (r == 0)
            continue;

        bool ready = false;

        if (r & (POLLIN | POLLRDNORM | POLLHUP | POLLERR)) {
            kern_fd_set(fd, kr);
            ready = true;
        }
        if (r & (POLLOUT | POLLWRNORM | POLLERR)) {
            kern_fd_set(fd, kw);
            ready = true;
        }
        if (r & (POLLPRI | POLLERR)) {
            kern_fd_set(fd, ke);
            ready = true;
        }
        if (ready)
            count++;
    }

    // ── Write back to user space (NULL → skip) ──────────────
    if (ur) memcpy(ur, kr, sizeof(kernel_fd_set));
    if (uw) memcpy(uw, kw, sizeof(kernel_fd_set));
    if (ue) memcpy(ue, ke, sizeof(kernel_fd_set));

out:
    kfree(pfds);
    poll_table_destroy(pt);
    return (ret < 0) ? ret : count;
}

// ── do_select — select(2) syscall implementation ─────────────
//
// Linux ABI: int select(int nfds, fd_set *readfds, fd_set *writefds,
//                       fd_set *exceptfds, struct timeval *timeout);
// Returns: ready count, 0 = timeout, -1 = -errno (via trap.c).
//
// Timeout: NULL → block indefinitely; {0,0} → non-blocking;
//          >0 → milliseconds (rounded up from timeval).

int64_t do_select(int nfds, void *readfds, void *writefds,
                  void *exceptfds, void *timeout_tv)
{
    // ── nfds validation ──────────────────────────────────────
    if (nfds < 0 || nfds > FD_SETSIZE)
        return -EINVAL;

    // ── nfds==0 path (before any allocation) ─────────────────
    if (nfds == 0) {
        if (!timeout_tv)
            return -ENOSYS;

        struct timeval ktv;
        if ((uint64_t)timeout_tv + sizeof(ktv) > current->addr_limit)
            return -EFAULT;
        memcpy(&ktv, timeout_tv, sizeof(ktv));
        if (ktv.tv_sec > INT32_MAX / 1000)
            return -EINVAL;
        if (ktv.tv_usec >= 1000000)
            return -EINVAL;
        if (ktv.tv_sec == 0 && ktv.tv_usec == 0)
            return 0;

        int64_t ms = (int64_t)(ktv.tv_sec * 1000
                               + (ktv.tv_usec + 999) / 1000);
        return do_select_nofds(ms);
    }

    // ── Signal check (before poll_table allocation) ──────────
    if (current->signal & ~current->blocked)
        return -EINTR;

    // ── Zero-init kernel fd_sets ─────────────────────────────
    // Prevents random stack bits from leaking into events when
    // a pointer is NULL (the set stays zeroed).
    kernel_fd_set kr = {0}, kw = {0}, ke = {0};

    // ── Copy fd_sets from user space (NULL → skip) ──────────
    if (readfds) {
        if ((uint64_t)readfds + sizeof(kernel_fd_set) > current->addr_limit)
            return -EFAULT;
        memcpy(&kr, readfds, sizeof(kernel_fd_set));
    }
    if (writefds) {
        if ((uint64_t)writefds + sizeof(kernel_fd_set) > current->addr_limit)
            return -EFAULT;
        memcpy(&kw, writefds, sizeof(kernel_fd_set));
    }
    if (exceptfds) {
        if ((uint64_t)exceptfds + sizeof(kernel_fd_set) > current->addr_limit)
            return -EFAULT;
        memcpy(&ke, exceptfds, sizeof(kernel_fd_set));
    }

    // ── Parse timeout ────────────────────────────────────────
    int64_t ms;
    if (!timeout_tv) {
        ms = -1;   // block indefinitely
    } else {
        struct timeval ktv;
        if ((uint64_t)timeout_tv + sizeof(ktv) > current->addr_limit)
            return -EFAULT;
        memcpy(&ktv, timeout_tv, sizeof(ktv));
        if (ktv.tv_sec > INT32_MAX / 1000)
            return -EINVAL;
        if (ktv.tv_usec >= 1000000)
            return -EINVAL;
        ms = (int64_t)(ktv.tv_sec * 1000
                       + (ktv.tv_usec + 999) / 1000);
    }

    // ── Allocate pollfd array ────────────────────────────────
    struct pollfd *pfds = kmalloc(nfds * sizeof(struct pollfd));
    if (!pfds)
        return -ENOMEM;

    // ── Convert fd_set → pollfd ──────────────────────────────
    for (int i = 0; i < nfds; i++) {
        pfds[i].fd      = (int)i;
        pfds[i].events  = 0;
        pfds[i].revents = 0;

        // fd past end of fd table → mark invalid
        if ((uint32_t)i >= NOFILE) {
            pfds[i].fd = -1;
            continue;
        }

        if (kern_fd_isset(i, &kr))
            pfds[i].events |= POLLIN | POLLRDNORM;
        if (kern_fd_isset(i, &kw))
            pfds[i].events |= POLLOUT | POLLWRNORM;
        if (kern_fd_isset(i, &ke))
            pfds[i].events |= POLLPRI;
    }

    // ── Setup poll table ─────────────────────────────────────
    poll_table_t pt;
    if (poll_table_setup(&pt, nfds) != 0) {
        kfree(pfds);
        return -ENOMEM;
    }

    // ── Common path (kfree + poll_table_destroy happen inside) ─
    return do_select_common(nfds, &kr, &kw, &ke, pfds, &pt, ms,
                            readfds, writefds, exceptfds);
}

// ── do_pselect6 — pselect6(2) syscall implementation ────────
//
// Linux ABI:
//   int pselect6(int nfds, fd_set *readfds, fd_set *writefds,
//                fd_set *exceptfds, const struct timespec *timeout,
//                const struct pselect6_sigmask *sigmask_packed);
// Returns: ready count, 0 = timeout, -1 = -errno (via trap.c).
//
// Sigmask handling:
//   If sigmask_packed is non-NULL, the current blocked mask is
//   swapped atomically before the poll and restored on return.
//   ALL error/success paths after the swap go through "out:" to
//   guarantee restore.

int64_t do_pselect6(int nfds, void *readfds, void *writefds,
                    void *exceptfds, void *timeout_ts,
                    const void *sigmask_packed)
{
    // ── nfds validation (before sigmask swap) ────────────────
    if (nfds < 0 || nfds > FD_SETSIZE)
        return -EINVAL;

    // ── nfds==0 path (sigmask swap still needed for atomicity) ──
    if (nfds == 0) {
        if (!timeout_ts)
            return -ENOSYS;

        struct timespec kts;
        if ((uint64_t)timeout_ts + sizeof(kts) > current->addr_limit)
            return -EFAULT;
        memcpy(&kts, timeout_ts, sizeof(kts));
        if (kts.tv_sec > INT32_MAX / 1000)
            return -EINVAL;
        if (kts.tv_nsec >= 1000000000)
            return -EINVAL;
        if (kts.tv_sec == 0 && kts.tv_nsec == 0)
            return 0;

        int64_t ms = (int64_t)(kts.tv_sec * 1000
                               + (kts.tv_nsec + 999999) / 1000000);

        // Unpack + swap signal mask for nfds==0 path
        struct pselect6_sigmask sm;
        sigset_t sigmask_kern_n0 = 0;
        sigset_t *sigmask_ptr_n0 = NULL;
        if (sigmask_packed) {
            if ((uint64_t)sigmask_packed + sizeof(sm) > current->addr_limit)
                return -EFAULT;
            memcpy(&sm, sigmask_packed, sizeof(sm));
            if (sm.ss_len != sizeof(sigset_t))
                return -EINVAL;
            if (sm.ss) {
                if ((uint64_t)sm.ss + sizeof(sigset_t) > current->addr_limit)
                    return -EFAULT;
                memcpy(&sigmask_kern_n0, sm.ss, sizeof(sigset_t));
                sigmask_ptr_n0 = &sigmask_kern_n0;
            }
        }

        uint64_t old_blocked_n0 = 0;
        bool mask_swapped_n0 = false;
        if (sigmask_ptr_n0) {
            old_blocked_n0 = current->blocked;
            current->blocked = *sigmask_ptr_n0;
            mask_swapped_n0 = true;
        }

        int64_t ret = do_select_nofds(ms);

        if (mask_swapped_n0)
            current->blocked = old_blocked_n0;
        return ret;
    }

    // ── Unpack sigmask_packed ────────────────────────────────
    struct pselect6_sigmask sm;
    sigset_t sigmask_kern = 0;
    sigset_t *sigmask_ptr = NULL;

    if (sigmask_packed) {
        if ((uint64_t)sigmask_packed + sizeof(sm) > current->addr_limit)
            return -EFAULT;
        memcpy(&sm, sigmask_packed, sizeof(sm));
        if (sm.ss_len != sizeof(sigset_t))
            return -EINVAL;
        if (sm.ss) {
            if ((uint64_t)sm.ss + sizeof(sigset_t) > current->addr_limit)
                return -EFAULT;
            memcpy(&sigmask_kern, sm.ss, sizeof(sigset_t));
            sigmask_ptr = &sigmask_kern;
        }
    }

    // ── Sigmask swap + goto-out pattern ──────────────────────
    // ALL code paths after this point must go through "out:" to
    // guarantee blocked mask restoration.
    uint64_t old_blocked = 0;
    bool     mask_swapped = false;
    int64_t  ret = 0;
    kernel_fd_set kr = {0}, kw = {0}, ke = {0};

    if (sigmask_ptr) {
        old_blocked = current->blocked;
        current->blocked = *sigmask_ptr;
        mask_swapped = true;
    }

    // ── Signal check (uses new blocked mask) ─────────────────
    if (current->signal & ~current->blocked) {
        ret = -EINTR;
        goto out;
    }

    // ── Parse timeout (NULL → infinite) ──────────────────────
    int64_t ms;
    if (!timeout_ts) {
        ms = -1;
        goto after_timeout;
    }

    struct timespec kts;
    if ((uint64_t)timeout_ts + sizeof(kts) > current->addr_limit) {
        ret = -EFAULT;
        goto out;
    }
    memcpy(&kts, timeout_ts, sizeof(kts));
    if (kts.tv_sec > INT32_MAX / 1000) {
        ret = -EINVAL;
        goto out;
    }
    if (kts.tv_nsec >= 1000000000) {
        ret = -EINVAL;
        goto out;
    }
    ms = (int64_t)(kts.tv_sec * 1000
                   + (kts.tv_nsec + 999999) / 1000000);

after_timeout:
    // ── Copy fd_sets from user space (NULL → skip) ──────────

    if (readfds) {
        if ((uint64_t)readfds + sizeof(kernel_fd_set) > current->addr_limit) {
            ret = -EFAULT;
            goto out;
        }
        memcpy(&kr, readfds, sizeof(kernel_fd_set));
    }
    if (writefds) {
        if ((uint64_t)writefds + sizeof(kernel_fd_set) > current->addr_limit) {
            ret = -EFAULT;
            goto out;
        }
        memcpy(&kw, writefds, sizeof(kernel_fd_set));
    }
    if (exceptfds) {
        if ((uint64_t)exceptfds + sizeof(kernel_fd_set) > current->addr_limit) {
            ret = -EFAULT;
            goto out;
        }
        memcpy(&ke, exceptfds, sizeof(kernel_fd_set));
    }

    // ── Allocate and fill pollfd array ───────────────────────
    struct pollfd *pfds = kmalloc(nfds * sizeof(struct pollfd));
    if (!pfds) {
        ret = -ENOMEM;
        goto out;
    }

    for (int i = 0; i < nfds; i++) {
        pfds[i].fd      = (int)i;
        pfds[i].events  = 0;
        pfds[i].revents = 0;

        // fd past end of fd table → mark invalid
        if ((uint32_t)i >= NOFILE) {
            pfds[i].fd = -1;
            continue;
        }

        if (kern_fd_isset(i, &kr))
            pfds[i].events |= POLLIN | POLLRDNORM;
        if (kern_fd_isset(i, &kw))
            pfds[i].events |= POLLOUT | POLLWRNORM;
        if (kern_fd_isset(i, &ke))
            pfds[i].events |= POLLPRI;
    }

    // ── Setup poll table ─────────────────────────────────────
    poll_table_t pt;
    if (poll_table_setup(&pt, nfds) != 0) {
        kfree(pfds);
        ret = -ENOMEM;
        goto out;
    }

    // ── Common path (cleanup inside do_select_common) ────────
    ret = do_select_common(nfds, &kr, &kw, &ke, pfds, &pt, ms,
                           readfds, writefds, exceptfds);

out:
    if (mask_swapped)
        current->blocked = old_blocked;
    return ret;
}
