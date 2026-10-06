/* hosttests/cases/test_e1000_instances.c — ARCH-9 e1000 private instance and IRQ mode tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <net/device.h>
#include <driver/e1000.h>
#include <intr/interrupt.h>
#include <arch/pci.h>
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
uint32_t fake_sys_mbox_wake_count = 0;
uint32_t fake_mmio_write_count = 0;

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
/* Mailbox lock-held observation (Task 9).  Referenced by
 * fake_net_runtime_reset() — only test_net_lwip.c actually uses
 * it; this definition just satisfies the linker. */
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

/* Tracking for IMS enable timing relative to handler registration */
static bool s_ims_written = false;
static bool s_driver_data_set_at_ims = false;
static bool s_handler_registered_at_ims = false;

void test_e1000_mmio_write_hook(void *inst_ptr, uint32_t reg, uint32_t val)
{
    (void)val;
    struct e1000_instance *inst = (struct e1000_instance *)inst_ptr;
    if (reg == E1000_REG_IMS) {
        s_ims_written = true;
        if (inst && inst->pdev && inst->pdev->driver_data == inst) {
            s_driver_data_set_at_ims = true;
        }
        if (inst && inst->irq_owned && irq_table[inst->gsi].handler != NULL) {
            s_handler_registered_at_ims = true;
        }
    }
}

void sys_mbox_wake(void)
{
    fake_sys_mbox_wake_count++;
}

/* ── Simulated Page Allocator ─────────────────────────────────────── */
#define MAX_MOCK_PAGES 64
static struct Page s_mock_pages[MAX_MOCK_PAGES];
static void *s_mock_page_bufs[MAX_MOCK_PAGES];
static bool s_page_in_use[MAX_MOCK_PAGES];

struct Page *alloc_pages(int zone, int count, int flags)
{
    (void)zone; (void)count; (void)flags;
    if (s_inject_alloc_pages_fail) return NULL;
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (!s_page_in_use[i]) {
            s_page_in_use[i] = true;
            if (!s_mock_page_bufs[i]) {
                s_mock_page_bufs[i] = calloc(1, 4096);
            } else {
                memset(s_mock_page_bufs[i], 0, 4096);
            }
            s_mock_pages[i].virt_address = s_mock_page_bufs[i];
            s_mock_pages[i].phy_address = (uint64_t)(uintptr_t)s_mock_page_bufs[i];
            fake_alloc_pages_count++;
            return &s_mock_pages[i];
        }
    }
    return NULL;
}

void free_pages(struct Page *page, int count)
{
    (void)count;
    if (!page) return;
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (s_page_in_use[i] && &s_mock_pages[i] == page) {
            s_page_in_use[i] = false;
            fake_free_pages_count++;
            return;
        }
    }
}

#define MAX_MOCK_4K 128
static void *s_mock_4k_bufs[MAX_MOCK_4K];
static bool s_4k_in_use[MAX_MOCK_4K];

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

/* ── Mock PCI backend & MSI-X mapping ────────────────────────────── */
static uint32_t s_fake_msix_table[16];

int arch_pci_msix_map(const struct pci_device *pdev, uint64_t table_phys, void **out_virt)
{
    (void)pdev; (void)table_phys;
    if (out_virt) *out_virt = s_fake_msix_table;
    return 0;
}

#define MAX_CONFIG_DEVS 4
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

/* ── Fixture Helpers ──────────────────────────────────────────────── */
struct fake_e1000_fixture {
    struct pci_device pdev;
    uint8_t *mmio_space;
};

