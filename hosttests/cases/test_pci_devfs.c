/*
 * test_pci_devfs.c — ARCH-12: the PCI device namespace in devfs
 *
 * Verifies:
 *  - Every attribute renders the exact documented bytes.
 *  - The configuration space attribute is 256 raw bytes.
 *  - The BAR list reports address and kind, and never a fabricated size.
 *  - Registration publishes /dev/pci/<BDF>/<attr> into the real devfs tree.
 *  - Reads are sliced by offset and end at EOF.
 */
#include "test_framework.h"
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <bus/pci/pci_devfs.h>
#include <arch/pci.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <block/blockdev.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arch/x86_64/uaccess.h>

/* ── Host runtime stubs (same set as test_devfs_hierarchy.c) ── */

static task_t s_test_task = {
    .pid = 1,
    .addr_limit = 0x00007fffffffffffULL,
};
task_t *vfs_test_current = &s_test_task;

int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_info_impl(const char *fmt, ...) { (void)fmt; }

int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -EIO;
}

int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -EIO;
}

uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

void get_random_bytes(void *b, size_t n) { if (b && n) memset(b, 0x42, n); }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int  user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }
tty_t *get_dev_tty(void) { return NULL; }

static const vfs_inode_ops_t s_dummy_proc_iops = {0};
static const vfs_file_ops_t  s_dummy_proc_fops = {0};
static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    mp->i_ops = &s_dummy_proc_iops;
    mp->f_ops = &s_dummy_proc_fops;
    mp->root->i_ops = &s_dummy_proc_iops;
    mp->root->f_ops = &s_dummy_proc_fops;
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0555;
    mp->root->nlink = 2;
    return 0;
}
static vfs_fs_type_t s_host_procfs_type = {
    .name     = "procfs",
    .fs_flags = VFS_FS_NO_DCACHE,
    .mount    = host_procfs_mount,
};
int procfs_register_fs(void) { return vfs_register_filesystem(&s_host_procfs_type); }

file_t *file_alloc(void)
{
    file_t *f = (file_t *)calloc(1, sizeof(file_t));
    if (f) {
        f->refcount = 1;
        f->type = FD_NONE;
    }
    return f;
}

void file_free(file_t *f)
{
    if (!f) return;
    if (f->type == FD_DEV) {
        devfs_release_file(f);
    }
    if (f->node) {
        vfs_node_put(f->node);
        f->node = NULL;
    }
    free(f);
}

/* ── Synthetic PCI device + mock backend ────────────────────── */

#define MOCK_VENDOR       0x8086u
#define MOCK_DEVICE       0x2922u
#define MOCK_CLASS        0x010601u
#define MOCK_REV          0x00u
#define MOCK_GSI          16u
#define MOCK_BAR0_IO      0xc000ull
#define MOCK_BAR2_MMIO    0xfebf0000ull
#define MOCK_BDF          "0000:00:1f.2"

static uint8_t s_config[256];
static struct pci_device s_mock_device;
static const struct pci_driver s_mock_driver = { .name = "ahci" };

static void mock_device_init(void)
{
    memset(s_config, 0, sizeof(s_config));
    s_config[0x00] = (uint8_t)(MOCK_VENDOR & 0xffu);
    s_config[0x01] = (uint8_t)((MOCK_VENDOR >> 8) & 0xffu);
    s_config[0x02] = (uint8_t)(MOCK_DEVICE & 0xffu);
    s_config[0x03] = (uint8_t)((MOCK_DEVICE >> 8) & 0xffu);
    s_config[0x04] = 0x07;                     /* command  */
    s_config[0x05] = 0x00;
    s_config[0x06] = 0x00;
    s_config[0x07] = 0x02;                     /* status   */
    s_config[0x08] = MOCK_REV;                 /* revision */
    s_config[0x09] = (uint8_t)((MOCK_CLASS >> 16) & 0xffu);
    s_config[0x0a] = (uint8_t)((MOCK_CLASS >> 8) & 0xffu);
    s_config[0x0b] = (uint8_t)(MOCK_CLASS & 0xffu);
    s_config[0x3c] = 0x0a;                     /* interrupt line 10 */
    s_config[0x3d] = 0x01;                     /* interrupt pin  1 */

    memset(&s_mock_device, 0, sizeof(s_mock_device));
    s_mock_device.domain     = 0;
    s_mock_device.bus        = 0;
    s_mock_device.slot       = 0x1f;
    s_mock_device.fn         = 2;
    s_mock_device.vendor     = MOCK_VENDOR;
    s_mock_device.device     = MOCK_DEVICE;
    s_mock_device.class_code = MOCK_CLASS;
    s_mock_device.driver     = &s_mock_driver;

    s_mock_device.bars[0].kind    = PCI_BAR_IO;
    s_mock_device.bars[0].address = MOCK_BAR0_IO;
    s_mock_device.bars[0].valid   = true;
    s_mock_device.bars[0].index   = 0;
    /* BAR 1 stays invalid on purpose. */
    s_mock_device.bars[2].kind    = PCI_BAR_MMIO32;
    s_mock_device.bars[2].address = MOCK_BAR2_MMIO;
    s_mock_device.bars[2].valid   = true;
    s_mock_device.bars[2].index   = 2;
    /* BAR 3 stays invalid on purpose. */
}

