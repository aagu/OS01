#ifndef _KERNEL_SELECT_H
#define _KERNEL_SELECT_H

#include <stdint.h>
#include <stddef.h>

// ── Select limits ─────────────────────────────────────────

#define FD_SETSIZE  1024

// ── Signal mask (formerly local to trap.c:42) ─────────────
// Moved here so both trap.c and select.c can use it.
// Matches libc/include/signal.h layout on x86_64 LP64.

typedef unsigned long sigset_t;

// ── pselect6 packed sigmask ───────────────────────────────
// Linux ABI passes {sigmask, sigsetsize} as a packed struct
// in the 6th syscall argument.  void* avoids <signal.h>
// dependency in this header.

struct pselect6_sigmask {
    const void *ss;       // const sigset_t * (void* for kernel use)
    size_t      ss_len;   // must equal sizeof(sigset_t) = 8
};

// ── Kernel-side fd_set ────────────────────────────────────
// 128 bytes, same layout as libc fd_set {long __fds_bits[16]}
// on x86_64 LP64.  uint64_t avoids long-vs-long long ambiguity.

typedef struct {
    uint64_t __bits[16];   // 1024 bits = FD_SETSIZE
} kernel_fd_set;

// ── kern_fd_* inline helpers ──────────────────────────────
// Bounds-checked via (uint32_t)fd < FD_SETSIZE.

static inline void kern_fd_zero(kernel_fd_set *set)
{
    for (int i = 0; i < 16; i++)
        set->__bits[i] = 0;
}

static inline void kern_fd_set(int fd, kernel_fd_set *set)
{
    if ((uint32_t)fd < FD_SETSIZE)
        set->__bits[fd / 64] |= (1ULL << (fd % 64));
}

static inline void kern_fd_clr(int fd, kernel_fd_set *set)
{
    if ((uint32_t)fd < FD_SETSIZE)
        set->__bits[fd / 64] &= ~(1ULL << (fd % 64));
}

static inline int kern_fd_isset(int fd, const kernel_fd_set *set)
{
    return (uint32_t)fd < FD_SETSIZE
        && !!(set->__bits[fd / 64] & (1ULL << (fd % 64)));
}

// ── API prototypes ────────────────────────────────────────
// User-space pointers are void* (matching trap.c convention).
// do_select_common is static in select.c — not exposed here.

int64_t do_select(int nfds, void *readfds, void *writefds,
                  void *exceptfds, void *timeout_tv);

int64_t do_pselect6(int nfds, void *readfds, void *writefds,
                    void *exceptfds, void *timeout_ts,
                    const void *sigmask_packed);

#endif // _KERNEL_SELECT_H
