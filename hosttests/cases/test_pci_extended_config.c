/* @os01-test
 * id: test_pci_extended_config
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: pr
 * link: pci-extended-config
 * scenarios: [drivers.pci.extended_config]
 */
/* hosttests/cases/test_pci_extended_config.c — 4 KiB ECAM config, BAR & ranges tests */
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

void *kmalloc(size_t size)
{
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

#define MAX_FAKE_DEVS 16
#define CFG_DWORDS    1024 /* 4096 bytes */

struct fake_dev {
    uint16_t domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t fn;
    bool present;
    uint32_t dwords[CFG_DWORDS];
};

struct fake_range {
    uint16_t domain;
    uint8_t space;
    uint64_t pci_base;
    uint64_t cpu_base;
    uint64_t size;
};

static struct fake_dev s_devs[MAX_FAKE_DEVS];
static unsigned int s_dev_count = 0;
static struct pci_root s_roots[4];
static unsigned int s_root_count = 0;
static struct fake_range s_ranges[16];
static unsigned int s_range_count = 0;
static uint8_t s_bus_start = 0;
static uint8_t s_bus_end = 255;

static struct pci_backend s_backend;
static const struct pci_backend *s_active_backend = &s_backend;

const struct pci_backend *arch_pci_backend(void)
{
    return s_active_backend;
}

static int fake_read32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                       uint16_t offset, uint32_t *out, enum pci_error_scope *scope)
{
    if (scope) {
        *scope = PCI_ERROR_FUNCTION;
    }
    if (!out || (offset & 3U) != 0 || offset >= 4096U) {
        return -EINVAL;
    }
    for (unsigned int i = 0; i < s_dev_count; i++) {
        struct fake_dev *d = &s_devs[i];
        if (d->present && d->domain == domain && d->bus == bus && d->slot == slot && d->fn == fn) {
            *out = d->dwords[offset / 4U];
            return 0;
        }
    }
    *out = 0xFFFFFFFFU;
    return 0;
}

static int fake_write32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                        uint16_t offset, uint32_t value, enum pci_error_scope *scope)
{
    if (scope) {
        *scope = PCI_ERROR_FUNCTION;
    }
    if ((offset & 3U) != 0 || offset >= 4096U) {
        return -EINVAL;
    }
    for (unsigned int i = 0; i < s_dev_count; i++) {
        struct fake_dev *d = &s_devs[i];
        if (d->present && d->domain == domain && d->bus == bus && d->slot == slot && d->fn == fn) {
            d->dwords[offset / 4U] = value;
            return 0;
        }
    }
    return 0;
}

static int fake_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    (void)pdev;
    (void)out;
    return -ENOTSUP;
}

static int fake_translate_resource(uint16_t domain, uint8_t space,
                                   uint64_t pci_addr, uint64_t size,
                                   uint64_t *out_cpu_addr)
{
    if (!out_cpu_addr) {
        return -EINVAL;
    }
    if (size > 0 && pci_addr > UINT64_MAX - size) {
        return -EOVERFLOW;
    }
    for (unsigned int i = 0; i < s_range_count; i++) {
        const struct fake_range *r = &s_ranges[i];
        if (r->domain != domain || r->space != space) {
            continue;
        }
        if (pci_addr < r->pci_base) {
            continue;
        }
        uint64_t off = pci_addr - r->pci_base;
        if (off >= r->size) {
            continue;
        }
        if (size > r->size - off) {
            continue;
        }
        *out_cpu_addr = r->cpu_base + off;
        return 0;
    }
    return -ENOENT;
}

static int fake_bus_range(uint16_t domain, uint8_t *out_start, uint8_t *out_end)
{
    if (domain != 0) {
        return -ENODEV;
    }
    if (out_start) {
        *out_start = s_bus_start;
    }
    if (out_end) {
        *out_end = s_bus_end;
    }
    return 0;
}

static void reset_env(uint16_t max_cfg, bool with_translate, bool with_bus_range)
{
    s_dev_count = 0;
    s_root_count = 0;
    s_range_count = 0;
    s_bus_start = 0;
    s_bus_end = 255;

    memset(&s_backend, 0, sizeof(s_backend));
    s_backend.roots = s_roots;
    s_backend.root_count = 0;
    s_backend.read32 = fake_read32;
    s_backend.write32 = fake_write32;
    s_backend.route_gsi = fake_route_gsi;
    s_backend.max_config_offset = max_cfg;
    s_backend.translate_resource = with_translate ? fake_translate_resource : NULL;
    s_backend.bus_range = with_bus_range ? fake_bus_range : NULL;
    s_active_backend = &s_backend;

    device_core_init();
    pci_core_reset_for_test();
}

