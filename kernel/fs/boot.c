// kernel/fs/boot.c — Boot-time filesystem wiring, extracted from
// kernel/core/main.c as part of the kernel-main refactor (Task 2).
//
// Owns fs_boot_prepare() and fs_boot_mounts(); see kernel/include/fs/boot.h
// for the contract.  Implementation mirrors the original 64 lines
// (kernel/core/main.c:147-210 in the d0e0fda baseline) verbatim in call
// order, edge-case handling, and log strings.  Any intentional change
// belongs in a follow-up task with its own test/RED-first cycle; silent
// behavior changes are out of scope here.

#include <fs/boot.h>
#include <core/bootinfo.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <fs/gpt.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <block/blockdev.h>
#include <core/boot_failure.h>
#include <selftest/boot_failure_fixture.h>
#include <log/log.h>          // log_* for GPT/ext2/FAT32 fallback logs
#include <core/panic.h>
#include <memory/slab.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

// ── fs_boot_prepare ───────────────────────────────────────────
// Init the VFS mount table, then mount devfs at /dev.  devfs_init()
// also registers the built-in chrdevs (null, zero, random, urandom,
// serial).  After this returns, devfs_register_chrdev() /
// devfs_register_blkdev() calls are valid.
//
// MUST run before any vfs_mount() and before any devfs_register_*() call.
int fs_boot_prepare(void)
{
    vfs_init();
    int ret = devfs_init();
    if (ret != 0) {
        log_err("fs_boot_prepare: devfs_init failed (%d)\n", ret);
        return ret;
    }
    return 0;
}

// ── Static helpers for fs_boot_mounts ─────────────────────────
// Splitting the mount logic into focused helpers keeps the public
// function short without changing the call order or the failure log
// strings, both of which are load-bearing for the test_boot phase-0
// ordered-marker assertion.

// Register every block device enumerated by the storage subsystem
// (AHCI, virtio-blk, etc.) under /dev as a block device.
//
// The enumeration is snapshot based: block_device_count() and
// block_device_get() take the registry lock separately, so a device
// that is rolled back in between simply drops out of the walk.
static void register_block_devices(void)
{
    int n = block_device_count();
    for (int i = 0; i < n; i++) {
        block_device_t *dev = block_device_get(i);
        if (!dev) continue;
        int ret = devfs_register_blkdev(dev->name, dev);
        if (ret != 0 && ret != -EEXIST) {
            log_err("boot: failed to register blkdev '%s' in devfs (%d)\n",
                    dev->name, ret);
        }
    }
}

// Scan the GPT on the first block device, then mount the dual-partition
// layout (FAT32 ESP → /boot, ext2 → /).  Falls back to single-FAT32 on
// the whole disk if the GPT is missing or has fewer than two partitions.
// Preserves the original log strings:
//   "EXT2: mount failed — / not available\n"
//   "FAT32: /boot mount failed\n"
static block_device_t *find_first_disk(void)
{
    int n = block_device_count();
    for (int i = 0; i < n; i++) {
        block_device_t *dev = block_device_get(i);
        if (dev && dev->kind == BLOCK_DISK)
            return dev;
    }
    return NULL;
}

static int mount_partitioned_disk(void)
{
    block_device_t *disk = find_first_disk();
    if (!disk) return -ENODEV;

    gpt_info_t *gpt = gpt_scan(disk);
    if (!gpt) {
        // Fallback: old single-FAT32 layout (whole disk is FAT32).
        int rc = vfs_mount_fs("vfat", "/", disk, NULL);
        return rc;
    }

    // Dual-partition layout:
    //   gpt->partitions[0] = hda1 (FAT32 ESP) → /boot
    //   gpt->partitions[1] = hda2 (ext2)      → /
    if (gpt->count < 2) {
        kfree(gpt);
        return -EINVAL;
    }

    if (boot3_test_fail_site("root")) {
        kfree(gpt);
        return -EIO;
    }

    int rc = vfs_mount_fs("ext2", "/", gpt->partitions[1].dev, NULL);
    if (rc != 0) {
        log_err("EXT2: mount failed — / not available\n");
        kfree(gpt);
        return rc;
    }

    int boot_rc = vfs_mount_fs("vfat", "/boot", gpt->partitions[0].dev, NULL);
    if (boot_rc != 0) {
        log_warn("FAT32: /boot mount failed\n");
        boot_failure_warn("fs", "/boot", boot_rc, -1, -1);
    }

    kfree(gpt);
    return 0;
}