static void setup_fixture(struct fake_e1000_fixture *fix, uint8_t slot)
{
    memset(fix, 0, sizeof(*fix));
    fix->pdev.dev.id = device_alloc_id();
    snprintf(fix->pdev.dev.name, sizeof(fix->pdev.dev.name), "pci-0000:00:%02x.0", slot);
    fix->pdev.bus = 0;
    fix->pdev.slot = slot;
    fix->pdev.fn = 0;
    fix->pdev.vendor = 0x8086;
    fix->pdev.device = 0x100e;
    fix->pdev.class_code = (PCI_CLASS_NETWORK << 16) | (PCI_SUBCLASS_ETHERNET << 8);

    fix->mmio_space = calloc(1, 0x20000); // 128KB MMIO space
    fix->pdev.bars[0].kind = PCI_BAR_MMIO64;
    fix->pdev.bars[0].valid = true;
    fix->pdev.bars[0].address = (uint64_t)(uintptr_t)fix->mmio_space;

    // Set up MAC address in RAL0/RAH0
    uint32_t ral = 0x12005452; // 52:54:00:12
    uint32_t rah = 0x80005634 | (slot << 8); // 34:(56+slot) with AV bit
    *(volatile uint32_t *)(fix->mmio_space + E1000_REG_RAL0) = ral;
    *(volatile uint32_t *)(fix->mmio_space + E1000_REG_RAH0) = rah;

    // Set up PCI config space for MSI-X support
    uint8_t cidx = slot < MAX_CONFIG_DEVS ? slot : 0;
    memset(s_config_spaces[cidx], 0, sizeof(s_config_spaces[cidx]));
    s_config_spaces[cidx][0x04 / 4] = (1U << 20); // Capabilities list supported
    s_config_spaces[cidx][0x34 / 4] = 0x40;       // Cap pointer
    s_config_spaces[cidx][0x40 / 4] = 0x00000011; // Cap ID 0x11 (MSI-X), next 0
    s_config_spaces[cidx][0x44 / 4] = 0x00000000; // BIR 0, Table offset 0
}

static void teardown_fixture(struct fake_e1000_fixture *fix)
{
    if (fix->mmio_space) {
        free(fix->mmio_space);
        fix->mmio_space = NULL;
    }
}

static void reset_test_state(void)
{
    fake_free_pages_count = 0;
    fake_free_4k_count = 0;
    fake_alloc_pages_count = 0;
    fake_alloc_4k_count = 0;
    fake_sys_mbox_wake_count = 0;
    fake_mmio_write_count = 0;
    s_inject_alloc_pages_fail = false;
    s_inject_alloc_4k_fail_after = -1;
    s_inject_net_register_fail = false;
    s_ims_written = false;
    s_driver_data_set_at_ims = false;
    s_handler_registered_at_ims = false;

    for (int i = 0; i < MAX_MOCK_PAGES; i++) s_page_in_use[i] = false;
    for (int i = 0; i < MAX_MOCK_4K; i++) s_4k_in_use[i] = false;
    for (uint32_t g = 0; g < NR_IQRS; g++) {
        irq_table[g].handler = NULL;
        irq_table[g].controller = NULL;
        irq_table[g].parameter = 0;
    }
    fake_net_runtime_reset();
}

/* ── Test 1: Two independent e1000 instances ─────────────────────── */
static void test_two_e1000_instances(void)
{
    TEST_CASE("Two e1000 instances have independent MMIO/rings/RX queue/TX lock");
    reset_test_state();

    struct fake_e1000_fixture fix1, fix2;
    setup_fixture(&fix1, 1);
    setup_fixture(&fix2, 2);

    int rc1 = e1000_probe(&fix1.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc1);
    int rc2 = e1000_probe(&fix2.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc2);

    struct e1000_instance *inst1 = (struct e1000_instance *)fix1.pdev.driver_data;
    struct e1000_instance *inst2 = (struct e1000_instance *)fix2.pdev.driver_data;

    assert_true(inst1 != NULL);
    assert_true(inst2 != NULL);
    assert_true(inst1 != inst2);
    assert_true(inst1->mmio != inst2->mmio);
    assert_true(inst1->rx_descs != inst2->rx_descs);
    assert_true(inst1->tx_descs != inst2->tx_descs);
    assert_true(inst1->ndev != inst2->ndev);
    assert_true(inst1->ndev->priv == inst1);
    assert_true(inst2->ndev->priv == inst2);
    assert_true(&inst1->tx_lock != &inst2->tx_lock);

    // TX on instance 1 does not affect instance 2
    struct pbuf *p = fake_pbuf_alloc(64);
    assert_true(p != NULL);
    int xmit_rc = inst1->ndev->ops->xmit(inst1->ndev, p);
    assert_eq(0, xmit_rc);
    assert_eq(1, inst1->tx_head);
    assert_eq(0, inst2->tx_head);
    pbuf_free(p);

    e1000_remove(&fix1.pdev);
    e1000_remove(&fix2.pdev);
    teardown_fixture(&fix1);
    teardown_fixture(&fix2);
}

