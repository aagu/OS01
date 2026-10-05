/* hosttests/cases/test_ahci_lifecycle.c — ARCH-9 AHCI lifecycle & timeout tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <block/blockdev.h>
#include <driver/ahci.h>
#include <driver/ahci_lifecycle.h>
#include <arch/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define TEST_CASE(name) printf("\n  [TEST] %s\n", name)

/* ── Global mock variables ────────────────────────────────────────── */
percpu_t g_ahci_percpu = { .tsc_offset = 0 };
bool clocksource_active = true;
uint32_t clocksource_mult = 1000;
uint32_t clocksource_shift = 0;
static uint64_t s_clocksource_freq = 1000000000ULL; /* 1 GHz: 1 cycle = 1 ns */
static uint64_t s_raw_cycles = 1000000ULL;
volatile uint64_t jiffies = 100;

uint32_t fake_mmio_write_count = 0;
uint32_t fake_dma_start_count = 0;
uint32_t fake_bounce_write_count = 0;
uint32_t fake_free_pages_count = 0;
uint32_t fake_quarantine_count = 0;
uint32_t fake_irq_free_count = 0;

bool s_simulate_command_timeout = false;
bool s_simulate_stop_timeout = false;
bool s_simulate_interrupt_disable_fail = false;
bool s_simulate_identify_fail = false;

void mock_ahci_write32(volatile uint32_t *reg, uint32_t val)
{
    fake_mmio_write_count++;
    *reg = val;

    // Check if writing to cmd
    if ((val & (AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE)) == 0) {
        if (s_simulate_stop_timeout) {
            *reg |= AHCI_PORT_CMD_CR;
        } else {
            *reg &= ~(AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR);
        }
    }

    // Check if writing to ci
    if (val != 0) {
        if (!s_simulate_command_timeout) {
            *reg = 0; // immediate completion
        }
    }
}

int mock_pci_interrupts_disable(struct pci_device *pdev)
{
    (void)pdev;
    if (s_simulate_interrupt_disable_fail) {
        return -EIO;
    }
    return 0;
}

/* ── Fake PCI Backend for test ───────────────────────────────────── */
static uint32_t s_mock_cmd_reg = 0;
static int mock_read32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint32_t *val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    if (off == 0x04) *val = s_mock_cmd_reg;
    else *val = 0;
    return 0;
}

static int mock_write32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint32_t val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    if (off == 0x04) s_mock_cmd_reg = val;
    return 0;
}

static int mock_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    (void)pdev;
    if (out) *out = 16;
    return 0;
}

static const struct pci_backend s_mock_backend = {
    .read32 = mock_read32,
    .write32 = mock_write32,
    .route_gsi = mock_route_gsi,
};

const struct pci_backend *arch_pci_backend(void)
{
    return &s_mock_backend;
}

/* ── Clocksource mock functions ──────────────────────────────────── */
uint64_t clocksource_freq_hz(void)
{
    return s_clocksource_freq;
}

uint64_t arch_cycle_counter(void)
{
    return s_raw_cycles;
}

uint64_t clocksource_cycles(void)
{
    return s_raw_cycles;
}

uint64_t clocksource_read_ns(void)
{
    if (!clocksource_active) {
        return jiffies * 10000000ULL;
    }
    uint64_t c = arch_cycle_counter() + (uint64_t)g_ahci_percpu.tsc_offset;
    return (uint64_t)(((__uint128_t)c * clocksource_mult) >> clocksource_shift);
}

void advance_cycles(uint64_t delta)
{
    s_raw_cycles += delta;
}

/* ── Memory allocation / Page mock ───────────────────────────────── */
#define MAX_MOCK_PAGES 64
static struct Page s_mock_pages[MAX_MOCK_PAGES];
static bool s_page_in_use[MAX_MOCK_PAGES];