static int mock_read32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                       uint16_t offset, uint32_t *out, enum pci_error_scope *scope)
{
    (void)domain; (void)bus; (void)slot; (void)fn;
    if (scope) *scope = PCI_ERROR_FUNCTION;
    if (!out) return -EINVAL;
    if ((size_t)offset + 4 > sizeof(s_config)) return -EINVAL;
    *out = (uint32_t)s_config[offset]
         | ((uint32_t)s_config[offset + 1] << 8)
         | ((uint32_t)s_config[offset + 2] << 16)
         | ((uint32_t)s_config[offset + 3] << 24);
    return 0;
}

static int mock_write32(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t fn,
                        uint16_t offset, uint32_t value, enum pci_error_scope *scope)
{
    (void)domain; (void)bus; (void)slot; (void)fn; (void)offset; (void)value;
    if (scope) *scope = PCI_ERROR_FUNCTION;
    return 0;
}

static int mock_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    (void)pdev;
    if (!out) return -EINVAL;
    *out = MOCK_GSI;
    return 0;
}

static const struct pci_root s_mock_roots[] = { { 0, 0 } };

const struct pci_backend *arch_pci_backend(void)
{
    static const struct pci_backend backend = {
        .roots      = s_mock_roots,
        .root_count = 1,
        .read32     = mock_read32,
        .write32    = mock_write32,
        .route_gsi  = mock_route_gsi,
    };
    return &backend;
}

/* ── Helpers ─────────────────────────────────────────────────── */

// Does `dir` list an entry called `name` with the given type?
// type == 0 matches any type.
static bool readdir_has(vfs_node_t *dir, const char *name, uint8_t type)
{
    char name_buf[VFS_NAME_MAX];
    vfs_dirent_t entry;
    uint64_t idx = 0;
    while (1) {
        memset(&entry, 0, sizeof(entry));
        entry.name = name_buf;
        if (dir->f_ops->readdir(dir, idx++, &entry) != 0) return false;
        if (entry.name[0] == '\0') return false;
        if (strcmp(entry.name, name) == 0)
            return type == 0 || entry.type == type;
    }
}

/* ── Test cases ─────────────────────────────────────────────── */

TEST_FUNC(test_pci_devfs_attr_vendor_device_class) {
    char buf[64];

    int n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_VENDOR, buf, sizeof(buf));
    assert_eq(7, n);
    assert_str_eq("0x8086\n", buf);

    n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_DEVICE, buf, sizeof(buf));
    assert_eq(7, n);
    assert_str_eq("0x2922\n", buf);

    n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_CLASS, buf, sizeof(buf));
    assert_eq(9, n);
    assert_str_eq("0x010601\n", buf);
}

TEST_FUNC(test_pci_devfs_attr_config_is_256_raw_bytes) {
    char buf[512];

    int n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_CONFIG, buf, sizeof(buf));
    assert_eq(256, n);

    uint32_t id = (uint32_t)MOCK_VENDOR | ((uint32_t)MOCK_DEVICE << 16);
    uint8_t expect[4] = {
        (uint8_t)(id & 0xffu),
        (uint8_t)((id >> 8) & 0xffu),
        (uint8_t)((id >> 16) & 0xffu),
        (uint8_t)((id >> 24) & 0xffu),
    };
    assert_mem_eq(expect, buf, 4);
    assert_eq(MOCK_REV, (unsigned char)buf[0x08]);
    assert_eq(0x01, (unsigned char)buf[0x09]);   /* class base */
    assert_eq(0x06, (unsigned char)buf[0x0a]);   /* subclass  */
    assert_eq(0x01, (unsigned char)buf[0x0b]);   /* prog-if   */
}

TEST_FUNC(test_pci_devfs_attr_resource_lists_bars) {
    char buf[512];

    int n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_RESOURCE, buf, sizeof(buf));
    assert_true(n > 0);

    char expect[128];
    snprintf(expect, sizeof(expect),
             "Region 0: I/O ports at 0x%llx\nRegion 2: Memory at 0x%llx\n",
             (unsigned long long)MOCK_BAR0_IO,
             (unsigned long long)MOCK_BAR2_MMIO);
    assert_str_eq(expect, buf);

    // An invalid BAR never shows up, and the capacity is never invented.
    assert_null(strstr(buf, "Region 1:"));
    assert_null(strstr(buf, "Region 3:"));
    assert_null(strstr(buf, "size="));
}

