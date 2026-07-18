#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *addr, socklen_t addrlen) {
    (void)addrlen;
    int64_t ret = syscall6(SYS_sendto, fd, (uint64_t)buf, len,
                           (uint64_t)(int64_t)flags, (uint64_t)addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}
