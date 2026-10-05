/* hosttests/cases/test_pci_backend_irq.c — ARCH-9 PCI backend, resources & IRQ tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <arch/pci.h>
#include <arch/x86_64/pci.h>
#include <intr/interrupt.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define TEST_CASE(name) printf("\n  [TEST] %s\n", name)

/* Host pthread prototypes (avoiding shadow from libc/include/pthread.h) */
typedef unsigned long host_pthread_t;
extern int pthread_create(host_pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
extern int pthread_join(host_pthread_t thread, void **retval);

/* ── Global mock hooks for backend runtime ───────────────────────── */
mock_outd_fn g_mock_outd = NULL;
mock_ind_fn  g_mock_ind  = NULL;

/* ── Memory allocation hook ──────────────────────────────────────── */
void *kmalloc(size_t size)
{
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

/* ── IRQ Controller Mock ─────────────────────────────────────────── */
static uint32_t s_ctrl_enable_count = 0;
static uint32_t s_ctrl_disable_count = 0;
static uint32_t s_ctrl_install_count = 0;
static uint32_t s_ctrl_uninstall_count = 0;

static void mock_ctrl_enable(uint64_t irq)
{
    (void)irq;
    s_ctrl_enable_count++;
}

static void mock_ctrl_disable(uint64_t irq)
{
    (void)irq;
    s_ctrl_disable_count++;
}

static uint64_t mock_ctrl_install(uint64_t irq, void *arg)
{
    (void)irq;
    (void)arg;
    s_ctrl_install_count++;
    return 0;
}

static void mock_ctrl_uninstall(uint64_t irq)
{
    (void)irq;
    s_ctrl_uninstall_count++;
}

static void mock_ctrl_ack(uint64_t irq)
{
    (void)irq;
}

static hw_int_controller_t s_mock_controller = {
    .enable = mock_ctrl_enable,
    .disable = mock_ctrl_disable,
    .install = mock_ctrl_install,
    .uninstall = mock_ctrl_uninstall,
    .ack = mock_ctrl_ack,
};

hw_int_controller_t *arch_irq_select_controller(uint32_t gsi)
{
    if (gsi >= MAX_GSI) {
        return NULL;
    }
    return &s_mock_controller;
}

uint64_t arch_irq_gsi_to_vector(uint32_t gsi)
{
    return 0x20 + gsi;
}

void arch_irq_install(void) {}
void softirq_init(void) {}

/* ── Mock MSI-X Platform Mapping ─────────────────────────────────── */
static bool s_inject_msix_map_fail = false;
static uint32_t s_fake_msix_table[4];
static uint32_t s_msix_map_call_count = 0;

int arch_pci_msix_map(const struct pci_device *pdev, uint64_t table_phys, void **out_virt)
{
    (void)pdev;
    (void)table_phys;
    s_msix_map_call_count++;
    if (s_inject_msix_map_fail) {
        return -ENOMEM;
    }
    if (out_virt) {
        *out_virt = s_fake_msix_table;
    }
    return 0;
}

void arch_pci_msix_unmap(const struct pci_device *pdev, void *virt)
{
    (void)pdev;
    (void)virt;
}

uint32_t arch_pci_msi_address(const struct pci_device *pdev)
{
    (void)pdev;
    return 0xFEE00000;
}

/* ── Test 1: Config Port CF8/CFC Pairing Atomicity ───────────────── */
static int s_cf8_active = 0;
static int s_cf8_interleaved = 0;
static uint32_t s_simulated_dev_reg = 0x12345678;

static void atomicity_mock_outd(uint16_t port, uint32_t value)
{
    if (port == 0xCF8) {
        int prev = __atomic_fetch_add(&s_cf8_active, 1, __ATOMIC_SEQ_CST);
        if (prev != 0) {
            __atomic_fetch_add(&s_cf8_interleaved, 1, __ATOMIC_SEQ_CST);
        }
    } else if (port == 0xCFC) {
        s_simulated_dev_reg = value;
        int active = __atomic_load_n(&s_cf8_active, __ATOMIC_SEQ_CST);
        if (active != 1) {
            __atomic_fetch_add(&s_cf8_interleaved, 1, __ATOMIC_SEQ_CST);
        }
        __atomic_fetch_sub(&s_cf8_active, 1, __ATOMIC_SEQ_CST);
    }
}

static uint32_t atomicity_mock_ind(uint16_t port)
{
    if (port == 0xCFC) {
        int active = __atomic_load_n(&s_cf8_active, __ATOMIC_SEQ_CST);
        if (active != 1) {
            __atomic_fetch_add(&s_cf8_interleaved, 1, __ATOMIC_SEQ_CST);
        }
        __atomic_fetch_sub(&s_cf8_active, 1, __ATOMIC_SEQ_CST);
        return s_simulated_dev_reg;
    }
    return 0;
}

#define THREAD_ITERATIONS 1000
#define NUM_THREADS 4

static void *worker_thread(void *arg)
{
    uintptr_t tid = (uintptr_t)arg;
    const struct pci_backend *backend = arch_pci_backend();
    for (int i = 0; i < THREAD_ITERATIONS; i++) {
        enum pci_error_scope scope = PCI_ERROR_FUNCTION;
        uint32_t val = 0;
        int rc = backend->read32(0, 0, (uint8_t)tid, 0, 0x00, &val, &scope);
        (void)rc;

        rc = backend->write32(0, 0, (uint8_t)tid, 0, 0x04, 0x10000000 + i, &scope);
        (void)rc;
    }
    return NULL;
}

static void test_config_pair_atomicity(void)
{
    TEST_CASE("CF8/CFC pair atomicity under concurrent requests");

    g_mock_outd = atomicity_mock_outd;
    g_mock_ind  = atomicity_mock_ind;
    __atomic_store_n(&s_cf8_active, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_cf8_interleaved, 0, __ATOMIC_SEQ_CST);

    host_pthread_t threads[NUM_THREADS];
    for (uintptr_t t = 0; t < NUM_THREADS; t++) {
        int prc = pthread_create(&threads[t], NULL, worker_thread, (void *)t);
        assert_eq(0, prc);
    }

    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    assert_eq(0, __atomic_load_n(&s_cf8_active, __ATOMIC_SEQ_CST));
    assert_eq(0, __atomic_load_n(&s_cf8_interleaved, __ATOMIC_SEQ_CST));

    g_mock_outd = NULL;
    g_mock_ind  = NULL;
}

/* ── Test 2: IRQ Slot Preserved On Occupied Rejection ────────────── */
static void dummy_handler_a(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)param; (void)regs;
}

static void dummy_handler_b(uint64_t nr, uint64_t param, pt_regs_t *regs)
{
    (void)nr; (void)param; (void)regs;
}

static void test_irq_slot_preserved(void)
{
    TEST_CASE("IRQ slot preserved when registration collides with existing owner");

    uint32_t gsi = 11;
    unregister_irq(gsi);

    s_ctrl_install_count = 0;
    s_ctrl_enable_count = 0;
    s_ctrl_disable_count = 0;
    s_ctrl_uninstall_count = 0;

    void (*first_handler)(uint64_t, uint64_t, pt_regs_t *) = dummy_handler_a;
    void (*second_handler)(uint64_t, uint64_t, pt_regs_t *) = dummy_handler_b;

    int32_t first_registration_result = register_irq(gsi, NULL, first_handler, 0x1111, 0, "driverA");
    assert_eq(1, first_registration_result);
    assert_eq(first_handler, irq_table[gsi].handler);
    assert_eq(0x1111, irq_table[gsi].parameter);
    assert_str_eq("driverA", irq_table[gsi].irq_name);
    assert_eq(1, s_ctrl_install_count);
    assert_eq(1, s_ctrl_enable_count);

    /* Second registration on same slot should be rejected (return 0) without mutating first */
    int32_t second_registration_result = register_irq(gsi, NULL, second_handler, 0x2222, 0, "driverB");
    assert_eq(0, second_registration_result);
    assert_eq(first_handler, irq_table[gsi].handler);
    assert_eq(0x1111, irq_table[gsi].parameter);
    assert_str_eq("driverA", irq_table[gsi].irq_name);
    assert_eq(1, s_ctrl_install_count);
    assert_eq(1, s_ctrl_enable_count);
    assert_eq(0, s_ctrl_disable_count);
    assert_eq(0, s_ctrl_uninstall_count);

    /* After unregistering driverA, driverB can register */
    uint32_t unreg_res = unregister_irq(gsi);
    assert_eq(1, unreg_res);
    assert_eq(NULL, irq_table[gsi].handler);

    int32_t third_registration_result = register_irq(gsi, NULL, second_handler, 0x2222, 0, "driverB");
    assert_eq(1, third_registration_result);
    assert_eq(second_handler, irq_table[gsi].handler);
    assert_eq(0x2222, irq_table[gsi].parameter);
    assert_str_eq("driverB", irq_table[gsi].irq_name);

    unregister_irq(gsi);
}

/* ── Test 3: MSI-X Mapping Failure Isolation ─────────────────────── */
static uint32_t s_fake_config_space[64];

static uint8_t s_mock_cf8_offset = 0;

static void fake_cf8_cfc_outd(uint16_t port, uint32_t value)
{
    if (port == 0xCF8) {
        s_mock_cf8_offset = (uint8_t)(value & 0xFC);
    } else if (port == 0xCFC) {
        s_fake_config_space[s_mock_cf8_offset / 4] = value;
    }
}

static uint32_t fake_cf8_cfc_ind(uint16_t port)
{
    if (port == 0xCFC) {
        return s_fake_config_space[s_mock_cf8_offset / 4];
    }
    return 0;
}

static void test_msix_mapping_failure(void)
{
    TEST_CASE("MSI-X mapping failure does not write table or enable capability");

    g_mock_outd = fake_cf8_cfc_outd;
    g_mock_ind  = fake_cf8_cfc_ind;

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.domain = 0; dev.bus = 0; dev.slot = 1; dev.fn = 0;
    dev.bars[0].kind = PCI_BAR_MMIO32;
    dev.bars[0].address = 0xC0000000ULL;
    dev.bars[0].valid = true;
    dev.bars[0].index = 0;

    memset(s_fake_config_space, 0, sizeof(s_fake_config_space));
    /* Status register bit 4 = 1 (capabilities list) */
    s_fake_config_space[1] = 0x00100000;
    /* Cap pointer at 0x34 -> 0x40 */
    s_fake_config_space[0x34 / 4] = 0x40;
    /* Cap at 0x40: ID 0x11 (MSI-X), next 0, Table size 1 (msg_ctrl = 0) */
    s_fake_config_space[0x40 / 4] = 0x00000011;
    /* Cap at 0x44: BIR 0, Table offset 0x0000 */
    s_fake_config_space[0x44 / 4] = 0x00000000;

    memset(s_fake_msix_table, 0xAA, sizeof(s_fake_msix_table));

    /* Case A: Injected mapping failure */
    s_inject_msix_map_fail = true;
    int rc = pci_msix_enable(&dev, 0x45);
    assert_true(rc < 0);
    assert_eq(1, s_msix_map_call_count);

    /* Table memory remains untouched */
    assert_eq(0xAAAAAAAAU, s_fake_msix_table[0]);
    assert_eq(0xAAAAAAAAU, s_fake_msix_table[1]);
    assert_eq(0xAAAAAAAAU, s_fake_msix_table[2]);
    assert_eq(0xAAAAAAAAU, s_fake_msix_table[3]);

    /* MSI-X Enable bit (bit 31 of cap dword at 0x40) remains 0 */
    assert_eq(0, s_fake_config_space[0x40 / 4] & 0x80000000U);

    /* Case B: Mapping success */
    s_inject_msix_map_fail = false;
    rc = pci_msix_enable(&dev, 0x45);
    assert_eq(0, rc);
    assert_eq(2, s_msix_map_call_count);

    /* Table entry 0 is populated */
    assert_eq(0xFEE00000U, s_fake_msix_table[0]); /* address */
    assert_eq(0x00000000U, s_fake_msix_table[1]); /* upper address */
    assert_eq(0x00000045U, s_fake_msix_table[2]); /* vector */
    assert_eq(0x00000000U, s_fake_msix_table[3]); /* unmasked */

    /* MSI-X Enable bit is set */
    assert_true((s_fake_config_space[0x40 / 4] & 0x80000000U) != 0);
}

/* ── Test 4: Disable Interrupt Sources ───────────────────────────── */
static void test_disable_interrupt_sources(void)
{
    TEST_CASE("Disable interrupt sources clears MSI, MSI-X and sets INTx disable");

    g_mock_outd = fake_cf8_cfc_outd;
    g_mock_ind  = fake_cf8_cfc_ind;

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));

    memset(s_fake_config_space, 0, sizeof(s_fake_config_space));
    /* Command reg: INTx enabled (bit 10=0), BusMaster=1 (bit 2=1) */
    s_fake_config_space[1] = 0x00100004; /* Status bit 4=1 (has caps), Command bit 2=1 */
    s_fake_config_space[0x34 / 4] = 0x40; /* Cap ptr = 0x40 */

    /* Cap 0x40: MSI (ID 0x05), next 0x50, Message Control bit 0=1 (MSI enable) -> bit 16=1 */
    s_fake_config_space[0x40 / 4] = 0x00015005;

    /* Cap 0x50: MSI-X (ID 0x11), next 0, Message Control bit 15=1 (MSI-X enable) -> bit 31=1 */
    s_fake_config_space[0x50 / 4] = 0x80000011;

    int rc = pci_interrupts_disable(&dev);
    assert_eq(0, rc);

    /* MSI enable bit cleared */
    assert_eq(0, s_fake_config_space[0x40 / 4] & (1U << 16));
    /* MSI-X enable bit cleared */
    assert_eq(0, s_fake_config_space[0x50 / 4] & (1U << 31));
    /* INTx disable bit set (bit 10 of Command Register, i.e. 1 << 10) */
    assert_true((s_fake_config_space[1] & (1U << 10)) != 0);
}

