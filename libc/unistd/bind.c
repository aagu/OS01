#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    (void)addrlen;
    int64_t ret = syscall(SYS_bind, fd, (uint64_t)addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
