/*
 * hosttests/cases/test_bga_non_x86.c
 *
 * Host test suite asserting non-x86 BGA stub behavior and absence of port I/O
 * (QEMU Resolution Switcher Task 4).
 * (Spec §1, §4)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <test_framework.h>

#include "fb_resolution_runtime.h"
#include <uapi/fb.h>
#include <driver/bga.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <core/printk.h>

position Pos;

static int g_failed = 0;

#define CHECK_TRUE(cond) do { \
    if (!(cond)) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

#define CHECK_EQ(exp, act) do { \
    long _e = (long)(exp); \
    long _a = (long)(act); \
    if (_e != _a) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s (expected %ld, got %ld)\n", __FILE__, __LINE__, #act, _e, _a); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

static unsigned s_port_io_calls = 0;
static unsigned s_pci_access_calls = 0;

uint16_t forbidden_inw(uint16_t port)
{
    (void)port;
    s_port_io_calls++;
    CHECK_TRUE(false && "Port I/O inw must never be called on non-x86 stub!");
    return 0;
}

void forbidden_outw(uint16_t port, uint16_t val)
{
    (void)port;
    (void)val;
    s_port_io_calls++;
    CHECK_TRUE(false && "Port I/O outw must never be called on non-x86 stub!");
}

int forbidden_pci_read(struct pci_device *pdev, uint16_t off, uint32_t *val)
{
    (void)pdev;
    (void)off;
    (void)val;
    s_pci_access_calls++;
    CHECK_TRUE(false && "PCI config read must never be called on non-x86 stub!");
    return -ENODEV;
}

int forbidden_pci_write(struct pci_device *pdev, uint16_t off, uint32_t val)
{
    (void)pdev;
    (void)off;
    (void)val;
    s_pci_access_calls++;
    CHECK_TRUE(false && "PCI config write must never be called on non-x86 stub!");
    return -ENODEV;
}

TEST_FUNC(test_non_x86_stub_behavior)
{
    s_port_io_calls = 0;
    s_pci_access_calls = 0;

#ifdef OS01_HOST_TEST
    bga_set_transport_for_test(forbidden_inw, forbidden_outw, forbidden_pci_read, forbidden_pci_write);
#endif

    struct pci_device dummy_pdev;
    memset(&dummy_pdev, 0, sizeof(dummy_pdev));
    dummy_pdev.vendor = BGA_PCI_VENDOR_ID;
    dummy_pdev.device = BGA_PCI_DEVICE_ID;
    dummy_pdev.class_code = 0x030000;

    struct pci_device_id dummy_id;
    memset(&dummy_id, 0, sizeof(dummy_id));

    /* 1. bga_probe on non-x86 must return -ENODEV immediately */
    int rc = bga_probe(&dummy_pdev, &dummy_id);
    CHECK_EQ(-ENODEV, rc);
    CHECK_EQ(0, s_port_io_calls);
    CHECK_EQ(0, s_pci_access_calls);

    /* 2. bga_apply_mode on non-x86 must return BGA_FAILED without any I/O */
    struct fb_info target = {
        .width = 1024,
        .height = 768,
        .stride = 4096,
        .bpp = 32,
        .format = FB_FORMAT_RGB32
    };

    enum bga_result res = bga_apply_mode(&target);
    CHECK_EQ(BGA_FAILED, res);
    CHECK_EQ(0, s_port_io_calls);
    CHECK_EQ(0, s_pci_access_calls);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_non_x86_stub_behavior),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    if (g_failed > 0) {
        printf("test_bga_non_x86: %d failures\n", g_failed);
        return 1;
    }
    return 0;
}
