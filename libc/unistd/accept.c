#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int64_t ret = syscall(SYS_accept, fd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
