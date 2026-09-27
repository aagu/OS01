#ifndef _FS_BOOT_H
#define _FS_BOOT_H

// ── Boot-time filesystem wiring ──────────────────────────────
// Pulled out of kernel/core/main.c as part of the kernel-main refactor
// (Task 2).  These helpers own the early FS bring-up sequence:
//
//   fs_boot_prepare() — initialize VFS mount table and mount devfs at /dev.
//                       Must run BEFORE any devfs_register_*() / vfs_mount()
//                       call.  Tasks 3+ stack additional mount work on top.
//
//   fs_boot_mounts()  — register physical disks in devfs, then scan for a
//                       GPT partition table and mount the matching filesystems
//                       (ext2 root, FAT32 /boot).  Falls back to single-FAT32
//                       if GPT scan fails.  Then tmpfs (/) and procfs (/proc).
//
// Both helpers preserve the ordering and edge cases that existed inline in
// kernel_main (see kernel/core/main.c history).  The functions must remain
// called from kernel_main in this exact order, with pty_init() and
// x86_64_boot_device_nodes() interleaved per the brief.
void fs_boot_prepare(void);
void fs_boot_mounts(void);

#endif // _FS_BOOT_H
