/* kernel/include/net/lwip.h — Unified lwIP stack adapter and readiness interface */
#ifndef _NET_LWIP_H
#define _NET_LWIP_H

#include <stdint.h>
#include <stdbool.h>

enum net_service_state {
    NET_OFF = 0,
    NET_STARTING,
    NET_ONLINE,
    NET_FAILED,
};

/* Stack readiness: true only when tcpip core is confirmed and >=1 adapter is bound */
bool net_service_ready(void);

/* Default interface IPv4 address in host order (0 if not ready or no interface) */
uint32_t net_default_ipv4(void);

/* Post-SMP network stack bring-up */
void net_lwip_start(void);

/* Query internal service state */
enum net_service_state net_service_get_state(void);

/* Reset state for tests or re-initialization */
void net_lwip_reset_state(void);

#endif /* _NET_LWIP_H */
