/* hosttests/mock/arch9/net_runtime.h — mock environment for net_device and lwIP adapter tests */
#ifndef ARCH9_NET_RUNTIME_H
#define ARCH9_NET_RUNTIME_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "test_platform.h"

/* Guard arch/spinlock.h so kernel code that does
 * `#include <arch/spinlock.h>` (in OS01_HOST_TEST builds) does NOT
 * pull in the x86_64 asm-based spin_lock / spin_lock_irqsave
 * definitions that test_platform.h already replaced with host stubs. */
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H 1
#endif
#ifndef _ARCH_X86_64_SPINLOCK_H
#define _ARCH_X86_64_SPINLOCK_H 1
#endif
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
#endif
#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H
#endif

#ifndef debug_block
#define debug_block(...) do {} while (0)
#endif
#ifndef debug_net
#define debug_net(...) do {} while (0)
#endif
#ifndef log_info
#define log_info(...) do {} while (0)
#endif
#ifndef log_warn
#define log_warn(...) do {} while (0)
#endif
#ifndef log_err
#define log_err(...) do {} while (0)
#endif
#ifndef log_debug
#define log_debug(...) do {} while (0)
#endif

/* Intercept lwIP headers so mock lwIP definitions are used in hosttests */
#define LWIP_HDR_TCPIP_H
#define LWIP_HDR_NETIF_H
#define LWIP_HDR_DHCP_H
#define LWIP_HDR_NETIF_ETHERNET_H
#define LWIP_HDR_NETIF_ETHARP_H
#define LWIP_HDR_PBUF_H
#define LWIP_HDR_IP4_ADDR_H
#define LWIP_HDR_ERR_H
#define LWIP_HDR_OPT_H

typedef int8_t err_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_IF -11
#define ERR_ARG -16

typedef struct ip4_addr {
    uint32_t addr;
} ip4_addr_t;

#define IP4_ADDR(ipaddr, a,b,c,d) \
    (ipaddr)->addr = ((uint32_t)((a) & 0xff) | \
                     ((uint32_t)((b) & 0xff) << 8) | \
                     ((uint32_t)((c) & 0xff) << 16) | \
                     ((uint32_t)((d) & 0xff) << 24))

#define ip4_addr_set_zero(ipaddr) ((ipaddr)->addr = 0)
#define ip4_addr_set_u32(ipaddr, val) ((ipaddr)->addr = (uint32_t)(val))
#define ip4_addr_get_u32(ipaddr) ((ipaddr)->addr)
#define netif_ip4_addr(netif) (&((netif)->ip_addr))

struct pbuf {
    struct pbuf *next;
    void *payload;
    uint16_t tot_len;
    uint16_t len;
    uint8_t type;
    uint8_t flags;
    uint16_t ref;
};

struct netif;
typedef err_t (*netif_init_fn)(struct netif *netif);
typedef err_t (*netif_input_fn)(struct pbuf *p, struct netif *inp);
typedef err_t (*netif_linkoutput_fn)(struct netif *netif, struct pbuf *p);
typedef err_t (*netif_output_fn)(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr);

struct netif {
    struct netif *next;
    ip4_addr_t ip_addr;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    netif_input_fn input;
    netif_output_fn output;
    netif_linkoutput_fn linkoutput;
    void *state;
    uint16_t mtu;
    uint8_t hwaddr[6];
    uint8_t hwaddr_len;
    uint8_t flags;
    char name[2];
    uint8_t num;
};

#define NETIF_FLAG_UP 0x01U
#define NETIF_FLAG_BROADCAST 0x02U
#define NETIF_FLAG_LINK_UP 0x04U
#define NETIF_FLAG_ETHARP 0x08U
#define NETIF_FLAG_ETHERNET 0x10U
#define NETIF_FLAG_IGMP 0x20U

typedef void (*tcpip_init_done_fn)(void *arg);

/* Fake tracking and controls */
extern unsigned fake_poll_budget;
extern int fake_tcpip_init_calls;
extern tcpip_init_done_fn fake_tcpip_done_cb;
extern void *fake_tcpip_done_arg;
extern bool fake_tcpip_auto_callback;

extern int fake_netif_add_calls;
extern bool fake_netif_add_fail_all;

extern int fake_dhcp_start_calls;
extern struct netif *fake_dhcp_last_netif;

extern int fake_ethernet_input_calls;
extern struct pbuf *fake_last_rx_pbuf;
extern struct netif *fake_last_rx_netif;

extern int fake_tcpip_input_calls;
extern int fake_etharp_output_calls;

extern int fake_pbuf_free_calls;

/* Task 9: core mailbox / sweep observation counters */
extern int fake_core_mailbox_msg_count;
extern int fake_app_mailbox_msg_count;
extern int fake_core_fetch_rx_sweeps;
extern int fake_core_mailbox_lock_check;
extern int fake_pci_drivers_count;

