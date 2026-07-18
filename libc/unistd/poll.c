#include <poll.h>
#include <errno.h>
#include <sys/syscall.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    int64_t ret = syscall(SYS_poll,
                          (uint64_t)fds,
                          (uint64_t)nfds,
                          (uint64_t)(int64_t)timeout);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}
