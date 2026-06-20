#ifndef _NETINET_IN_H
#define _NETINET_IN_H 1

#include <sys/cdefs.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* INADDR_* constants */
#define INADDR_ANY      ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)

#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_RAW     255

/* IPv6 socket address */
struct sockaddr_in6 {
    unsigned short  sin6_family;
    unsigned short  sin6_port;
    unsigned long   sin6_flowinfo;
    unsigned char   sin6_addr[16];
    unsigned long   sin6_scope_id;
};

#ifdef __cplusplus
}
#endif

#endif