/* ── Test 5: Capability Traversal Cycles and Bounds ───────────────── */
static void test_capability_cycle(void)
{
    TEST_CASE("Capability traversal handles cycles and malformed pointers boundedly");

    g_mock_outd = fake_cf8_cfc_outd;
    g_mock_ind  = fake_cf8_cfc_ind;

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));

    /* Subcase 1: Cap pointer < 0x40 (malformed) */
    memset(s_fake_config_space, 0, sizeof(s_fake_config_space));
    s_fake_config_space[1] = 0x00100000;
    s_fake_config_space[0x34 / 4] = 0x20; /* malformed < 0x40 */
    int rc = pci_interrupts_disable(&dev);
    assert_true(rc < 0);

    /* Subcase 2: Cap pointer unaligned */
    memset(s_fake_config_space, 0, sizeof(s_fake_config_space));
    s_fake_config_space[1] = 0x00100000;
    s_fake_config_space[0x34 / 4] = 0x42; /* unaligned */
    rc = pci_interrupts_disable(&dev);
    assert_true(rc < 0);

    /* Subcase 3: Cycle A -> B -> A */
    memset(s_fake_config_space, 0, sizeof(s_fake_config_space));
    s_fake_config_space[1] = 0x00100000;
    s_fake_config_space[0x34 / 4] = 0x40;
    s_fake_config_space[0x40 / 4] = 0x00005009; /* next = 0x50 */
    s_fake_config_space[0x50 / 4] = 0x00004009; /* next = 0x40 (cycle!) */
    rc = pci_interrupts_disable(&dev);
    assert_true(rc < 0);

    /* Subcase 4: Self loop */
    memset(s_fake_config_space, 0, sizeof(s_fake_config_space));
    s_fake_config_space[1] = 0x00100000;
    s_fake_config_space[0x34 / 4] = 0x40;
    s_fake_config_space[0x40 / 4] = 0x00004009; /* next = 0x40 */
    rc = pci_interrupts_disable(&dev);
    assert_true(rc < 0);
}

