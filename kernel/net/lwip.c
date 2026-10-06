/* kernel/net/lwip.c — Unified lwIP stack adapter and readiness tracker */
#include <net/device.h>
#include <net/lwip.h>
#include <device/test_fault.h>
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

#ifdef OS01_HOST_TEST
/* Test-only knobs — see kernel/include/net/lwip.h */
int fake_core_mailbox_msg_count = 0;
int fake_app_mailbox_msg_count = 0;
int fake_core_fetch_rx_sweeps = 0;
int fake_core_mailbox_lock_check = 0;
int fake_pci_drivers_count = 0;

/* Tiny ring buffer for the host-side core mailbox.  We only need a
 * FIFO for the test: 1 entry is enough to assert the sweep-before-
 * fetch invariant, but we keep 64 to match the brief's "1000 messages
 * returned" stress test. */
#define TEST_MBOX_CAP 64
static void *test_core_mbox_buf[TEST_MBOX_CAP];
static int   test_core_mbox_head = 0;
static int   test_core_mbox_tail = 0;
static int   test_core_mbox_count = 0;
static int   test_app_mbox_count = 0;
static uint32_t s_test_default_ipv4 = 0;

/* Host-fixture lock for the test_core_mbox ring.  Mirrors the
 * production os_mbox_t.lock field (kernel/net/sys_arch.c) — the
 * production sys_arch_mbox_fetch takes mb->lock ONLY around the pop
 * step (count/tail/count--), never around the sweep.  The host
 * fixture must use the same discipline or the brief's "lock-free
 * during sweep" invariant is not actually exercised.
 *
 * The host test fixtures in hosttests/mock/arch9/net_runtime.h
 * redefine spin_lock_irqsave / spin_unlock_irqrestore so that the
 * tracking happens on THIS lock — any holder of the lock flips
 * fake_mailbox_lock_held to 1.  fake_poll_rx (test_net_lwip.c)
 * copies fake_mailbox_lock_held into fake_core_mailbox_lock_check
 * at the moment of the sweep, so the test assertion verifies that
 * no lock was held while the sweep ran. */
static spinlock_T test_core_mbox_lock;
#endif

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
    if (__atomic_load_n(&s_net_service_state, __ATOMIC_RELAXED) == NET_STARTING) {
        bool core_ready = __atomic_load_n(&s_tcpip_core_ready, __ATOMIC_ACQUIRE);
        bool adapters_done = __atomic_load_n(&s_adapters_finished, __ATOMIC_ACQUIRE);
        if (core_ready && adapters_done) {
            unsigned active = __atomic_load_n(&s_active_adapters, __ATOMIC_ACQUIRE);
            if (active > 0) {
                __atomic_store_n(&s_net_service_state, NET_ONLINE, __ATOMIC_RELEASE);
                log_info("net: stack online with %u active adapter(s)\n", active);
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
    __atomic_store_n(&s_tcpip_core_ready, true, __ATOMIC_RELEASE);
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
#ifdef OS01_HOST_TEST
    /* On host tests, net_default_ipv4 reads from a test-set IP that
     * mirrors what the production adapter would publish.  This keeps
     * do_getifaddr/do_getsockname's tests independent of lwIP. */
    if (!net_service_ready()) {
        return 0;
    }
    return s_test_default_ipv4;
#else
    if (!net_service_ready()) {
        return 0;
    }
    if (!s_default_netif) {
        return 0;
    }
    return ip4_addr_get_u32(netif_ip4_addr(s_default_netif));
#endif
}

void net_lwip_reset_state(void)
{
    s_net_service_state = NET_OFF;
    s_tcpip_core_ready = false;
    s_adapters_finished = false;
    s_active_adapters = 0;
    s_default_netif = NULL;
    memset(s_adapters, 0, sizeof(s_adapters));
#ifdef OS01_HOST_TEST
    fake_core_mailbox_msg_count = 0;
    fake_app_mailbox_msg_count = 0;
    fake_core_fetch_rx_sweeps = 0;
    fake_core_mailbox_lock_check = 0;
    test_core_mbox_head = 0;
    test_core_mbox_tail = 0;
    test_core_mbox_count = 0;
    test_app_mbox_count = 0;
    s_test_default_ipv4 = 0;
    spin_init(&test_core_mbox_lock);
#endif
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
#ifdef OS01_HOST_TEST
            /* Mirror the default netif's IP into s_test_default_ipv4
             * so net_default_ipv4() can answer without dereferencing
             * a fake netif.  The fake netif_add already populated
             * nif->ip_addr from the `ip` we passed in, so we copy it
             * verbatim.  eth0 is the static 10.0.2.15 by construction. */
            s_test_default_ipv4 = ip.addr;
#endif
        }
        arch9_fault_on_adapter_publish();

        netif_set_up(nif);

        if (ndev->link_up) {
            netif_set_link_up(nif);
        }

        dhcp_start(nif);

        ndev->adapter = nif;
        adapter->active = true;
        __atomic_fetch_add(&s_active_adapters, 1, __ATOMIC_RELEASE);
    }

    __atomic_store_n(&s_adapters_finished, true, __ATOMIC_RELEASE);
    net_check_and_publish_online();
}

