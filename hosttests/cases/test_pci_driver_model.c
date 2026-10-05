/* hosttests/cases/test_pci_driver_model.c — ARCH-9 PCI driver model and enumeration tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <arch/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Memory allocation hook for OOM testing ─────────────────────── */
static bool s_inject_kmalloc_fail = false;

void *kmalloc(size_t size)
{
    if (s_inject_kmalloc_fail) {
        return NULL;
    }
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

/* ── Fake PCI Backend Infrastructure ────────────────────────────── */
#define MAX_FAKE_DEVICES 32

struct fake_device_desc {
    uint16_t domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t fn;
    bool present;
    uint32_t dwords[64]; /* 256 bytes standard config space */
    int fail_read_offset;
    enum pci_error_scope fail_scope;
    int fail_errno;
};

static struct fake_device_desc s_fake_devices[MAX_FAKE_DEVICES];
static unsigned s_fake_device_count = 0;
static struct pci_root s_fake_roots[4];
static unsigned s_fake_root_count = 0;
static bool s_inject_root_error = false;
static int s_inject_root_errno = -EIO;

static void fake_reset(void)
{
    s_fake_device_count = 0;
    s_fake_root_count = 0;
    s_inject_root_error = false;
    s_inject_root_errno = -EIO;
    s_inject_kmalloc_fail = false;
    device_core_init();
    pci_core_reset_for_test();
}

static void fake_add_root(uint16_t domain, uint8_t bus)
{
    if (s_fake_root_count < 4) {
        s_fake_roots[s_fake_root_count].domain = domain;
        s_fake_roots[s_fake_root_count].bus = bus;
        s_fake_root_count++;
    }
}

static struct fake_device_desc *fake_add_device(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                                                uint16_t vendor, uint16_t device, uint32_t class_code, uint8_t header_type)
{
    if (s_fake_device_count >= MAX_FAKE_DEVICES) {
        return NULL;
    }
    struct fake_device_desc *f = &s_fake_devices[s_fake_device_count++];
    memset(f, 0, sizeof(*f));
    f->domain = domain;
    f->bus = bus;
    f->slot = slot;
    f->fn = fn;
    f->present = true;
    f->fail_read_offset = -1;
    /* Dword 0: vendor (15:0), device (31:16) */
    f->dwords[0] = ((uint32_t)device << 16) | vendor;
    /* Dword 2: revision (7:0), prog_if (15:8), subclass (23:16), class (31:24) */
    f->dwords[2] = (class_code << 8);
    /* Dword 3: header_type at byte 2 (23:16) */
    f->dwords[3] = ((uint32_t)header_type << 16);
    /* Dword 11: subvendor (15:0), subdevice (31:16) */
    f->dwords[11] = 0;
    return f;
}

static int fake_pci_read32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                           uint16_t offset, uint32_t *out, enum pci_error_scope *scope)
{
    if (s_inject_root_error) {
        if (scope) *scope = PCI_ERROR_ROOT;
        return s_inject_root_errno;
    }
    for (unsigned i = 0; i < s_fake_device_count; i++) {
        struct fake_device_desc *f = &s_fake_devices[i];
        if (f->domain == domain && f->bus == bus && f->slot == slot && f->fn == fn && f->present) {
            if (f->fail_read_offset >= 0 && (int)offset == f->fail_read_offset) {
                if (scope) *scope = f->fail_scope;
                return f->fail_errno ? f->fail_errno : -EIO;
            }
            if (offset / 4 < 64) {
                *out = f->dwords[offset / 4];
            } else {
                *out = 0;
            }
            return 0;
        }
    }
    /* Absence sentinel */
    *out = 0xFFFFFFFFU;
    return 0;
}

static int fake_pci_write32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                            uint16_t offset, uint32_t value, enum pci_error_scope *scope)
{
    (void)scope;
    for (unsigned i = 0; i < s_fake_device_count; i++) {
        struct fake_device_desc *f = &s_fake_devices[i];
        if (f->domain == domain && f->bus == bus && f->slot == slot && f->fn == fn && f->present) {
            if (offset / 4 < 64) {
                f->dwords[offset / 4] = value;
            }
            return 0;
        }
    }
    return 0;
}

