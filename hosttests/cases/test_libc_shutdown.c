/*
 * hosttests/cases/test_libc_shutdown.c — OS01 libc completion, shutdown
 * group (T4).
 *
 * The shutdown group checks that shutdown(2) really reaches the kernel:
 *
 *   shutdown_forwards_args  the wrapper issues SYS_shutdown exactly once
 *                           with the fd and the `how` it was given
 *   shutdown_errno          a kernel error becomes -1 with errno set to
 *                           the negated kernel code (EBADF)
 *   shutdown_invalid_how    an invalid `how` is EINVAL and NO syscall is
 *                           issued: the kernel maps every `how` it does
 *                           not know to "neither direction" and would
 *                           accept it silently, so the wrapper is where
 *                           the POSIX check belongs
 *
 * The binary links the REAL production wrapper (libc/unistd/shutdown.c)
 * under the `os01_` prefix, so a call in this file can never resolve to
 * the host libc's same-named implementation.
 *
 * The syscall boundary is the one thing that is replaced: the production
 * wrapper routes every syscall through the `host_test_syscall` hook that
 * <sys/syscall.h> declares for host tests, so the test sees the exact
 * syscall number and arguments.
 */
#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/syscall.h>

/* errno is a plain global in the OS01 libc; the host glibc does not
 * export one, so the test provides it (the production object
 * references it). */
int errno = 0;

/* ── the syscall boundary ─────────────────────────────────────────
 * The production wrapper calls SYS_shutdown through this hook.  The
 * test records what was asked for and answers with whatever the case
 * wants the kernel to answer. */
static uint64_t sys_last_nr;
static uint64_t sys_last_arg1;
static uint64_t sys_last_arg2;
static int      sys_call_count;
static int64_t  sys_answer;

static void sys_reset(int64_t answer)
{
    sys_last_nr = 0;
    sys_last_arg1 = 0;
    sys_last_arg2 = 0;
    sys_call_count = 0;
    sys_answer = answer;
}

int64_t host_test_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg3;
    sys_last_nr = nr;
    sys_last_arg1 = arg1;
    sys_last_arg2 = arg2;
    sys_call_count++;
    return sys_answer;
}

/* ── the cases ────────────────────────────────────────────────────── */

TEST_FUNC(shutdown_forwards_args)
{
    sys_reset(0);

    int rc = shutdown(7, SHUT_WR);
    assert_eq(0, rc);
    assert_eq(0, errno);

    /* exactly one syscall, and it is the shutdown one */
    assert_eq(1, sys_call_count);
    assert_eq(SYS_shutdown, (int)sys_last_nr);
    assert_eq(7, (int)sys_last_arg1);
    assert_eq(SHUT_WR, (int)sys_last_arg2);

    /* the other two directions travel the same way */
    sys_reset(0);
    assert_eq(0, shutdown(3, SHUT_RD));
    assert_eq(1, sys_call_count);
    assert_eq(3, (int)sys_last_arg1);
    assert_eq(SHUT_RD, (int)sys_last_arg2);

    sys_reset(0);
    assert_eq(0, shutdown(4, SHUT_RDWR));
    assert_eq(1, sys_call_count);
    assert_eq(4, (int)sys_last_arg1);
    assert_eq(SHUT_RDWR, (int)sys_last_arg2);
}

TEST_FUNC(shutdown_errno)
{
    /* the kernel's -EBADF (a closed or unknown fd) */
    sys_reset(-EBADF);
    errno = 0;

    int rc = shutdown(9, SHUT_RDWR);
    assert_eq(-1, rc);
    assert_eq(EBADF, errno);
    assert_eq(1, sys_call_count);
    assert_eq(SYS_shutdown, (int)sys_last_nr);
    assert_eq(9, (int)sys_last_arg1);

    /* the kernel's -ENOTCONN (a socket that is not connected) */
    sys_reset(-ENOTCONN);
    errno = 0;
    rc = shutdown(9, SHUT_WR);
    assert_eq(-1, rc);
    assert_eq(ENOTCONN, errno);
}

TEST_FUNC(shutdown_invalid_how)
{
    /* The kernel maps every `how` it does not know to "neither
     * direction" (kernel/net/socket.c: do_shutdown), so an invalid
     * value would be accepted there.  The wrapper rejects it first. */
    sys_reset(0);
    errno = 0;

    int rc = shutdown(5, 99);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    /* no syscall was issued at all */
    assert_eq(0, sys_call_count);

    /* a negative `how` is invalid too */
    errno = 0;
    rc = shutdown(5, -1);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);
    assert_eq(0, sys_call_count);
}

TEST_LIST_BEGIN
    TEST_ENTRY(shutdown_forwards_args),
    TEST_ENTRY(shutdown_errno),
    TEST_ENTRY(shutdown_invalid_how),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
