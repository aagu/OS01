/* hosttests/cases/test_net_lwip.c — ARCH-9 unified lwIP adapter and readiness tests */
#include "test_framework.h"
#include <net/device.h>
#include <net/lwip.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Global mock tracking variables declared in net_runtime.h */
unsigned fake_poll_budget = 0;
int fake_tcpip_init_calls = 0;
tcpip_init_done_fn fake_tcpip_done_cb = NULL;
void *fake_tcpip_done_arg = NULL;
bool fake_tcpip_auto_callback = true;

int fake_netif_add_calls = 0;
bool fake_netif_add_fail_all = false;

int fake_dhcp_start_calls = 0;
struct netif *fake_dhcp_last_netif = NULL;

int fake_ethernet_input_calls = 0;
struct pbuf *fake_last_rx_pbuf = NULL;
struct netif *fake_last_rx_netif = NULL;

int fake_tcpip_input_calls = 0;
int fake_etharp_output_calls = 0;
int fake_pbuf_free_calls = 0;

err_t ethernet_input(struct pbuf *p, struct netif *netif)
{
    fake_ethernet_input_calls++;
    fake_last_rx_pbuf = p;
    fake_last_rx_netif = netif;
    if (p) {
        fake_pbuf_free_calls++;
        if (p->ref > 0) p->ref--;
        if (p->ref == 0) free(p);
    }
    return ERR_OK;
}

err_t tcpip_input(struct pbuf *p, struct netif *inp)
{
    (void)p;
    (void)inp;
    fake_tcpip_input_calls++;
    return ERR_OK;
}

err_t etharp_output(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
    (void)netif;
    (void)q;
    (void)ipaddr;
    fake_etharp_output_calls++;
    return ERR_OK;
}

static int fake_xmit(struct net_device *dev, struct pbuf *p)
{
    (void)dev;
    (void)p;
    return 0;
}

static unsigned fake_poll_rx(struct net_device *dev, unsigned budget)
{
    (void)dev;
    fake_poll_budget = budget;
    return 0;
}

static bool fake_get_link(struct net_device *dev)
{
    (void)dev;
    return true;
}

static int fake_stop(struct net_device *dev)
{
    (void)dev;
    return 0;
}

static const struct net_device_ops fake_ops = {
    .xmit = fake_xmit,
    .poll_rx = fake_poll_rx,
    .get_link = fake_get_link,
    .stop = fake_stop,
};

/* ── Test 1: No NIC and failed adapters ────────────────────────── */
TEST_FUNC(test_no_nic_and_failed_adapters)
{
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    /* Subtest A: No NIC present -> OFF, ready=false, ip=0, no tcpip_init */
    assert_eq(false, net_service_ready());
    assert_eq(0, net_default_ipv4());

    net_lwip_start();

    assert_eq(false, net_service_ready());
    assert_eq(0, net_default_ipv4());
    assert_eq(0, fake_tcpip_init_calls);

    /* Subtest B: Registered device, but adapter setup fails -> FAILED */
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev;
    memset(&dev, 0, sizeof(dev));
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(dev.mac, mac, 6);
    dev.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev));

    /* Inject failure into netif_add */
    fake_netif_add_fail_all = true;
    fake_tcpip_auto_callback = true;

    net_lwip_start();

    assert_eq(1, fake_tcpip_init_calls);
    assert_eq(false, net_service_ready());
    assert_eq(0, net_default_ipv4());

    /* Subtest C: No duplicate tcpip_init call once FAILED */
    net_lwip_start();
    assert_eq(1, fake_tcpip_init_calls);
    assert_eq(false, net_service_ready());
    assert_eq(0, net_default_ipv4());
}