static int fake_pci_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    (void)pdev;
    if (out) *out = 16;
    return 0;
}

static struct pci_backend s_fake_backend = {
    .roots = s_fake_roots,
    .root_count = 0,
    .read32 = fake_pci_read32,
    .write32 = fake_pci_write32,
    .route_gsi = fake_pci_route_gsi,
};

static const struct pci_backend *s_current_backend = &s_fake_backend;

const struct pci_backend *arch_pci_backend(void)
{
    return s_current_backend;
}

/* ── Test 1: Class mask, wildcard, and first entry match ─────────── */
static int t1_probe_calls = 0;
static const struct pci_device_id *t1_matched_id = NULL;

static int t1_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    t1_probe_calls++;
    t1_matched_id = id;
    pdev->driver_data = (void *)0xCAFE;
    return 0;
}

static const struct pci_device_id t1_ids[] = {
    /* entry 0: matches via class_mask */
    { .vendor = PCI_ID_ANY, .device = PCI_ID_ANY, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0x010601, .class_mask = 0xFFFFFF },
    /* entry 1: also matches via exact vendor/dev */
    { .vendor = 0x1234, .device = 0x5678, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0, .class_mask = 0 },
    /* entry 2: subvendor mismatch */
    { .vendor = 0x1234, .device = 0x5678, .subvendor = 0x9999, .subdevice = 0x2222,
      .class_value = 0, .class_mask = 0 },
};

static const struct pci_driver t1_driver = {
    .name = "t1_driver",
    .id_table = t1_ids,
    .id_count = 3,
    .probe = t1_probe,
    .remove = NULL,
};

TEST_FUNC(test_class_mask_wildcard_first_entry)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    struct fake_device_desc *f = fake_add_device(0, 0, 1, 0, 0x1234, 0x5678, 0x010601, 0);
    f->dwords[11] = (0x2222U << 16) | 0x1111U; /* subvendor 0x1111, subdevice 0x2222 */

    t1_probe_calls = 0;
    t1_matched_id = NULL;

    assert_eq(0, pci_register_driver(&t1_driver));
    assert_eq(0, pci_enumerate());
    assert_eq(1, pci_device_count());

    struct pci_device *pdev = pci_device_get(0);
    assert_true(pdev != NULL);
    assert_eq(DEV_DISCOVERED, pdev->dev.state);
    assert_eq(0x010601, pdev->class_code);
    assert_eq(0x1111, pdev->subvendor);
    assert_eq(0x2222, pdev->subdevice);

    assert_eq(0, pci_bind_all());
    assert_eq(1, t1_probe_calls);
    /* Multiple entries match in the same driver: MUST pick the first matching entry (entry 0) */
    assert_eq(&t1_ids[0], t1_matched_id);
    assert_eq(DEV_BOUND, pdev->dev.state);
    assert_eq(&t1_driver, pdev->driver);
    assert_eq((void *)0xCAFE, pdev->driver_data);

    /* Direct pci_match_id verification */
    assert_eq(&t1_ids[0], pci_match_id(&t1_driver, pdev));

    /* Test class mask mismatch */
    struct pci_device dummy_pdev;
    memset(&dummy_pdev, 0, sizeof(dummy_pdev));
    dummy_pdev.vendor = 0x9999;
    dummy_pdev.device = 0x9999;
    dummy_pdev.class_code = 0x020000; /* network, does not match 0x010601 */
    assert_null(pci_match_id(&t1_driver, &dummy_pdev));
}

/* ── Test 2: ENODEV unbind contract ──────────────────────────────── */
static int t2_probe_calls = 0;
static int t2_remove_calls = 0;

static int t2_probe_enodev(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    t2_probe_calls++;
    return -ENODEV;
}

static void t2_remove(struct pci_device *pdev)
{
    (void)pdev;
    t2_remove_calls++;
}