/* ── Test 2: Success remove one of two instances ─────────────────── */
static void test_success_remove_one_of_two(void)
{
    TEST_CASE("Remove one instance cleanly while preserving the other");
    reset_test_state();

    struct fake_e1000_fixture fix1, fix2;
    setup_fixture(&fix1, 1); // slot 1 gets GSI 16 (MSI-X)
    setup_fixture(&fix2, 2); // slot 2 gets GSI 12 (INTx because 16 is occupied)

    int rc1 = e1000_probe(&fix1.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc1);
    int rc2 = e1000_probe(&fix2.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc2);

    struct e1000_instance *inst1 = (struct e1000_instance *)fix1.pdev.driver_data;
    struct e1000_instance *inst2 = (struct e1000_instance *)fix2.pdev.driver_data;
    assert_eq(16, inst1->gsi);
    assert_eq(12, inst2->gsi);
    assert_eq(NIC_MSIX, inst1->irq_mode);
    assert_eq(NIC_INTX, inst2->irq_mode);

    uint32_t frees_before_remove = fake_free_pages_count + fake_free_4k_count;

    // Remove instance 1
    e1000_remove(&fix1.pdev);

    // Verify instance 1 is fully dismantled
    assert_eq(NULL, fix1.pdev.driver_data);
    assert_eq(NULL, irq_table[16].handler); // Only instance 1's IRQ revoked
    uint32_t frees_after_remove = fake_free_pages_count + fake_free_4k_count;
    assert_true(frees_after_remove > frees_before_remove); // DMA freed

    // Verify instance 2 is completely intact
    assert_true(fix2.pdev.driver_data == inst2);
    assert_true(inst2->irq_owned);
    assert_eq(12, inst2->gsi);
    assert_true(irq_table[12].handler != NULL);
    assert_true(inst2->ndev != NULL);
    assert_true(inst2->rx_descs != NULL);

    e1000_remove(&fix2.pdev);
    teardown_fixture(&fix1);
    teardown_fixture(&fix2);
}

/* ── Test 3: Polling fallback and mode selection ─────────────────── */
static void test_poll_mode_does_not_register_irq(void)
{
    TEST_CASE("Fallback to NIC_POLL when MSI-X and INTx slots are both occupied");
    reset_test_state();

    struct fake_e1000_fixture fix;
    setup_fixture(&fix, 1); // candidate INTx GSI is 1 + 10 = 11

    // Pre-occupy slot 16 (first_gsi) with first_handler
    uint32_t first_gsi = 16;
    void (*first_handler)(uint64_t, uint64_t, pt_regs_t *) = (void *)0x11111111;
    irq_table[first_gsi].handler = first_handler;

    // Pre-occupy candidate INTx slot 11 with candidate_handler
    uint32_t intx_gsi = 11;
    void (*cand_handler)(uint64_t, uint64_t, pt_regs_t *) = (void *)0x22222222;
    irq_table[intx_gsi].handler = cand_handler;

    // Probe e1000: both slots occupied -> should enter NIC_POLL
    int rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc);

    struct e1000_instance *inst = (struct e1000_instance *)fix.pdev.driver_data;
    assert_true(inst != NULL);

    enum nic_irq_mode second_irq_mode = inst->irq_mode;
    int second_irq_owned = inst->irq_owned ? 1 : 0;

    assert_eq(NIC_POLL, second_irq_mode);
    assert_eq(0, second_irq_owned);
    assert_eq(first_handler, irq_table[first_gsi].handler);
    assert_eq(cand_handler, irq_table[intx_gsi].handler);

    // Verify IMC disabled device interrupt sources
    uint32_t imc = *(volatile uint32_t *)(fix.mmio_space + E1000_REG_IMC);
    assert_eq(0xFFFFFFFF, imc);

    e1000_remove(&fix.pdev);

    // Second subcase: MSI-X slot 16 occupied, BUT candidate INTx slot 11 is FREE
    irq_table[intx_gsi].handler = NULL;
    rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc);

    inst = (struct e1000_instance *)fix.pdev.driver_data;
    assert_eq(NIC_INTX, inst->irq_mode);
    assert_eq(1, inst->irq_owned);
    assert_eq(11, inst->gsi);
    assert_eq(first_handler, irq_table[first_gsi].handler);

    e1000_remove(&fix.pdev);
    teardown_fixture(&fix);
}

/* ── Test 4: Enable interrupt sources after owner is ready ───────── */
static void test_enable_after_owner_ready(void)
{
    TEST_CASE("IRQ handler and driver_data ready before enabling interrupt sources");
    reset_test_state();

    struct fake_e1000_fixture fix;
    setup_fixture(&fix, 1);

    int rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc);

    struct e1000_instance *inst = (struct e1000_instance *)fix.pdev.driver_data;
    assert_true(inst != NULL);
    assert_true(inst->irq_owned);
    assert_true(s_ims_written);
    assert_true(s_driver_data_set_at_ims);
    assert_true(s_handler_registered_at_ims);
    uint32_t ims = *(volatile uint32_t *)(fix.mmio_space + E1000_REG_IMS);
    assert_true(ims != 0);

    e1000_remove(&fix.pdev);
    teardown_fixture(&fix);
}

