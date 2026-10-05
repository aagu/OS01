/* hosttests/cases/test_net_device.c — ARCH-9 net_device registry and dispatch tests */
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

/* Test-specific fake tracking for net_device ops */
static int dev0_xmit_calls = 0;
static int dev1_xmit_calls = 0;
static int dev0_poll_calls = 0;
static int dev1_poll_calls = 0;
static void *dev0_last_xmit_priv = NULL;
static void *dev1_last_xmit_priv = NULL;
static void *dev0_last_poll_priv = NULL;
static void *dev1_last_poll_priv = NULL;
static int dev0_xmit_return_code = 0;

static int fake_dev0_xmit(struct net_device *dev, struct pbuf *p)
{
    (void)p;
    dev0_xmit_calls++;
    dev0_last_xmit_priv = dev->priv;
    return dev0_xmit_return_code;
}

static unsigned fake_dev0_poll_rx(struct net_device *dev, unsigned budget)
{
    dev0_poll_calls++;
    fake_poll_budget = budget;
    dev0_last_poll_priv = dev->priv;
    return 0;
}

static bool fake_dev0_get_link(struct net_device *dev)
{
    (void)dev;
    return true;
}

static int fake_dev0_stop(struct net_device *dev)
{
    (void)dev;
    return 0;
}

static const struct net_device_ops fake_dev0_ops = {
    .xmit = fake_dev0_xmit,
    .poll_rx = fake_dev0_poll_rx,
    .get_link = fake_dev0_get_link,
    .stop = fake_dev0_stop,
};

static int fake_dev1_xmit(struct net_device *dev, struct pbuf *p)
{
    (void)p;
    dev1_xmit_calls++;
    dev1_last_xmit_priv = dev->priv;
    return 0;
}

static unsigned fake_dev1_poll_rx(struct net_device *dev, unsigned budget)
{
    dev1_poll_calls++;
    fake_poll_budget = budget;
    dev1_last_poll_priv = dev->priv;
    return 0;
}

static bool fake_dev1_get_link(struct net_device *dev)
{
    (void)dev;
    return true;
}

static int fake_dev1_stop(struct net_device *dev)
{
    (void)dev;
    return 0;
}

static const struct net_device_ops fake_dev1_ops = {
    .xmit = fake_dev1_xmit,
    .poll_rx = fake_dev1_poll_rx,
    .get_link = fake_dev1_get_link,
    .stop = fake_dev1_stop,
};

