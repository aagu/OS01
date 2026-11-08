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
#include <fs/gpt.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <block/blockdev.h>
#include <core/boot_failure.h>
#include <log/log.h>          // log_* for GPT/ext2/FAT32 fallback logs
#include <core/panic.h>
#include <memory/slab.h>
#include <stdbool.h>
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

// ── fs_boot_mounts ────────────────────────────────────────────
// Order matters: block-device registration must precede the GPT scan
// (gpt_scan targets block_device_get(0)); mount_partitioned_disk must
// precede tmpfs_init/procfs_init because tmpfs/procfs layer on top of
// the mount table.  tmpfs and procfs are independent of the disk
// layout so they run after every disk-mount branch.
void fs_boot_mounts(void)
{
    register_block_devices();
    int root_rc = mount_partitioned_disk();
    if (root_rc != 0) {
        boot_failure_stop("fs", "root", root_rc, -1, -1);
    }

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
