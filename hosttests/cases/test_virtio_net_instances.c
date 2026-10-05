/* hosttests/cases/test_virtio_net_instances.c — ARCH-9 virtio-net private instance and polling mode tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <net/device.h>
#include <driver/virtio-net.h>
#include <intr/interrupt.h>
#include <arch/pci.h>
#include <driver/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define TEST_CASE(name) printf("\n  [TEST] %s\n", name)

/* ── Memory and slab stubs ────────────────────────────────────────── */
void *kmalloc(size_t sz) { return malloc(sz); }
void kfree(void *ptr) { free(ptr); }
void *kzalloc(size_t sz) { return calloc(1, sz); }

void arch_irq_install(void) {}
void softirq_init(void) {}

/* ── Global mock variables ────────────────────────────────────────── */
uint32_t fake_free_pages_count = 0;
uint32_t fake_free_4k_count = 0;
uint32_t fake_alloc_pages_count = 0;
uint32_t fake_alloc_4k_count = 0;
uint32_t fake_modern_device_io_writes = 0;
uint32_t fake_other_instance_queue_mutations = 0;
uint32_t fake_sys_mbox_wake_count = 0;

void sys_mbox_wake(void)
{
    fake_sys_mbox_wake_count++;
}

bool s_inject_alloc_pages_fail = false;
int s_inject_alloc_4k_fail_after = -1;
bool s_inject_net_register_fail = false;

/* Net runtime fake definitions */
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

err_t etharp_output(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
    (void)netif; (void)q; (void)ipaddr;
    fake_etharp_output_calls++;
    return ERR_OK;
}

err_t tcpip_input(struct pbuf *p, struct netif *inp)
{
    (void)p; (void)inp;
    fake_tcpip_input_calls++;
    return ERR_OK;
}

/* ── Simulated Page Allocator ─────────────────────────────────────── */
#define MAX_MOCK_PAGES 64
static struct Page s_mock_pages[MAX_MOCK_PAGES];
static bool s_page_in_use[MAX_MOCK_PAGES];

#define MAX_MOCK_4K 512
static void *s_mock_4k_bufs[MAX_MOCK_4K];
static bool s_4k_in_use[MAX_MOCK_4K];

struct Page *alloc_pages(int zone, int count, int flags)
{
    (void)zone; (void)flags;
    if (s_inject_alloc_pages_fail) return NULL;

    for (int i = 0; i <= MAX_MOCK_PAGES - count; i++) {
        bool match = true;
        for (int j = 0; j < count; j++) {
            if (s_page_in_use[i + j]) { match = false; break; }
        }
        if (match) {
            for (int j = 0; j < count; j++) {
                s_page_in_use[i + j] = true;
                if (!s_mock_pages[i + j].virt_address) {
                    s_mock_pages[i + j].virt_address = calloc(1, 16384);
                    s_mock_pages[i + j].phy_address = (uint64_t)(uintptr_t)s_mock_pages[i + j].virt_address;
                } else {
                    memset(s_mock_pages[i + j].virt_address, 0, 16384);
                }
            }
            fake_alloc_pages_count++;
            return &s_mock_pages[i];
        }
    }
    return NULL;
}

void free_pages(struct Page *page, int count)
{
    if (!page) return;
    int idx = page - s_mock_pages;
    if (idx >= 0 && idx + count <= MAX_MOCK_PAGES) {
        for (int j = 0; j < count; j++) {
            s_page_in_use[idx + j] = false;
        }
        fake_free_pages_count++;
    }
}

uint64_t alloc_4k_page(void)
{
    if (s_inject_alloc_4k_fail_after == 0) return 0;
    if (s_inject_alloc_4k_fail_after > 0) s_inject_alloc_4k_fail_after--;

    for (int i = 0; i < MAX_MOCK_4K; i++) {
        if (!s_4k_in_use[i]) {
            s_4k_in_use[i] = true;
            if (!s_mock_4k_bufs[i]) {
                s_mock_4k_bufs[i] = calloc(1, 4096);
            } else {
                memset(s_mock_4k_bufs[i], 0, 4096);
            }
            fake_alloc_4k_count++;
            return (uint64_t)(uintptr_t)s_mock_4k_bufs[i];
        }
    }
    return 0;
}