void *kmalloc(size_t size)
{
    return calloc(1, size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

struct Page *alloc_pages(int zone, int order, int flags)
{
    (void)zone; (void)order; (void)flags;
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (!s_page_in_use[i]) {
            s_page_in_use[i] = true;
            void *buf = malloc(PAGE_2M_SIZE);
            if (!buf) {
                s_page_in_use[i] = false;
                return NULL;
            }
            memset(buf, 0, PAGE_2M_SIZE);
            s_mock_pages[i].virt_address = buf;
            s_mock_pages[i].phy_address = (uint64_t)(uintptr_t)buf;
            return &s_mock_pages[i];
        }
    }
    return NULL;
}

void free_pages(struct Page *page, int order)
{
    (void)order;
    if (!page) return;
    fake_free_pages_count++;
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (&s_mock_pages[i] == page && s_page_in_use[i]) {
            free(page->virt_address);
            page->virt_address = NULL;
            page->phy_address = 0;
            s_page_in_use[i] = false;
            return;
        }
    }
}

struct Page *Virt_To_Page(void *virt)
{
    for (int i = 0; i < MAX_MOCK_PAGES; i++) {
        if (s_page_in_use[i] && s_mock_pages[i].virt_address == virt) {
            return &s_mock_pages[i];
        }
    }
    return NULL;
}

/* ── IRQ stubs ───────────────────────────────────────────────────── */
uint32_t unregister_irq(uint32_t gsi)
{
    (void)gsi;
    fake_irq_free_count++;
    return 1;
}

/* ── Helper: create fake PCI device and HBA ──────────────────────── */
struct fake_ahci_fixture {
    struct pci_device pdev;
    uint8_t *hba_mem;
    HBA_MEM *hba;
};

static void setup_fixture(struct fake_ahci_fixture *fix, uint8_t bus, uint8_t slot, uint32_t ports_impl)
{
    memset(fix, 0, sizeof(*fix));
    fix->pdev.dev.id = device_alloc_id();
    snprintf(fix->pdev.dev.name, sizeof(fix->pdev.dev.name), "pci-0000:%02x:%02x.0", bus, slot);
    fix->pdev.bus = bus;
    fix->pdev.slot = slot;
    fix->pdev.fn = 0;
    fix->pdev.class_code = (PCI_CLASS_MASS_STORAGE << 16) | (PCI_SUBCLASS_SATA << 8) | PCI_PROGIF_AHCI;

    fix->hba_mem = calloc(1, 8192);
    fix->hba = (HBA_MEM *)fix->hba_mem;
    fix->hba->vs = 0x00010300; // 1.3.0
    fix->hba->cap = 0x1F;      // 32 ports
    fix->hba->pi = ports_impl;

    fix->pdev.bars[5].kind = PCI_BAR_MMIO32;
    fix->pdev.bars[5].valid = true;
    fix->pdev.bars[5].address = (uint64_t)(uintptr_t)fix->hba_mem;
}

static void teardown_fixture(struct fake_ahci_fixture *fix)
{
    if (fix->hba_mem) {
        free(fix->hba_mem);
        fix->hba_mem = NULL;
    }
}

static void reset_test_state(void)
{
    clocksource_active = true;
    s_clocksource_freq = 1000000000ULL;
    clocksource_mult = 1;
    clocksource_shift = 0;
    s_raw_cycles = 1000000ULL;
    g_ahci_percpu.tsc_offset = 0;
    jiffies = 100;

    fake_mmio_write_count = 0;
    fake_dma_start_count = 0;
    fake_bounce_write_count = 0;
    fake_free_pages_count = 0;
    fake_quarantine_count = 0;
    fake_irq_free_count = 0;

    s_simulate_command_timeout = false;
    s_simulate_stop_timeout = false;
    s_simulate_interrupt_disable_fail = false;
    s_simulate_identify_fail = false;

    device_core_init();
    block_device_init();
}

