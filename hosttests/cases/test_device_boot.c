/* hosttests/cases/test_device_boot.c — ARCH-9 Phase 6 device coordinator & root filesystem tests */
#include "test_framework.h"
#include <device/device.h>
#include <device/boot.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <arch/pci.h>
#include <block/blockdev.h>
#include <fs/boot.h>
#include <fs/gpt.h>
#include <fs/fat.h>
#include <fs/ext2.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

#ifndef TEST_CASE
#define TEST_CASE(name) printf("  [CASE] %s\n", (name))
#endif

/* ── Pre-GS Call Tracking ────────────────────────────────────────── */
int fake_pre_gs_cpu_id_calls = 0;

/* ── Test Observation Counters ───────────────────────────────────── */
int absent_ahci_probe_calls = 0;

/* ── Memory Allocation Hook for OOM Testing ──────────────────────── */
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

/* ── Panic Interception ──────────────────────────────────────────── */
static bool s_panic_jmp_armed = false;
static jmp_buf s_panic_jmp;
static char s_last_panic_msg[256];

#include <unistd.h>

void kpanic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_panic_msg, sizeof(s_last_panic_msg), fmt, ap);
    va_end(ap);

    if (s_panic_jmp_armed) {
        longjmp(s_panic_jmp, 1);
    }
    write(2, "\nUnexpected PANIC: ", 19);
    write(2, s_last_panic_msg, strlen(s_last_panic_msg));
    write(2, "\n", 1);
    exit(1);
}


void serial_printk(const char *fmt, ...)
{
    (void)fmt;
}

/* ── VFS & FS Mocks for fs_boot_mounts ────────────────────────────── */
static gpt_info_t *s_mock_gpt = NULL;
static int s_mock_ext2_init_ret = 0;
static int s_mock_fat32_init_ret = 0;
static bool s_root_mounted = false;
static bool s_boot_mounted = false;
static bool s_tmpfs_init_called = false;
static bool s_procfs_init_called = false;

gpt_info_t *gpt_scan(block_device_t *disk)
{
    (void)disk;
    return s_mock_gpt;
}

int ext2_init(block_device_t *dev, ext2_fs_t **fs)
{
    (void)dev;
    if (fs) *fs = (ext2_fs_t *)0x1234;
    return s_mock_ext2_init_ret;
}

int fat32_init(block_device_t *dev, fat32_fs_t **fs)
{
    (void)dev;
    if (fs) *fs = (fat32_fs_t *)0x5678;
    return s_mock_fat32_init_ret;
}

struct vfs_ops ext2_vfs_ops = {0};
struct vfs_ops fat_vfs_ops = {0};

int vfs_mount(const char *path, block_device_t *dev, struct vfs_ops *ops, void *data)
{
    (void)dev;
    (void)ops;
    (void)data;
    if (strcmp(path, "/") == 0) {
        s_root_mounted = true;
    } else if (strcmp(path, "/boot") == 0) {
        s_boot_mounted = true;
    }
    return 0;
}

void tmpfs_init(void)
{
    s_tmpfs_init_called = true;
}

void procfs_init(void)
{
    s_procfs_init_called = true;
}

int devfs_register_blkdev(const char *name, block_device_t *dev)
{
    (void)name;
    (void)dev;
    return 0;
}

void vfs_init(void) {}
void devfs_init(void) {}
void vfs_debug_list(const char *path) { (void)path; }
struct vfs_node *vfs_lookup(const char *path) { (void)path; return NULL; }
int vfs_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buf) { (void)node; (void)offset; (void)size; (void)buf; return 0; }
int vfs_write(struct vfs_node *node, uint64_t offset, uint64_t size, void *buf) { (void)node; (void)offset; (void)size; (void)buf; return 0; }
void vfs_node_put(struct vfs_node *node) { (void)node; }

/* ── Transitional net_hw_init stub ──────────────────────────────── */
static int s_mock_net_hw_ret = 0;
static int s_net_hw_calls = 0;

int net_hw_init(void)
{
    s_net_hw_calls++;
    return s_mock_net_hw_ret;
}

/* ── Fake PCI Backend Infrastructure ────────────────────────────── */
#define MAX_FAKE_DEVICES 16

