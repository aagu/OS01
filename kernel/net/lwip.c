/* kernel/net/lwip.c — Unified lwIP stack adapter and readiness tracker */
#include <net/device.h>
#include <net/lwip.h>
#include <core/debug.h>
#include <log/log.h>
#include <errno.h>
#include <string.h>

#ifndef OS01_HOST_TEST
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"
#include "lwip/etharp.h"
#endif

struct net_adapter {
    struct netif netif;
    struct net_device *ndev;
    bool active;
};

static struct net_adapter s_adapters[NET_DEVICE_MAX];
static enum net_service_state s_net_service_state = NET_OFF;
static bool s_tcpip_core_ready = false;
static bool s_adapters_finished = false;
static unsigned s_active_adapters = 0;
static struct netif *s_default_netif = NULL;

static err_t net_adapter_linkoutput(struct netif *netif, struct pbuf *p)
{
    if (!netif) {
        return ERR_IF;
    }
    struct net_device *ndev = (struct net_device *)netif->state;
    if (!ndev || !ndev->ops || !ndev->ops->xmit) {
        return ERR_IF;
    }
    int rc = ndev->ops->xmit(ndev, p);
    return (rc == 0) ? ERR_OK : ERR_IF;
}

static err_t net_adapter_init(struct netif *netif)
{
    struct net_device *ndev = (struct net_device *)netif->state;
    if (!ndev) {
        return ERR_ARG;
    }

    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, ndev->mac, 6);
    netif->mtu = ndev->mtu ? ndev->mtu : 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->linkoutput = net_adapter_linkoutput;
    netif->output = etharp_output;
    return ERR_OK;
}

static void net_check_and_publish_online(void)
{
    if (s_net_service_state == NET_STARTING) {
        if (s_tcpip_core_ready && s_adapters_finished) {
            if (s_active_adapters > 0) {
                __atomic_store_n(&s_net_service_state, NET_ONLINE, __ATOMIC_RELEASE);
                log_info("net: stack online with %u active adapter(s)\n", s_active_adapters);
            } else {
                __atomic_store_n(&s_net_service_state, NET_FAILED, __ATOMIC_RELEASE);
                log_warn("net: all adapters failed, stack entering FAILED state\n");
            }
        }
    }
}

static void tcpip_init_done_cb(void *arg)
{
    (void)arg;
    s_tcpip_core_ready = true;
    net_check_and_publish_online();
}

bool net_service_ready(void)
{
    return __atomic_load_n(&s_net_service_state, __ATOMIC_ACQUIRE) == NET_ONLINE;
}

enum net_service_state net_service_get_state(void)
{
    return __atomic_load_n(&s_net_service_state, __ATOMIC_ACQUIRE);
}

uint32_t net_default_ipv4(void)
{
    if (!net_service_ready()) {
        return 0;
    }
    if (!s_default_netif) {
        return 0;
    }
    return ip4_addr_get_u32(netif_ip4_addr(s_default_netif));
}

void net_lwip_reset_state(void)
{
    s_net_service_state = NET_OFF;
    s_tcpip_core_ready = false;
    s_adapters_finished = false;
    s_active_adapters = 0;
    s_default_netif = NULL;
    memset(s_adapters, 0, sizeof(s_adapters));
}

void net_lwip_start(void)
{
    if (s_net_service_state != NET_OFF) {
        return;
    }

    unsigned count = net_device_count();
    if (count == 0) {
        s_net_service_state = NET_OFF;
        log_info("net: no network devices registered, skipping stack start\n");
        return;
    }

    s_net_service_state = NET_STARTING;
    s_tcpip_core_ready = false;
    s_adapters_finished = false;
    s_active_adapters = 0;
    s_default_netif = NULL;

    tcpip_init(tcpip_init_done_cb, NULL);

    for (unsigned i = 0; i < count; i++) {
        struct net_device *ndev = net_device_get(i);
        if (!ndev) {
            continue;
        }

        struct net_adapter *adapter = &s_adapters[i];
        memset(adapter, 0, sizeof(*adapter));
        adapter->ndev = ndev;

        ip4_addr_t ip, mask, gw;
        if (i == 0) {
            /* Default interface (eth0): static fallback 10.0.2.15/24 gw 10.0.2.2 */
            IP4_ADDR(&ip,   10, 0, 2, 15);
            IP4_ADDR(&mask, 255, 255, 255, 0);
            IP4_ADDR(&gw,   10, 0, 2, 2);
        } else {
            /* Subsequent interfaces start with 0 and use independent DHCP */
            ip4_addr_set_zero(&ip);
            ip4_addr_set_zero(&mask);
            ip4_addr_set_zero(&gw);
        }

        struct netif *nif = &adapter->netif;
        if (!netif_add(nif, &ip, &mask, &gw, ndev, net_adapter_init, ethernet_input)) {
            log_warn("net: netif_add failed for %s\n", ndev->name);
            continue;
        }

        if (s_active_adapters == 0) {
            netif_set_default(nif);
            s_default_netif = nif;
        }

        netif_set_up(nif);

        if (i == 0) {
            netif_set_addr(nif, &ip, &mask, &gw);
            if (ndev->link_up) {
                netif_set_link_up(nif);
            }
        } else {
            if (ndev->link_up) {
                netif_set_link_up(nif);
            }
        }

        dhcp_start(nif);

        ndev->adapter = nif;
        adapter->active = true;
        s_active_adapters++;
    }

    s_adapters_finished = true;
    net_check_and_publish_online();
}