void free_4k_page(uint64_t phys)
{
    void *ptr = (void *)(uintptr_t)phys;
    for (int i = 0; i < MAX_MOCK_4K; i++) {
        if (s_4k_in_use[i] && s_mock_4k_bufs[i] == ptr) {
            s_4k_in_use[i] = false;
            fake_free_4k_count++;
            return;
        }
    }
}

/* ── Mock IRQ Controller ─────────────────────────────────────────── */
static void mock_ctrl_enable(uint64_t irq) { (void)irq; }
static void mock_ctrl_disable(uint64_t irq) { (void)irq; }
static uint64_t mock_ctrl_install(uint64_t irq, void *arg) { (void)irq; (void)arg; return 0; }
static void mock_ctrl_uninstall(uint64_t irq) { (void)irq; }
static void mock_ctrl_ack(uint64_t irq) { (void)irq; }

static hw_int_controller_t s_mock_controller = {
    .enable = mock_ctrl_enable,
    .disable = mock_ctrl_disable,
    .install = mock_ctrl_install,
    .uninstall = mock_ctrl_uninstall,
    .ack = mock_ctrl_ack,
};

hw_int_controller_t *arch_irq_select_controller(uint32_t gsi)
{
    if (gsi >= MAX_GSI) return NULL;
    return &s_mock_controller;
}

uint64_t arch_irq_gsi_to_vector(uint32_t gsi)
{
    return 0x20 + gsi;
}

/* ── Mock PCI backend & Fake VirtIO Hardware ──────────────────────── */
#define MAX_CONFIG_DEVS 8
static uint32_t s_config_spaces[MAX_CONFIG_DEVS][64];

static int fake_pci_read32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t offset, uint32_t *out, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)f; (void)scope;
    if (!out) return -EINVAL;
    uint8_t idx = s < MAX_CONFIG_DEVS ? s : 0;
    if (offset / 4 < 64) {
        *out = s_config_spaces[idx][offset / 4];
    } else {
        *out = 0;
    }
    return 0;
}

static int fake_pci_write32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t offset, uint32_t value, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)f; (void)scope;
    uint8_t idx = s < MAX_CONFIG_DEVS ? s : 0;
    if (offset / 4 < 64) {
        s_config_spaces[idx][offset / 4] = value;
    }
    return 0;
}

static int fake_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    if (!out) return -EINVAL;
    *out = pdev ? (pdev->slot + 10) : 11;
    return 0;
}

static struct pci_backend s_fake_backend = {
    .roots = NULL,
    .root_count = 0,
    .read32 = fake_pci_read32,
    .write32 = fake_pci_write32,
    .route_gsi = fake_route_gsi,
};

const struct pci_backend *arch_pci_backend(void)
{
    return &s_fake_backend;
}

#define MAX_FAKE_VIRTIO 4
struct fake_virtio_hw {
    uint16_t io_base;
    uint32_t host_features;
    uint32_t guest_features;
    uint8_t  status;
    uint8_t  isr;
    uint8_t  mac[6];
    uint16_t selected_queue;
    uint16_t qsize[2];
    uint32_t q_pfn[2];
    uint16_t q_notify[2];
    bool     is_modern_or_invalid;
    bool     reject_features;
    bool     reset_unconfirmed;
};

static struct fake_virtio_hw s_fake_hws[MAX_FAKE_VIRTIO];

static struct fake_virtio_hw *find_hw_by_port(uint16_t port)
{
    for (int i = 0; i < MAX_FAKE_VIRTIO; i++) {
        if (s_fake_hws[i].io_base != 0 &&
            port >= s_fake_hws[i].io_base &&
            port < s_fake_hws[i].io_base + 0x40) {
            if (s_fake_hws[i].is_modern_or_invalid) {
                fake_modern_device_io_writes++;
            }
            return &s_fake_hws[i];
        }
    }
    fake_modern_device_io_writes++;
    return NULL;
}

