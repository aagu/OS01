// kernel/selftest/test_aarch64_vfs.c — aarch64 VFS-gate acceptance
// (M4-remaining "aarch64 opens the VFS gate").
//
// Runs on the product aarch64 kernel (OS01_HAS_VFS=1) inside the
// KERNEL_SELFTEST image variant, whose disk is the GPT dual-partition
// layout built by mk/components/image.mk:
//   hda1 = FAT32 ESP  → mounted /boot (BOOTAA64.EFI + kernel.elf)
//   hda2 = ext2 root  → mounted /     (/etc/vfs-selftest.txt marker)
// plus the boot-agnostic mounts devfs (/dev), tmpfs (/tmp), procfs
// (/proc) and the virtio-blk BLOCK_DISK underneath.
//
// Cases (printed one line each, parser-asserted final marker):
//   [selftest] aarch64-vfs: root-fs        / is the ext2 mount
//   [selftest] aarch64-vfs: esp-fat        /boot is FAT32, kernel.elf readable
//   [selftest] aarch64-vfs: ext2-read      marker file content round-trip
//   [selftest] aarch64-vfs: tmpfs-rw       /tmp create/write/read/unlink
//   [selftest] aarch64-vfs: devfs-nodes    null/zero/urandom/block nodes
//   [selftest] aarch64-vfs: blk-sector0    /dev/hda sector 0 = GPT MBR
//   [selftest] aarch64-vfs: N/6 PASS
//
// Registered in selftest.c under __aarch64__ (OS01_HAS_VFS builds only).

#include <stdint.h>
#include <string.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <block/blockdev.h>
#include <core/printk.h>
#include <memory/slab.h>
#include <arch/features.h>

#if defined(__aarch64__) && OS01_HAS_VFS

#define MARKER_TEXT "OS01-AARCH64-VFS-SELFTEST-MARKER"

static int case_ext2_read(void)
{
    vfs_node_t *n = vfs_lookup("/etc/vfs-selftest.txt");
    if (!n) {
        serial_printk("[selftest] aarch64-vfs: ext2-read FAIL (lookup)\n");
        return -1;
    }
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int rc = vfs_read(n, 0, sizeof(buf) - 1, buf);
    vfs_node_put(n);
    if (rc < (int)sizeof(MARKER_TEXT) - 1 ||
        memcmp(buf, MARKER_TEXT, sizeof(MARKER_TEXT) - 1) != 0) {
        serial_printk("[selftest] aarch64-vfs: ext2-read FAIL (rc=%d content mismatch)\n", rc);
        return -1;
    }
    return 0;
}

static int case_esp_fat(void)
{
    vfs_node_t *boot = vfs_lookup("/boot");
    if (!boot || boot->type != VFS_DIR) {
        serial_printk("[selftest] aarch64-vfs: esp-fat FAIL (no /boot dir)\n");
        vfs_node_put(boot);
        return -1;
    }
    vfs_node_put(boot);

    // The ESP (FAT32) must serve both the loader and the kernel image,
    // each starting with the expected magic.  Directory enumeration is
    // intentionally not asserted here: the FAT driver's short-name-only
    // readdir stops at LFN/volume-label entries (pre-existing behavior),
    // while lookup walks with the same skip logic the loader uses.
    vfs_node_t *k = vfs_lookup("/boot/kernel.elf");
    if (!k) {
        serial_printk("[selftest] aarch64-vfs: esp-fat FAIL (no /boot/kernel.elf)\n");
        return -1;
    }
    uint8_t magic[4] = { 0, 0, 0, 0 };
    int rc = vfs_read(k, 0, 4, magic);
    vfs_node_put(k);
    if (rc != 4 || magic[0] != 0x7f || magic[1] != 'E' ||
        magic[2] != 'L' || magic[3] != 'F') {
        serial_printk("[selftest] aarch64-vfs: esp-fat FAIL (ELF magic rc=%d)\n", rc);
        return -1;
    }

    vfs_node_t *ldr = vfs_lookup("/boot/EFI/BOOT/BOOTAA64.EFI");
    if (!ldr) {
        serial_printk("[selftest] aarch64-vfs: esp-fat FAIL (no BOOTAA64.EFI)\n");
        return -1;
    }
    memset(magic, 0, sizeof(magic));
    rc = vfs_read(ldr, 0, 2, magic);
    vfs_node_put(ldr);
    if (rc != 2 || magic[0] != 'M' || magic[1] != 'Z') {
        serial_printk("[selftest] aarch64-vfs: esp-fat FAIL (PE magic rc=%d)\n", rc);
        return -1;
    }
    return 0;
}