static const struct pci_device_id t2_ids[] = {
    { .vendor = 0x8086, .device = 0x100e, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0, .class_mask = 0 },
};

static const struct pci_driver t2_driver = {
    .name = "t2_driver",
    .id_table = t2_ids,
    .id_count = 1,
    .probe = t2_probe_enodev,
    .remove = t2_remove,
};

TEST_FUNC(test_enodev_unbound)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    fake_add_device(0, 0, 2, 0, 0x8086, 0x100e, 0x020000, 0);

    t2_probe_calls = 0;
    t2_remove_calls = 0;

    assert_eq(0, pci_register_driver(&t2_driver));
    assert_eq(0, pci_enumerate());
    assert_eq(1, pci_device_count());

    assert_eq(0, pci_bind_all());
    assert_eq(1, t2_probe_calls);

    struct pci_device *pdev = pci_device_get(0);
    assert_true(pdev != NULL);
    /* Probe returned -ENODEV: MUST roll back to DEV_UNBOUND, NOT DEV_FAILED */
    assert_eq(DEV_UNBOUND, pdev->dev.state);
    assert_eq(-ENODEV, pdev->dev.last_error);
    assert_null(pdev->driver);
    /* probe failure must NOT invoke remove */
    assert_eq(0, t2_remove_calls);
}

/* ── Test 3: Multifunction, bridge traversal, cycle prevention, sorted BDF ── */
TEST_FUNC(test_all_functions_and_bridge_cycle)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    /* Bus 0, Slot 1 has functions 0..7 (multifunction) */
    fake_add_device(0, 0, 1, 0, 0x8086, 0x7000, 0x060100, 0x80); /* header_type bit 7 set */
    for (uint8_t fn = 1; fn < 8; fn++) {
        fake_add_device(0, 0, 1, fn, 0x8086, 0x7000 + fn, 0x060100, 0x00);
    }

    /* Bus 0, Slot 2 is a PCI-to-PCI bridge to Bus 1 */
    struct fake_device_desc *bridge = fake_add_device(0, 0, 2, 0, 0x8086, 0x1234, 0x060400, 0x01);
    /* Dword 6 (offset 0x18): primary=0, secondary=1, subordinate=1 */
    bridge->dwords[6] = (1U << 16) | (1U << 8) | 0U;

    /* Bus 1, Slot 0 has a child device */
    fake_add_device(0, 1, 0, 0, 0x10EC, 0x8139, 0x020000, 0x00);

    /* Bus 1, Slot 1 has a malformed/cyclical bridge pointing back to Bus 0 */
    struct fake_device_desc *bad_bridge = fake_add_device(0, 1, 1, 0, 0x8086, 0x5678, 0x060400, 0x01);
    bad_bridge->dwords[6] = (0U << 16) | (0U << 8) | 1U; /* secondary bus 0 (cycle!) */

    assert_eq(0, pci_enumerate());

    /* Total devices: 8 on slot 1 + 1 bridge on slot 2 + 1 child on bus 1 + 1 bad bridge on bus 1 = 11 devices */
    assert_eq(11, pci_device_count());

    /* Check lookup of all functions 0..7 on bus 0 slot 1 */
    for (uint8_t fn = 0; fn < 8; fn++) {
        struct pci_device *p = pci_device_lookup(0, 0, 1, fn);
        assert_true(p != NULL);
        assert_eq(fn, p->fn);
        assert_eq(0x7000 + fn, p->device);
    }

    /* Check lookup of child device on bus 1 */
    struct pci_device *child = pci_device_lookup(0, 1, 0, 0);
    assert_true(child != NULL);
    assert_eq(0x10EC, child->vendor);

    /* Verify strict BDF sorting across entire list */
    for (unsigned i = 0; i + 1 < pci_device_count(); i++) {
        struct pci_device *a = pci_device_get(i);
        struct pci_device *b = pci_device_get(i + 1);
        bool ordered = false;
        if (a->domain < b->domain) ordered = true;
        else if (a->domain == b->domain) {
            if (a->bus < b->bus) ordered = true;
            else if (a->bus == b->bus) {
                if (a->slot < b->slot) ordered = true;
                else if (a->slot == b->slot) {
                    if (a->fn < b->fn) ordered = true;
                }
            }
        }
        assert_true(ordered);
    }
}