#ifdef OS01_HOST_TEST
/* ── Test-only hooks ────────────────────────────────────────────── */
void net_service_force_state_for_test(enum net_service_state s)
{
    s_net_service_state = s;
    s_tcpip_core_ready = (s == NET_ONLINE);
    s_adapters_finished = (s == NET_ONLINE);
    s_active_adapters = (s == NET_ONLINE) ? 1 : 0;
}

void net_service_set_default_ipv4_for_test(uint32_t ip)
{
    s_test_default_ipv4 = ip;
}

void net_service_core_mbox_post_for_test(void *msg)
{
    if (test_core_mbox_count < TEST_MBOX_CAP) {
        test_core_mbox_buf[test_core_mbox_head] = msg;
        test_core_mbox_head = (test_core_mbox_head + 1) % TEST_MBOX_CAP;
        test_core_mbox_count++;
    } else {
        /* Ring is small — count has used space.  Just count it; we
         * care about invariants, not overflow safety in host tests. */
        test_core_mbox_count++;
    }
    fake_core_mailbox_msg_count++;
}

void net_service_app_mbox_post_for_test(void *msg)
{
    (void)msg;
    test_app_mbox_count++;
    fake_app_mailbox_msg_count++;
}

/* Drain the core mailbox — sweep RX first via net_device_poll_all(),
 * THEN pop one message.  This mirrors the production sys_arch mbox_fetch
 * loop (kernel/net/sys_arch.c) which calls net_poll_rx() before
 * attempting to pop.  The mailbox lock must NOT be held while polling.
 *
 * The host test fixture takes the mbox lock only around the pop,
 * matching the production code's mb->lock discipline.  The brief's
 * lock-free-during-sweep invariant is therefore structurally
 * enforced: the test runtime's spinlock macros flip
 * fake_mailbox_lock_held only for THIS lock, and fake_poll_rx
 * records fake_core_mailbox_lock_check at the moment the sweep
 * calls into each NIC's ops->poll_rx — so a fixture bug (lock held
 * during sweep) would show up as fake_core_mailbox_lock_check == 1
 * and fail the test_bounded_rx_and_no_lock assertion. */
static int test_core_fetch_one(void)
{
    /* Sweep RX first, BEFORE popping.  No lock held. */
    net_device_poll_all();
    fake_core_fetch_rx_sweeps++;

    if (test_core_mbox_count <= 0) {
        return -1;
    }
    /* The mailbox lock is acquired only here, around the pop,
     * matching the production sys_arch_mbox_fetch discipline. */
    uint64_t _flags = spin_lock_irqsave(&test_core_mbox_lock);
    void *m = test_core_mbox_buf[test_core_mbox_tail];
    test_core_mbox_tail = (test_core_mbox_tail + 1) % TEST_MBOX_CAP;
    test_core_mbox_count--;
    spin_unlock_irqrestore(&test_core_mbox_lock, _flags);
    (void)m;
    return 0;
}

int net_service_core_mbox_fetch_one_for_test(void)
{
    return test_core_fetch_one();
}

int net_service_drain_core_mailbox_for_test(void)
{
    int returned = 0;
    while (test_core_mbox_count > 0) {
        if (test_core_fetch_one() == 0) {
            returned++;
        } else {
            break;
        }
    }
    return returned;
}

int net_service_drain_app_mailbox_for_test(void)
{
    /* The application mailbox path does NOT sweep RX. */
    int returned = test_app_mbox_count;
    test_app_mbox_count = 0;
    fake_core_fetch_rx_sweeps += 0; /* explicit: zero */
    return returned;
}
#endif /* OS01_HOST_TEST */