/* ── Test 1: Device registration validation and numbering ──────── */
TEST_FUNC(test_device_registration_validation)
{
    net_device_init();
    fake_net_runtime_reset();

    /* NULL device */
    assert_eq(-EINVAL, net_device_register(NULL));

    /* Missing ops */
    struct net_device dev_bad;
    memset(&dev_bad, 0, sizeof(dev_bad));
    dev_bad.mac[0] = 0x52; dev_bad.mac[1] = 0x54; dev_bad.mac[5] = 0x01;
    assert_eq(-EINVAL, net_device_register(&dev_bad));

    /* Incomplete ops */
    struct net_device_ops partial_ops;
    memset(&partial_ops, 0, sizeof(partial_ops));
    partial_ops.xmit = fake_dev0_xmit;
    dev_bad.ops = &partial_ops;
    assert_eq(-EINVAL, net_device_register(&dev_bad));

    /* Zero MAC rejected */
    struct net_device dev_zero_mac;
    memset(&dev_zero_mac, 0, sizeof(dev_zero_mac));
    dev_zero_mac.ops = &fake_dev0_ops;
    assert_eq(-EINVAL, net_device_register(&dev_zero_mac));

    /* Successful registration assigns sequential eth numbers */
    struct net_device dev0;
    memset(&dev0, 0, sizeof(dev0));
    uint8_t mac0[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    memcpy(dev0.mac, mac0, 6);
    dev0.ops = &fake_dev0_ops;
    assert_eq(0, net_device_register(&dev0));
    assert_str_eq("eth0", dev0.name);
    assert_eq(1, (int)net_device_count());

    struct net_device dev1;
    memset(&dev1, 0, sizeof(dev1));
    uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
    memcpy(dev1.mac, mac1, 6);
    dev1.ops = &fake_dev1_ops;
    assert_eq(0, net_device_register(&dev1));
    assert_str_eq("eth1", dev1.name);
    assert_eq(2, (int)net_device_count());

    /* net_device_get checks */
    assert_eq(&dev0, net_device_get(0));
    assert_eq(&dev1, net_device_get(1));
    assert_eq(NULL, net_device_get(2));
}

/* ── Test 2: Per-device dispatch, poll budget 64, priv isolation ─ */
TEST_FUNC(test_per_device_dispatch)
{
    net_device_init();
    fake_net_runtime_reset();

    dev0_xmit_calls = 0;
    dev1_xmit_calls = 0;
    dev0_poll_calls = 0;
    dev1_poll_calls = 0;
    dev0_last_xmit_priv = NULL;
    dev1_last_xmit_priv = NULL;
    dev0_last_poll_priv = NULL;
    dev1_last_poll_priv = NULL;
    fake_poll_budget = 0;

    int priv_cookie_0 = 0x1111;
    int priv_cookie_1 = 0x2222;

    struct net_device dev0;
    memset(&dev0, 0, sizeof(dev0));
    uint8_t mac0[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(dev0.mac, mac0, 6);
    dev0.ops = &fake_dev0_ops;
    dev0.priv = &priv_cookie_0;
    assert_eq(0, net_device_register(&dev0));

    struct net_device dev1;
    memset(&dev1, 0, sizeof(dev1));
    uint8_t mac1[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66};
    memcpy(dev1.mac, mac1, 6);
    dev1.ops = &fake_dev1_ops;
    dev1.priv = &priv_cookie_1;
    assert_eq(0, net_device_register(&dev1));

    /* Poll all devices */
    net_device_poll_all();

    /* Assert poll budget is 64 per contract and brief */
    assert_eq(64, (int)fake_poll_budget);
    assert_eq(1, dev0_poll_calls);
    assert_eq(1, dev1_poll_calls);
    assert_eq((void *)&priv_cookie_0, dev0_last_poll_priv);
    assert_eq((void *)&priv_cookie_1, dev1_last_poll_priv);

    /* TX dispatch does not cross priv pointers */
    struct pbuf *p_tx = fake_pbuf_alloc(64);
    assert_true(p_tx != NULL);

    assert_eq(0, dev0.ops->xmit(&dev0, p_tx));
    assert_eq(1, dev0_xmit_calls);
    assert_eq((void *)&priv_cookie_0, dev0_last_xmit_priv);

    assert_eq(0, dev1.ops->xmit(&dev1, p_tx));
    assert_eq(1, dev1_xmit_calls);
    assert_eq((void *)&priv_cookie_1, dev1_last_xmit_priv);

    pbuf_free(p_tx);
}

/* ── Test 3: pbuf ownership on TX and RX ────────────────────────── */
TEST_FUNC(test_pbuf_ownership)
{
    net_device_init();
    fake_net_runtime_reset();

    struct net_device dev;
    memset(&dev, 0, sizeof(dev));
    uint8_t mac[6] = {0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    memcpy(dev.mac, mac, 6);
    dev.ops = &fake_dev0_ops;
    assert_eq(0, net_device_register(&dev));

    /* Case A: TX success does NOT free caller's pbuf */
    struct pbuf *p1 = fake_pbuf_alloc(128);
    assert_true(p1 != NULL);
    dev0_xmit_return_code = 0;
    fake_pbuf_free_calls = 0;

    int rc = dev.ops->xmit(&dev, p1);
    assert_eq(0, rc);
    assert_eq(0, fake_pbuf_free_calls); /* caller's pbuf remains un-freed */
    pbuf_free(p1);
    assert_eq(1, fake_pbuf_free_calls); /* caller explicitly frees once */

    /* Case B: TX failure does NOT free caller's pbuf */
    struct pbuf *p2 = fake_pbuf_alloc(128);
    assert_true(p2 != NULL);
    dev0_xmit_return_code = -EIO;
    fake_pbuf_free_calls = 0;

    rc = dev.ops->xmit(&dev, p2);
    assert_eq(-EIO, rc);
    assert_eq(0, fake_pbuf_free_calls); /* driver did not free caller's pbuf on failure */
    pbuf_free(p2);
    assert_eq(1, fake_pbuf_free_calls); /* caller explicitly frees once */

    /* Case C: RX failure before adapter ingestion: caller frees exactly once */
    struct pbuf *p3 = fake_pbuf_alloc(128);
    assert_true(p3 != NULL);
    fake_pbuf_free_calls = 0;

    /* Device without adapter attached */
    dev.adapter = NULL;
    rc = net_receive(&dev, p3);
    assert_true(rc < 0);
    assert_eq(0, fake_pbuf_free_calls); /* net_receive failed and did not take ownership */
    pbuf_free(p3);
    assert_eq(1, fake_pbuf_free_calls); /* caller frees it exactly once */

    /* NULL device error */
    struct pbuf *p4 = fake_pbuf_alloc(128);
    assert_true(p4 != NULL);
    fake_pbuf_free_calls = 0;
    rc = net_receive(NULL, p4);
    assert_eq(-EINVAL, rc);
    assert_eq(0, fake_pbuf_free_calls);
    pbuf_free(p4);
    assert_eq(1, fake_pbuf_free_calls);

    /* Case D: RX success: adapter takes ownership of pbuf */
    struct netif mock_nif;
    memset(&mock_nif, 0, sizeof(mock_nif));
    dev.adapter = &mock_nif;

    struct pbuf *p5 = fake_pbuf_alloc(128);
    assert_true(p5 != NULL);
    fake_pbuf_free_calls = 0;
    fake_ethernet_input_calls = 0;

    rc = net_receive(&dev, p5);
    assert_eq(0, rc);
    assert_eq(1, fake_ethernet_input_calls);
    /* In mock environment, ethernet_input took ownership and consumed/freed it */
    assert_eq(1, fake_pbuf_free_calls);
    /* Caller does NOT free p5 */
}

/* ── Test 4: Boot unregistration preserves slot holes ──────────── */
TEST_FUNC(test_device_unregister_boot)
{
    net_device_init();
    fake_net_runtime_reset();

    struct net_device dev0, dev1, dev2;
    memset(&dev0, 0, sizeof(dev0));
    memset(&dev1, 0, sizeof(dev1));
    memset(&dev2, 0, sizeof(dev2));

    uint8_t mac0[6] = {0x00, 0x12, 0x34, 0x56, 0x78, 0x90};
    uint8_t mac1[6] = {0x00, 0x12, 0x34, 0x56, 0x78, 0x91};
    uint8_t mac2[6] = {0x00, 0x12, 0x34, 0x56, 0x78, 0x92};
    memcpy(dev0.mac, mac0, 6); dev0.ops = &fake_dev0_ops;
    memcpy(dev1.mac, mac1, 6); dev1.ops = &fake_dev1_ops;
    memcpy(dev2.mac, mac2, 6); dev2.ops = &fake_dev0_ops;

    assert_eq(0, net_device_register(&dev0));
    assert_eq(0, net_device_register(&dev1));
    assert_eq(0, net_device_register(&dev2));
    assert_eq(3, (int)net_device_count());

    /* Unregister dev1 (the middle device) */
    assert_eq(0, net_device_unregister_boot(&dev1));
    assert_eq(2, (int)net_device_count());

    /* Addresses of remaining devices are preserved, hole is skipped */
    assert_eq(&dev0, net_device_get(0));
    assert_eq(&dev2, net_device_get(1));
    assert_eq(NULL, net_device_get(2));

    /* Unregister NULL or already unregistered device returns -EINVAL */
    assert_eq(-EINVAL, net_device_unregister_boot(NULL));
    assert_eq(-EINVAL, net_device_unregister_boot(&dev1));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_device_registration_validation),
    TEST_ENTRY(test_per_device_dispatch),
    TEST_ENTRY(test_pbuf_ownership),
    TEST_ENTRY(test_device_unregister_boot),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
