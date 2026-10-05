/* kernel/include/net/device.h — Network device abstraction and registry */
#ifndef _NET_DEVICE_H
#define _NET_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NET_DEVICE_NAME_MAX 16
#define NET_DEVICE_MAX 8
#define NET_DEVICE_POLL_BUDGET 64

struct net_device;
struct pbuf;
struct device;

/* Operations table for a network device driver */
struct net_device_ops {
    int (*xmit)(struct net_device *dev, struct pbuf *p);
    unsigned (*poll_rx)(struct net_device *dev, unsigned budget);
    bool (*get_link)(struct net_device *dev);
    int (*stop)(struct net_device *dev); /* returns 0, negative errno, or DEVICE_UNSAFE */
};

/* Network device structure */
struct net_device {
    char name[NET_DEVICE_NAME_MAX];
    uint8_t mac[6];
    uint16_t mtu;
    bool link_up;
    struct device *parent;
    const struct net_device_ops *ops;
    void *priv;
    void *adapter;
    int present;
    int last_error;
};

/* Device registry and polling */
int net_device_init(void);
int net_device_register(struct net_device *dev);
int net_device_unregister_boot(struct net_device *dev);
struct net_device *net_device_get(unsigned index);
unsigned net_device_count(void);
void net_device_poll_all(void);

/* Packet reception: deliver to unified lwIP adapter */
int net_receive(struct net_device *dev, struct pbuf *p);

#endif /* _NET_DEVICE_H */