uint8_t vio_in8(uint16_t port)
{
    struct fake_virtio_hw *hw = find_hw_by_port(port);
    if (!hw) return 0xFF;
    uint16_t reg = port - hw->io_base;
    if (reg == 0x12) return hw->status; /* VIRTIO_LEGACY_DEVICE_STATUS */
    if (reg == 0x13) {                  /* VIRTIO_LEGACY_ISR_STATUS */
        uint8_t isr = hw->isr;
        hw->isr = 0;
        return isr;
    }
    if (reg >= 0x14 && reg <= 0x19) {
        return hw->mac[reg - 0x14];
    }
    return 0;
}

void vio_out8(uint16_t port, uint8_t v)
{
    struct fake_virtio_hw *hw = find_hw_by_port(port);
    if (!hw) return;
    uint16_t reg = port - hw->io_base;
    if (reg == 0x12) { /* VIRTIO_LEGACY_DEVICE_STATUS */
        if (v == 0 && hw->reset_unconfirmed) {
            hw->status = 0x01; /* refuse to settle to 0 */
        } else {
            hw->status = v;
            if (hw->reject_features && (v & 0x08)) {
                hw->status &= ~0x08; /* reject FEATURES_OK */
            }
        }
    }
}

uint16_t vio_in16(uint16_t port)
{
    struct fake_virtio_hw *hw = find_hw_by_port(port);
    if (!hw) return 0xFFFF;
    uint16_t reg = port - hw->io_base;
    if (reg == 0x0C) { /* VIRTIO_LEGACY_QUEUE_SIZE */
        if (hw->selected_queue < 2) return hw->qsize[hw->selected_queue];
    }
    return 0;
}

void vio_out16(uint16_t port, uint16_t v)
{
    struct fake_virtio_hw *hw = find_hw_by_port(port);
    if (!hw) return;
    uint16_t reg = port - hw->io_base;
    if (reg == 0x0E) { /* VIRTIO_LEGACY_QUEUE_SELECT */
        hw->selected_queue = v;
    } else if (reg == 0x0C) { /* VIRTIO_LEGACY_QUEUE_SIZE */
        if (hw->selected_queue < 2) hw->qsize[hw->selected_queue] = v;
    } else if (reg == 0x10) { /* VIRTIO_LEGACY_QUEUE_NOTIFY */
        if (v < 2) hw->q_notify[v]++;
    }
}

uint32_t vio_in32(uint16_t port)
{
    struct fake_virtio_hw *hw = find_hw_by_port(port);
    if (!hw) return 0xFFFFFFFF;
    uint16_t reg = port - hw->io_base;
    if (reg == 0x00) return hw->host_features; /* VIRTIO_LEGACY_HOST_FEATURES */
    if (reg == 0x08) {                         /* VIRTIO_LEGACY_QUEUE_PFN */
        if (hw->selected_queue < 2) return hw->q_pfn[hw->selected_queue];
    }
    return 0;
}

void vio_out32(uint16_t port, uint32_t v)
{
    struct fake_virtio_hw *hw = find_hw_by_port(port);
    if (!hw) return;
    uint16_t reg = port - hw->io_base;
    if (reg == 0x04) hw->guest_features = v; /* VIRTIO_LEGACY_GUEST_FEATURES */
    if (reg == 0x08) {                       /* VIRTIO_LEGACY_QUEUE_PFN */
        if (hw->selected_queue < 2) hw->q_pfn[hw->selected_queue] = v;
    }
}

/* ── Fixture Helpers ──────────────────────────────────────────────── */
struct fake_virtio_fixture {
    struct pci_device pdev;
    struct fake_virtio_hw *hw;
    int hw_index;
};

