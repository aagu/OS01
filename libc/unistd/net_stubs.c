#include <netdb.h>
#include <string.h>

int h_errno = 0;

const char *hstrerror(int err) {
    (void)err;
    return "Unknown host";
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints,
                struct addrinfo **res) {
    (void)node; (void)service; (void)hints; (void)res;
    return -1; /* EAI_NONAME or similar */
}

void freeaddrinfo(struct addrinfo *res) {
    (void)res;
}

struct servent *getservbyname(const char *name, const char *proto) {
    (void)name; (void)proto;
    return NULL;
}

struct hostent *gethostbyname(const char *name) {
    (void)name;
    return NULL;
}

uint16_t htons(uint16_t n) { return n; }
uint16_t ntohs(uint16_t n) { return n; }
uint32_t htonl(uint32_t n) { return n; }
uint32_t ntohl(uint32_t n) { return n; }

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)sockfd; (void)addr; (void)addrlen;
    return -1;
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)sockfd; (void)addr; (void)addrlen;
    return -1;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    (void)sa; (void)salen; (void)host; (void)hostlen;
    (void)serv; (void)servlen; (void)flags;
    return -1;
}
