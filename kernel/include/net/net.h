// kernel/include/net/net.h — network subsystem interface
#ifndef _NET_NET_H
#define _NET_NET_H

// Stage A: Phase 6 subsys — hardware init only (PCI probe, BAR map, IRQ register)
int net_hw_init(void);

// Stage B: Post-SMP, pre-task_init — lwIP stack init + tcpip_thread creation
void net_lwip_init(void);

#endif // _NET_NET_H