static void setup_virtio_fixture(struct fake_virtio_fixture *fix, int hw_index,
                                 uint16_t io_base, uint8_t slot, const uint8_t *mac)
{
    memset(fix, 0, sizeof(*fix));
    fix->hw_index = hw_index;
    fix->hw = &s_fake_hws[hw_index];
    memset(fix->hw, 0, sizeof(*fix->hw));

    fix->hw->io_base = io_base;
    fix->hw->host_features = (1ULL << 5) | (1ULL << 16); /* VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS */
    fix->hw->qsize[0] = 64;
    fix->hw->qsize[1] = 64;
    memcpy(fix->hw->mac, mac, 6);

    fix->pdev.dev.id = device_alloc_id();
    snprintf(fix->pdev.dev.name, sizeof(fix->pdev.dev.name), "pci-0000:00:%02x.0", slot);
    fix->pdev.bus = 0;
    fix->pdev.slot = slot;
    fix->pdev.fn = 0;
    fix->pdev.vendor = 0x1af4;
    fix->pdev.device = 0x1000;
    fix->pdev.class_code = (PCI_CLASS_NETWORK << 16) | (PCI_SUBCLASS_ETHERNET << 8);

    fix->pdev.bars[0].kind = PCI_BAR_IO;
    fix->pdev.bars[0].valid = true;
    fix->pdev.bars[0].address = io_base;
}

static void reset_test_environment(void)
{
    pci_core_reset_for_test();
    for (int g = 0; g < MAX_GSI; g++) {
        irq_table[g].handler = NULL;
        irq_table[g].controller = NULL;
        irq_table[g].parameter = 0;
    }
    fake_net_runtime_reset();

    memset(s_fake_hws, 0, sizeof(s_fake_hws));
    fake_modern_device_io_writes = 0;
    fake_other_instance_queue_mutations = 0;
    fake_alloc_pages_count = 0;
    fake_free_pages_count = 0;
    fake_alloc_4k_count = 0;
    fake_free_4k_count = 0;

    s_inject_alloc_pages_fail = false;
    s_inject_alloc_4k_fail_after = -1;
    s_inject_net_register_fail = false;
}

