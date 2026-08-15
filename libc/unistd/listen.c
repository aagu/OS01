#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
int listen(int fd, int backlog) {
    int64_t ret = syscall(SYS_listen, fd, backlog, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
