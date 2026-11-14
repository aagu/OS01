/* @os01-test
 * id: test_gpt_bootdev
 * layer: host
 * subsystem: fs
 * timing: strict
 * tier: nightly
 * link: gpt-bootdev
 * scenarios: [fs.gpt.bootdev]
 */
#include "test_framework.h"
#include <block/blockdev.h>
#include <core/bootinfo.h>
#include <fs/boot.h>
#include <fs/gpt.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fake_pre_gs_cpu_id_calls = 0;

/* ── OOM injection ────────────────────────────────────────────── */
static int s_kmalloc_fail_after = -1;
static int s_kmalloc_calls = 0;

void *kmalloc(size_t size)
{
    if (s_kmalloc_fail_after >= 0 && s_kmalloc_calls >= s_kmalloc_fail_after)
        return NULL;
    s_kmalloc_calls++;
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

/* ── Fake block & devfs registry ──────────────────────────────── */
#define MAX_FAKE_DEVS 32
static block_device_t s_fake_devs[MAX_FAKE_DEVS];
static int s_fake_dev_count = 0;
static int s_blk_reg_calls = 0;
static int s_blk_unreg_calls = 0;
static int s_devfs_reg_calls = 0;
static int s_devfs_fail_on_call = -1;
static int s_devfs_fail_rc = -EEXIST;
static char s_devfs_fail_name[32];

/* ── VFS / boot_failure mocks for fs_boot_mounts ──────────────── */
static bool s_stop_armed = false;
static jmp_buf s_stop_jmp;
static int s_stop_calls = 0;
static int64_t s_stop_rc = 0;
static int s_warn_calls = 0;

struct mount_rec {
    char type[16];
    char path[32];
    char dev_name[16];
};
static struct mount_rec s_mounts[8];
static int s_mount_count = 0;
static int s_fail_root_mount_rc = 0;
static int s_fail_boot_mount_rc = 0;
static int s_tmpfs_calls = 0;
static int s_procfs_calls = 0;

static void reset_fake_registry(void)
{
    memset(s_fake_devs, 0, sizeof(s_fake_devs));
    s_fake_dev_count = 0;
    s_blk_reg_calls = 0;
    s_blk_unreg_calls = 0;
    s_devfs_reg_calls = 0;
    s_devfs_fail_on_call = -1;
    s_devfs_fail_rc = -EEXIST;
    s_devfs_fail_name[0] = '\0';
    s_kmalloc_fail_after = -1;
    s_kmalloc_calls = 0;
    s_stop_armed = false;
    s_stop_calls = 0;
    s_stop_rc = 0;
    s_warn_calls = 0;
    memset(s_mounts, 0, sizeof(s_mounts));
    s_mount_count = 0;
    s_fail_root_mount_rc = 0;
    s_fail_boot_mount_rc = 0;
    s_tmpfs_calls = 0;
    s_procfs_calls = 0;
}

int block_device_count(void)
{
    return s_fake_dev_count;
}

block_device_t *block_device_get(int index)
{
    if (index < 0 || index >= s_fake_dev_count)
        return NULL;
    return &s_fake_devs[index];
}

__attribute__((noreturn))
void boot_failure_stop(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    (void)stage; (void)component; (void)phase; (void)cpu;
    s_stop_calls++;
    s_stop_rc = rc;
    if (s_stop_armed)
        longjmp(s_stop_jmp, 1);
    abort();
}

void boot_failure_warn(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    (void)stage; (void)component; (void)rc; (void)phase; (void)cpu;
    s_warn_calls++;
}

void kpanic(const char *fmt, ...)
{
    (void)fmt;
    abort();
}

bool boot3_test_fail_site(const char *site) { (void)site; return false; }
void boot3_test_sentinel(const char *name) { (void)name; }
void vfs_init(void) {}
int devfs_init(void) { return 0; }
void vfs_debug_list(const char *path) { (void)path; }
void *vfs_lookup(const char *path) { (void)path; return NULL; }
int vfs_read(void *node, uint64_t off, size_t len, void *buf)
{ (void)node; (void)off; (void)len; (void)buf; return 0; }
int vfs_write(void *node, uint64_t off, size_t len, const void *buf)
{ (void)node; (void)off; (void)len; (void)buf; return 0; }
void vfs_node_put(void *node) { (void)node; }
int tmpfs_init(void) { s_tmpfs_calls++; return 0; }
int procfs_init(void) { s_procfs_calls++; return 0; }

int vfs_mount_fs(const char *type_name, const char *path,
                 block_device_t *dev, void *data)
{
    (void)data;
    if (strcmp(path, "/") == 0 && s_fail_root_mount_rc != 0)
        return s_fail_root_mount_rc;
    if (strcmp(path, "/boot") == 0 && s_fail_boot_mount_rc != 0)
        return s_fail_boot_mount_rc;
    if (s_mount_count < 8) {
        snprintf(s_mounts[s_mount_count].type, 16, "%s", type_name);
        snprintf(s_mounts[s_mount_count].path, 32, "%s", path);
        snprintf(s_mounts[s_mount_count].dev_name, 16, "%s", dev ? dev->name : "(null)");
        s_mount_count++;
    }
    return 0;
}

int block_device_register(const struct block_device_desc *desc,
                          block_device_t **out_dev)
{
    s_blk_reg_calls++;
    if (!desc || !desc->name || s_fake_dev_count >= MAX_FAKE_DEVS)
        return -ENOMEM;
    block_device_t *d = &s_fake_devs[s_fake_dev_count++];
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "%s", desc->name);
    d->sector_count = desc->sector_count;
    d->sector_size = desc->sector_size;
    d->ops = desc->ops;
    d->private_data = desc->private_data;
    d->parent = desc->parent;
    d->kind = desc->kind;
    d->present = true;
    if (out_dev)
        *out_dev = d;
    return 0;
}

