#include <sys/socket.h>
#include <errno.h>

int socket(int domain, int type, int protocol)
    { (void)domain; (void)type; (void)protocol; errno = ENOSYS; return -1; }

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
    { (void)sockfd; (void)addr; (void)addrlen; errno = ENOSYS; return -1; }

int listen(int sockfd, int backlog)
    { (void)sockfd; (void)backlog; errno = ENOSYS; return -1; }

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest, socklen_t addrlen)
    { (void)sockfd; (void)buf; (void)len; (void)flags;
      (void)dest; (void)addrlen; errno = ENOSYS; return -1; }

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    { (void)sockfd; (void)addr; (void)addrlen; errno = ENOSYS; return -1; }

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
    { (void)sockfd; (void)addr; (void)addrlen; errno = ENOSYS; return -1; }