static void add_root(uint16_t domain, uint8_t bus)
{
    s_roots[s_root_count].domain = domain;
    s_roots[s_root_count].bus = bus;
    s_root_count++;
    s_backend.root_count = s_root_count;
}

static struct fake_dev *add_dev(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                                uint16_t vendor, uint16_t device, uint32_t class_code, uint8_t hdr)
{
    struct fake_dev *d = &s_devs[s_dev_count++];
    memset(d, 0, sizeof(*d));
    d->domain = domain;
    d->bus = bus;
    d->slot = slot;
    d->fn = fn;
    d->present = true;
    d->dwords[0] = ((uint32_t)device << 16) | vendor;
    d->dwords[2] = class_code << 8;
    d->dwords[3] = (uint32_t)hdr << 16;
    return d;
}

static void add_range(uint16_t domain, uint8_t space, uint64_t pci_base, uint64_t cpu_base, uint64_t size)
{
    struct fake_range *r = &s_ranges[s_range_count++];
    r->domain = domain;
    r->space = space;
    r->pci_base = pci_base;
    r->cpu_base = cpu_base;
    r->size = size;
}

TEST_FUNC(test_legacy_vs_ecam_config_limits)
{
    /* Legacy backend: max_config_offset = 0 -> limit 252 */
    reset_env(0, false, false);
    add_root(0, 0);
    struct fake_dev *d = add_dev(0, 0, 1, 0, 0x1b36, 0x0008, 0x060000, 0);
    d->dwords[0x100 / 4U] = 0xCAFEBABEU;
    d->dwords[0xFFC / 4U] = 0xDEADBEEFU;

    assert_eq(0, pci_enumerate());
    struct pci_device *pdev = pci_device_lookup(0, 0, 1, 0);
    assert_not_null(pdev);

    uint32_t val = 0;
    assert_eq(0, pci_config_read32(pdev, 0xFC, &val));
    assert_eq(-EINVAL, pci_config_read32(pdev, 256, &val));
    assert_eq(-EINVAL, pci_config_read32(pdev, 4092, &val));
    assert_eq(-EINVAL, pci_config_write32(pdev, 256, 0x12345678U));
    assert_eq(-EINVAL, pci_config_write32(pdev, 4092, 0x12345678U));

    /* ECAM backend: max_config_offset = 4092 -> 0x100..0xFFC allowed */
    reset_env(4092, false, false);
    add_root(0, 0);
    d = add_dev(0, 0, 1, 0, 0x1af4, 0x1044, 0x00ff00, 0);
    d->dwords[0x100 / 4U] = 0x12340001U;
    d->dwords[0xFFC / 4U] = 0x89ABCDEFU;

    assert_eq(0, pci_enumerate());
    pdev = pci_device_lookup(0, 0, 1, 0);
    assert_not_null(pdev);

    assert_eq(0, pci_config_read32(pdev, 0x100, &val));
    assert_eq(0x12340001U, val);
    assert_eq(0, pci_config_read32(pdev, 0xFFC, &val));
    assert_eq(0x89ABCDEFU, val);
    assert_eq(0, pci_config_write32(pdev, 0x200, 0x55AA33CCU));
    assert_eq(0, pci_config_read32(pdev, 0x200, &val));
    assert_eq(0x55AA33CCU, val);

    assert_eq(-EINVAL, pci_config_read32(pdev, 4096, &val));
    assert_eq(-EINVAL, pci_config_read32(pdev, 0x102, &val));
    assert_eq(-EINVAL, pci_config_write32(pdev, 4096, 1));
    assert_eq(-EINVAL, pci_config_write32(pdev, 0x101, 1));
}

