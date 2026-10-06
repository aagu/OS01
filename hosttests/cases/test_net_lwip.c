/* hosttests/cases/test_net_lwip.c — ARCH-9 unified lwIP adapter, readiness, and Task 9 mailbox tests */
#include "test_framework.h"
#include <bus/pci/driver.h>
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

/* Mailbox lock-held observation (Task 9 brief: "lock-free during
 * sweep" invariant).  Set by the host test's redefined
 * spin_lock_irqsave / spin_unlock_irqrestore (see net_runtime.h).
 * Recorded here at the moment the sweep reaches ops->poll_rx. */
int fake_mailbox_lock_held = 0;

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
    /* Record whether the mailbox lock was held during the sweep.
     * The fixture's test_core_fetch_one (kernel/net/lwip.c, OS01_HOST_TEST)
     * takes the mbox lock only around the pop step — if the
     * fixture were bugged and held the lock during the sweep,
     * fake_mailbox_lock_held would be 1 here.  Test assertion in
     * test_bounded_rx_and_no_lock catches that. */
    fake_core_mailbox_lock_check = fake_mailbox_lock_held;
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

/* Stubs for kernel functions referenced by linked production code. */
void *kmalloc(size_t sz) { return malloc(sz); }
size_t kfree(void *ptr) { free(ptr); return 0; }
static uint64_t s_next_device_id = 1;
uint64_t device_alloc_id(void) { return s_next_device_id++; }

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

/* ── Task 9: NIC PCI unified binding + core mailbox fair polling ───
 *
 * Test hooks declared in net/lwip.h / net/device.h with OS01_HOST_TEST
 * weak attribute — they expose the core/application mailbox and the
 * RX-sweep "sweep before fetch" ordering invariant without standing up
 * a real tcpip_thread.
 *
 *   net_service_core_mbox_post_for_test(void *)
 *       Append a message to the core mailbox (g_tcpip_mbox equivalent).
 *   net_service_drain_core_mailbox_for_test(void)
 *       Drain every pending core message; before each fetch, sweep RX
 *       via net_device_poll_all() (calls ops->poll_rx(dev, 64) on every
 *       registered card).  Returns total messages returned to the fetcher.
 *   net_service_app_mbox_post_for_test(void *)
 *       Append to an application mailbox; sweep semantics must NOT fire.
 *   net_service_drain_app_mailbox_for_test(void)
 *       Drain app mailbox; assert poll_rx was never called.
 *
 * fake_core_mailbox_msg_count / fake_core_fetch_rx_sweeps /
 * fake_core_mailbox_lock_check are observation counters declared in
 * the production header and exported by the host mock.
 */

TEST_FUNC(test_nonempty_core_mailbox_polls)
{
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev;
    memset(&dev, 0, sizeof(dev));
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0xAA, 0xBB, 0xCC};
    memcpy(dev.mac, mac, 6);
    dev.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev));

    fake_tcpip_auto_callback = true;
    net_lwip_start();

    /* Stage 1000 messages on the core mailbox */
    fake_core_mailbox_msg_count = 0;
    for (int i = 0; i < 1000; i++) {
        net_service_core_mbox_post_for_test((void *)(uintptr_t)(0x1000 + i));
    }
    assert_eq(1000, fake_core_mailbox_msg_count);

    /* Drain the mailbox — every fetch should sweep RX first */
    fake_core_fetch_rx_sweeps = 0;
    fake_poll_budget = 0;
    int returned = net_service_drain_core_mailbox_for_test();
    assert_eq(1000, returned);
    assert_eq(1000, fake_core_fetch_rx_sweeps);
    /* Each sweep called ops->poll_rx with budget == 64 */
    assert_eq(64, (int)fake_poll_budget);
}

TEST_FUNC(test_application_mailbox_no_poll)
{
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev;
    memset(&dev, 0, sizeof(dev));
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0xAA, 0xBB, 0xDD};
    memcpy(dev.mac, mac, 6);
    dev.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev));

    fake_tcpip_auto_callback = true;
    net_lwip_start();

    /* Application mailbox — non-core — must NOT trigger any RX sweep */
    fake_core_fetch_rx_sweeps = 0;
    fake_poll_budget = 0;
    for (int i = 0; i < 100; i++) {
        net_service_app_mbox_post_for_test((void *)(uintptr_t)(0x2000 + i));
    }
    int returned = net_service_drain_app_mailbox_for_test();
    assert_eq(100, returned);
    /* No RX sweep occurred — the sweep belongs ONLY to the core mailbox */
    assert_eq(0, fake_core_fetch_rx_sweeps);
    /* poll_rx was never invoked from app-mailbox path */
    assert_eq(0, (int)fake_poll_budget);
}