/* ── Test 4: Matching precedence and conflict isolation ─────────── */
static int t4_exact_calls = 0;
static int t4_class_calls = 0;
static int t4_conflict1_calls = 0;
static int t4_conflict2_calls = 0;

static int t4_exact_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    t4_exact_calls++;
    return 0;
}

static int t4_class_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    t4_class_calls++;
    return 0;
}

static int t4_conflict1_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    t4_conflict1_calls++;
    return 0;
}

static int t4_conflict2_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    t4_conflict2_calls++;
    return 0;
}

/* Exact ID driver */
static const struct pci_device_id t4_exact_ids[] = {
    { .vendor = 0x8086, .device = 0x1234, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0, .class_mask = 0 },
};
static const struct pci_driver t4_exact_driver = {
    .name = "t4_exact",
    .id_table = t4_exact_ids,
    .id_count = 1,
    .probe = t4_exact_probe,
    .remove = NULL,
};

/* Class driver */
static const struct pci_device_id t4_class_ids[] = {
    { .vendor = PCI_ID_ANY, .device = PCI_ID_ANY, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0x020000, .class_mask = 0xFF0000 },
};
static const struct pci_driver t4_class_driver = {
    .name = "t4_class",
    .id_table = t4_class_ids,
    .id_count = 1,
    .probe = t4_class_probe,
    .remove = NULL,
};

/* Conflicting drivers */
static const struct pci_device_id t4_conflict_ids[] = {
    { .vendor = 0x1111, .device = 0x2222, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0, .class_mask = 0 },
};
static const struct pci_driver t4_conflict1_driver = {
    .name = "t4_conflict1",
    .id_table = t4_conflict_ids,
    .id_count = 1,
    .probe = t4_conflict1_probe,
    .remove = NULL,
};
static const struct pci_driver t4_conflict2_driver = {
    .name = "t4_conflict2",
    .id_table = t4_conflict_ids,
    .id_count = 1,
    .probe = t4_conflict2_probe,
    .remove = NULL,
};

TEST_FUNC(test_matching_precedence)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    /* Device 1 matches both exact driver and class driver */
    fake_add_device(0, 0, 1, 0, 0x8086, 0x1234, 0x020000, 0);

    /* Device 2 matches both conflict1 and conflict2 drivers at exact level */
    fake_add_device(0, 0, 2, 0, 0x1111, 0x2222, 0x000000, 0);

    t4_exact_calls = 0;
    t4_class_calls = 0;
    t4_conflict1_calls = 0;
    t4_conflict2_calls = 0;

    /* Register class driver FIRST to prove registration order does not win over exact match */
    assert_eq(0, pci_register_driver(&t4_class_driver));
    assert_eq(0, pci_register_driver(&t4_exact_driver));
    assert_eq(0, pci_register_driver(&t4_conflict1_driver));
    assert_eq(0, pci_register_driver(&t4_conflict2_driver));

    assert_eq(0, pci_enumerate());
    assert_eq(2, pci_device_count());

    assert_eq(0, pci_bind_all());

    /* Device 1: exact match wins */
    struct pci_device *pdev1 = pci_device_lookup(0, 0, 1, 0);
    assert_true(pdev1 != NULL);
    assert_eq(DEV_BOUND, pdev1->dev.state);
    assert_eq(&t4_exact_driver, pdev1->driver);
    assert_eq(1, t4_exact_calls);
    assert_eq(0, t4_class_calls);

    /* Device 2: conflict at same level -> DEV_FAILED, last_error -EEXIST */
    struct pci_device *pdev2 = pci_device_lookup(0, 0, 2, 0);
    assert_true(pdev2 != NULL);
    assert_eq(DEV_FAILED, pdev2->dev.state);
    assert_eq(-EEXIST, pdev2->dev.last_error);
    assert_null(pdev2->driver);
    assert_eq(0, t4_conflict1_calls);
    assert_eq(0, t4_conflict2_calls);
}

