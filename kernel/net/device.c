/* kernel/net/device.c — Network device registration, polling, and reception */
#include <net/device.h>
#include <device/test_fault.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <core/debug.h>
#include <log/log.h>

#ifndef OS01_HOST_TEST
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#endif

static struct net_device *s_net_devices[NET_DEVICE_MAX];
static int s_net_devices_initialized = 0;

int net_device_init(void)
{
    memset(s_net_devices, 0, sizeof(s_net_devices));
    s_net_devices_initialized = 1;
    return 0;
}

int net_device_register(struct net_device *dev)
{
    if (!dev) {
        return -EINVAL;
    }

    if (!dev->ops) {
        return -EINVAL;
    }

    if (!dev->ops->xmit || !dev->ops->poll_rx || !dev->ops->get_link || !dev->ops->stop) {
        return -EINVAL;
    }

    /* adapter-fail fault: reject every adapter registration. */
    if (arch9_fault_should_inject_adapter_fail()) {
        return -EINVAL;
    }

#ifdef OS01_HOST_TEST
    extern bool s_inject_net_register_fail __attribute__((weak));
    if (&s_inject_net_register_fail && s_inject_net_register_fail) {
        return -ENOMEM;
    }
#endif

    /* Validate MAC: must not be all zeros */
    uint8_t zero_mac[6] = {0, 0, 0, 0, 0, 0};
    if (memcmp(dev->mac, zero_mac, 6) == 0) {
        return -EINVAL;
    }

    if (!s_net_devices_initialized) {
        net_device_init();
    }

    /* Reject duplicate registration */
    for (int i = 0; i < NET_DEVICE_MAX; i++) {
        if (s_net_devices[i] == dev) {
            return -EEXIST;
        }
    }

    /* Find first free slot */
    int slot = -1;
    for (int i = 0; i < NET_DEVICE_MAX; i++) {
        if (!s_net_devices[i]) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        log_warn("net: maximum devices (%d) reached\n", NET_DEVICE_MAX);
        return -ENOSPC;
    }

    /* Assign sequential eth name based on slot */
    snprintf(dev->name, sizeof(dev->name), "eth%d", slot);

    if (dev->mtu == 0) {
        dev->mtu = 1500;
    }

    dev->present = 1;
    dev->last_error = 0;
    dev->link_up = dev->ops->get_link(dev);
    dev->adapter = NULL;

    s_net_devices[slot] = dev;

    log_info("net: registered %s (MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s)\n",
             dev->name,
             dev->mac[0], dev->mac[1], dev->mac[2],
             dev->mac[3], dev->mac[4], dev->mac[5],
             dev->link_up ? "up" : "down");
    /* ARCH-9 Task 11: per-BDF observation.  Drivers attribute the
     * adapter_registrations counter to their device's BDF (see
     * kernel/driver/e1000.c and kernel/driver/virtio-net.c), so this
     * site is intentionally a no-op for observation. */
    return 0;
}

int net_device_unregister_boot(struct net_device *dev)
{
    if (!dev || !dev->present) {
        return -EINVAL;
    }

    for (int i = 0; i < NET_DEVICE_MAX; i++) {
        if (s_net_devices[i] == dev) {
            s_net_devices[i] = NULL;
            dev->present = 0;
            dev->adapter = NULL;
            log_info("net: unregistered %s during boot\n", dev->name);
            return 0;
        }
    }

    return -EINVAL;
}

struct net_device *net_device_get(unsigned index)
{
    unsigned current = 0;
    for (int i = 0; i < NET_DEVICE_MAX; i++) {
        if (s_net_devices[i]) {
            if (current == index) {
                return s_net_devices[i];
            }
            current++;
        }
    }
    return NULL;
}

unsigned net_device_count(void)
{
    unsigned count = 0;
    for (int i = 0; i < NET_DEVICE_MAX; i++) {
        if (s_net_devices[i]) {
            count++;
        }
    }
    return count;
}

void net_device_poll_all(void)
{
    for (int i = 0; i < NET_DEVICE_MAX; i++) {
        struct net_device *dev = s_net_devices[i];
        if (dev && dev->present && dev->ops && dev->ops->poll_rx) {
            dev->ops->poll_rx(dev, NET_DEVICE_POLL_BUDGET);
        }
    }
}

int net_receive(struct net_device *dev, struct pbuf *p)
{
    if (!dev || !p) {
        return -EINVAL;
    }

    struct netif *nif = (struct netif *)dev->adapter;
    if (!nif) {
        return -ENETDOWN;
    }

    /* Pass packet directly to unified adapter's ethernet_input in current thread */
    err_t err = ethernet_input(p, nif);
    if (err != ERR_OK) {
        /* lwIP ethernet_input frees pbuf on drop/error; adapter took ownership */
        return 0;
    }

    return 0;
}