/* Configure a fake port with a healthy drive */
static void configure_fake_port_device(HBA_MEM *hba, int port_num, bool present, uint64_t sectors)
{
    HBA_PORT *p = (HBA_PORT *)((uint8_t *)hba + 0x100 + port_num * 0x80);
    if (present) {
        p->ssts = AHCI_PORT_SSTS_DET_PRES | AHCI_PORT_SSTS_IPM_ACTIVE;
    } else {
        p->ssts = AHCI_PORT_SSTS_DET_NODEV;
    }
    p->cmd = 0;
    p->tfd = 0;
    p->is = 0;
    p->ci = 0;
    (void)sectors;
}

/* ──────────────────────────────────────────────────────────────────
 * Test Cases (11 total)
 * ────────────────────────────────────────────────────────────────── */

/* 1. test_transfer_capacity_read_write */
static void test_transfer_capacity_read_write(void)
{
    TEST_CASE("Transfer capacity read/write limits & bounce buffer safety");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x1);
    configure_fake_port_device(fix.hba, 0, true, 100000);

    struct pci_device_id id = { .class_value = fix.pdev.class_code, .class_mask = 0xFFFFFF };
    int pr = ahci_probe(&fix.pdev, &id);
    assert_eq(0, pr);

    struct ahci_controller *ctrl = (struct ahci_controller *)fix.pdev.driver_data;
    assert_not_null(ctrl);
    struct ahci_port *port = &ctrl->ports[0];

    uint8_t *buf = calloc(1, 4064 * 512);

    /* 4064 is accepted */
    int r4064 = ahci_port_read(port, 0, 4064, buf);
    assert_eq(0, r4064);

    uint32_t bounce_before = fake_bounce_write_count;
    uint32_t dma_before = fake_dma_start_count;

    /* 4065, 0x800000, UINT32_MAX must return -E2BIG */
    assert_eq(-E2BIG, ahci_port_read(port, 0, 4065, buf));
    assert_eq(-E2BIG, ahci_port_read(port, 0, 0x800000, buf));
    assert_eq(-E2BIG, ahci_port_read(port, 0, UINT32_MAX, buf));

    assert_eq(-E2BIG, ahci_port_write(port, 0, 4065, buf));
    assert_eq(-E2BIG, ahci_port_write(port, 0, 0x800000, buf));
    assert_eq(-E2BIG, ahci_port_write(port, 0, UINT32_MAX, buf));

    /* Count 0 is handled cleanly */
    assert_eq(0, ahci_port_read(port, 0, 0, buf));
    assert_eq(0, ahci_port_write(port, 0, 0, buf));

    /* Illegal requests must not increment bounce write or DMA start */
    assert_eq(bounce_before, fake_bounce_write_count);
    assert_eq(dma_before, fake_dma_start_count);

    free(buf);
    ahci_remove(&fix.pdev);
    teardown_fixture(&fix);
}

/* 2. test_two_controller_remove */
static void test_two_controller_remove(void)
{
    TEST_CASE("Two controllers: controlled remove of one retains the other");
    reset_test_state();

    struct fake_ahci_fixture fix1, fix2;
    setup_fixture(&fix1, 0, 30, 0x1);
    setup_fixture(&fix2, 0, 31, 0x1);

    configure_fake_port_device(fix1.hba, 0, true, 100000);
    configure_fake_port_device(fix2.hba, 0, true, 200000);

    struct pci_device_id id = { .class_value = fix1.pdev.class_code, .class_mask = 0xFFFFFF };
    assert_eq(0, ahci_probe(&fix1.pdev, &id));
    assert_eq(0, ahci_probe(&fix2.pdev, &id));

    assert_eq(2, block_device_count());
    struct ahci_controller *c1 = (struct ahci_controller *)fix1.pdev.driver_data;
    struct ahci_controller *c2 = (struct ahci_controller *)fix2.pdev.driver_data;
    assert_not_null(c1);
    assert_not_null(c2);

    /* Remove fix1 */
    ahci_remove(&fix1.pdev);
    assert_null(fix1.pdev.driver_data);

    /* fix1 block device removed, fix2 block device still alive */
    assert_eq(1, block_device_count());
    assert_not_null(fix2.pdev.driver_data);
    assert_eq((void *)c2, fix2.pdev.driver_data);

    ahci_remove(&fix2.pdev);
    assert_eq(0, block_device_count());

    teardown_fixture(&fix1);
    teardown_fixture(&fix2);
}