TEST_FUNC(test_unprogrammed_mmio_bars_and_bar5_64bit)
{
    reset_env(4092, true, true);
    add_root(0, 0);
    /* QEMU virt ranges: IO 0x0->0x3eff0000, MMIO32 0x10000000->0x10000000, MMIO64 0x80_00000000 */
    add_range(0, PCI_SPACE_IO, 0x0ULL, 0x3eff0000ULL, 0x10000ULL);
    add_range(0, PCI_SPACE_MMIO32, 0x10000000ULL, 0x10000000ULL, 0x2eff0000ULL);
    add_range(0, PCI_SPACE_MMIO64, 0x8000000000ULL, 0x8000000000ULL, 0x8000000000ULL);

    /* Unprogrammed 64-bit / prefetchable MMIO BARs: 0x4, 0x8, 0xC with upper=0 */
    struct fake_dev *ok_dev = add_dev(0, 0, 1, 0, 0x1af4, 0x1044, 0x00ff00, 0);
    ok_dev->dwords[4] = 0x00000004U; /* BAR0: 64-bit unprogrammed (upper BAR1=0) */
    ok_dev->dwords[5] = 0x00000000U;
    ok_dev->dwords[6] = 0x00000008U; /* BAR2: 32-bit prefetch unprogrammed */
    ok_dev->dwords[7] = 0x0000000CU; /* BAR3: 64-bit prefetch unprogrammed (upper BAR4=0) */
    ok_dev->dwords[8] = 0x00000000U;
    ok_dev->dwords[9] = 0x00000000U; /* BAR5: 0 */

    /* BAR5 64-bit must still fail as bad_bar */
    struct fake_dev *bad_bar5 = add_dev(0, 0, 2, 0, 0x8086, 0x1001, 0x020000, 0);
    bad_bar5->dwords[9] = 0xF0000004U;

    /* BAR5 0x00000004 (64-bit at BAR5) must also fail as bad_bar */
    struct fake_dev *bad_bar5_zero = add_dev(0, 0, 3, 0, 0x8086, 0x1002, 0x020000, 0);
    bad_bar5_zero->dwords[9] = 0x00000004U;

    assert_eq(0, pci_enumerate());
    assert_eq(3, pci_device_count());

    struct pci_device *p_ok = pci_device_lookup(0, 0, 1, 0);
    assert_not_null(p_ok);
    assert_false(p_ok->enum_error);
    assert_eq(DEV_DISCOVERED, p_ok->dev.state);
    for (unsigned int i = 0; i < 6; i++) {
        assert_eq(PCI_BAR_NONE, p_ok->bars[i].kind);
        assert_false(p_ok->bars[i].valid);
    }

    struct pci_device *p_bad5 = pci_device_lookup(0, 0, 2, 0);
    assert_not_null(p_bad5);
    assert_true(p_bad5->enum_error);
    assert_eq(DEV_FAILED, p_bad5->dev.state);

    struct pci_device *p_bad5z = pci_device_lookup(0, 0, 3, 0);
    assert_not_null(p_bad5z);
    assert_true(p_bad5z->enum_error);
    assert_eq(DEV_FAILED, p_bad5z->dev.state);
}

TEST_FUNC(test_mmio64_below_4g_fallback_and_translation)
{
    reset_env(4092, true, true);
    add_root(0, 0);
    add_range(0, PCI_SPACE_IO, 0x0ULL, 0x3eff0000ULL, 0x10000ULL);
    add_range(0, PCI_SPACE_MMIO32, 0x10000000ULL, 0x10000000ULL, 0x2eff0000ULL);
    add_range(0, PCI_SPACE_MMIO64, 0x8000000000ULL, 0x8000000000ULL, 0x8000000000ULL);

    /* Device with 64-bit BAR assigned below 4 GiB (in MMIO32 window 0x10004000) + IO BAR 0x1000 */
    struct fake_dev *d = add_dev(0, 0, 1, 0, 0x1af4, 0x1005, 0x00ff00, 0);
    d->dwords[4] = 0x00001001U; /* BAR0: IO @ 0x1000 */
    d->dwords[5] = 0x10004004U; /* BAR1..2: MMIO64 @ 0x10004000 (upper=0) */
    d->dwords[6] = 0x00000000U;

    /* Out-of-range non-zero BAR -> bad_bar */
    struct fake_dev *oob = add_dev(0, 0, 2, 0, 0x1234, 0x5678, 0x020000, 0);
    oob->dwords[4] = 0x70000000U;

    assert_eq(0, pci_enumerate());

    struct pci_device *pdev = pci_device_lookup(0, 0, 1, 0);
    assert_not_null(pdev);
    assert_false(pdev->enum_error);
    assert_eq(PCI_BAR_IO, pdev->bars[0].kind);
    assert_true(pdev->bars[0].valid);
    assert_true(pdev->bars[0].translated);
    assert_eq(0x1000ULL, pdev->bars[0].bus_address);
    assert_eq(0x3eff1000ULL, pdev->bars[0].address);

    assert_eq(PCI_BAR_MMIO64, pdev->bars[1].kind);
    assert_true(pdev->bars[1].valid);
    assert_true(pdev->bars[1].translated);
    assert_eq(0x10004000ULL, pdev->bars[1].bus_address);
    assert_eq(0x10004000ULL, pdev->bars[1].address);
    assert_eq(PCI_BAR_UPPER, pdev->bars[2].kind);
    assert_eq(0x10004000ULL, pdev->bars[2].address);

    uint64_t phys = 0;
    assert_eq(0, pci_bar_window(pdev, 0, PCI_BAR_IO, 0x10, 4, &phys));
    assert_eq(0x3eff1010ULL, phys);
    assert_eq(0, pci_bar_window(pdev, 1, PCI_BAR_MMIO64, 0x20, 4, &phys));
    assert_eq(0x10004020ULL, phys);

    struct pci_device *p_oob = pci_device_lookup(0, 0, 2, 0);
    assert_not_null(p_oob);
    assert_true(p_oob->enum_error);
    assert_eq(DEV_FAILED, p_oob->dev.state);
}