/* ── Test 6: BAR Space Validation ────────────────────────────────── */
static void test_bar_space_validation(void)
{
    TEST_CASE("BAR window validates type spaces, non-zero length and prevents mixing");

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));

    /* BAR 0: I/O at 0x2000 */
    dev.bars[0].kind = PCI_BAR_IO;
    dev.bars[0].address = 0x2000;
    dev.bars[0].valid = true;
    dev.bars[0].index = 0;

    /* BAR 1: MMIO32 at 0xD0000000 */
    dev.bars[1].kind = PCI_BAR_MMIO32;
    dev.bars[1].address = 0xD0000000;
    dev.bars[1].valid = true;
    dev.bars[1].index = 1;

    /* BAR 2: MMIO64 at 0x100000000 */
    dev.bars[2].kind = PCI_BAR_MMIO64;
    dev.bars[2].address = 0x100000000ULL;
    dev.bars[2].valid = true;
    dev.bars[2].index = 2;

    /* BAR 3: UPPER slot corresponding to BAR 2 */
    dev.bars[3].kind = PCI_BAR_UPPER;
    dev.bars[3].address = 0x100000000ULL;
    dev.bars[3].valid = false;
    dev.bars[3].index = 3;

    uint64_t phys = 0;

    /* 1. I/O matching succeeds */
    int rc = pci_bar_window(&dev, 0, PCI_BAR_IO, 0x10, 0x20, &phys);
    assert_eq(0, rc);
    assert_eq(0x2010ULL, phys);

    /* 2. Requesting MMIO on an I/O BAR fails */
    rc = pci_bar_window(&dev, 0, PCI_BAR_MMIO32, 0, 0x20, &phys);
    assert_true(rc < 0);
    rc = pci_bar_window(&dev, 0, PCI_BAR_MMIO64, 0, 0x20, &phys);
    assert_true(rc < 0);

    /* 3. Requesting I/O on an MMIO BAR fails */
    rc = pci_bar_window(&dev, 1, PCI_BAR_IO, 0, 0x20, &phys);
    assert_true(rc < 0);
    rc = pci_bar_window(&dev, 2, PCI_BAR_IO, 0, 0x20, &phys);
    assert_true(rc < 0);

    /* 4. UPPER slot cannot be used directly */
    rc = pci_bar_window(&dev, 3, PCI_BAR_MMIO64, 0, 0x20, &phys);
    assert_true(rc < 0);
    rc = pci_bar_window(&dev, 3, PCI_BAR_UPPER, 0, 0x20, &phys);
    assert_true(rc < 0);

    /* 5. Zero length is rejected */
    rc = pci_bar_window(&dev, 1, PCI_BAR_MMIO32, 0, 0, &phys);
    assert_true(rc < 0);

    /* 6. Invalid required kinds */
    rc = pci_bar_window(&dev, 1, PCI_BAR_NONE, 0, 0x10, &phys);
    assert_true(rc < 0);
    rc = pci_bar_window(&dev, 1, PCI_BAR_UPPER, 0, 0x10, &phys);
    assert_true(rc < 0);

    /* 7. Integer overflow in offset + length */
    rc = pci_bar_window(&dev, 1, PCI_BAR_MMIO32, UINT64_MAX - 10, 20, &phys);
    assert_true(rc < 0);

    /* 8. Address overflow */
    rc = pci_bar_window(&dev, 2, PCI_BAR_MMIO64, UINT64_MAX - dev.bars[2].address, 0x10, &phys);
    assert_true(rc < 0);

    /* 9. Invalid index */
    rc = pci_bar_window(&dev, 6, PCI_BAR_MMIO32, 0, 0x10, &phys);
    assert_true(rc < 0);

    /* 10. Unpopulated / invalid BAR */
    rc = pci_bar_window(&dev, 4, PCI_BAR_MMIO32, 0, 0x10, &phys);
    assert_true(rc < 0);

    /* 11. MMIO32 and MMIO64 valid windows */
    rc = pci_bar_window(&dev, 1, PCI_BAR_MMIO32, 0x20, 0x100, &phys);
    assert_eq(0, rc);
    assert_eq(0xD0000020ULL, phys);

    rc = pci_bar_window(&dev, 2, PCI_BAR_MMIO64, 0x50, 0x200, &phys);
    assert_eq(0, rc);
    assert_eq(0x100000050ULL, phys);
}

