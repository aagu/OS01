#include <sys/socket.h>
#include <sys/types.h>
ssize_t recv(int fd, void *buf, size_t len, int flags) {
    return recvfrom(fd, buf, len, flags, NULL, 0);
}