static const uint8_t k_esp_type_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

static const uint8_t k_linux_type_guid[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
};

static int mount_disk_by_guid(const struct boot_context *bootctx)
{
    int total = block_device_count();
    if (total <= 0)
        return -ENODEV;

    block_device_t **disks = kmalloc((size_t)total * sizeof(block_device_t *));
    if (!disks)
        return -ENOMEM;

    int disk_count = 0;
    for (int i = 0; i < total; i++) {
        block_device_t *dev = block_device_get(i);
        if (dev && dev->kind == BLOCK_DISK)
            disks[disk_count++] = dev;
    }
    if (disk_count == 0) {
        kfree(disks);
        return -ENODEV;
    }

    log_info("fs: boot scan first_disk=%s candidates=%d\n",
             disks[0]->name, disk_count);

    block_device_t *matched_disk = NULL;
    struct gpt_table *matched_table = NULL;
    gpt_partition_t esp_entry;
    int match_count = 0;

    for (int i = 0; i < disk_count; i++) {
        struct gpt_table *t = NULL;
        int rc = gpt_read_table(disks[i], &t);
        if (rc == -ENOENT || rc == -EINVAL)
            continue;
        if (rc != 0) {
            log_err("fs: boot scan failed on disk '%s' (%d)\n",
                    disks[i]->name, rc);
            gpt_free_table(matched_table);
            kfree(disks);
            return rc;
        }

        size_t count = gpt_table_count(t);
        for (size_t j = 0; j < count; j++) {
            gpt_partition_t entry;
            if (!gpt_table_entry(t, j, &entry))
                continue;
            if (memcmp(entry.type_guid, k_esp_type_guid, 16) == 0 &&
                memcmp(entry.unique_guid, bootctx->boot_device.partition_guid, 16) == 0) {
                match_count++;
                if (match_count > 1) {
                    log_err("fs: ambiguous boot ESP GUID (multiple matches)\n");
                    if (matched_table != t)
                        gpt_free_table(matched_table);
                    gpt_free_table(t);
                    kfree(disks);
                    return -EINVAL;
                }
                matched_disk = disks[i];
                matched_table = t;
                esp_entry = entry;
            }
        }
        if (t != matched_table)
            gpt_free_table(t);
    }
    kfree(disks);

    if (match_count == 0 || !matched_table || !matched_disk) {
        log_err("fs: no disk matched firmware boot ESP GUID\n");
        return -ENOENT;
    }

    uint64_t esp_size = esp_entry.end_lba - esp_entry.start_lba + 1u;
    if (esp_entry.entry_number != bootctx->boot_device.partition_number ||
        esp_entry.start_lba != bootctx->boot_device.partition_start_lba ||
        esp_size != bootctx->boot_device.partition_size_lba) {
        log_warn("fs: ESP geometry/slot mismatch (gpt slot=%u start=%lu size=%lu, uefi slot=%u start=%lu size=%lu)\n",
                 esp_entry.entry_number, esp_entry.start_lba, esp_size,
                 bootctx->boot_device.partition_number,
                 bootctx->boot_device.partition_start_lba,
                 bootctx->boot_device.partition_size_lba);
    }

    gpt_partition_t linux_entry;
    gpt_partition_t non_esp_entry;
    int linux_count = 0;
    int non_esp_count = 0;
    size_t count = gpt_table_count(matched_table);
    for (size_t j = 0; j < count; j++) {
        gpt_partition_t entry;
        if (!gpt_table_entry(matched_table, j, &entry))
            continue;
        if (entry.entry_number == esp_entry.entry_number)
            continue;
        if (memcmp(entry.type_guid, k_linux_type_guid, 16) == 0) {
            linux_count++;
            linux_entry = entry;
        }
        non_esp_count++;
        non_esp_entry = entry;
    }

    gpt_partition_t root_entry;
    if (linux_count == 1) {
        root_entry = linux_entry;
    } else if (linux_count == 0 && non_esp_count == 1) {
        root_entry = non_esp_entry;
    } else {
        log_err("fs: ambiguous or missing root partition on '%s' (linux=%d non_esp=%d)\n",
                matched_disk->name, linux_count, non_esp_count);
        gpt_free_table(matched_table);
        return -EINVAL;
    }

    gpt_info_t *gpt = gpt_publish_table(matched_table, true);
    gpt_free_table(matched_table);
    if (!gpt) {
        log_err("fs: failed to publish partitions for '%s'\n", matched_disk->name);
        return -EIO;
    }

    gpt_partition_t *root_part = NULL;
    gpt_partition_t *esp_part = NULL;
    for (int i = 0; i < gpt->count; i++) {
        if (gpt->partitions[i].entry_number == root_entry.entry_number)
            root_part = &gpt->partitions[i];
        else if (gpt->partitions[i].entry_number == esp_entry.entry_number)
            esp_part = &gpt->partitions[i];
    }

    if (!root_part || !root_part->dev || !esp_part || !esp_part->dev) {
        log_err("fs: required boot/root partition excluded by %d-slot cap on '%s'\n",
                GPT_PARTITION_MAX, matched_disk->name);
        kfree(gpt);
        return -ENOSPC;
    }

    if (boot3_test_fail_site("root")) {
        kfree(gpt);
        return -EIO;
    }

    int rc = vfs_mount_fs("ext2", "/", root_part->dev, NULL);
    if (rc != 0) {
        log_err("EXT2: mount failed — / not available\n");
        kfree(gpt);
        return rc;
    }

    int boot_rc = vfs_mount_fs("vfat", "/boot", esp_part->dev, NULL);
    if (boot_rc != 0) {
        log_warn("FAT32: /boot mount failed\n");
        boot_failure_warn("fs", "/boot", boot_rc, -1, -1);
    }

    log_info("fs: boot disk selected by GUID (%s), root=%s boot=%s (/boot %s)\n",
             matched_disk->name, root_part->dev->name, esp_part->dev->name,
             boot_rc == 0 ? "ok" : "failed");

    kfree(gpt);
    return 0;
}

