// kernel/include/uapi/sockaddr.h — shared struct sockaddr_in (kernel + userspace)
#ifndef _UAPI_SOCKADDR_H
#define _UAPI_SOCKADDR_H

#include <stdint.h>

#define AF_INET     2

struct sockaddr_in {
    uint16_t sin_family;   // AF_INET
    uint16_t sin_port;     // network byte order
    uint32_t sin_addr;     // 4-byte IPv4 address (network byte order)
    uint8_t  sin_zero[8];  // padding to sizeof(struct sockaddr)
} __attribute__((packed));

#endif
