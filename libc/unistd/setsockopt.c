#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
int setsockopt(int fd, int level, int optname,
               const void *optval, socklen_t optlen) {
    int64_t ret = syscall6(SYS_setsockopt, fd, level, optname, (uint64_t)optval, optlen, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