int block_device_unregister_boot(block_device_t *dev)
{
    s_blk_unreg_calls++;
    if (dev)
        dev->present = false;
    return 0;
}

int devfs_register_blkdev(const char *name, block_device_t *dev)
{
    (void)dev;
    int idx = s_devfs_reg_calls++;
    if (s_devfs_fail_name[0] && name && strcmp(name, s_devfs_fail_name) == 0)
        return s_devfs_fail_rc;
    if (s_devfs_fail_on_call >= 0 && idx == s_devfs_fail_on_call)
        return s_devfs_fail_rc;
    return 0;
}

/* ── Backing disk image & read mock ───────────────────────────── */
struct fake_disk_backing {
    uint8_t *sectors;
    uint64_t sector_count;
    int read_calls;
    int fail_lba;
    int fail_rc;
};

static int fake_disk_read(block_device_t *dev, uint64_t lba,
                          uint32_t count, void *buf)
{
    struct fake_disk_backing *b = (struct fake_disk_backing *)dev->private_data;
    b->read_calls++;
    if (b->fail_lba >= 0 && lba <= (uint64_t)b->fail_lba &&
        lba + count > (uint64_t)b->fail_lba) {
        return b->fail_rc;
    }
    if (lba + count < lba || lba + count > b->sector_count)
        return -EIO;
    if (b->sectors) {
        memcpy(buf, b->sectors + lba * dev->sector_size,
               (size_t)count * dev->sector_size);
    } else {
        memset(buf, 0, (size_t)count * dev->sector_size);
    }
    return 0;
}

static const struct block_device_ops s_fake_disk_ops = {
    .read = fake_disk_read,
};

int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->ops || !dev->ops->read)
        return -EINVAL;
    return dev->ops->read(dev, lba, count, buf);
}

int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -EROFS;
}

