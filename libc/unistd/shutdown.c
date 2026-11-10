/* libc/unistd/shutdown.c — shutdown(2) connected to the kernel.
 *
 * The kernel already implements SYS_shutdown (SYS_shutdown == 64,
 * kernel/net/socket.c: do_shutdown()); what was missing was the libc
 * wrapper: net_stubs.c used to answer ENOSYS, so busybox wget's
 * shutdown(fd, SHUT_WR) after sending the request never reached the
 * kernel.
 *
 * `how` is validated HERE and not in the kernel: do_shutdown() maps
 * every value it does not know to "neither direction", so an invalid
 * `how` would silently succeed.  POSIX says shutdown() fails with
 * EINVAL for an invalid `how`, and the syscall ABI is not allowed to
 * change, so the check belongs in the wrapper.
 */
#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>

int shutdown(int sockfd, int how)
{
#if defined(__is_libk)
    /* The kernel-side libc cannot issue a user syscall.  Fail
     * explicitly instead of executing one. */
    (void)sockfd;
    (void)how;
    return -1;
#else
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
        errno = EINVAL;
        return -1;
    }
    int64_t ret = syscall(SYS_shutdown, (uint64_t)sockfd, (uint64_t)how, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
#endif
}
