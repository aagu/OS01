#include <sys/socket.h>
#include <sys/syscall.h>
#include <errno.h>
int socket(int domain, int type, int protocol) {
    int64_t ret = syscall(SYS_socket, domain, type, protocol);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}
