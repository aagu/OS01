// kernel/include/net/net.h — network subsystem interface (ARCH-9 shim)
#ifndef _NET_NET_H
#define _NET_NET_H

// Stage B: Post-SMP, pre-task_init — lwIP stack init + tcpip_thread creation
void net_lwip_init(void);
void net_poll_rx(void);

#endif // _NET_NET_H