/* ── Test 5: Absent driver probe zero and empty bus ──────────────── */
static int absent_driver_probe_calls = 0;

static int absent_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    absent_driver_probe_calls++;
    return 0;
}

static const struct pci_device_id absent_ids[] = {
    { .vendor = 0x9999, .device = 0x9999, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0, .class_mask = 0 },
};

static const struct pci_driver absent_driver = {
    .name = "absent_driver",
    .id_table = absent_ids,
    .id_count = 1,
    .probe = absent_probe,
    .remove = NULL,
};

TEST_FUNC(test_absent_driver_probe_zero)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    /* Empty bus: no devices present */
    assert_eq(0, pci_register_driver(&absent_driver));
    assert_eq(0, pci_enumerate());
    assert_eq(0, pci_device_count());

    /* Reset and set up 2 devices that do not match the driver */
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    fake_add_device(0, 0, 1, 0, 0x1111, 0x1111, 0, 0);
    fake_add_device(0, 0, 2, 0, 0x2222, 0x2222, 0, 0);

    absent_driver_probe_calls = 0;
    assert_eq(0, pci_register_driver(&absent_driver));

    /* Key assertions from Task 2 brief */
    assert_eq(0, pci_enumerate());
    assert_eq(2, pci_device_count());
    assert_eq(0, absent_driver_probe_calls);

    assert_eq(0, pci_bind_all());
    assert_eq(0, absent_driver_probe_calls);

    assert_eq(DEV_UNBOUND, pci_device_get(0)->dev.state);
    assert_eq(DEV_UNBOUND, pci_device_get(1)->dev.state);
}

/* ── Test 6: Fault isolation (bad BAR / config function / bus error) ─ */
static int t6_probe_calls = 0;

static int t6_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    t6_probe_calls++;
    return 0;
}

static const struct pci_device_id t6_ids[] = {
    { .vendor = PCI_ID_ANY, .device = PCI_ID_ANY, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0, .class_mask = 0 },
};

static const struct pci_driver t6_driver = {
    .name = "t6_driver",
    .id_table = t6_ids,
    .id_count = 1,
    .probe = t6_probe,
    .remove = NULL,
};

TEST_FUNC(test_fault_isolation)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    /* Function 0: healthy device */
    struct fake_device_desc *f0 = fake_add_device(0, 0, 1, 0, 0x8086, 0x1000, 0x020000, 0);
    f0->dwords[4] = 0xE0000000; /* 32-bit MMIO at BAR0 */

    /* Function 1: bad BAR device (64-bit BAR at BAR5: invalid upper slot!) */
    struct fake_device_desc *f1 = fake_add_device(0, 0, 1, 1, 0x8086, 0x1001, 0x020000, 0);
    f1->dwords[9] = 0xF0000004; /* 64-bit MMIO indicator (bit 2 set) at BAR5 */

    /* Function 2: config read failure at function scope */
    struct fake_device_desc *f2 = fake_add_device(0, 0, 1, 2, 0x8086, 0x1002, 0x020000, 0);
    f2->fail_read_offset = 0x08; /* class code read fails */
    f2->fail_scope = PCI_ERROR_FUNCTION;
    f2->fail_errno = -EIO;

    t6_probe_calls = 0;
    assert_eq(0, pci_register_driver(&t6_driver));
    assert_eq(0, pci_enumerate());
    assert_eq(3, pci_device_count());

    struct pci_device *p0 = pci_device_lookup(0, 0, 1, 0);
    struct pci_device *p1 = pci_device_lookup(0, 0, 1, 1);
    struct pci_device *p2 = pci_device_lookup(0, 0, 1, 2);

    assert_true(p0 != NULL);
    assert_true(p1 != NULL);
    assert_true(p2 != NULL);

    assert_eq(DEV_DISCOVERED, p0->dev.state);
    assert_eq(DEV_FAILED, p1->dev.state);
    assert_eq(DEV_FAILED, p2->dev.state);

    assert_eq(0, pci_bind_all());

    /* Healthy function 0 binds successfully */
    assert_eq(DEV_BOUND, p0->dev.state);
    assert_eq(&t6_driver, p0->driver);
    assert_eq(1, t6_probe_calls);

    /* Bad BAR and read error functions are isolated: probe never called */
    assert_eq(DEV_FAILED, p1->dev.state);
    assert_null(p1->driver);
    assert_eq(DEV_FAILED, p2->dev.state);
    assert_null(p2->driver);

    /* Bus-level error: pci_enumerate returns negative error */
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;
    s_inject_root_error = true;
    s_inject_root_errno = -EIO;

    assert_eq(-EIO, pci_enumerate());
}

