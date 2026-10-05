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
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <fs/fat.h>
#include <fs/ext2.h>
#include <fs/gpt.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <block/blockdev.h>
#include <core/printk.h>      // serial_printk for GPT/ext2/FAT32 fallback logs

// ── fs_boot_prepare ───────────────────────────────────────────
// Init the VFS mount table, then mount devfs at /dev.  devfs_init()
// also registers the built-in chrdevs (null, zero, random, urandom,
// serial).  After this returns, devfs_register_chrdev() /
// devfs_register_blkdev() calls are valid.
//
// MUST run before any vfs_mount() and before any devfs_register_*() call.
void fs_boot_prepare(void)
{
    vfs_init();
    devfs_init();
}

// ── Static helpers for fs_boot_mounts ─────────────────────────
// Splitting the mount logic into focused helpers keeps the public
// function short without changing the call order or the failure log
// strings, both of which are load-bearing for the test_boot phase-0
// ordered-marker assertion.

// Register every block device enumerated by the storage subsystem
// (AHCI, virtio-blk, etc.) under /dev as a block device.
static void register_block_devices(void)
{
    int n = block_device_count();
    for (int i = 0; i < n; i++) {
        block_device_t *dev = block_device_get(i);
        devfs_register_blkdev(dev->name, dev);
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

static void mount_partitioned_disk(void)
{
    block_device_t *disk = find_first_disk();
    if (!disk) return;

    gpt_info_t *gpt = gpt_scan(disk);
    if (!gpt) {
        // Fallback: old single-FAT32 layout (whole disk is FAT32).
        fat32_fs_t *fs = NULL;
        if (0 == fat32_init(disk, &fs))
            vfs_mount("/", disk, &fat_vfs_ops, fs);
        return;
    }


    // Dual-partition layout:
    //   gpt->partitions[0] = hda1 (FAT32 ESP) → /boot
    //   gpt->partitions[1] = hda2 (ext2)      → /
    if (gpt->count < 2) return;

    ext2_fs_t *ext2_fs = NULL;
    fat32_fs_t *fat_fs = NULL;

    if (0 == ext2_init(gpt->partitions[1].dev, &ext2_fs))
        vfs_mount("/", gpt->partitions[1].dev, &ext2_vfs_ops, ext2_fs);
    else
        serial_printk("EXT2: mount failed — / not available\n");

    if (0 == fat32_init(gpt->partitions[0].dev, &fat_fs))
        vfs_mount("/boot", gpt->partitions[0].dev, &fat_vfs_ops, fat_fs);
    else
        serial_printk("FAT32: /boot mount failed\n");
}

// ── fs_boot_mounts ────────────────────────────────────────────
// Order matters: block-device registration must precede the GPT scan
// (gpt_scan targets block_device_get(0)); mount_partitioned_disk must
// precede tmpfs_init/procfs_init because tmpfs/procfs layer on top of
// the mount table.  tmpfs and procfs are independent of the disk
// layout so they run after every disk-mount branch.
void fs_boot_mounts(void)
{
    register_block_devices();
    mount_partitioned_disk();

    // /tmp → tmpfs (independent of disk)
    tmpfs_init();

    procfs_init();                  // /proc
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
        serial_printk("devfs: /dev/null read=%d write=%d\n", r, w);
        vfs_node_put(nul);
    }
}
