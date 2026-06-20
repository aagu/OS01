#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H 1

#include <sys/cdefs.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Socket types */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5

/* Address families */
#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_INET         2
#define AF_INET6        10

/* Protocol families */
#define PF_UNSPEC      AF_UNSPEC
#define PF_UNIX        AF_UNIX
#define PF_INET        AF_INET

/* Socket options */
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_BROADCAST   6
#define SO_KEEPALIVE   9

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

/* in_addr needed by sockaddr_in; defined here to avoid
   circular dependency with netinet/in.h */
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    unsigned short  sin_family;
    unsigned short  sin_port;
    struct in_addr  sin_addr;
    unsigned char   sin_zero[8];
};

int  socket(int domain, int type, int protocol);
int  bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int  listen(int sockfd, int backlog);
int  accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int  connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
int  setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
int  getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t addrlen);
int  getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int  getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

#ifdef __cplusplus
}
#endif

#endif