/* ── Test 7: Probe cleanup contract (no remove on probe error) ───── */
static int t7_probe_calls = 0;
static int t7_remove_calls = 0;

static int t7_probe_err(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    t7_probe_calls++;
    return -EIO;
}

static void t7_remove(struct pci_device *pdev)
{
    (void)pdev;
    t7_remove_calls++;
}

static const struct pci_device_id t7_ids[] = {
    { .vendor = 0x1234, .device = 0x5678, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY,
      .class_value = 0, .class_mask = 0 },
};

static const struct pci_driver t7_driver = {
    .name = "t7_driver",
    .id_table = t7_ids,
    .id_count = 1,
    .probe = t7_probe_err,
    .remove = t7_remove,
};

TEST_FUNC(test_probe_cleanup_contract)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    fake_add_device(0, 0, 1, 0, 0x1234, 0x5678, 0, 0);

    t7_probe_calls = 0;
    t7_remove_calls = 0;

    assert_eq(0, pci_register_driver(&t7_driver));
    assert_eq(0, pci_enumerate());

    assert_eq(0, pci_bind_all());
    assert_eq(1, t7_probe_calls);

    struct pci_device *pdev = pci_device_get(0);
    assert_true(pdev != NULL);
    assert_eq(DEV_FAILED, pdev->dev.state);
    assert_eq(-EIO, pdev->dev.last_error);
    assert_null(pdev->driver);
    /* Probe failure must NEVER invoke remove */
    assert_eq(0, t7_remove_calls);
}

/* ── Test 8: Registry OOM and duplicate rejection ───────────────── */
static const struct pci_driver t8_driver1 = {
    .name = "t8_driver",
    .id_table = NULL,
    .id_count = 0,
    .probe = NULL,
    .remove = NULL,
};

static const struct pci_driver t8_driver1_dup = {
    .name = "t8_driver", /* duplicate name */
    .id_table = NULL,
    .id_count = 0,
    .probe = NULL,
    .remove = NULL,
};

static const struct pci_driver t8_driver2 = {
    .name = "t8_driver2",
    .id_table = NULL,
    .id_count = 0,
    .probe = NULL,
    .remove = NULL,
};

TEST_FUNC(test_registry_oom_and_duplicates)
{
    fake_reset();

    /* Successful registration */
    assert_eq(0, pci_register_driver(&t8_driver1));

    /* Duplicate driver name rejected with -EEXIST */
    assert_eq(-EEXIST, pci_register_driver(&t8_driver1_dup));

    /* OOM rejection */
    s_inject_kmalloc_fail = true;
    assert_eq(-ENOMEM, pci_register_driver(&t8_driver2));
    s_inject_kmalloc_fail = false;

    /* Null driver or null name rejected with -EINVAL */
    assert_eq(-EINVAL, pci_register_driver(NULL));
    struct pci_driver null_name_drv = { .name = NULL };
    assert_eq(-EINVAL, pci_register_driver(&null_name_drv));

    /* Confirm normal registration works again after failure, no half-objects */
    assert_eq(0, pci_register_driver(&t8_driver2));
}

/* ── Test 9: NULL backend, DEVICE_UNSAFE, and quarantine ─────────── */
static int t9_probe_unsafe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    return DEVICE_UNSAFE;
}