TEST_FUNC(test_non_identity_mmio32_above_4g_and_bridge_bus_range)
{
    /* RPi5-like non-identity MMIO32: PCI 0x0 -> CPU 0x1f_0000_0000 */
    reset_env(4092, true, true);
    s_bus_start = 0;
    s_bus_end = 3;
    add_root(0, 0);
    add_range(0, PCI_SPACE_MMIO32, 0x0ULL, 0x1f00000000ULL, 0xfffffffcULL);

    struct fake_dev *d = add_dev(0, 0, 1, 0, 0x1111, 0x2222, 0x020000, 0);
    d->dwords[4] = 0x00000008U; /* Prefetch MMIO32 at PCI 0x0 -> valid when range starts at 0x0 */
    d->dwords[5] = 0x00008000U; /* PCI 0x8000 -> CPU 0x1f_0000_8000 */
    d->dwords[6] = 0x00000000U; /* BAR2=0 -> PCI_BAR_NONE even when range starts at 0x0 */

    /* Type 1 bridge whose subordinate=5 exceeds bus_end=3 -> isolated */
    struct fake_dev *br_bad = add_dev(0, 0, 2, 0, 0x1b36, 0x000c, 0x060400, 0x01);
    br_bad->dwords[0x18 / 4U] = (5U << 16) | (1U << 8) | 0U;
    /* Child on bus 5 should NOT be scanned */
    add_dev(0, 5, 0, 0, 0x9999, 0x8888, 0x010601, 0);

    assert_eq(0, pci_enumerate());
    struct pci_device *pdev = pci_device_lookup(0, 0, 1, 0);
    assert_not_null(pdev);
    assert_false(pdev->enum_error);
    assert_eq(PCI_BAR_MMIO32, pdev->bars[0].kind);
    assert_true(pdev->bars[0].valid);
    assert_true(pdev->bars[0].translated);
    assert_eq(0x0ULL, pdev->bars[0].bus_address);
    assert_eq(0x1f00000000ULL, pdev->bars[0].address);

    assert_eq(PCI_BAR_MMIO32, pdev->bars[1].kind);
    assert_eq(0x8000ULL, pdev->bars[1].bus_address);
    assert_eq(0x1f00008000ULL, pdev->bars[1].address);

    uint64_t phys = 0;
    assert_eq(0, pci_bar_window(pdev, 1, PCI_BAR_MMIO32, 0x100, 0x40, &phys));
    assert_eq(0x1f00008100ULL, phys);

    struct pci_device *p_br = pci_device_lookup(0, 0, 2, 0);
    assert_not_null(p_br);
    assert_true(p_br->enum_error);
    assert_null(pci_device_lookup(0, 5, 0, 0));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_legacy_vs_ecam_config_limits),
    TEST_ENTRY(test_unprogrammed_mmio_bars_and_bar5_64bit),
    TEST_ENTRY(test_mmio64_below_4g_fallback_and_translation),
    TEST_ENTRY(test_non_identity_mmio32_above_4g_and_bridge_bus_range),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