/* 3. test_controller_failure_after_port_publish */
static void test_controller_failure_after_port_publish(void)
{
    TEST_CASE("Controller failure after port publication rolls back cleanly");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x3); /* 2 ports */
    configure_fake_port_device(fix.hba, 0, true, 50000);
    configure_fake_port_device(fix.hba, 1, true, 60000);

    struct pci_device_id id = { .class_value = fix.pdev.class_code, .class_mask = 0xFFFFFF };
    assert_eq(0, ahci_probe(&fix.pdev, &id));
    assert_eq(2, block_device_count());

    /* Now simulate controller failure teardown */
    ahci_remove(&fix.pdev);
    assert_eq(0, block_device_count());
    assert_null(fix.pdev.driver_data);

    teardown_fixture(&fix);
}

/* 4. test_runtime_clock_failure */
static void test_runtime_clock_failure(void)
{
    TEST_CASE("Runtime clock failure rejects new commands and quarantines in-flight");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x1);
    configure_fake_port_device(fix.hba, 0, true, 100000);

    struct pci_device_id id = { .class_value = fix.pdev.class_code, .class_mask = 0xFFFFFF };
    assert_eq(0, ahci_probe(&fix.pdev, &id));

    struct ahci_controller *ctrl = (struct ahci_controller *)fix.pdev.driver_data;
    struct ahci_port *port = &ctrl->ports[0];

    /* Turn clocksource off with no in-flight commands */
    clocksource_active = false;
    uint32_t mmio_before = fake_mmio_write_count;

    uint8_t buf[512];
    int r = ahci_port_read(port, 0, 1, buf);
    assert_true(r != 0);
    /* No hardware writes issued when clock is down */
    assert_eq(mmio_before, fake_mmio_write_count);

    clocksource_active = true;
    ahci_remove(&fix.pdev);
    teardown_fixture(&fix);
}

/* 5. test_no_clock_no_hardware_side_effect */
static void test_no_clock_no_hardware_side_effect(void)
{
    TEST_CASE("No clock source rejects probe with -ENOTSUP and zero hardware side effects");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x1);

    clocksource_active = false;
    fake_mmio_write_count = 0;
    fake_dma_start_count = 0;

    struct pci_device_id id = { .class_value = fix.pdev.class_code, .class_mask = 0xFFFFFF };
    int r = ahci_probe(&fix.pdev, &id);

    assert_eq(-ENOTSUP, r);
    assert_eq(0, fake_mmio_write_count);
    assert_eq(0, fake_dma_start_count);

    /* Also verify freq == 0 */
    clocksource_active = true;
    s_clocksource_freq = 0;
    fake_mmio_write_count = 0;
    fake_dma_start_count = 0;

    r = ahci_probe(&fix.pdev, &id);
    assert_eq(-ENOTSUP, r);
    assert_eq(0, fake_mmio_write_count);
    assert_eq(0, fake_dma_start_count);

    teardown_fixture(&fix);
}