/* ── Test 1: test_success_remove_one_of_two ────────────────────────── */
static void test_success_remove_one_of_two(void)
{
    TEST_CASE("virtio-net: removing one instance preserves other instance and queues");
    reset_test_environment();

    uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x01};
    uint8_t mac2[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x02};

    struct fake_virtio_fixture fix1, fix2;
    setup_virtio_fixture(&fix1, 0, 0xC000, 1, mac1);
    setup_virtio_fixture(&fix2, 1, 0xD000, 2, mac2);

    int rc = virtio_net_probe(&fix1.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_eq(0, rc);
    assert_true(fix1.pdev.driver_data != NULL);

    rc = virtio_net_probe(&fix2.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_eq(0, rc);
    assert_true(fix2.pdev.driver_data != NULL);

    struct virtio_net_instance *inst1 = (struct virtio_net_instance *)fix1.pdev.driver_data;
    struct virtio_net_instance *inst2 = (struct virtio_net_instance *)fix2.pdev.driver_data;

    uint32_t gsi1 = inst1->gsi;
    uint32_t gsi2 = inst2->gsi;
    assert_true(gsi1 != gsi2);
    assert_true(irq_table[gsi1].handler != NULL);
    assert_true(irq_table[gsi2].handler != NULL);

    uint32_t free_pages_before = fake_free_pages_count;
    uint32_t free_4k_before = fake_free_4k_count;

    /* Remove instance 1 */
    virtio_net_remove(&fix1.pdev);

    assert_eq(NULL, fix1.pdev.driver_data);
    assert_eq(NULL, irq_table[gsi1].handler);
    assert_true(fake_free_pages_count > free_pages_before);
    assert_true(fake_free_4k_count > free_4k_before);

    /* Verify instance 2 is completely intact and operational */
    assert_true(fix2.pdev.driver_data == inst2);
    assert_true(irq_table[gsi2].handler != NULL);
    assert_true(inst2->ndev != NULL);

    /* Test transmit on instance 2 */
    struct pbuf *tx_pbuf = fake_pbuf_alloc(64);
    assert_true(tx_pbuf != NULL);
    rc = inst2->ndev->ops->xmit(inst2->ndev, tx_pbuf);
    assert_eq(0, rc);
    free(tx_pbuf);

    virtio_net_remove(&fix2.pdev);
    assert_eq(NULL, fix2.pdev.driver_data);
}

/* ── Test 2: test_two_virtio_instances ────────────────────────────── */
static void test_two_virtio_instances(void)
{
    TEST_CASE("virtio-net: two instances operate without queue cross-mutation");
    reset_test_environment();

    uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x11};
    uint8_t mac2[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x22};

    struct fake_virtio_fixture fix1, fix2;
    setup_virtio_fixture(&fix1, 0, 0xC000, 1, mac1);
    setup_virtio_fixture(&fix2, 1, 0xD000, 2, mac2);

    assert_eq(0, virtio_net_probe(&fix1.pdev, &virtio_net_pci_driver.id_table[0]));
    assert_eq(0, virtio_net_probe(&fix2.pdev, &virtio_net_pci_driver.id_table[0]));

    struct virtio_net_instance *inst1 = (struct virtio_net_instance *)fix1.pdev.driver_data;
    struct virtio_net_instance *inst2 = (struct virtio_net_instance *)fix2.pdev.driver_data;

    assert_true(inst1->io_base != inst2->io_base);
    assert_true(inst1->rx_vq.desc != inst2->rx_vq.desc);
    assert_true(inst1->tx_vq.desc != inst2->tx_vq.desc);

    /* Take snapshot of inst2 */
    uint16_t snap_rx_avail = inst2->rx_vq.avail->idx;
    uint16_t snap_rx_used = inst2->rx_vq.last_used_idx;
    uint16_t snap_tx_avail = inst2->tx_vq.avail->idx;
    uint16_t snap_tx_head = inst2->tx_desc_head;
    uint16_t snap_tx_tail = inst2->tx_desc_tail;

    /* Operate heavily on inst1: transmit 5 packets */
    for (int i = 0; i < 5; i++) {
        struct pbuf *p = fake_pbuf_alloc(128);
        assert_true(p != NULL);
        assert_eq(0, inst1->ndev->ops->xmit(inst1->ndev, p));
        free(p);
    }

    /* Simulate an RX packet arrival on inst1 */
    uint16_t di = inst1->rx_vq.avail->ring[0];
    uint8_t *buf = (uint8_t *)inst1->rx_bufs[di];
    memset(buf, 0, 10 + 64);
    inst1->rx_vq.used->ring[0].id = di;
    inst1->rx_vq.used->ring[0].len = 10 + 64;
    inst1->rx_vq.used->idx = 1;
    struct netif fake_adapter1;
    memset(&fake_adapter1, 0, sizeof(fake_adapter1));
    inst1->ndev->adapter = &fake_adapter1;

    unsigned polled = inst1->ndev->ops->poll_rx(inst1->ndev, 64);
    assert_eq(1, polled);

    /* Verify inst2 was not mutated */
    if (inst2->rx_vq.avail->idx != snap_rx_avail ||
        inst2->rx_vq.last_used_idx != snap_rx_used ||
        inst2->tx_vq.avail->idx != snap_tx_avail ||
        inst2->tx_desc_head != snap_tx_head ||
        inst2->tx_desc_tail != snap_tx_tail) {
        fake_other_instance_queue_mutations++;
    }

    assert_eq(0, fake_other_instance_queue_mutations);

    virtio_net_remove(&fix1.pdev);
    virtio_net_remove(&fix2.pdev);
}