static const struct pci_driver t9_unsafe_driver = {
    .name = "t9_unsafe",
    .id_table = t6_ids,
    .id_count = 1,
    .probe = t9_probe_unsafe,
    .remove = NULL,
};

TEST_FUNC(test_null_backend_and_device_unsafe)
{
    /* NULL backend returns BUS_UNAVAILABLE */
    s_current_backend = NULL;
    assert_eq(BUS_UNAVAILABLE, pci_enumerate());
    s_current_backend = &s_fake_backend;

    /* DEVICE_UNSAFE propagation */
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    fake_add_device(0, 0, 1, 0, 0x1234, 0x5678, 0, 0);

    assert_eq(0, pci_register_driver(&t9_unsafe_driver));
    assert_eq(0, pci_enumerate());

    /* pci_bind_all propagates DEVICE_UNSAFE fatal result */
    assert_eq(DEVICE_UNSAFE, pci_bind_all());

    struct pci_device *pdev = pci_device_get(0);
    assert_true(pdev != NULL);
    assert_eq(DEV_FAILED, pdev->dev.state);
    assert_eq(DEVICE_UNSAFE, pdev->dev.last_error);

    /* device_quarantine contract */
    void *retained = (void *)0xDEADBEEF;
    device_quarantine(&pdev->dev, retained);
    assert_true(pdev->dev.quarantined);
    assert_eq(retained, pdev->dev.retained_owner);
}

/* ── Test 10: Binding guards and DEV_BOUND idempotency ──────────── */
TEST_FUNC(test_bind_guards_and_idempotency)
{
    fake_reset();

    /* pci_bind_all without successful enumeration must return -EIO */
    assert_eq(-EIO, pci_bind_all());

    /* Enumerate 1 device and bind */
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;
    fake_add_device(0, 0, 1, 0, 0x1234, 0x5678, 0x010601, 0);

    t1_probe_calls = 0;
    assert_eq(0, pci_register_driver(&t1_driver));
    assert_eq(0, pci_enumerate());
    assert_eq(1, pci_device_count());

    assert_eq(0, pci_bind_all());
    assert_eq(1, t1_probe_calls);
    assert_eq(DEV_BOUND, pci_device_get(0)->dev.state);

    /* Second pci_bind_all must skip DEV_BOUND and NOT call probe again */
    assert_eq(0, pci_bind_all());
    assert_eq(1, t1_probe_calls);
}

/* ── Test 11: Bridge config read failure ─────────────────────────── */
TEST_FUNC(test_bridge_read_error)
{
    fake_reset();
    fake_add_root(0, 0);
    s_fake_backend.root_count = s_fake_root_count;

    struct fake_device_desc *b = fake_add_device(0, 0, 1, 0, 0x8086, 0x1234, 0x060400, 0x01);
    b->fail_read_offset = 0x18; /* offset 0x18 bus registers read fails */
    b->fail_scope = PCI_ERROR_FUNCTION;
    b->fail_errno = -EIO;

    assert_eq(0, pci_enumerate());
    assert_eq(1, pci_device_count());

    struct pci_device *pdev = pci_device_get(0);
    assert_true(pdev != NULL);
    assert_true(pdev->enum_error);
    assert_eq(DEV_FAILED, pdev->dev.state);
    assert_eq(-EIO, pdev->dev.last_error);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_class_mask_wildcard_first_entry),
    TEST_ENTRY(test_enodev_unbound),
    TEST_ENTRY(test_all_functions_and_bridge_cycle),
    TEST_ENTRY(test_matching_precedence),
    TEST_ENTRY(test_absent_driver_probe_zero),
    TEST_ENTRY(test_fault_isolation),
    TEST_ENTRY(test_probe_cleanup_contract),
    TEST_ENTRY(test_registry_oom_and_duplicates),
    TEST_ENTRY(test_null_backend_and_device_unsafe),
    TEST_ENTRY(test_bind_guards_and_idempotency),
    TEST_ENTRY(test_bridge_read_error),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