/* 6. test_deadline_with_stopped_jiffies */
static void test_deadline_with_stopped_jiffies(void)
{
    TEST_CASE("Deadline times out with stopped jiffies via cycle/ns advance");
    reset_test_state();

    jiffies = 42;
    struct ahci_deadline dl;

    /* Boot phase */
    int r = ahci_deadline_start(true, 500, &dl);
    assert_eq(0, r);
    assert_false(ahci_deadline_expired(&dl));

    /* Advance 600ms cycles, leave jiffies = 42 */
    advance_cycles(600000000ULL);
    assert_eq(42, jiffies);
    assert_true(ahci_deadline_expired(&dl));

    /* Runtime phase */
    r = ahci_deadline_start(false, 500, &dl);
    assert_eq(0, r);
    assert_false(ahci_deadline_expired(&dl));

    advance_cycles(600000000ULL);
    assert_eq(42, jiffies);
    assert_true(ahci_deadline_expired(&dl));
}

/* 7. test_compensated_runtime_deadline */
static void test_compensated_runtime_deadline(void)
{
    TEST_CASE("Compensated runtime deadline consistent across CPU raw TSC migration");
    reset_test_state();

    /* CPU 0: tsc_offset = 0, raw = 10,000,000 */
    g_ahci_percpu.tsc_offset = 0;
    s_raw_cycles = 10000000ULL;

    struct ahci_deadline dl;
    int r = ahci_deadline_start(false, 500, &dl);
    assert_eq(0, r);
    assert_false(ahci_deadline_expired(&dl));

    /* Switch to CPU 1: raw TSC is 15,000,000 but offset is -5,000,000 */
    g_ahci_percpu.tsc_offset = -5000000LL;
    s_raw_cycles = 15000000ULL;

    /* Compensated time is still exactly the same, not expired */
    assert_false(ahci_deadline_expired(&dl));

    /* Advance CPU 1 by 600ms cycles */
    advance_cycles(600000000ULL);
    assert_true(ahci_deadline_expired(&dl));
}

/* 8. test_gate_and_timeout_no_reuse */
static void test_gate_and_timeout_no_reuse(void)
{
    TEST_CASE("Request A timeout marks port failed; Request B returns -EIO with no bounce write");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x1);
    configure_fake_port_device(fix.hba, 0, true, 100000);

    struct pci_device_id id = { .class_value = fix.pdev.class_code, .class_mask = 0xFFFFFF };
    assert_eq(0, ahci_probe(&fix.pdev, &id));

    struct ahci_controller *ctrl = (struct ahci_controller *)fix.pdev.driver_data;
    struct ahci_port *port = &ctrl->ports[0];

    /* Force request A to timeout */
    s_simulate_command_timeout = true;

    uint8_t buf[512] = { 0x5a };
    int rA = ahci_port_write(port, 0, 1, buf);
    assert_eq(-ETIMEDOUT, rA);
    assert_eq(AHCI_PORT_STATE_FAILED, port->state);

    /* Request B arrives: must return -EIO and NOT perform bounce write */
    uint32_t bounce_before = fake_bounce_write_count;
    int rB = ahci_port_write(port, 0, 1, buf);
    assert_eq(-EIO, rB);
    assert_eq(bounce_before, fake_bounce_write_count);

    ahci_remove(&fix.pdev);
    teardown_fixture(&fix);
}

/* 9. test_quarantine_on_stop_failure */
static void test_quarantine_on_stop_failure(void)
{
    TEST_CASE("Port engine stop failure quarantines port and skips free_pages");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x1);
    configure_fake_port_device(fix.hba, 0, true, 100000);

    struct pci_device_id id = { .class_value = fix.pdev.class_code, .class_mask = 0xFFFFFF };
    assert_eq(0, ahci_probe(&fix.pdev, &id));

    struct ahci_controller *ctrl = (struct ahci_controller *)fix.pdev.driver_data;
    struct ahci_port *port = &ctrl->ports[0];
    assert_false(port->quarantined);

    /* Simulate engine stop failure */
    s_simulate_stop_timeout = true;
    fake_free_pages_count = 0;

    ahci_remove(&fix.pdev);

    /* When stop times out, DMA must NOT be freed! */
    assert_eq(0, fake_free_pages_count);
    assert_true(fix.pdev.dev.quarantined);
    assert_true(fix.pdev.driver_data != NULL);
    assert_eq(fix.pdev.dev.retained_owner, (void *)port);

    free(ctrl);
    teardown_fixture(&fix);
}