/* ── Test 3: test_transport_validation ────────────────────────────── */
static void test_transport_validation(void)
{
    TEST_CASE("virtio-net: modern devices and non-I/O BARs rejected without I/O writes");
    reset_test_environment();

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x33};
    struct fake_virtio_fixture fix;
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);
    fix.hw->is_modern_or_invalid = true;

    /* 1. Modern device ID (0x1041) */
    fix.pdev.device = 0x1041;
    int rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(0, fake_modern_device_io_writes);
    assert_eq(NULL, fix.pdev.driver_data);

    /* 2. Device 0x1000 but BAR0 is MMIO32 */
    fix.pdev.device = 0x1000;
    fix.pdev.bars[0].kind = PCI_BAR_MMIO32;
    rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(0, fake_modern_device_io_writes);
    assert_eq(NULL, fix.pdev.driver_data);

    /* 3. Device 0x1000 but BAR0 is MMIO64 */
    fix.pdev.bars[0].kind = PCI_BAR_MMIO64;
    rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(0, fake_modern_device_io_writes);
    assert_eq(NULL, fix.pdev.driver_data);

    /* 4. Device 0x1000 but BAR0 is invalid */
    fix.pdev.bars[0].kind = PCI_BAR_IO;
    fix.pdev.bars[0].valid = false;
    rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(0, fake_modern_device_io_writes);
    assert_eq(NULL, fix.pdev.driver_data);
}

/* ── Test 4: test_intx_conflict_poll ──────────────────────────────── */
static bool s_dummy_irq_fired = false;
static void dummy_irq_handler(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)param; (void)regs;
    s_dummy_irq_fired = true;
}

static void test_intx_conflict_poll(void)
{
    TEST_CASE("virtio-net: GSI conflict falls back to NIC_POLL without stealing IRQ");
    reset_test_environment();
    s_dummy_irq_fired = false;

    /* Pre-register GSI 11 with another dummy driver */
    int irq_rc = register_irq(11, NULL, dummy_irq_handler, 0x1234, IRQF_TRIGGER_LEVEL, "dummy-dev");
    assert_eq(1, irq_rc);

    /* Setup virtio device on slot 1 (route_gsi maps slot 1 -> GSI 11) */
    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x44};
    struct fake_virtio_fixture fix;
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);

    int rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_eq(0, rc);
    assert_true(fix.pdev.driver_data != NULL);

    struct virtio_net_instance *inst = (struct virtio_net_instance *)fix.pdev.driver_data;
    assert_eq(NIC_POLL, inst->irq_mode);
    assert_eq(false, inst->irq_owned);

    /* Verify GSI 11 is STILL owned by dummy driver and not overwritten */
    assert_true(irq_table[11].handler == dummy_irq_handler);

    /* Trigger dummy IRQ */
    irq_table[11].handler(11, irq_table[11].parameter, NULL);
    assert_true(s_dummy_irq_fired);

    /* Both devices can operate: virtio transmits and polls successfully */
    struct pbuf *p = fake_pbuf_alloc(64);
    assert_eq(0, inst->ndev->ops->xmit(inst->ndev, p));
    free(p);

    /* Simulate RX and poll */
    struct netif fake_adapter;
    memset(&fake_adapter, 0, sizeof(fake_adapter));
    inst->ndev->adapter = &fake_adapter;

    uint16_t di = inst->rx_vq.avail->ring[0];
    uint8_t *buf = (uint8_t *)inst->rx_bufs[di];
    memset(buf, 0, 10 + 64);
    inst->rx_vq.used->ring[0].id = di;
    inst->rx_vq.used->ring[0].len = 10 + 64;
    inst->rx_vq.used->idx = 1;

    unsigned n = inst->ndev->ops->poll_rx(inst->ndev, 64);
    assert_eq(1, n);

    /* Removing virtio instance does NOT unregister GSI 11 */
    virtio_net_remove(&fix.pdev);
    assert_true(irq_table[11].handler == dummy_irq_handler);

    unregister_irq(11);
    assert_eq(NULL, irq_table[11].handler);
}

