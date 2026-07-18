// kernel/net/lwipopts.h — OS01 lwIP compile-time configuration
#ifndef LWIP_OPTS_H
#define LWIP_OPTS_H

#define NO_SYS                  0   // OS mode
#define MEM_LIBC_MALLOC         1   // use OS01 kmalloc() instead of internal heap
#define LWIP_NETCONN            1   // netconn API (needed for socket layer)
#define LWIP_SOCKET             0   // don't use lwIP's own socket layer
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_DHCP               1
#define LWIP_DNS                1
#define LWIP_ARP                1
#define LWIP_ICMP               1
#define LWIP_RAW                0
#define MEM_ALIGNMENT           8
// MEM_SIZE not needed — MEM_LIBC_MALLOC uses OS01 kmalloc()
#define MEMP_NUM_TCP_SEG        32
#define MEMP_NUM_TCP_PCB        8
#define MEMP_NUM_UDP_PCB        4
#define MEMP_NUM_NETCONN        12
#define PBUF_POOL_SIZE          16
#define TCP_MSS                 1460
#define TCP_SND_BUF             (8 * TCP_MSS)
#define TCP_WND                 (8 * TCP_MSS)
// LWIP_NETIF_HOSTNAME: 0 = disable hostname field (avoids DNS code path
// that tries to access netif->hostname)
#define LWIP_NETIF_HOSTNAME     0

// Prevent lwIP from including OS userspace headers (<unistd.h> etc.)
// that would conflict with kernel headers (struct timeval, etc.)
#define LWIP_NO_UNISTD_H        1

#endif // LWIP_OPTS_H