/* ── Test 2: Readiness callback order ──────────────────────────── */
TEST_FUNC(test_readiness_callback_order)
{
    /* Subtest A: Adapters complete before tcpip callback */
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev;
    memset(&dev, 0, sizeof(dev));
    uint8_t mac[6] = {0x00, 0x50, 0x56, 0x12, 0x34, 0x56};
    memcpy(dev.mac, mac, 6);
    dev.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev));

    /* tcpip_init will NOT invoke callback immediately */
    fake_tcpip_auto_callback = false;

    net_lwip_start();

    /* Adapter is configured, but tcpip core is not yet confirmed */
    assert_eq(1, fake_tcpip_init_calls);
    assert_eq(false, net_service_ready());
    assert_eq(0, net_default_ipv4());
    assert_true(fake_tcpip_done_cb != NULL);

    /* Callback fires from tcpip thread -> transitions to ONLINE */
    fake_tcpip_done_cb(fake_tcpip_done_arg);

    assert_eq(true, net_service_ready());
    assert_true(net_default_ipv4() != 0);

    /* Subtest B: tcpip callback completes before/during adapter registration */
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev_b;
    memset(&dev_b, 0, sizeof(dev_b));
    uint8_t mac_b[6] = {0x00, 0x50, 0x56, 0x12, 0x34, 0x57};
    memcpy(dev_b.mac, mac_b, 6);
    dev_b.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev_b));

    fake_tcpip_auto_callback = true;

    net_lwip_start();

    assert_eq(true, net_service_ready());
    assert_true(net_default_ipv4() != 0);
}

/* ── Test 3: Default addresses (eth0 static, eth1 zero, independent DHCP) ─ */
TEST_FUNC(test_default_addresses)
{
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev0, dev1;
    memset(&dev0, 0, sizeof(dev0));
    memset(&dev1, 0, sizeof(dev1));

    uint8_t mac0[6] = {0x52, 0x54, 0x00, 0x11, 0x22, 0x33};
    uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x11, 0x22, 0x44};
    memcpy(dev0.mac, mac0, 6); dev0.ops = &fake_ops;
    memcpy(dev1.mac, mac1, 6); dev1.ops = &fake_ops;

    assert_eq(0, net_device_register(&dev0));
    assert_eq(0, net_device_register(&dev1));

    fake_tcpip_auto_callback = true;
    net_lwip_start();

    assert_eq(true, net_service_ready());

    /* eth0 must have static 10.0.2.15 */
    struct netif *nif0 = (struct netif *)dev0.adapter;
    assert_true(nif0 != NULL);
    ip4_addr_t expected_ip;
    IP4_ADDR(&expected_ip, 10, 0, 2, 15);
    assert_eq(expected_ip.addr, nif0->ip_addr.addr);

    /* eth1 must have address 0 (not duplicated 10.0.2.15) */
    struct netif *nif1 = (struct netif *)dev1.adapter;
    assert_true(nif1 != NULL);
    assert_eq(0, (int)nif1->ip_addr.addr);

    /* net_default_ipv4 returns default interface (eth0) address */
    assert_eq(expected_ip.addr, net_default_ipv4());

    /* Both interfaces receive independent DHCP start */
    assert_eq(2, fake_dhcp_start_calls);
}

/* ── Test 4: Adapter input in core thread without mailbox enqueue ── */
TEST_FUNC(test_adapter_input_in_core)
{
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev;
    memset(&dev, 0, sizeof(dev));
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x99, 0x88, 0x77};
    memcpy(dev.mac, mac, 6);
    dev.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev));

    fake_tcpip_auto_callback = true;
    net_lwip_start();

    struct netif *nif = (struct netif *)dev.adapter;
    assert_true(nif != NULL);

    /* Check that netif input handler is ethernet_input, NOT tcpip_input */
    assert_true(nif->input == ethernet_input);

    /* Ingest a packet via net_receive */
    struct pbuf *p = fake_pbuf_alloc(128);
    assert_true(p != NULL);

    fake_ethernet_input_calls = 0;
    fake_tcpip_input_calls = 0;

    int rc = net_receive(&dev, p);
    assert_eq(0, rc);

    /* Assert packet was routed directly to ethernet_input in current thread */
    assert_eq(1, fake_ethernet_input_calls);
    assert_eq(nif, fake_last_rx_netif);

    /* Assert tcpip_input was NOT called (no mailbox enqueue to own thread) */
    assert_eq(0, fake_tcpip_input_calls);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_no_nic_and_failed_adapters),
    TEST_ENTRY(test_readiness_callback_order),
    TEST_ENTRY(test_default_addresses),
    TEST_ENTRY(test_adapter_input_in_core),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