struct fake_pci_desc {
    uint16_t domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t fn;
    bool present;
    uint32_t dwords[64];
};

static struct fake_pci_desc s_fake_pci[MAX_FAKE_DEVICES];
static unsigned s_fake_pci_count = 0;
static struct pci_root s_fake_roots[2];

static int fake_pci_read32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                           uint16_t offset, uint32_t *val, enum pci_error_scope *scope)
{
    (void)scope;
    for (unsigned i = 0; i < s_fake_pci_count; i++) {
        struct fake_pci_desc *d = &s_fake_pci[i];
        if (d->domain == domain && d->bus == bus && d->slot == slot && d->fn == fn) {
            if (!d->present) {
                *val = 0xFFFFFFFF;
                return 0;
            }
            if (offset / 4 < 64) {
                *val = d->dwords[offset / 4];
                return 0;
            }
            *val = 0;
            return 0;
        }
    }
    *val = 0xFFFFFFFF;
    return 0;
}

static int fake_pci_write32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                            uint16_t offset, uint32_t val, enum pci_error_scope *scope)
{
    (void)scope;
    for (unsigned i = 0; i < s_fake_pci_count; i++) {
        struct fake_pci_desc *d = &s_fake_pci[i];
        if (d->domain == domain && d->bus == bus && d->slot == slot && d->fn == fn) {
            if (offset / 4 < 64) {
                d->dwords[offset / 4] = val;
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
    .root_count = 1,
    .read32 = fake_pci_read32,
    .write32 = fake_pci_write32,
    .route_gsi = fake_pci_route_gsi,
};

const struct pci_backend *arch_pci_backend(void)
{
    return &s_fake_backend;
}

static void add_fake_pci_device(uint8_t bus, uint8_t slot, uint8_t fn,
                               uint16_t vendor, uint16_t device, uint32_t class_code)
{
    if (s_fake_pci_count >= MAX_FAKE_DEVICES) return;
    struct fake_pci_desc *d = &s_fake_pci[s_fake_pci_count++];
    memset(d, 0, sizeof(*d));
    d->domain = 0;
    d->bus = bus;
    d->slot = slot;
    d->fn = fn;
    d->present = true;
    d->dwords[0] = (uint32_t)vendor | ((uint32_t)device << 16);
    d->dwords[2] = (class_code << 8);
    d->dwords[3] = 0x00000000; /* header type 0 */
}

/* ── Declared Driver Table Mocking ───────────────────────────────── */
extern const struct pci_driver **g_test_pci_drivers_start;
extern const struct pci_driver **g_test_pci_drivers_end;

const struct pci_driver *__pci_drivers_start[1] = {NULL};
const struct pci_driver *__pci_drivers_end[1] = {NULL};

static const struct pci_driver *s_test_driver_ptrs[8];

static void set_test_pci_drivers(const struct pci_driver **drivers, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        s_test_driver_ptrs[i] = drivers[i];
    }
    g_test_pci_drivers_start = s_test_driver_ptrs;
    g_test_pci_drivers_end = s_test_driver_ptrs + count;
}

/* ── Test Reset Helper ───────────────────────────────────────────── */
static void reset_test_state(void)
{
    s_fake_pci_count = 0;
    s_fake_roots[0].domain = 0;
    s_fake_roots[0].bus = 0;
    s_fake_backend.root_count = 1;

    fake_pre_gs_cpu_id_calls = 0;
    absent_ahci_probe_calls = 0;
    s_inject_kmalloc_fail = false;
    s_panic_jmp_armed = false;
    s_last_panic_msg[0] = '\0';

    s_mock_gpt = NULL;
    s_mock_ext2_init_ret = 0;
    s_mock_fat32_init_ret = 0;
    s_root_mounted = false;
    s_boot_mounted = false;
    s_tmpfs_init_called = false;
    s_procfs_init_called = false;

    s_mock_net_hw_ret = 0;
    s_net_hw_calls = 0;

    device_boot_reset_for_test();
    pci_core_reset_for_test();
    block_device_init();
    device_core_init();

    set_test_pci_drivers(NULL, 0);
}

/* ── 1. test_optional_absent_continues ───────────────────────────── */
static int mock_absent_ahci_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev;
    (void)id;
    absent_ahci_probe_calls++;
    return 0;
}

static void test_optional_absent_continues(void)
{
    TEST_CASE("Optional absent AHCI continues: probe=0, coordinator success, SKIPPED_ABSENT recorded");
    reset_test_state();

    /* PCI bus has only an unrelated non-AHCI device (or empty bus) */
    add_fake_pci_device(0, 1, 0, 0x1234, 0x5678, 0x060000); /* host bridge */

    /* Declared AHCI driver */
    static const struct pci_device_id ahci_match_ids[] = {
        {
            .vendor = PCI_ID_ANY,
            .device = PCI_ID_ANY,
            .subvendor = PCI_ID_ANY,
            .subdevice = PCI_ID_ANY,
            .class_value = 0x010601, /* Mass Storage, SATA, AHCI */
            .class_mask = 0xFFFFFF,
        },
    };
    static const struct pci_driver ahci_mock_driver = {
        .name = "ahci",
        .id_table = ahci_match_ids,
        .id_count = 1,
        .probe = mock_absent_ahci_probe,
    };
    const struct pci_driver *drivers[] = { &ahci_mock_driver };
    set_test_pci_drivers(drivers, 1);

    int rc = device_boot_init();
    assert_eq(0, rc);
    assert_eq(0, absent_ahci_probe_calls);
    assert_eq(0, device_boot_result());
    assert_true(s_net_hw_calls > 0);

    const struct device_boot_summary *sum = device_boot_get_summary();
    assert_not_null(sum);
    assert_eq(1, sum->skipped_absent_drivers);
    assert_eq(0, sum->bound_devices);
}

/* ── 2. test_registration_and_root_failure ───────────────────────── */
static int mock_unsafe_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev;
    (void)id;
    return DEVICE_UNSAFE;
}