/* ── Test 7: Strongly Typed Wrappers ─────────────────────────────── */
static void test_pci_wrappers(void)
{
    TEST_CASE("PCI strongly typed wrappers manipulate command register and GSI routing");

    g_mock_outd = fake_cf8_cfc_outd;
    g_mock_ind  = fake_cf8_cfc_ind;

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.slot = 3;

    memset(s_fake_config_space, 0, sizeof(s_fake_config_space));
    s_fake_config_space[1] = 0x00000000;

    /* Bus master enable & disable */
    int rc = pci_set_bus_master(&dev, true);
    assert_eq(0, rc);
    assert_true((s_fake_config_space[1] & (1U << 2)) != 0);

    rc = pci_set_bus_master(&dev, false);
    assert_eq(0, rc);
    assert_eq(0, s_fake_config_space[1] & (1U << 2));

    /* Decode enable & disable */
    rc = pci_set_decode(&dev, true, true);
    assert_eq(0, rc);
    assert_true((s_fake_config_space[1] & 0x03) == 0x03);

    rc = pci_set_decode(&dev, false, true);
    assert_eq(0, rc);
    assert_true((s_fake_config_space[1] & 0x03) == 0x02);

    /* INTx enable & disable */
    rc = pci_set_intx(&dev, false);
    assert_eq(0, rc);
    assert_true((s_fake_config_space[1] & (1U << 10)) != 0);

    rc = pci_set_intx(&dev, true);
    assert_eq(0, rc);
    assert_eq(0, s_fake_config_space[1] & (1U << 10));

    /* GSI route with backend */
    s_fake_config_space[0x3C / 4] = 0x0000010B; /* pin=1 (INTA), line=11 */
    uint32_t gsi = 0;
    rc = pci_route_gsi(&dev, &gsi);
    assert_eq(0, rc);
    /* Q35 formula: 16 + ((slot + pin - 1) & 3) = 16 + ((3 + 1 - 1) & 3) = 16 + 3 = 19 */
    assert_eq(19, gsi);

    /* Firmware pre-assigned line >= 16 */
    s_fake_config_space[0x3C / 4] = 0x00000115; /* line=21 */
    rc = pci_route_gsi(&dev, &gsi);
    assert_eq(0, rc);
    assert_eq(21, gsi);

    /* Pin=0 returns line */
    s_fake_config_space[0x3C / 4] = 0x0000000A; /* pin=0, line=10 */
    rc = pci_route_gsi(&dev, &gsi);
    assert_eq(0, rc);
    assert_eq(10, gsi);
}

int main(void)
{
    TEST_SUITE("ARCH-9 PCI Backend, Resource Access & IRQ Ownership");

    test_config_pair_atomicity();
    test_irq_slot_preserved();
    test_msix_mapping_failure();
    test_disable_interrupt_sources();
    test_capability_cycle();
    test_bar_space_validation();
    test_pci_wrappers();

    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