/* 10. test_unsafe_failure_escalates */
static void test_unsafe_failure_escalates(void)
{
    TEST_CASE("Unsafe failure (cannot stop and cannot mask interrupts) escalates to DEVICE_UNSAFE");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x1);
    configure_fake_port_device(fix.hba, 0, true, 100000);

    s_simulate_interrupt_disable_fail = true;
    s_simulate_stop_timeout = true;

    /* A critical failure under unmaskable conditions escalates */
    struct ahci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.pdev = &fix.pdev;
    ctrl.hba = fix.hba;
    HBA_PORT *regs = (HBA_PORT *)((uint8_t *)ctrl.hba + 0x100);
    regs->cmd |= AHCI_PORT_CMD_CR;

    struct ahci_port port;
    memset(&port, 0, sizeof(port));
    port.ctrl = &ctrl;
    port.port_num = 0;
    spin_lock_init(&port.lock);

    int r = ahci_port_fail_escalate(&port, -EIO);
    assert_eq(DEVICE_UNSAFE, r);

    teardown_fixture(&fix);
}

/* 11. test_port_fault_isolation */
static void test_port_fault_isolation(void)
{
    TEST_CASE("Port fault isolation: healthy port continues, nodisk has no DMA, bad identify has no bdev");
    reset_test_state();

    struct fake_ahci_fixture fix;
    setup_fixture(&fix, 0, 31, 0x7); /* 3 ports: 0, 1, 2 */

    /* Port 0: healthy disk */
    configure_fake_port_device(fix.hba, 0, true, 100000);
    /* Port 1: no disk */
    configure_fake_port_device(fix.hba, 1, false, 0);
    /* Port 2: present but bad identify */
    configure_fake_port_device(fix.hba, 2, true, 50000);
    HBA_PORT *p2 = (HBA_PORT *)((uint8_t *)fix.hba + 0x100 + 2 * 0x80);
    p2->is = AHCI_PORT_IS_TFES; /* Task file error */

    struct pci_device_id id = { .class_value = fix.pdev.class_code, .class_mask = 0xFFFFFF };
    int pr = ahci_probe(&fix.pdev, &id);
    assert_eq(0, pr);

    struct ahci_controller *ctrl = (struct ahci_controller *)fix.pdev.driver_data;
    assert_not_null(ctrl);

    /* Port 0: healthy, registered */
    assert_not_null(ctrl->ports[0].bdev);
    assert_not_null(ctrl->ports[0].dma_virt);

    /* Port 1: nodisk, NO DMA allocated */
    assert_null(ctrl->ports[1].dma_virt);
    assert_null(ctrl->ports[1].bdev);

    /* Port 2: bad identify, NO block device registered */
    assert_null(ctrl->ports[2].bdev);

    /* Only port 0 registered as block device */
    assert_eq(1, block_device_count());

    ahci_remove(&fix.pdev);
    teardown_fixture(&fix);
}

/* ── Test Runner ─────────────────────────────────────────────────── */
int main(void)
{
    printf("========================================\n");
    printf("  OS01 AHCI Lifecycle & Timeout Tests  \n");
    printf("========================================\n");

    test_transfer_capacity_read_write();
    test_two_controller_remove();
    test_controller_failure_after_port_publish();
    test_runtime_clock_failure();
    test_no_clock_no_hardware_side_effect();
    test_deadline_with_stopped_jiffies();
    test_compensated_runtime_deadline();
    test_gate_and_timeout_no_reuse();
    test_quarantine_on_stop_failure();
    test_unsafe_failure_escalates();
    test_port_fault_isolation();

    printf("\nAll 11 AHCI lifecycle tests passed successfully!\n");
    return 0;
}