// ── fs_boot_mounts ────────────────────────────────────────────
// Order matters: block-device registration must precede the GPT scan
// (gpt_scan targets block_device_get(0)); mount_partitioned_disk must
// precede tmpfs_init/procfs_init because tmpfs/procfs layer on top of
// the mount table.  tmpfs and procfs are independent of the disk
// layout so they run after every disk-mount branch.
void fs_boot_mounts(const struct boot_context *bootctx)
{
    register_block_devices();
    int root_rc;
    if (bootctx && (bootctx->flags & BOOT_CONTEXT_HAS_BOOT_DEVICE)) {
        root_rc = mount_disk_by_guid(bootctx);
    } else {
        log_info("fs: boot device identity absent, using first disk\n");
        root_rc = mount_partitioned_disk();
    }
    if (root_rc != 0) {
        boot_failure_stop("fs", "root", root_rc, -1, -1);
    }
    boot3_test_sentinel("root-ready");

    // /tmp → tmpfs (independent of disk)
    int tmp_rc = tmpfs_init();
    if (tmp_rc != 0) {
        boot_failure_warn("fs", "tmpfs", tmp_rc, -1, -1);
    }

    int proc_rc = procfs_init();                  // /proc
    if (proc_rc != 0) {
        boot_failure_warn("fs", "procfs", proc_rc, -1, -1);
    }
}


// ── fs_boot_probe_devfs ──────────────────────────────────────
// After tty_boot_init() registers /dev/tty and /dev/tty0, list the
// /dev directory and run a /dev/null smoke probe.  Implementation
// mirrors kernel/core/main.c:176-186 (f500458 baseline) verbatim:
// same vfs_debug_list() call, same read/write smoke test, same
// log format.  MUST run AFTER tty_boot_init() so the listing shows
// the tty and tty0 nodes.  MUST NOT be called from fs_boot_mounts()
// (the brief is explicit).
void fs_boot_probe_devfs(void)
{
    vfs_debug_list("/dev");

    // Quick smoke test: /dev/null
    vfs_node_t *nul = vfs_lookup("/dev/null");
    if (nul) {
        char c;
        int r = vfs_read(nul, 0, 1, &c);
        int w = vfs_write(nul, 0, 4, "test");
        log_info("devfs: /dev/null read=%d write=%d\n", r, w);
        vfs_node_put(nul);
    }
}