int block_device_flush(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/* ── GPT image builder helpers ────────────────────────────────── */
static uint32_t test_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static const uint8_t k_esp_type_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

static const uint8_t k_linux_type_guid[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
};

static void write_gpt_entry(uint8_t *entries, uint32_t slot, uint32_t entry_size,
                            const uint8_t type_guid[16],
                            const uint8_t uniq_guid[16],
                            uint64_t start_lba, uint64_t end_lba)
{
    uint8_t *e = entries + (size_t)slot * entry_size;
    memset(e, 0, entry_size);
    memcpy(e, type_guid, 16);
    memcpy(e + 16, uniq_guid, 16);
    put_le64(e + 32, start_lba);
    put_le64(e + 40, end_lba);
}

static void finalize_gpt_header(uint8_t *hdr, uint32_t header_size,
                                uint64_t entry_lba, uint32_t num_entries,
                                uint32_t entry_size, uint64_t last_usable,
                                const uint8_t *entries)
{
    memset(hdr, 0, 512);
    memcpy(hdr, "EFI PART", 8);
    put_le32(hdr + 8, 0x00010000u);
    put_le32(hdr + 12, header_size);
    put_le64(hdr + 40, 34u);
    put_le64(hdr + 48, last_usable);
    put_le64(hdr + 72, entry_lba);
    put_le32(hdr + 80, num_entries);
    put_le32(hdr + 84, entry_size);
    if (entries) {
        uint32_t ecrc = test_crc32(entries, (size_t)num_entries * entry_size);
        put_le32(hdr + 88, ecrc);
    }
    put_le32(hdr + 16, 0);
    uint32_t hcrc = test_crc32(hdr, header_size);
    put_le32(hdr + 16, hcrc);
}

static void init_disk(block_device_t *disk, struct fake_disk_backing *b,
                      const char *name, uint64_t sector_count,
                      uint32_t sector_size, uint8_t *buf)
{
    memset(disk, 0, sizeof(*disk));
    memset(b, 0, sizeof(*b));
    b->sectors = buf;
    b->sector_count = sector_count;
    b->fail_lba = -1;
    b->fail_rc = -EIO;
    snprintf(disk->name, sizeof(disk->name), "%s", name);
    disk->sector_count = sector_count;
    disk->sector_size = sector_size;
    disk->ops = &s_fake_disk_ops;
    disk->private_data = b;
    disk->kind = BLOCK_DISK;
    disk->present = true;
}

/* ── Tests ────────────────────────────────────────────────────── */
static void gpt_metadata_has_no_side_effects(void)
{
    reset_fake_registry();

    uint8_t *img = calloc(4096, 512);
    assert_not_null(img);

    uint8_t g1[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,1 };
    uint8_t g2[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,2 };
    uint8_t g3[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,3 };

    uint8_t *entries = img + 2 * 512;
    write_gpt_entry(entries, 0, 128, k_esp_type_guid, g1, 2048, 3071);
    write_gpt_entry(entries, 17, 128, k_linux_type_guid, g2, 4096, 8191);
    write_gpt_entry(entries, 19, 128, k_linux_type_guid, g3, 8192, 10000);
    finalize_gpt_header(img + 512, 92, 2, 20, 128, 400000, entries);

    block_device_t disk;
    struct fake_disk_backing b;
    init_disk(&disk, &b, "usba", 395264, 512, img);

    /* Read-only parse must have zero block/devfs side effects */
    struct gpt_table *t = NULL;
    assert_eq(0, gpt_read_table(&disk, &t));
    assert_not_null(t);
    assert_eq(0, s_blk_reg_calls);
    assert_eq(0, s_devfs_reg_calls);
    assert_eq(3, gpt_table_count(t));

    gpt_partition_t p;
    assert_true(gpt_table_entry(t, 0, &p));
    assert_null(p.dev);
    assert_eq(1u, p.entry_number);
    assert_true(memcmp(p.unique_guid, g1, 16) == 0);

    assert_true(gpt_table_entry(t, 1, &p));
    assert_null(p.dev);
    assert_eq(18u, p.entry_number);
    assert_true(memcmp(p.unique_guid, g2, 16) == 0);

    assert_true(gpt_table_entry(t, 2, &p));
    assert_null(p.dev);
    assert_eq(20u, p.entry_number);
    assert_true(memcmp(p.unique_guid, g3, 16) == 0);

    assert_false(gpt_table_entry(t, 3, &p));

    /* Publish strictly */
    gpt_info_t *info = gpt_publish_table(t, true);
    assert_not_null(info);
    assert_eq(3, info->count);
    assert_eq(3, s_blk_reg_calls);
    assert_eq(3, s_devfs_reg_calls);
    assert_str_eq("usba1", info->partitions[0].dev->name);
    assert_str_eq("usba2", info->partitions[1].dev->name);
    assert_str_eq("usba3", info->partitions[2].dev->name);
    assert_eq(18u, info->partitions[1].entry_number);
    free(info);

    /* Strict publish treats -EEXIST as failure and rolls back */
    reset_fake_registry();
    s_devfs_fail_on_call = 1;
    s_devfs_fail_rc = -EEXIST;
    info = gpt_publish_table(t, true);
    assert_null(info);
    assert_eq(2, s_blk_unreg_calls);

    /* Non-strict publish tolerates -EEXIST */
    reset_fake_registry();
    s_devfs_fail_on_call = 1;
    s_devfs_fail_rc = -EEXIST;
    info = gpt_publish_table(t, false);
    assert_not_null(info);
    assert_eq(3, info->count);
    free(info);

    gpt_free_table(t);

    /* Stock mkdisk shape: backing 395264, last_usable 393182, root ends 395263 */
    memset(img, 0, 4096 * 512);
    write_gpt_entry(entries, 0, 128, k_esp_type_guid, g1, 2048, 133119);
    write_gpt_entry(entries, 1, 128, k_linux_type_guid, g2, 133120, 395263);
    finalize_gpt_header(img + 512, 92, 2, 128, 128, 393182, entries);
    t = NULL;
    assert_eq(0, gpt_read_table(&disk, &t));
    assert_not_null(t);
    assert_eq(2, gpt_table_count(t));
    gpt_free_table(t);

    free(img);
}

static void gpt_bounds(void)
{
    reset_fake_registry();

    uint8_t *img = calloc(128, 512);
    assert_not_null(img);
    uint8_t *entries = img + 2 * 512;
    uint8_t g1[16] = { 9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6 };

    block_device_t disk;
    struct fake_disk_backing b;
    struct gpt_table *t = NULL;

    /* Non-512 sector size rejected before any read */
    init_disk(&disk, &b, "nvme0", 1024, 4096, img);
    assert_eq(-E2BIG, gpt_read_table(&disk, &t));
    assert_eq(0, b.read_calls);
    assert_null(t);

    /* 512-byte disk too small for header -> -ENOENT */
    init_disk(&disk, &b, "tiny", 1, 512, img);
    assert_eq(-ENOENT, gpt_read_table(&disk, &t));
    assert_null(t);

    /* Missing signature -> -ENOENT */
    init_disk(&disk, &b, "hda", 1024, 512, img);
    memset(img, 0, 128 * 512);
    assert_eq(-ENOENT, gpt_read_table(&disk, &t));
    assert_null(gpt_scan(&disk));

    /* Unsupported revision -> -E2BIG */
    finalize_gpt_header(img + 512, 92, 2, 4, 128, 1000, entries);
    put_le32(img + 512 + 8, 0x00020000u);
    assert_eq(-E2BIG, gpt_read_table(&disk, &t));

    /* Header size 91 / 513 -> -EINVAL */
    finalize_gpt_header(img + 512, 91, 2, 4, 128, 1000, entries);
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));
    finalize_gpt_header(img + 512, 513, 2, 4, 128, 1000, entries);
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));

    /* Zero count -> -EINVAL */
    finalize_gpt_header(img + 512, 92, 2, 0, 128, 1000, entries);
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));

    /* Entry size < 128 -> -EINVAL */
    finalize_gpt_header(img + 512, 92, 2, 4, 64, 1000, entries);
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));

    /* Table > 1 MiB -> -E2BIG */
    finalize_gpt_header(img + 512, 92, 2, 8193, 128, 1000, NULL);
    assert_eq(-E2BIG, gpt_read_table(&disk, &t));

    /* Off-disk entry area -> -EINVAL */
    finalize_gpt_header(img + 512, 92, 1023, 16, 128, 1000, entries);
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));

    /* Bad header CRC -> -EINVAL */
    finalize_gpt_header(img + 512, 92, 2, 4, 128, 1000, entries);
    img[512 + 16] ^= 0xffu;
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));

    /* Bad entries CRC -> -EINVAL */
    finalize_gpt_header(img + 512, 92, 2, 4, 128, 1000, entries);
    entries[0] ^= 0x01u;
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));

    /* Invalid used entry range -> -EINVAL in gpt_read_table */
    memset(entries, 0, 512);
    write_gpt_entry(entries, 0, 128, k_esp_type_guid, g1, 100, 1024); /* end == sector_count */
    finalize_gpt_header(img + 512, 92, 2, 4, 128, 1000, entries);
    assert_eq(-EINVAL, gpt_read_table(&disk, &t));

    /* I/O error maps to -EIO regardless of lower-layer errno */
    memset(entries, 0, 512);
    write_gpt_entry(entries, 0, 128, k_esp_type_guid, g1, 10, 20);
    finalize_gpt_header(img + 512, 92, 2, 4, 128, 1000, entries);
    b.fail_lba = 1;
    b.fail_rc = -ETIMEDOUT;
    assert_eq(-EIO, gpt_read_table(&disk, &t));
    b.fail_lba = 2;
    b.fail_rc = -EINVAL;
    assert_eq(-EIO, gpt_read_table(&disk, &t));
    b.fail_lba = -1;

    /* OOM maps to -ENOMEM */
    s_kmalloc_fail_after = 0;
    assert_eq(-ENOMEM, gpt_read_table(&disk, &t));
    s_kmalloc_fail_after = -1;

    /* Legacy gpt_scan: valid 0 partition and valid 1 partition */
    reset_fake_registry();
    memset(entries, 0, 512);
    finalize_gpt_header(img + 512, 92, 2, 4, 128, 1000, entries);
    gpt_info_t *info0 = gpt_scan(&disk);
    assert_not_null(info0);
    assert_eq(0, info0->count);
    free(info0);

    write_gpt_entry(entries, 0, 128, k_esp_type_guid, g1, 10, 20);
    finalize_gpt_header(img + 512, 92, 2, 4, 128, 1000, entries);
    gpt_info_t *info1 = gpt_scan(&disk);
    assert_not_null(info1);
    assert_eq(1, info1->count);
    assert_str_eq("hda1", info1->partitions[0].dev->name);
    free(info1);

    free(img);
}