/* ── Test 5: test_queue_feature_failure ───────────────────────────── */
static void test_queue_feature_failure(void)
{
    TEST_CASE("virtio-net: feature/queue/net failure unwinds cleanly");
    reset_test_environment();

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x55};
    struct fake_virtio_fixture fix;

    /* 1. Host features lacks MAC */
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);
    fix.hw->host_features = 0; /* no MAC */
    int rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);

    /* 2. Device rejects FEATURES_OK */
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);
    fix.hw->reject_features = true;
    rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);

    /* 3. alloc_pages fails during queue initialization */
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);
    s_inject_alloc_pages_fail = true;
    rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);
    s_inject_alloc_pages_fail = false;

    /* 4. alloc_4k_page fails during RX buffer allocation */
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);
    s_inject_alloc_4k_fail_after = 5; /* fail after 5 pages */
    rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);
    s_inject_alloc_4k_fail_after = -1;

    /* 5. net_device_register fails */
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);
    s_inject_net_register_fail = true;
    rc = virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);
    assert_eq(NULL, irq_table[11].handler);
    s_inject_net_register_fail = false;
}

/* ── Test 6: test_reset_unconfirmed_quarantines ───────────────────── */
static void test_reset_unconfirmed_quarantines(void)
{
    TEST_CASE("virtio-net: unconfirmed reset quarantines device DMA pages");
    reset_test_environment();

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x66};
    struct fake_virtio_fixture fix;
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);

    assert_eq(0, virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]));
    assert_true(fix.pdev.driver_data != NULL);

    /* Invalidate reset confirmation: status stays non-zero */
    fix.hw->reset_unconfirmed = true;

    uint32_t free_pages_before = fake_free_pages_count;

    /* Remove device */
    virtio_net_remove(&fix.pdev);

    /* Device must be quarantined, and DMA pages NOT freed */
    assert_true(fix.pdev.dev.quarantined);
    assert_eq(free_pages_before, fake_free_pages_count);
    assert_eq(NULL, irq_table[11].handler);
}

/* ── Test 7: test_budget_and_ownership ────────────────────────────── */
static void test_budget_and_ownership(void)
{
    TEST_CASE("virtio-net: poll_rx respects 64-packet budget and pbuf ownership");
    reset_test_environment();

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x77};
    struct fake_virtio_fixture fix;
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);

    assert_eq(0, virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]));
    struct virtio_net_instance *inst = (struct virtio_net_instance *)fix.pdev.driver_data;

    /* Simulate 64 completed RX packets in used ring */
    for (int i = 0; i < 64; i++) {
        uint32_t di = i;
        uint8_t *buf = (uint8_t *)inst->rx_bufs[di];
        memset(buf, 0, 10 + 64);
        inst->rx_vq.used->ring[i].id = di;
        inst->rx_vq.used->ring[i].len = 10 + 64;
    }
    inst->rx_vq.used->idx = 64;

    /* Budget requested: 30 */
    struct netif fake_adapter;
    memset(&fake_adapter, 0, sizeof(fake_adapter));
    inst->ndev->adapter = &fake_adapter;

    fake_ethernet_input_calls = 0;
    unsigned n = inst->ndev->ops->poll_rx(inst->ndev, 30);
    assert_eq(30, n);
    assert_eq(30, fake_ethernet_input_calls);

    /* Budget requested: 100 (must be clamped to <= 64, here 34 remain) */
    fake_ethernet_input_calls = 0;
    n = inst->ndev->ops->poll_rx(inst->ndev, 100);
    assert_eq(34, n);
    assert_eq(34, fake_ethernet_input_calls);

    /* Verify pbuf ownership when net_receive fails (adapter set to NULL) */
    inst->ndev->adapter = NULL;
    int pbuf_free_before = fake_pbuf_free_calls;

    uint32_t di = inst->rx_vq.avail->ring[0];
    uint8_t *buf = (uint8_t *)inst->rx_bufs[di];
    memset(buf, 0, 10 + 64);
    uint16_t uidx = inst->rx_vq.used->idx;
    inst->rx_vq.used->ring[uidx % inst->rx_qsize].id = di;
    inst->rx_vq.used->ring[uidx % inst->rx_qsize].len = 10 + 64;
    inst->rx_vq.used->idx = uidx + 1;

    n = inst->ndev->ops->poll_rx(inst->ndev, 10);
    assert_eq(1, n);
    assert_true(fake_pbuf_free_calls > pbuf_free_before);

    virtio_net_remove(&fix.pdev);
}

