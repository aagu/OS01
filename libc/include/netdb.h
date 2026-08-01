#ifndef _NETDB_H
#define _NETDB_H 1
#include <sys/cdefs.h>
#include <sys/socket.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int h_errno;
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NAMEREQD    8

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags);

const char *hstrerror(int err);

struct hostent { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };
#define h_addr h_addr_list[0]

struct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };

struct addrinfo {
    int ai_flags; int ai_family; int ai_socktype; int ai_protocol;
    socklen_t ai_addrlen; struct sockaddr *ai_addr;
    char *ai_canonname; struct addrinfo *ai_next;
};

#define AI_CANONNAME  2
#define AI_PASSIVE    1
#define AI_NUMERICHOST 4

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
struct servent *getservbyname(const char *name, const char *proto);
struct hostent *gethostbyname(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _NETDB_H */
