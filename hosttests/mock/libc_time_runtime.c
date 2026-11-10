/*
 * hosttests/mock/libc_time_runtime.c — the syscall boundary of the
 * OS01 time group (T13).
 *
 * This translation unit is deliberately compiled WITHOUT the OS01 libc
 * include directory, so it sees the HOST's <unistd.h> and can use the
 * host's fork().
 *
 * Why it exists: the host test's <unistd.h> is OS01's, and OS01's
 * <sys/syscall.h> carries a raw `fork()` wrapper — the recorded
 * shadowing of the POSIX fork() (see the T1 ledger).  In a host test
 * that wrapper goes through the host_test_syscall() hook, which is weak
 * and undefined unless a mock provides it; without this mock the
 * wrapper answers -ENOSYS and the test cannot start the oracle
 * process.  This mock forwards OS01's SYS_fork to the HOST's fork().
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdint.h>

int64_t host_test_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    if (nr == 11) {   /* OS01 SYS_fork */
        return (int64_t)fork();
    }
    return -38;       /* -ENOSYS */
}