TEST_FUNC(test_pci_devfs_attr_irq_and_driver) {
    char buf[64];

    int n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_IRQ, buf, sizeof(buf));
    assert_eq(3, n);
    assert_str_eq("16\n", buf);

    n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_DRIVER, buf, sizeof(buf));
    assert_eq(5, n);
    assert_str_eq("ahci\n", buf);

    // An unbound device reports "none".
    struct pci_device unbound = s_mock_device;
    unbound.driver = NULL;
    n = pci_devfs_format_attr(&unbound, PCI_ATTR_DRIVER, buf, sizeof(buf));
    assert_eq(5, n);
    assert_str_eq("none\n", buf);
}

TEST_FUNC(test_pci_devfs_format_reports_full_length) {
    char buf[8];

    // "0x8086\n" is 7 bytes; a 4-byte buffer truncates to "0x8\0" but the
    // return value still reports the full length.
    int n = pci_devfs_format_attr(&s_mock_device, PCI_ATTR_VENDOR, buf, 4);
    assert_eq(7, n);
    assert_mem_eq("0x8", buf, 4);

    // A NULL device or an out-of-range attribute is rejected.
    assert_eq(-EINVAL, pci_devfs_format_attr(NULL, PCI_ATTR_VENDOR, buf, sizeof(buf)));
    assert_eq(-EINVAL, pci_devfs_format_attr(&s_mock_device, PCI_ATTR_COUNT, buf, sizeof(buf)));
}

TEST_FUNC(test_pci_devfs_register_publishes_paths) {
    vfs_init();
    devfs_init();

    int published = pci_devfs_register_device(&s_mock_device);
    assert_eq(7, published);

    // The attribute node is reachable through the real VFS walk.
    vfs_node_t *node = vfs_lookup("/dev/pci/" MOCK_BDF "/vendor");
    assert_not_null(node);
    if (node) {
        assert_eq(VFS_CHRDEV, node->type);
        vfs_node_put(node);
    }

    // /dev/pci lists the BDF as a directory.
    vfs_node_t *ns = vfs_lookup("/dev/pci");
    assert_not_null(ns);
    if (ns) {
        assert_eq(VFS_DIR, ns->type);
        assert_true(readdir_has(ns, MOCK_BDF, VFS_DIR));
        vfs_node_put(ns);
    }

    // The BDF directory lists all seven attributes.
    vfs_node_t *bdf = vfs_lookup("/dev/pci/" MOCK_BDF);
    assert_not_null(bdf);
    if (bdf) {
        assert_true(readdir_has(bdf, "vendor", VFS_CHRDEV));
        assert_true(readdir_has(bdf, "device", VFS_CHRDEV));
        assert_true(readdir_has(bdf, "class", VFS_CHRDEV));
        assert_true(readdir_has(bdf, "config", VFS_CHRDEV));
        assert_true(readdir_has(bdf, "resource", VFS_CHRDEV));
        assert_true(readdir_has(bdf, "irq", VFS_CHRDEV));
        assert_true(readdir_has(bdf, "driver", VFS_CHRDEV));
        vfs_node_put(bdf);
    }
}

TEST_FUNC(test_pci_devfs_read_slices_by_offset) {
    vfs_init();
    devfs_init();
    pci_devfs_register_device(&s_mock_device);

    vfs_node_t *node = vfs_lookup("/dev/pci/" MOCK_BDF "/vendor");
    assert_not_null(node);
    if (!node) return;

    char buf[32];
    memset(buf, 0, sizeof(buf));

    // A full read returns the whole attribute.
    assert_eq(7, vfs_read(node, 0, sizeof(buf), buf));
    assert_mem_eq("0x8086\n", buf, 7);

    // A read at an offset returns the tail only.  "0x8086\n" ends with
    // "86\n" at index 4, so the tail is three bytes long.
    memset(buf, 0, sizeof(buf));
    assert_eq(3, vfs_read(node, 4, sizeof(buf), buf));
    assert_mem_eq("86\n", buf, 3);

    // A read past the end is EOF.
    assert_eq(0, vfs_read(node, 7, sizeof(buf), buf));

    // A zero-length read transfers nothing.
    assert_eq(0, vfs_read(node, 0, 0, buf));

    vfs_node_put(node);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_pci_devfs_attr_vendor_device_class),
    TEST_ENTRY(test_pci_devfs_attr_config_is_256_raw_bytes),
    TEST_ENTRY(test_pci_devfs_attr_resource_lists_bars),
    TEST_ENTRY(test_pci_devfs_attr_irq_and_driver),
    TEST_ENTRY(test_pci_devfs_format_reports_full_length),
    TEST_ENTRY(test_pci_devfs_register_publishes_paths),
    TEST_ENTRY(test_pci_devfs_read_slices_by_offset),
TEST_LIST_END

int main(void) {
    mock_device_init();
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