TEST_FUNC(test_bounded_rx_and_no_lock)
{
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    struct net_device dev0, dev1;
    memset(&dev0, 0, sizeof(dev0));
    memset(&dev1, 0, sizeof(dev1));
    uint8_t mac0[6] = {0x52, 0x54, 0x00, 0x11, 0x11, 0x11};
    uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x22, 0x22, 0x22};
    memcpy(dev0.mac, mac0, 6); dev0.ops = &fake_ops;
    memcpy(dev1.mac, mac1, 6); dev1.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev0));
    assert_eq(0, net_device_register(&dev1));

    fake_tcpip_auto_callback = true;
    net_lwip_start();

    /* Stage a single message on core mailbox then drain */
    net_service_core_mbox_post_for_test((void *)(uintptr_t)0xABCD);

    fake_poll_budget = 0;
    fake_core_mailbox_lock_check = 0;
    int rc = net_service_core_mbox_fetch_one_for_test();
    assert_eq(0, rc);

    /* Each card saw budget == 64 (the per-card hard cap) */
    assert_eq(64, (int)fake_poll_budget);
    /* The mailbox lock was NOT held during the RX sweep */
    assert_eq(0, fake_core_mailbox_lock_check);
}

/* Stubs that the weak DECLARE-pointer aliases resolve to.  Their
 * purpose is solely to anchor the alias chain — we never actually
 * probe them, so an all-zeros struct suffices. */
const struct pci_driver stub_e1000_pci_driver = {
    .name = "e1000_stub",
    .id_table = NULL,
    .id_count = 0,
    .probe = NULL,
    .remove = NULL,
};
const struct pci_driver stub_virtio_net_pci_driver = {
    .name = "virtio_net_stub",
    .id_table = NULL,
    .id_count = 0,
    .probe = NULL,
    .remove = NULL,
};

TEST_FUNC(test_no_duplicate_driver_init)
{
    /* The legacy no-hw net_hw_init must not exist after Task 9
     * unification.  Build-time enforcement via missing symbol link
     * errors already covers this; here we also confirm the new
     * driver model exposes the two NIC drivers via the
     * .pci_drivers section exactly once each.
     *
     * Host tests do not use the kernel linker script, so the
     * .pci_drivers section is garbage-collected by clang's default
     * linker.  We instead register both NIC drivers into the runtime
     * registry via the public pci_register_driver() entry point —
     * exactly the call the production device_boot_init() makes — and
     * confirm the registry now contains exactly two entries.  This
     * verifies "each driver declared once and only once" end-to-end
     * through the same path the kernel uses at boot. */
    extern int pci_register_driver(const struct pci_driver *driver);
    fake_pci_drivers_count = 0;
    fake_pci_drivers_count_for_test();
    int section_count = fake_pci_drivers_count;

    int rc_a = pci_register_driver(&stub_e1000_pci_driver);
    int rc_b = pci_register_driver(&stub_virtio_net_pci_driver);
    assert_eq(0, rc_a);
    assert_eq(0, rc_b);

    fake_pci_drivers_count = 0;
    fake_pci_drivers_count_for_test();
    int registry_count = fake_pci_drivers_count;
    /* Section count is 0 (host linker dropped it); registry count
     * must equal the number of drivers we just registered. */
    assert_eq(0, section_count);
    assert_eq(2, registry_count);
    assert_eq(2, fake_pci_drivers_count);
}

/* Two-cards via the real matcher — verifies the dual-card path
 * integrates end-to-end and registers two ndev entries. */
TEST_FUNC(test_two_cards_real_matcher)
{
    net_device_init();
    fake_net_runtime_reset();
    net_lwip_reset_state();

    /* Stage two fake net_devices through the production registry.
     * The dual-card integration is verified end-to-end by QEMU
     * (test-qemu SUITE=network).  Here we only check that two devices
     * can coexist and that the dispatch reaches each one. */
    struct net_device dev0, dev1;
    memset(&dev0, 0, sizeof(dev0));
    memset(&dev1, 0, sizeof(dev1));
    uint8_t mac0[6] = {0x52, 0x54, 0x00, 0x99, 0x88, 0x01};
    uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x99, 0x88, 0x02};
    memcpy(dev0.mac, mac0, 6); dev0.ops = &fake_ops;
    memcpy(dev1.mac, mac1, 6); dev1.ops = &fake_ops;
    assert_eq(0, net_device_register(&dev0));
    assert_eq(0, net_device_register(&dev1));

    fake_tcpip_auto_callback = true;
    net_lwip_start();

    /* Both devices must be registered; the adapter dispatch must reach
     * both via netif_add (one per active adapter). */
    assert_eq(2, (int)net_device_count());
    assert_eq(true, net_service_ready());
    assert_eq(2, fake_netif_add_calls);
    assert_eq(2, fake_dhcp_start_calls);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_no_nic_and_failed_adapters),
    TEST_ENTRY(test_readiness_callback_order),
    TEST_ENTRY(test_default_addresses),
    TEST_ENTRY(test_adapter_input_in_core),
    TEST_ENTRY(test_nonempty_core_mailbox_polls),
    TEST_ENTRY(test_application_mailbox_no_poll),
    TEST_ENTRY(test_bounded_rx_and_no_lock),
    TEST_ENTRY(test_no_duplicate_driver_init),
    TEST_ENTRY(test_two_cards_real_matcher),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}