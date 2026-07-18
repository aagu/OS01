#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
int getsockopt(int fd, int level, int optname,
               void *optval, socklen_t *optlen) {
    int64_t ret = syscall6(SYS_getsockopt, fd, level, optname,
                           (uint64_t)optval, (uint64_t)optlen, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
