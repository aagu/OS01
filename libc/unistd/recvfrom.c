#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *addr, socklen_t *addrlen) {
    int64_t ret = syscall6(SYS_recvfrom, fd, (uint64_t)buf, len,
                           (uint64_t)(int64_t)flags, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}
