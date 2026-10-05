// kernel/net/net.c — Thin shim over the unified ARCH-9 net layer.
//
// Pre-ARCH-9 this file owned:
//   - net_hw_init(): PCI scan, vendor match, single-instance init of
//     e1000 or virtio-net into one global `struct netif os01_netif`.
//   - net_poll_rx(): dispatch the single instance's RX into the
//     single netif (tcpip-thread context).
//   - net_lwip_init(): tcpip_init + netif_add with the single legacy
//     netif_init callback, then a static 10.0.2.15 fallback.
//
// ARCH-9 collapses all of the above into the device/lwip core
// (kernel/net/{device,lwip}.c).  The PCI drivers (kernel/driver/
// {e1000,virtio-net}.c) register their own `struct net_device`
// instances through the coordinator in kernel/device/boot.c, and the
// unified lwIP adapter publishes ONLINE state via net_service_ready().
//
// This file now exists as a translation-unit shim so existing call
// sites (kernel/core/main.c) keep their original symbol names.  The
// bodies are one-liners that forward to the new core APIs.

#include <net/net.h>
#include <net/device.h>
#include <net/lwip.h>

/* RX dispatch called from the tcpip thread (see kernel/net/sys_arch.c
 * sys_arch_mbox_fetch).  Sweeps every registered card with the agreed
 * 64-packet budget.  Replaces the legacy is_virtio / e1000_poll_rx
 * dispatch. */
void net_poll_rx(void)
{
    net_device_poll_all();
}

/* Bring up the unified lwIP stack.  Replaces the legacy
 * tcpip_init + netif_add + DHCP sequence; the unified adapter code
 * in kernel/net/lwip.c handles every registered net_device. */
void net_lwip_init(void)
{
    net_lwip_start();
}