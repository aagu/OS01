#ifndef _ARPA_INET_H
#define _ARPA_INET_H 1

#include <sys/cdefs.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t in_addr_t;

/* IPv4 address conversion */
int inet_aton(const char *cp, struct in_addr *inp);
char *inet_ntoa(struct in_addr in);
in_addr_t inet_addr(const char *cp);
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#ifdef __cplusplus
}
#endif

#endif