/* ── Test 5: Probe failure unwinding ─────────────────────────────── */
static void test_probe_failure_unwinds(void)
{
    TEST_CASE("Probe failure at each step unwinds allocations cleanly");
    reset_test_state();

    struct fake_e1000_fixture fix;
    setup_fixture(&fix, 1);

    // 1. BAR window / mapping failure
    fix.pdev.bars[0].valid = false;
    int rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);
    fix.pdev.bars[0].valid = true;

    // 2. Descriptor page allocation failure
    s_inject_alloc_pages_fail = true;
    rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);
    s_inject_alloc_pages_fail = false;

    // 3. DMA buffer allocation failure halfway
    s_inject_alloc_4k_fail_after = 10;
    rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);
    s_inject_alloc_4k_fail_after = -1;

    // 4. Net device registration failure
    s_inject_net_register_fail = true;
    rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_true(rc != 0);
    assert_eq(NULL, fix.pdev.driver_data);
    assert_eq(NULL, irq_table[16].handler);
    assert_eq(0xFFFFFFFF, *(volatile uint32_t *)(fix.mmio_space + E1000_REG_IMC));
    s_inject_net_register_fail = false;

    teardown_fixture(&fix);
}

/* ── Test 6: Handler only acknowledges / wakes, poll consumes ─────── */
static void test_rx_single_consumer(void)
{
    TEST_CASE("Interrupt handler only acknowledges and wakes; poll_rx consumes ring");
    reset_test_state();

    struct fake_e1000_fixture fix;
    setup_fixture(&fix, 1);

    int rc = e1000_probe(&fix.pdev, &e1000_pci_driver.id_table[0]);
    assert_eq(0, rc);

    struct e1000_instance *inst = (struct e1000_instance *)fix.pdev.driver_data;
    assert_true(inst != NULL);

    // Put packet in RX descriptor 0
    inst->rx_descs[0].length = 64;
    inst->rx_descs[0].status = E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP;
    memset(inst->rx_bufs[0], 0xAB, 64);

    // Set ICR to indicate RX interrupt
    *(volatile uint32_t *)(fix.mmio_space + E1000_REG_ICR) = E1000_ICR_RXT0;

    fake_sys_mbox_wake_count = 0;

    // Fire interrupt handler
    assert_true(irq_table[inst->gsi].handler != NULL);
    irq_table[inst->gsi].handler(0x30, (uint64_t)(uintptr_t)inst, NULL);

    // Verify handler woke receiver but DID NOT consume descriptor
    assert_eq(1, fake_sys_mbox_wake_count);
    assert_eq(E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP, inst->rx_descs[0].status);
    assert_eq(0, inst->rx_tail);

    // Now invoke poll_rx: it must consume the descriptor and advance tail
    unsigned consumed = inst->ndev->ops->poll_rx(inst->ndev, 64);
    assert_eq(1, consumed);
    assert_eq(0, inst->rx_descs[0].status);
    assert_eq(1, inst->rx_tail);

    e1000_remove(&fix.pdev);
    teardown_fixture(&fix);
}

/* ── Test 7: Unsupported device ID rejected ──────────────────────── */
static void test_unsupported_id_no_probe(void)
{
    TEST_CASE("Unsupported device ID is rejected without probing");
    reset_test_state();

    struct fake_e1000_fixture fix;
    setup_fixture(&fix, 1);
    fix.pdev.vendor = 0x8086;
    fix.pdev.device = 0x100f; // unsupported device

    const struct pci_device_id *matched = pci_match_id(&e1000_pci_driver, &fix.pdev);
    assert_eq(NULL, matched);

    int rc = e1000_probe(&fix.pdev, NULL);
    assert_true(rc != 0);

    teardown_fixture(&fix);
}

/* ── Main Runner ─────────────────────────────────────────────────── */
int main(void)
{
    printf("=== ARCH-9 e1000 Private Instance & IRQ Mode Tests ===\n");

    test_two_e1000_instances();
    test_success_remove_one_of_two();
    test_poll_mode_does_not_register_irq();
    test_enable_after_owner_ready();
    test_probe_failure_unwinds();
    test_rx_single_consumer();
    test_unsupported_id_no_probe();

    printf("\n>>> ALL e1000 TESTS PASSED <<<\n");
    return 0;
}