static void test_registration_and_root_failure(void)
{
    TEST_CASE("Driver registration OOM and DEVICE_UNSAFE block consumers");
    reset_test_state();

    /* Subtest A: Registration OOM */
    static const struct pci_driver dummy_driver = {
        .name = "dummy",
        .id_table = NULL,
        .id_count = 0,
    };
    const struct pci_driver *drivers[] = { &dummy_driver };
    set_test_pci_drivers(drivers, 1);

    s_inject_kmalloc_fail = true;
    int rc = device_boot_init();
    assert_true(rc != 0);
    assert_eq(rc, device_boot_result());
    s_inject_kmalloc_fail = false;

    /* Subtest B: Probe returns DEVICE_UNSAFE */
    reset_test_state();
    add_fake_pci_device(0, 2, 0, 0x8086, 0x1234, 0x010601);

    static const struct pci_device_id unsafe_ids[] = {
        { .vendor = 0x8086, .device = 0x1234, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY },
    };
    static const struct pci_driver unsafe_driver = {
        .name = "unsafe_drv",
        .id_table = unsafe_ids,
        .id_count = 1,
        .probe = mock_unsafe_probe,
    };
    const struct pci_driver *u_drivers[] = { &unsafe_driver };
    set_test_pci_drivers(u_drivers, 1);

    s_panic_jmp_armed = true;
    if (setjmp(s_panic_jmp) == 0) {
        rc = device_boot_init();
    } else {
        rc = device_boot_result();
    }
    s_panic_jmp_armed = false;
    assert_eq(DEVICE_UNSAFE, rc);
    assert_eq(DEVICE_UNSAFE, device_boot_result());
}


/* ── 3. test_bad_nic_good_root ───────────────────────────────────── */
static int mock_failing_nic_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev;
    (void)id;
    return -EIO;
}

static int mock_good_bdev_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return 0;
}
static const struct block_device_ops s_ahci_mock_ops = {
    .read = mock_good_bdev_read,
};

static int mock_good_ahci_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    struct block_device_desc desc = {
        .name = "hda",
        .sector_count = 204800,
        .sector_size = 512,
        .ops = &s_ahci_mock_ops,
        .kind = BLOCK_DISK,
    };
    block_device_t *out = NULL;
    return block_device_register(&desc, &out);
}

