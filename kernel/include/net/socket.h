// kernel/include/net/socket.h — socket API declarations
#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include <kernel/file.h>
#include <stdint.h>

// Kernel-side byte swap helper (network order -> host order).
// lwIP sockaddr_in.sin_port arrives in network byte order; convert
// to host order before passing to netconn_connect etc.
static inline uint16_t os01_ntohs(uint16_t n) { return __builtin_bswap16(n); }

// socket_t is defined in kernel/file.h

// Socket state constants (shared with poll.c)
#define SOCK_UNCONNECTED  0
#define SOCK_CONNECTED    1
#define SOCK_LISTENING    2
#define SOCK_CLOSED       3

socket_t *socket_alloc(int domain, int type, int protocol);
void      socket_free(socket_t *s);
socket_t *socket_get(int fd);

int64_t do_socket(int domain, int type, int protocol);
int64_t do_connect(int fd, uint32_t ip, uint16_t port);
int64_t do_sendto(int fd, const void *buf, uint64_t len, int flags,
                  uint32_t ip, uint16_t port);
int64_t do_recvfrom(int fd, void *buf, uint64_t len, int flags,
                    uint32_t *out_ip, uint16_t *out_port);
int64_t do_bind(int fd, uint32_t ip, uint16_t port);
int64_t do_listen(int fd, int backlog);
int64_t do_accept(int fd, uint32_t *out_ip, uint16_t *out_port);
int64_t do_setsockopt(int fd, int level, int optname,
                      const void *optval, uint64_t optlen);
int64_t do_getsockname(int fd, void *addr, uint64_t *addrlen);
int64_t do_getsockopt(int fd, int level, int optname,
                      void *optval, uint64_t *optlen);
int64_t do_shutdown(int fd, int how);

#endif
int64_t do_getifaddr(void);