/* ── Test 8: test_handler_ack_wake_without_ring_consumption ────────── */
static void test_handler_ack_wake_without_ring_consumption(void)
{
    TEST_CASE("virtio-net: IRQ handler only acks/wakes, never consumes the ring");
    reset_test_environment();

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x88};
    struct fake_virtio_fixture fix;
    setup_virtio_fixture(&fix, 0, 0xC000, 1, mac);

    assert_eq(0, virtio_net_probe(&fix.pdev, &virtio_net_pci_driver.id_table[0]));
    struct virtio_net_instance *inst = (struct virtio_net_instance *)fix.pdev.driver_data;
    assert_true(inst != NULL);

    /* Stage an RX packet in the used ring */
    uint32_t di = 0;
    uint8_t *buf = (uint8_t *)inst->rx_bufs[di];
    memset(buf, 0xAB, 10 + 64);
    inst->rx_vq.used->ring[0].id = di;
    inst->rx_vq.used->ring[0].len = 10 + 64;
    inst->rx_vq.used->idx = 1;

    /* Set ISR to indicate queue interrupt */
    fix.hw->isr = VIRTIO_ISR_QUEUE_INTR;
    fake_sys_mbox_wake_count = 0;

    /* Fire interrupt handler */
    assert_true(irq_table[inst->gsi].handler != NULL);
    irq_table[inst->gsi].handler(0x30, (uint64_t)(uintptr_t)inst, NULL);

    /* Verify handler acknowledged ISR and called sys_mbox_wake, but DID NOT consume RX ring */
    assert_eq(1, fake_sys_mbox_wake_count);
    assert_eq(0, fix.hw->isr);
    assert_eq(0, inst->rx_vq.last_used_idx);

    /* Now invoke poll_rx in thread context: consumes descriptor and replenishes */
    uint16_t notify_before = fix.hw->q_notify[VIRTIO_NET_RX_QUEUE];
    unsigned consumed = inst->ndev->ops->poll_rx(inst->ndev, 64);
    assert_eq(1, consumed);
    assert_eq(1, inst->rx_vq.last_used_idx);
    assert_true(fix.hw->q_notify[VIRTIO_NET_RX_QUEUE] > notify_before);

    /* Verify TX completion reclamation under virtio_instance_xmit */
    struct pbuf fake_p;
    memset(&fake_p, 0, sizeof(fake_p));
    char payload[32] = "hello";
    fake_p.payload = payload;
    fake_p.len = 5;
    fake_p.tot_len = 5;

    /* Transmit a packet */
    assert_eq(0, inst->ndev->ops->xmit(inst->ndev, &fake_p));
    assert_eq(0, inst->tx_desc_tail);
    assert_eq(1, inst->tx_desc_head);

    /* Simulate device completing the TX descriptor */
    inst->tx_vq.used->idx = 1;

    /* Next transmit should reclaim completed TX descriptors before enqueue */
    assert_eq(0, inst->ndev->ops->xmit(inst->ndev, &fake_p));
    assert_eq(1, inst->tx_desc_tail);
    assert_eq(2, inst->tx_desc_head);

    virtio_net_remove(&fix.pdev);
}

/* ── Main Runner ─────────────────────────────────────────────────── */
int main(void)
{
    printf("\n=== ARCH-9 virtio-net instance and polling test suite ===\n");

    test_success_remove_one_of_two();
    test_two_virtio_instances();
    test_transport_validation();
    test_intx_conflict_poll();
    test_queue_feature_failure();
    test_reset_unconfirmed_quarantines();
    test_budget_and_ownership();
    test_handler_ack_wake_without_ring_consumption();

    printf("\nAll virtio-net tests passed successfully!\n\n");
    return 0;
}