static void test_bad_nic_good_root(void)
{
    TEST_CASE("Bad function does not prevent AHCI binding or root availability");
    reset_test_state();

    /* Device 1: Bad NIC (00:03.0) */
    add_fake_pci_device(0, 3, 0, 0x8086, 0x100E, 0x020000);
    /* Device 2: Healthy AHCI controller (00:04.0) */
    add_fake_pci_device(0, 4, 0, 0x8086, 0x2829, 0x010601);

    static const struct pci_device_id nic_ids[] = {
        { .vendor = 0x8086, .device = 0x100E, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY },
    };
    static const struct pci_driver nic_driver = {
        .name = "nic_drv",
        .id_table = nic_ids,
        .id_count = 1,
        .probe = mock_failing_nic_probe,
    };

    static const struct pci_device_id ahci_ids[] = {
        { .vendor = 0x8086, .device = 0x2829, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY },
    };
    static const struct pci_driver ahci_driver = {
        .name = "ahci_drv",
        .id_table = ahci_ids,
        .id_count = 1,
        .probe = mock_good_ahci_probe,
    };

    const struct pci_driver *drivers[] = { &nic_driver, &ahci_driver };
    set_test_pci_drivers(drivers, 2);

    int rc = device_boot_init();
    assert_eq(0, rc);
    assert_eq(0, device_boot_result());

    /* Verify NIC is DEV_FAILED, AHCI is DEV_BOUND */
    struct pci_device *nic_pdev = pci_device_lookup(0, 0, 3, 0);
    assert_not_null(nic_pdev);
    assert_eq(DEV_FAILED, nic_pdev->dev.state);
    assert_eq(-EIO, nic_pdev->dev.last_error);

    struct pci_device *ahci_pdev = pci_device_lookup(0, 0, 4, 0);
    assert_not_null(ahci_pdev);
    assert_eq(DEV_BOUND, ahci_pdev->dev.state);

    /* Verify disk registered */
    assert_eq(1, block_device_count());
    block_device_t *bdev = block_device_get(0);
    assert_not_null(bdev);
    assert_str_eq("hda", bdev->name);
}

/* ── 4. test_pre_gs_boot ─────────────────────────────────────────── */
static void test_pre_gs_boot(void)
{
    TEST_CASE("Pre-GS boot safety: phase 6 flow does not invoke this_cpu or cpu_id");
    reset_test_state();

    add_fake_pci_device(0, 4, 0, 0x8086, 0x2829, 0x010601);
    static const struct pci_device_id ahci_ids[] = {
        { .vendor = 0x8086, .device = 0x2829, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY },
    };
    static const struct pci_driver ahci_driver = {
        .name = "ahci_drv",
        .id_table = ahci_ids,
        .id_count = 1,
        .probe = mock_good_ahci_probe,
    };
    const struct pci_driver *drivers[] = { &ahci_driver };
    set_test_pci_drivers(drivers, 1);

    fake_pre_gs_cpu_id_calls = 0;
    int rc = device_boot_init();
    assert_eq(0, rc);
    assert_eq(0, fake_pre_gs_cpu_id_calls);
}

