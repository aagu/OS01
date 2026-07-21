#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

int h_errno = 0;

const char *hstrerror(int err) {
    (void)err;
    return "Unknown host";
}

/* getaddrinfo / freeaddrinfo / inet_aton are in libc/network/getaddrinfo.c */

/* gethostbyname stub — busybox wget uses getaddrinfo() */
struct hostent *gethostbyname(const char *name) {
    (void)name;
    return NULL;
}

struct servent *getservbyname(const char *name, const char *proto) {
    (void)name; (void)proto;
    return NULL;
}

uint16_t htons(uint16_t n) { return __builtin_bswap16(n); }
uint16_t ntohs(uint16_t n) { return __builtin_bswap16(n); }
uint32_t htonl(uint32_t n) { return __builtin_bswap32(n); }
uint32_t ntohl(uint32_t n) { return __builtin_bswap32(n); }

#include <sys/syscall.h>

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)addrlen;
    int64_t ret = syscall(SYS_getsockname, sockfd, (uint64_t)addr, (uint64_t)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)sockfd; (void)addr; (void)addrlen;
    return -1;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)domain; (void)type; (void)protocol; (void)sv;
    errno = ENOSYS; return -1;
}

int shutdown(int sockfd, int how) {
    (void)sockfd; (void)how;
    errno = ENOSYS; return -1;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)sa; (void)salen; (void)host; (void)hostlen;
    (void)serv; (void)servlen; (void)flags;
    return -1;
}
