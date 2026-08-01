#include <netdb.h>
#include <string.h>

int h_errno = 0;

const char *hstrerror(int err) { (void)err; return "Unknown host"; }

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
    { (void)node; (void)service; (void)hints; (void)res; return -1; }

void freeaddrinfo(struct addrinfo *res) { (void)res; }

struct servent *getservbyname(const char *name, const char *proto)
    { (void)name; (void)proto; return NULL; }

struct hostent *gethostbyname(const char *name)
    { (void)name; return NULL; }

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
    { (void)sa; (void)salen; (void)host; (void)hostlen;
      (void)serv; (void)servlen; (void)flags; return -1; }