static int run_mounts_expect_stop(const struct boot_context *ctx)
{
    s_stop_armed = true;
    if (setjmp(s_stop_jmp) == 0) {
        fs_boot_mounts(ctx);
        s_stop_armed = false;
        return 0; /* did not stop */
    }
    s_stop_armed = false;
    return 1; /* stopped */
}

static void register_test_disk(const char *name, uint64_t sectors,
                               uint32_t sector_size,
                               struct fake_disk_backing *b, uint8_t *buf)
{
    block_device_t tmp;
    init_disk(&tmp, b, name, sectors, sector_size, buf);
    struct block_device_desc desc = {
        .name = name,
        .sector_count = sectors,
        .sector_size = sector_size,
        .ops = &s_fake_disk_ops,
        .private_data = b,
        .parent = NULL,
        .kind = BLOCK_DISK,
    };
    block_device_t *out = NULL;
    assert_eq(0, block_device_register(&desc, &out));
}

static void guid_mount_selection(void)
{
    uint8_t *ahci_img = calloc(256, 512);
    uint8_t *usb_img = calloc(256, 512);
    assert_not_null(ahci_img);
    assert_not_null(usb_img);

    uint8_t ahci_esp[16] = { 0xaa,1,2,3,4,5,6,7,8,9,10,11,12,13,14,1 };
    uint8_t ahci_root[16] = { 0xaa,1,2,3,4,5,6,7,8,9,10,11,12,13,14,2 };
    uint8_t usb_esp[16] = { 0xbb,1,2,3,4,5,6,7,8,9,10,11,12,13,14,1 };
    uint8_t usb_root[16] = { 0xbb,1,2,3,4,5,6,7,8,9,10,11,12,13,14,2 };
    uint8_t misc_type[16] = { 0x11,0x22,0x33,0x44,5,6,7,8,9,10,11,12,13,14,15,16 };
    uint8_t misc_guid[16] = { 0xcc,1,2,3,4,5,6,7,8,9,10,11,12,13,14,3 };

    struct fake_disk_backing b_ahci, b_usb;
    struct boot_context ctx;
    boot_context_init(&ctx);
    ctx.flags |= BOOT_CONTEXT_HAS_BOOT_DEVICE;
    memcpy(ctx.boot_device.partition_guid, usb_esp, 16);
    ctx.boot_device.signature_type = 2;

    /* 1. AHCI first, USB second; USB ESP at slot 2 (gap) + geometry mismatch warns only */
    reset_fake_registry();
    memset(ahci_img, 0, 256 * 512);
    write_gpt_entry(ahci_img + 2 * 512, 0, 128, k_esp_type_guid, ahci_esp, 10, 20);
    write_gpt_entry(ahci_img + 2 * 512, 1, 128, k_linux_type_guid, ahci_root, 21, 100);
    finalize_gpt_header(ahci_img + 512, 92, 2, 32, 128, 250, ahci_img + 2 * 512);

    memset(usb_img, 0, 256 * 512);
    write_gpt_entry(usb_img + 2 * 512, 2, 128, k_esp_type_guid, usb_esp, 10, 20);
    write_gpt_entry(usb_img + 2 * 512, 5, 128, k_linux_type_guid, usb_root, 21, 100);
    write_gpt_entry(usb_img + 2 * 512, 7, 128, misc_type, misc_guid, 101, 150);
    finalize_gpt_header(usb_img + 512, 92, 2, 32, 128, 250, usb_img + 2 * 512);

    ctx.boot_device.partition_number = 99; /* slot gap mismatch */
    ctx.boot_device.partition_start_lba = 999; /* geometry mismatch */
    ctx.boot_device.partition_size_lba = 999;

    register_test_disk("hda", 256, 512, &b_ahci, ahci_img);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    s_blk_reg_calls = 0;

    fs_boot_mounts(&ctx);
    assert_eq(0, s_stop_calls);
    assert_eq(2, s_mount_count);
    assert_str_eq("ext2", s_mounts[0].type);
    assert_str_eq("/", s_mounts[0].path);
    assert_str_eq("usba2", s_mounts[0].dev_name);
    assert_str_eq("vfat", s_mounts[1].type);
    assert_str_eq("/boot", s_mounts[1].path);
    assert_str_eq("usba1", s_mounts[1].dev_name);
    assert_eq(3, s_blk_reg_calls); /* only usba's 3 partitions published, 0 on hda */
    assert_eq(2, b_ahci.read_calls); /* 1 header + 1 entries */
    assert_eq(2, b_usb.read_calls);  /* 1 header + 1 entries */

    /* 2. Unique non-ESP fallback (0 Linux-data, 1 non-ESP) */
    reset_fake_registry();
    memset(usb_img, 0, 256 * 512);
    write_gpt_entry(usb_img + 2 * 512, 0, 128, k_esp_type_guid, usb_esp, 10, 20);
    write_gpt_entry(usb_img + 2 * 512, 1, 128, misc_type, usb_root, 21, 100);
    finalize_gpt_header(usb_img + 512, 92, 2, 32, 128, 250, usb_img + 2 * 512);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    fs_boot_mounts(&ctx);
    assert_eq(0, s_stop_calls);
    assert_eq(2, s_mount_count);
    assert_str_eq("usba2", s_mounts[0].dev_name);

    /* 3. Ambiguous Linux-data stops with no mounts */
    reset_fake_registry();
    memset(usb_img, 0, 256 * 512);
    write_gpt_entry(usb_img + 2 * 512, 0, 128, k_esp_type_guid, usb_esp, 10, 20);
    write_gpt_entry(usb_img + 2 * 512, 1, 128, k_linux_type_guid, usb_root, 21, 50);
    write_gpt_entry(usb_img + 2 * 512, 2, 128, k_linux_type_guid, misc_guid, 51, 100);
    finalize_gpt_header(usb_img + 512, 92, 2, 32, 128, 250, usb_img + 2 * 512);
    register_test_disk("hda", 256, 512, &b_ahci, ahci_img);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    assert_eq(1, run_mounts_expect_stop(&ctx));
    assert_eq(0, s_mount_count);

    /* 4. ESP-only stops */
    reset_fake_registry();
    memset(usb_img, 0, 256 * 512);
    write_gpt_entry(usb_img + 2 * 512, 0, 128, k_esp_type_guid, usb_esp, 10, 20);
    finalize_gpt_header(usb_img + 512, 92, 2, 32, 128, 250, usb_img + 2 * 512);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    assert_eq(1, run_mounts_expect_stop(&ctx));
    assert_eq(0, s_mount_count);

    /* 5. Duplicate ESP GUID after entry 16 stops */
    reset_fake_registry();
    memset(usb_img, 0, 256 * 512);
    write_gpt_entry(usb_img + 2 * 512, 0, 128, k_esp_type_guid, usb_esp, 10, 20);
    write_gpt_entry(usb_img + 2 * 512, 1, 128, k_linux_type_guid, usb_root, 21, 50);
    write_gpt_entry(usb_img + 2 * 512, 18, 128, k_esp_type_guid, usb_esp, 51, 60);
    finalize_gpt_header(usb_img + 512, 92, 2, 32, 128, 250, usb_img + 2 * 512);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    assert_eq(1, run_mounts_expect_stop(&ctx));
    assert_eq(0, s_mount_count);

    /* 6. No match stops without AHCI fallback */
    reset_fake_registry();
    register_test_disk("hda", 256, 512, &b_ahci, ahci_img);
    assert_eq(1, run_mounts_expect_stop(&ctx));
    assert_eq(0, s_mount_count);

    /* 7. I/O error on candidate stops */
    reset_fake_registry();
    register_test_disk("hda", 256, 512, &b_ahci, ahci_img);
    b_ahci.fail_lba = 1;
    assert_eq(1, run_mounts_expect_stop(&ctx));
    b_ahci.fail_lba = -1;

    /* 8. Unsupported candidate (4096-byte sector) stops */
    reset_fake_registry();
    register_test_disk("nvme0", 256, 4096, &b_ahci, ahci_img);
    assert_eq(1, run_mounts_expect_stop(&ctx));

    /* 9. 16-slot publication cap excludes required partition -> stops */
    reset_fake_registry();
    memset(usb_img, 0, 256 * 512);
    write_gpt_entry(usb_img + 2 * 512, 0, 128, k_esp_type_guid, usb_esp, 10, 11);
    for (int i = 1; i <= 16; i++) {
        uint8_t mg[16] = { 0x55, (uint8_t)i };
        write_gpt_entry(usb_img + 2 * 512, (uint32_t)i, 128, misc_type, mg,
                        (uint64_t)(20 + i * 2), (uint64_t)(21 + i * 2));
    }
    write_gpt_entry(usb_img + 2 * 512, 17, 128, k_linux_type_guid, usb_root, 80, 100);
    finalize_gpt_header(usb_img + 512, 92, 2, 32, 128, 250, usb_img + 2 * 512);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    assert_eq(1, run_mounts_expect_stop(&ctx));
    assert_eq(0, s_mount_count);

    /* 10. Colliding devfs name (-EEXIST) on selected disk stops */
    reset_fake_registry();
    memset(usb_img, 0, 256 * 512);
    write_gpt_entry(usb_img + 2 * 512, 0, 128, k_esp_type_guid, usb_esp, 10, 20);
    write_gpt_entry(usb_img + 2 * 512, 1, 128, k_linux_type_guid, usb_root, 21, 100);
    finalize_gpt_header(usb_img + 512, 92, 2, 32, 128, 250, usb_img + 2 * 512);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    snprintf(s_devfs_fail_name, sizeof(s_devfs_fail_name), "usba2");
    s_devfs_fail_rc = -EEXIST;
    assert_eq(1, run_mounts_expect_stop(&ctx));
    assert_eq(0, s_mount_count);

    /* 11. Root mount failure stops */
    reset_fake_registry();
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    s_fail_root_mount_rc = -EIO;
    assert_eq(1, run_mounts_expect_stop(&ctx));
    assert_eq(0, s_mount_count);

    /* 12. /boot mount failure warns without remounting root */
    reset_fake_registry();
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    s_fail_boot_mount_rc = -EIO;
    fs_boot_mounts(&ctx);
    assert_eq(0, s_stop_calls);
    assert_eq(1, s_warn_calls);
    assert_eq(1, s_mount_count);
    assert_str_eq("/", s_mounts[0].path);

    /* 13. No flag preserves legacy first-disk behavior */
    reset_fake_registry();
    register_test_disk("hda", 256, 512, &b_ahci, ahci_img);
    register_test_disk("usba", 256, 512, &b_usb, usb_img);
    fs_boot_mounts(NULL);
    assert_eq(0, s_stop_calls);
    assert_eq(2, s_mount_count);
    assert_str_eq("hda2", s_mounts[0].dev_name);
    assert_str_eq("hda1", s_mounts[1].dev_name);

    free(ahci_img);
    free(usb_img);
}

TEST_LIST_BEGIN
    TEST_ENTRY(gpt_metadata_has_no_side_effects),
    TEST_ENTRY(gpt_bounds),
    TEST_ENTRY(guid_mount_selection),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("GPT Boot Device Metadata");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