/* Task 9: mailbox lock-held observation.  The fixture's
 * test_core_fetch_one (kernel/net/lwip.c, OS01_HOST_TEST) acquires
 * the test_core_mbox_lock only around the pop step, matching the
 * production sys_arch_mbox_fetch's mb->lock discipline.  We
 * redefine spin_lock_irqsave / spin_unlock_irqrestore below so
 * that ANY holder of any spinlock_T via these macros flips
 * fake_mailbox_lock_held — but the brief's invariant is verified
 * by the production fixture's own discipline, not by which lock
 * is being held.  A holder of THIS lock (test_core_mbox_lock)
 * during the sweep would set the counter to 1 and fail
 * test_bounded_rx_and_no_lock's assertion.
 *
 * Other host test fixtures (e1000/virtio_runtime.h) redefine
 * spin_lock_irqsave AFTER including net_runtime.h, so their
 * overrides take effect for their own test cases — they do not
 * care about fake_mailbox_lock_held. */
extern int fake_mailbox_lock_held;

static inline uint64_t net_test_spin_lock_irqsave(spinlock_T *l)
{
    (void)l;
    fake_mailbox_lock_held = 1;
    return 0;
}

static inline void net_test_spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{
    (void)l; (void)f;
    fake_mailbox_lock_held = 0;
}

#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#define spin_lock_irqsave(l)    net_test_spin_lock_irqsave(l)
#define spin_unlock_irqrestore(l, f) net_test_spin_unlock_irqrestore(l, f)

/* Mock functions matching lwIP signatures */
static inline void tcpip_init(tcpip_init_done_fn initfunc, void *arg)
{
    fake_tcpip_init_calls++;
    fake_tcpip_done_cb = initfunc;
    fake_tcpip_done_arg = arg;
    if (fake_tcpip_auto_callback && initfunc) {
        initfunc(arg);
    }
}

static inline struct netif *netif_add(struct netif *netif,
                                      const ip4_addr_t *ipaddr,
                                      const ip4_addr_t *netmask,
                                      const ip4_addr_t *gw,
                                      void *state,
                                      netif_init_fn init,
                                      netif_input_fn input)
{
    fake_netif_add_calls++;
    if (fake_netif_add_fail_all) {
        return NULL;
    }
    netif->state = state;
    netif->input = input;
    if (ipaddr) netif->ip_addr = *ipaddr;
    if (netmask) netif->netmask = *netmask;
    if (gw) netif->gw = *gw;
    if (init) {
        if (init(netif) != ERR_OK) {
            return NULL;
        }
    }
    return netif;
}

static inline void netif_set_default(struct netif *netif)
{
    (void)netif;
}

static inline void netif_set_up(struct netif *netif)
{
    if (netif) netif->flags |= NETIF_FLAG_UP;
}

static inline void netif_set_link_up(struct netif *netif)
{
    if (netif) netif->flags |= NETIF_FLAG_LINK_UP;
}

static inline void netif_set_addr(struct netif *netif,
                                  const ip4_addr_t *ipaddr,
                                  const ip4_addr_t *netmask,
                                  const ip4_addr_t *gw)
{
    if (!netif) return;
    if (ipaddr) netif->ip_addr = *ipaddr;
    if (netmask) netif->netmask = *netmask;
    if (gw) netif->gw = *gw;
}

static inline err_t dhcp_start(struct netif *netif)
{
    fake_dhcp_start_calls++;
    fake_dhcp_last_netif = netif;
    return ERR_OK;
}

err_t ethernet_input(struct pbuf *p, struct netif *netif);
err_t tcpip_input(struct pbuf *p, struct netif *inp);
err_t etharp_output(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr);

static inline struct pbuf *fake_pbuf_alloc(uint16_t len)
{
    struct pbuf *p = (struct pbuf *)calloc(1, sizeof(struct pbuf) + len);
    if (!p) return NULL;
    p->payload = (void *)(p + 1);
    p->len = len;
    p->tot_len = len;
    p->ref = 1;
    return p;
}

static inline uint8_t pbuf_free(struct pbuf *p)
{
    if (!p) return 0;
    fake_pbuf_free_calls++;
    if (p->ref > 0) p->ref--;
    if (p->ref == 0) {
        free(p);
        return 1;
    }
    return 0;
}

static inline void fake_net_runtime_reset(void)
{
    fake_poll_budget = 0;
    fake_tcpip_init_calls = 0;
    fake_tcpip_done_cb = NULL;
    fake_tcpip_done_arg = NULL;
    fake_tcpip_auto_callback = true;
    fake_netif_add_calls = 0;
    fake_netif_add_fail_all = false;
    fake_dhcp_start_calls = 0;
    fake_dhcp_last_netif = NULL;
    fake_ethernet_input_calls = 0;
    fake_last_rx_pbuf = NULL;
    fake_last_rx_netif = NULL;
    fake_tcpip_input_calls = 0;
    fake_etharp_output_calls = 0;
    fake_pbuf_free_calls = 0;
    fake_mailbox_lock_held = 0;
}

#endif /* ARCH9_NET_RUNTIME_H */