/* ── 5. test_root_requirement ────────────────────────────────────── */
static void test_root_requirement(void)
{
    TEST_CASE("Root requirement: missing disk/failed mount panics, valid GPT and FAT fallback succeed");
    reset_test_state();

    /* Subtest A: No block devices -> fs_boot_mounts panics with 'root filesystem unavailable' */
    s_panic_jmp_armed = true;
    if (setjmp(s_panic_jmp) == 0) {
        fs_boot_mounts();
        assert_true(false); /* Should not be reached */
    }
    s_panic_jmp_armed = false;
    assert_not_null(strstr(s_last_panic_msg, "root filesystem unavailable"));

    /* Subtest B: Dual-partition GPT layout -> ext2 root mount succeeds */
    reset_test_state();
    struct block_device_desc disk_desc = {
        .name = "hda",
        .sector_count = 204800,
        .sector_size = 512,
        .ops = &s_ahci_mock_ops,
        .kind = BLOCK_DISK,
    };
    block_device_t *disk_dev = NULL;
    assert_eq(0, block_device_register(&disk_desc, &disk_dev));

    gpt_info_t fake_gpt;
    memset(&fake_gpt, 0, sizeof(fake_gpt));
    fake_gpt.count = 2;
    fake_gpt.partitions[0].dev = disk_dev;
    fake_gpt.partitions[1].dev = disk_dev;
    s_mock_gpt = &fake_gpt;
    s_mock_ext2_init_ret = 0;
    s_mock_fat32_init_ret = 0;

    s_panic_jmp_armed = true;
    if (setjmp(s_panic_jmp) == 0) {
        fs_boot_mounts();
    } else {
        assert_true(false); /* Should not panic */
    }
    s_panic_jmp_armed = false;
    assert_true(s_root_mounted);
    assert_true(s_boot_mounted);
    assert_true(s_tmpfs_init_called);
    assert_true(s_procfs_init_called);

    /* Subtest C: Fallback single-FAT32 (no GPT) succeeds */
    reset_test_state();
    assert_eq(0, block_device_register(&disk_desc, &disk_dev));
    s_mock_gpt = NULL; /* No GPT */
    s_mock_fat32_init_ret = 0;

    s_panic_jmp_armed = true;
    if (setjmp(s_panic_jmp) == 0) {
        fs_boot_mounts();
    } else {
        assert_true(false); /* Should not panic */
    }
    s_panic_jmp_armed = false;
    assert_true(s_root_mounted);

    /* Subtest D: GPT present but ext2_init fails -> panics with root filesystem unavailable */
    reset_test_state();
    assert_eq(0, block_device_register(&disk_desc, &disk_dev));
    s_mock_gpt = &fake_gpt;
    s_mock_ext2_init_ret = -EIO;

    s_panic_jmp_armed = true;
    if (setjmp(s_panic_jmp) == 0) {
        fs_boot_mounts();
        assert_true(false); /* Should not reach */
    }
    s_panic_jmp_armed = false;
    assert_not_null(strstr(s_last_panic_msg, "root filesystem unavailable"));
}

/* ── 6. test_single_init ─────────────────────────────────────────── */
static int s_single_probe_calls = 0;
static int mock_counting_probe(struct pci_device *pdev, const struct pci_device_id *id)
{
    (void)pdev; (void)id;
    s_single_probe_calls++;
    struct block_device_desc desc = {
        .name = "hda",
        .sector_count = 204800,
        .sector_size = 512,
        .ops = &s_ahci_mock_ops,
        .kind = BLOCK_DISK,
    };
    block_device_t *out = NULL;
    return block_device_register(&desc, &out);
}

static void test_single_init(void)
{
    TEST_CASE("Single init idempotency: repeated device_boot_init does not clear table or re-probe");
    reset_test_state();

    add_fake_pci_device(0, 4, 0, 0x8086, 0x2829, 0x010601);

    static const struct pci_device_id ids[] = {
        { .vendor = 0x8086, .device = 0x2829, .subvendor = PCI_ID_ANY, .subdevice = PCI_ID_ANY },
    };
    static const struct pci_driver counting_driver = {
        .name = "counting_drv",
        .id_table = ids,
        .id_count = 1,
        .probe = mock_counting_probe,
    };
    const struct pci_driver *drivers[] = { &counting_driver };
    set_test_pci_drivers(drivers, 1);
    s_single_probe_calls = 0;

    /* First initialization */
    int r1 = device_boot_init();
    assert_eq(0, r1);
    assert_eq(1, s_single_probe_calls);
    assert_eq(1, block_device_count());

    /* Second initialization must be no-op */
    int r2 = device_boot_init();
    assert_eq(0, r2);
    assert_eq(1, s_single_probe_calls);
    assert_eq(1, block_device_count());
}

/* ── Test Suite Runner ───────────────────────────────────────────── */
int main(void)
{
    test_optional_absent_continues();
    test_registration_and_root_failure();
    test_bad_nic_good_root();
    test_pre_gs_boot();
    test_root_requirement();
    test_single_init();

    TEST_RESULTS();
    return 0;
}