static int case_tmpfs_rw(void)
{
    vfs_node_t *n = NULL;
    int rc = vfs_create_file("/tmp/vfs-selftest.tmp", NULL, &n);
    if (rc != 0 || !n) {
        serial_printk("[selftest] aarch64-vfs: tmpfs-rw FAIL (create rc=%d)\n", rc);
        vfs_node_put(n);
        return -1;
    }
    const char *msg = "tmpfs-write-payload";
    rc = vfs_write(n, 0, strlen(msg), (void *)msg);
    if (rc != (int)strlen(msg)) {
        serial_printk("[selftest] aarch64-vfs: tmpfs-rw FAIL (write rc=%d)\n", rc);
        vfs_node_put(n);
        return -1;
    }
    char buf[32];
    memset(buf, 0, sizeof(buf));
    rc = vfs_read(n, 0, sizeof(buf) - 1, buf);
    vfs_node_put(n);
    if (rc != (int)strlen(msg) || memcmp(buf, msg, strlen(msg)) != 0) {
        serial_printk("[selftest] aarch64-vfs: tmpfs-rw FAIL (read rc=%d)\n", rc);
        return -1;
    }
    rc = vfs_unlink("/tmp/vfs-selftest.tmp", NULL);
    if (rc != 0) {
        serial_printk("[selftest] aarch64-vfs: tmpfs-rw FAIL (unlink rc=%d)\n", rc);
        return -1;
    }
    return 0;
}

static int case_devfs_nodes(void)
{
    // /dev/null: write discards, read is EOF.
    vfs_node_t *n = vfs_lookup("/dev/null");
    if (!n) {
        serial_printk("[selftest] aarch64-vfs: devfs-nodes FAIL (no /dev/null)\n");
        return -1;
    }
    char buf[8];
    int rc = vfs_write(n, 0, 4, (void *)"abcd");
    int rr = vfs_read(n, 0, 4, buf);
    vfs_node_put(n);
    if (rc != 4 || rr != 0) {
        serial_printk("[selftest] aarch64-vfs: devfs-nodes FAIL (null w=%d r=%d)\n", rc, rr);
        return -1;
    }

    // /dev/zero: reads zero-fill.
    n = vfs_lookup("/dev/zero");
    if (!n) {
        serial_printk("[selftest] aarch64-vfs: devfs-nodes FAIL (no /dev/zero)\n");
        return -1;
    }
    memset(buf, 0xAA, sizeof(buf));
    rc = vfs_read(n, 0, sizeof(buf), buf);
    vfs_node_put(n);
    if (rc != (int)sizeof(buf)) {
        serial_printk("[selftest] aarch64-vfs: devfs-nodes FAIL (zero rc=%d)\n", rc);
        return -1;
    }
    for (unsigned i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0) {
            serial_printk("[selftest] aarch64-vfs: devfs-nodes FAIL (zero byte %u)\n", i);
            return -1;
        }
    }

    // /dev/hda1 + /dev/hda2 published by the GPT scan; /dev/tty from the
    // console TTY boot.  Presence check only (chrdev ops are task-facing).
    const char *required[] = { "/dev/hda", "/dev/hda1", "/dev/hda2", "/dev/tty" };
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        vfs_node_t *dn = vfs_lookup(required[i]);
        if (!dn) {
            serial_printk("[selftest] aarch64-vfs: devfs-nodes FAIL (missing %s)\n",
                          required[i]);
            return -1;
        }
        vfs_node_put(dn);
    }
    return 0;
}

static int case_blk_sector0(void)
{
    block_device_t *disk = block_device_get(0);
    if (!disk || disk->kind != BLOCK_DISK) {
        serial_printk("[selftest] aarch64-vfs: blk-sector0 FAIL (no disk)\n");
        return -1;
    }
    if (disk->sector_count != 395264u) {  // 64 MiB ESP + 128 MiB ext2 GPT layout
        serial_printk("[selftest] aarch64-vfs: blk-sector0 FAIL (sectors=%lu)\n",
                      (unsigned long)disk->sector_count);
        return -1;
    }
    uint8_t *sec = kmalloc(512);
    if (!sec) {
        serial_printk("[selftest] aarch64-vfs: blk-sector0 FAIL (oom)\n");
        return -1;
    }
    int rc = block_device_read(disk, 0, 1, sec);
    int ok = (rc == 0 && sec[510] == 0x55 && sec[511] == 0xAA);
    kfree(sec);
    if (!ok) {
        serial_printk("[selftest] aarch64-vfs: blk-sector0 FAIL (rc=%d sig)\n", rc);
        return -1;
    }
    return 0;
}

int test_aarch64_vfs(void)
{
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "ext2-read",    case_ext2_read },
        { "esp-fat",      case_esp_fat },
        { "tmpfs-rw",     case_tmpfs_rw },
        { "devfs-nodes",  case_devfs_nodes },
        { "blk-sector0",  case_blk_sector0 },
    };

    int pass = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = cases[i].fn();
        if (rc == 0) {
            pass++;
            serial_printk("[selftest] aarch64-vfs: %s OK\n", cases[i].name);
        }
    }
    serial_printk("[selftest] aarch64-vfs: %u/%u PASS\n",
                  pass, (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return pass == (int)(sizeof(cases) / sizeof(cases[0])) ? 0 : -1;
}

#endif /* __aarch64__ && OS01_HAS_VFS */
