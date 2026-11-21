/* hosttests/mock/libm_oracle_runtime.c — the host side of the libm oracle.
 *
 * The libm test runs the glibc oracle as a SEPARATE process, so the
 * reference values cannot be influenced by the OS01 implementation.
 *
 * The test translation unit is compiled with the OS01 include paths, and
 * there <sys/syscall.h> shadows the POSIX fork() with the raw syscall
 * wrapper (that is the production libc's own path, and the shadowing is
 * documented in the header).  Under OS01_HOST_TEST that wrapper routes
 * through the `host_test_syscall` hook, so this file provides it: it
 * maps the one syscall number the test needs onto the host's libc.
 *
 * This file is compiled WITHOUT the OS01 include paths on purpose: it
 * must call the HOST's fork(), not the wrapper it implements.
 */

#include <stdint.h>
#include <unistd.h>

/* The OS01 syscall numbers (libc/include/sys/syscall.h).  They are the
 * OS01 ones, not the host's: the wrapper asks for them by number. */
#define OS01_SYS_fork 11

int64_t host_test_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    switch (nr) {
    case OS01_SYS_fork:
        return (int64_t)fork();
    default:
        return -38;  /* -ENOSYS */
    }
}
