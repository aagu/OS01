# Ext2 + Tmpfs + GPT Multi-Partition Filesystem — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade from single-FAT32 disk to GPT dual-partition layout with ext2 (read-only `/`), FAT32 ESP (`/boot`), tmpfs (`/tmp`), and `/dev` block device nodes.

**Architecture:** 5 independent work streams: (A) block device infrastructure, (B) devfs blkdev support, (C) GPT partition scanner, (D) ext2 read-only driver, (E) tmpfs memory filesystem. Streams A+B run first as foundation for C; E is fully independent; D depends on A+B+C. Final integration tasks wire everything together in kernel_main + build system.

**Tech Stack:** C (kernel), x86_64, AHCI block I/O, VFS ops interface, GPT + CRC32, ext2 on-disk format

**Critical API notes** (verified against existing code):
- Spinlock type is `spinlock_T` (capital T) — see `kernel/include/kernel/arch/x86_64/spinlock.h:10`
- 4KB page alloc/free: `alloc_4k_page()` returns `uint64_t phys`, `free_4k_page(uint64_t phys)` — see `kernel/include/kernel/pmm.h:102-103`
- `vfs_dirent_t.ino` is `uint32_t` — must be changed to `uint64_t` to hold kernel pointers (tmpfs uses `ino = (uint64_t)(uintptr_t)node`)
- `ASSERT` macro does NOT exist in selftest.h — use `if (!(x)) return -1;`
- No `spin_lock_irqsave` needed in ext2 read — spin_lock alone suffices (ext2 is read-only, called from user-syscall context where IRQs are on)

---

## File Structure Map

```
New files (kernel):
  kernel/block/blockdev.c         — modify: +block_device_register_raw(), +private_data field
  kernel/include/block/blockdev.h — modify: +private_data, +block_device_register_raw() decl
  kernel/fs/gpt.c                 — create: GPT scan + CRC32
  kernel/include/fs/gpt.h         — create: gpt_partition_t, gpt_info_t, gpt_scan()
  kernel/fs/devfs.c               — modify: +private_data field, +blkdev dispatch, +register_blkdev
  kernel/include/fs/devfs.h       — modify: +devfs_register_blkdev() decl
  kernel/fs/ext2.c                — create: ext2 read-only driver
  kernel/include/fs/ext2.h        — create: ext2 on-disk structs, ext2_init(), ext2_vfs_ops extern
  kernel/fs/tmpfs.c               — create: tmpfs memory filesystem
  kernel/include/fs/tmpfs.h       — create: tmpfs_init() decl
  kernel/fs/fat.c                 — modify: fat32_mount → fat32_init, ops .flags
  kernel/include/fs/fat.h         — modify: fat32_mount → fat32_init signature
  kernel/fs/vfs.c                 — modify: __vfs_lookup per-fs case sensitivity
  kernel/include/fs/vfs.h         — modify: vfs_ops_t .flags field; vfs_dirent_t.ino uint32_t → uint64_t
  kernel/kernel/main.c            — modify: new init sequence
  kernel/sched/task.c             — modify: /init.elf → /bin/init
  user/init.c                     — modify: /systest.elf → /bin/systest, /busybox.elf → /bin/busybox
  kernel/Makefile                 — modify: (none — wildcard picks up new .c files automatically)

New files (tools):
  tools/mkdisk.c                  — create: disk image builder
  tools/Makefile                  — create: host compile + dep check

Modified root files:
  Makefile                        — modify: disk.img target, remove config.txt mcopy
  .gitignore                      — modify: +config/fsroot/

New directories:
  config/fsroot/bin/              — empty dir, populated by make
  config/fsroot/home/             — empty dir
  config/fsroot/etc/              — empty dir
```

---

### Task 1: block_device_t — add private_data field

**Files:**
- Modify: `kernel/include/block/blockdev.h:9-20`

- [ ] **Step 1: Add private_data to block_device_t struct**

```c
// kernel/include/block/blockdev.h — add after .write function pointer, before closing brace:
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);

    void   *private_data;         // partition_ctx_t* or NULL for raw AHCI
} block_device_t;
```

- [ ] **Step 2: Verify build still succeeds**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: `kernel.bin` created successfully.

- [ ] **Step 3: Commit**

```bash
git add kernel/include/block/blockdev.h
git commit -m "feat(block): add private_data field to block_device_t"
```

---

### Task 2: block_device_register_raw() — partition-safe registration

**Files:**
- Modify: `kernel/block/blockdev.c:34-103`

- [ ] **Step 1: Add block_device_register_raw() after existing block_device_register()**

In `kernel/block/blockdev.c`, after the closing `}` of `block_device_register` (after line 59), add:

```c
// Register a block device without AHCI hook defaults.
// Caller sets dev->read / dev->write / dev->private_data after this call.
// Unlike block_device_register(), this does NOT overwrite .read/.write.
block_device_t *block_device_register_raw(const char *name,
                                           uint64_t sector_count,
                                           void *private_data)
{
    if (block_device_count_val >= BLOCKDEV_MAX) {
        debug_block("block: max devices reached (raw)\n");
        return NULL;
    }
    block_device_t *dev = &block_devices[block_device_count_val];
    memset(dev, 0, sizeof(block_device_t));
    strcpy((char *)dev->name, name);
    dev->sector_count = sector_count;
    dev->sector_size  = 512;
    dev->present      = 1;
    dev->port_num     = 0;
    dev->private_data = private_data;
    block_device_count_val++;
    debug_block("block: registered raw %s (%lu sectors)\n",
                name, sector_count);
    return dev;
}
```

- [ ] **Step 2: Fix block_device_register() to init private_data**

In `block_device_register()` (after `dev->write = default_ahci_write;` at line 53), add:

```c
    dev->write = default_ahci_write;
    dev->private_data = NULL;      // ← add this line
```

This prevents garbage values in the BSS-initialized array.

- [ ] **Step 3: Update blockdev.h with declaration**

In `kernel/include/block/blockdev.h`, after `block_device_register` declaration:

```c
// Register a block device without AHCI defaults.
// Caller must set .read, .write, and .private_data after this call.
block_device_t *block_device_register_raw(const char *name,
                                           uint64_t sector_count,
                                           void *private_data);
```

- [ ] **Step 4: Build and verify**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success.

- [ ] **Step 5: Commit**

```bash
git add kernel/block/blockdev.c kernel/include/block/blockdev.h
git commit -m "feat(block): add block_device_register_raw() for partition devices"
```

---

### Task 3: devfs_device_t — add private_data field

**Files:**
- Modify: `kernel/fs/devfs.c:12-18`

- [ ] **Step 1: Add private_data to devfs_device_t**

```c
// kernel/fs/devfs.c — replace struct:
typedef struct devfs_device {
    char     name[DEVFS_NAME_MAX];
    uint8_t  type;          // VFS_CHRDEV or VFS_BLKDEV
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
    void    *private_data;  // chrdev: ctx ptr; blkdev: block_device_t *
    int      registered;
} devfs_device_t;
```

- [ ] **Step 2: Update all existing devfs_register_chrdev calls**

The `devfs_register_chrdev` function currently sets `devices[idx].read`, `.write`, `.registered` but not `.private_data`. Add `devices[idx].private_data = private_data;` inside `devfs_register_chrdev` (passing through the existing `private_data` parameter). Existing callers all pass `NULL` for private_data so no behavior change.

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/devfs.c
git commit -m "feat(devfs): add private_data field to devfs_device_t"
```

---

### Task 4: devfs — register_blkdev + blkdev dispatch

**Files:**
- Modify: `kernel/fs/devfs.c:121-139` (devfs_read/devfs_write)
- Add: after `devfs_register_chrdev` (~line 226)
- Modify: `kernel/include/fs/devfs.h:12-13`

- [ ] **Step 1: Add blkdev branch to devfs_read**

Replace `devfs_read` with:

```c
static int devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;

    // Block device path: sector-level read via block_device_t
    if (devices[idx].type == VFS_BLKDEV) {
        block_device_t *bdev = (block_device_t *)devices[idx].private_data;
        if (!bdev || !buffer || size == 0) return 0;
        uint32_t lba   = (uint32_t)(offset / 512);
        uint32_t count = (uint32_t)((size + 511) / 512);
        if (count == 0) return 0;
        uint8_t *tmp = kmalloc(count * 512);
        if (!tmp) return -1;
        int ret = block_device_read(bdev, lba, count, tmp);
        if (ret == 0)
            memcpy(buffer, tmp + (offset % 512), size);
        kfree(tmp);
        return (ret == 0) ? (int)size : -1;
    }

    // Character device path (existing)
    if (devices[idx].read)
        return devices[idx].read(node, offset, size, buffer);
    return -1;
}
```

- [ ] **Step 2: Add blkdev branch to devfs_write**

Replace `devfs_write` with:

```c
static int devfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;

    // Block device path: read-modify-write for unaligned edges
    if (devices[idx].type == VFS_BLKDEV) {
        block_device_t *bdev = (block_device_t *)devices[idx].private_data;
        if (!bdev || !buffer || size == 0) return 0;
        uint32_t lba   = (uint32_t)(offset / 512);
        uint32_t count = (uint32_t)((size + 511) / 512);
        if (count == 0) return 0;
        uint8_t *tmp = kmalloc(count * 512);
        if (!tmp) return -1;
        int ret = block_device_read(bdev, lba, count, tmp);
        if (ret == 0) {
            memcpy(tmp + (offset % 512), buffer, size);
            ret = block_device_write(bdev, lba, count, tmp);
        }
        kfree(tmp);
        return (ret == 0) ? (int)size : -1;
    }

    // Character device path (existing)
    if (devices[idx].write)
        return devices[idx].write(node, offset, size, buffer);
    return -1;
}
```

- [ ] **Step 3: Add devfs_register_blkdev()**

After `devfs_register_chrdev`, add:

```c
int devfs_register_blkdev(const char *name, block_device_t *dev)
{
    if (device_count >= DEVFS_MAX_DEVICES)
        return -1;

    int idx = device_count;
    size_t nlen = strlen(name);
    if (nlen >= DEVFS_NAME_MAX) nlen = DEVFS_NAME_MAX - 1;
    memcpy(devices[idx].name, name, nlen);
    devices[idx].name[nlen] = '\0';

    devices[idx].type = VFS_BLKDEV;
    devices[idx].read = NULL;           // dispatch handles this
    devices[idx].write = NULL;
    devices[idx].private_data = dev;    // block_device_t *
    devices[idx].registered = 1;
    device_count++;

    debug_fs("devfs: registered blkdev '%s'\n", name);
    return 0;
}
```

- [ ] **Step 4: Add declaration to devfs.h**

```c
// In kernel/include/fs/devfs.h, after devfs_register_chrdev:
int devfs_register_blkdev(const char *name, struct block_device *dev);
```

- [ ] **Step 5: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/devfs.c kernel/include/fs/devfs.h
git commit -m "feat(devfs): add blkdev dispatch + devfs_register_blkdev()"
```

---

### Task 5: GPT — header, CRC32, partition scanner

**Files:**
- Create: `kernel/include/fs/gpt.h`
- Create: `kernel/fs/gpt.c`

- [ ] **Step 1: Create gpt.h**

```c
// kernel/include/fs/gpt.h
#ifndef _FS_GPT_H
#define _FS_GPT_H

#include <stdint.h>
#include <block/blockdev.h>

#define GPT_PARTITION_MAX  16

typedef struct gpt_partition {
    char            name[40];
    uint8_t         type_guid[16];
    uint64_t        start_lba;
    uint64_t        end_lba;
    block_device_t *parent;
    block_device_t *dev;
} gpt_partition_t;

typedef struct gpt_info {
    gpt_partition_t partitions[GPT_PARTITION_MAX];
    int             count;
} gpt_info_t;

gpt_info_t *gpt_scan(block_device_t *disk);

#endif
```

- [ ] **Step 2: Create gpt.c — CRC32 + partition context + helpers + gpt_scan**

```c
// kernel/fs/gpt.c
#include <fs/gpt.h>
#include <fs/devfs.h>
#include <block/blockdev.h>
#include <kernel/debug.h>
#include <string.h>
#include <stdlib.h>

// ── CRC32 (standard reflected, polynomial 0xEDB88320) ────
static uint32_t gpt_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}

// ── Partition block device wrapper ──────────────────────
typedef struct partition_ctx {
    block_device_t *parent;
    uint64_t        offset_lba;
    uint64_t        length;
} partition_ctx_t;

static int partition_read(block_device_t *dev, uint64_t lba,
                          uint32_t count, void *buf)
{
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (lba + count > ctx->length) {
        debug_block("gpt: read past end of partition\n");
        return -1;
    }
    return ctx->parent->read(ctx->parent, ctx->offset_lba + lba, count, buf);
}

static int partition_write(block_device_t *dev, uint64_t lba,
                           uint32_t count, const void *buf)
{
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (lba + count > ctx->length) {
        debug_block("gpt: write past end of partition\n");
        return -1;
    }
    return ctx->parent->write(ctx->parent, ctx->offset_lba + lba, count, buf);
}

// Create a partition wrapper block device.
// Name: parent name + partition index (1-based), e.g. "hda1", "hda2".
// Partition index is tracked as a simple static counter — each call increments.
static block_device_t *block_device_create_partition(
    block_device_t *parent, uint64_t offset_lba, uint64_t length, int part_idx)
{
    partition_ctx_t *ctx = kmalloc(sizeof(partition_ctx_t));
    if (!ctx) return NULL;
    ctx->parent     = parent;
    ctx->offset_lba = offset_lba;
    ctx->length     = length;

    // Build name: parent->name + partition index (1-based)
    char name[16];
    int nlen = strlen(parent->name);
    memcpy(name, parent->name, nlen);
    int digit_start = nlen;
    int p = part_idx;
    // Convert part_idx to string in reverse, then fix in place
    char tmp[8]; int ti = 0;
    do { tmp[ti++] = '0' + (p % 10); p /= 10; } while (p > 0);
    while (ti > 0) name[digit_start++] = tmp[--ti];
    name[digit_start] = '\0';

    block_device_t *dev = block_device_register_raw(name, length, ctx);
    if (!dev) { kfree(ctx); return NULL; }

    // Set custom hooks AFTER register_raw (which does not overwrite)
    dev->read  = partition_read;
    dev->write = partition_write;
    return dev;
}

// ── GPT helpers ──────────────────────────────────────────
static int guid_is_zero(const uint8_t *guid)
{
    for (int i = 0; i < 16; i++)
        if (guid[i] != 0) return 0;
    return 1;
}

static void gpt_extract_name(const uint8_t *entry, int entry_size,
                              char *out, size_t out_len)
{
    if (entry_size < 128) { out[0] = '\0'; return; }
    const uint8_t *name_field = entry + 56;
    size_t pos = 0;
    for (int i = 0; i < 36 && pos < out_len - 1; i++) {
        uint16_t wc = (uint16_t)name_field[i * 2]
                    | ((uint16_t)name_field[i * 2 + 1] << 8);
        if (wc == 0) break;
        out[pos++] = (wc < 0x80) ? (char)wc : '?';
    }
    out[pos] = '\0';
}

// ── gpt_scan — main entry point ──────────────────────────
gpt_info_t *gpt_scan(block_device_t *disk)
{
    if (!disk || !disk->present) return NULL;

    // Phase 1: Read GPT header (LBA 1)
    uint8_t hdr[512];
    if (block_device_read(disk, 1, 1, hdr) != 0) {
        debug_block("gpt: failed to read header\n");
        return NULL;
    }
    if (memcmp(hdr, "EFI PART", 8) != 0) {
        debug_block("gpt: no EFI PART signature\n");
        return NULL;
    }
    uint32_t revision = *(uint32_t *)(hdr + 8);
    if (revision != 0x00010000) {
        debug_block("gpt: unsupported revision %#x\n", revision);
        return NULL;
    }

    // Phase 2: Dynamic parameters from header
    uint32_t header_size    = *(uint32_t *)(hdr + 12);
    uint64_t entry_lba      = *(uint64_t *)(hdr + 72);
    uint32_t num_entries    = *(uint32_t *)(hdr + 80);
    uint32_t entry_size     = *(uint32_t *)(hdr + 84);
    uint32_t hdr_crc_stored = *(uint32_t *)(hdr + 16);

    if (header_size < 92 || entry_size < 128) {
        debug_block("gpt: bad header/entry size\n");
        return NULL;
    }
    uint64_t array_size = (uint64_t)num_entries * entry_size;
    if (array_size > 1024 * 1024) {
        debug_block("gpt: partition table too large\n");
        return NULL;
    }

    // Phase 3: Validate header CRC32
    uint32_t crc_saved = hdr_crc_stored;
    memset(hdr + 16, 0, 4);
    uint32_t crc_computed = gpt_crc32(hdr, header_size);
    if (crc_computed != crc_saved) {
        debug_block("gpt: header CRC mismatch\n");
        return NULL;
    }

    // Phase 4: Read + validate partition entry array
    uint32_t array_sectors = (uint32_t)((array_size + 511) / 512);
    uint8_t *entries = kmalloc(array_sectors * 512);
    if (!entries) return NULL;
    if (block_device_read(disk, entry_lba, array_sectors, entries) != 0) {
        debug_block("gpt: failed to read partition entries\n");
        kfree(entries); return NULL;
    }
    uint32_t entries_crc_stored = *(uint32_t *)(hdr + 88);
    uint32_t entries_crc_computed = gpt_crc32(entries, (uint32_t)array_size);
    if (entries_crc_computed != entries_crc_stored) {
        debug_block("gpt: partition entries CRC mismatch\n");
        kfree(entries); return NULL;
    }

    // Phase 5: Allocate result + enumerate
    gpt_info_t *info = calloc(1, sizeof(gpt_info_t));
    if (!info) { kfree(entries); return NULL; }

    int part_idx = 1;  // 1-based partition index for naming
    for (uint32_t i = 0; i < num_entries && info->count < GPT_PARTITION_MAX; i++) {
        uint8_t *entry = entries + (uint64_t)i * entry_size;
        if (guid_is_zero(entry)) continue;

        uint64_t start_lba = *(uint64_t *)(entry + 32);
        uint64_t end_lba   = *(uint64_t *)(entry + 40);
        if (end_lba < start_lba) continue;

        uint64_t length = end_lba - start_lba + 1;
        gpt_partition_t *part = &info->partitions[info->count];

        memcpy(part->type_guid, entry, 16);
        part->start_lba = start_lba;
        part->end_lba   = end_lba;
        part->parent    = disk;
        gpt_extract_name(entry, (int)entry_size, part->name, sizeof(part->name));

        part->dev = block_device_create_partition(disk, start_lba, length, part_idx++);
        if (!part->dev) {
            debug_block("gpt: failed to create partition device\n");
            continue;
        }

        devfs_register_blkdev(part->dev->name, part->dev);

        debug_block("gpt: partition '%s' LBA %lu-%lu (%lu sectors)\n",
                    part->name, start_lba, end_lba, length);
        info->count++;
    }

    kfree(entries);
    debug_block("gpt: found %d partitions\n", info->count);
    return info;
}
```

Note: `gpt_info_t` is never freed — acceptable for kernel bootstrap init (lives for the kernel's lifetime).

- [ ] **Step 3: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success.

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/gpt.c kernel/include/fs/gpt.h
git commit -m "feat(gpt): add GPT partition scanner with CRC32 verification"
```

---

### Task 6: ext2 — header with on-disk structures

**Files:**
- Create: `kernel/include/fs/ext2.h`

- [ ] **Step 1: Create ext2.h**

```c
// kernel/include/fs/ext2.h
#ifndef _FS_EXT2_H
#define _FS_EXT2_H

#include <stdint.h>
#include <block/blockdev.h>
#include <fs/vfs.h>
#include <kernel/arch/x86_64/spinlock.h>

// ── Constants ──────────────────────────────────────────
#define EXT2_SB_OFFSET      1024
#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_S_IFREG        0x8000
#define EXT2_S_IFDIR        0x4000

// ── On-disk superblock (first 264 meaningful bytes of 1024-byte block) ──
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // Revision 1 fields (offset 84):
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
} ext2_superblock_t;

// ── Block Group Descriptor (32 bytes) ──────────────────
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_bgdesc_t;

// ── Inode (first 128 bytes) ─────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
} ext2_inode_t;

// ── Directory Entry (variable-length, rec_len linked list) ──
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];        // name_len bytes, no NUL terminator guaranteed
} ext2_dirent_t;

// ── Runtime filesystem context ─────────────────────────
typedef struct {
    block_device_t   *dev;
    uint32_t          block_size;
    uint32_t          sectors_per_block;
    uint32_t          inodes_per_group;
    uint32_t          blocks_per_group;
    uint32_t          num_block_groups;
    uint32_t          inode_size;
    ext2_bgdesc_t    *bgdesc_table;
    spinlock_T        lock;        // ★ spinlock_T (capital T), see spinlock.h:10
} ext2_fs_t;

// ── API ────────────────────────────────────────────────
int ext2_init(block_device_t *dev, ext2_fs_t **out_fs);
extern struct vfs_ops ext2_vfs_ops;

#endif
```

- [ ] **Step 2: Commit**

```bash
git add kernel/include/fs/ext2.h
git commit -m "feat(ext2): add on-disk structure definitions and API header"
```

---

### Task 7: ext2 — read-only driver (mount + read + readdir)

**Files:**
- Create: `kernel/fs/ext2.c`

- [ ] **Step 1: Create ext2.c with mount, read_inode, bmap, readdir ops**

```c
// kernel/fs/ext2.c
#include <fs/ext2.h>
#include <kernel/debug.h>
#include <string.h>
#include <stdlib.h>

// ── Block I/O helper ───────────────────────────────────
static int ext2_read_block(ext2_fs_t *fs, uint32_t block, void *buf)
{
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return block_device_read(fs->dev, lba, fs->sectors_per_block, buf);
}

// ── Read an inode from disk ─────────────────────────────
static int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->num_block_groups) return -1;

    uint32_t table_start    = fs->bgdesc_table[group].bg_inode_table;
    uint32_t inodes_per_blk = fs->block_size / fs->inode_size;
    uint32_t block_off      = index / inodes_per_blk;
    uint32_t inode_off      = (index % inodes_per_blk) * fs->inode_size;

    // Stack buffer: max inode size 256 bytes
    uint8_t buf[256];
    if (ext2_read_block(fs, table_start + block_off, buf) != 0)
        return -1;

    memcpy(out, buf + inode_off, sizeof(ext2_inode_t));
    return 0;
}

// ── Map logical block → physical (direct + single indirect) ─
static uint32_t ext2_bmap(ext2_fs_t *fs, ext2_inode_t *inode,
                          uint32_t logical_block)
{
    if (logical_block < 12)
        return inode->i_block[logical_block];

    uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
    if (logical_block < 12 + ptrs_per_block) {
        uint32_t indirect_blk = inode->i_block[12];
        if (indirect_blk == 0) return 0;

        uint32_t indirect[1024];  // up to 4096/4 = 1024 entries
        if (ext2_read_block(fs, indirect_blk, indirect) != 0)
            return 0;
        return indirect[logical_block - 12];
    }

    return 0;  // double/triple indirect not implemented
}

// ── VFS read implementation ─────────────────────────────
static int ext2_vfs_read(struct vfs_node *node, uint64_t offset,
                          uint64_t size, void *buffer)
{
    if (!node || !buffer || size == 0) return 0;
    uint32_t ino = (uint32_t)(uintptr_t)node->fs_data;
    ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;

    spin_lock(&fs->lock);

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) {
        spin_unlock(&fs->lock); return -1;
    }

    if (offset >= inode.i_size) { spin_unlock(&fs->lock); return 0; }
    if (offset + size > inode.i_size)
        size = inode.i_size - offset;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t file_off = offset;

    while (remaining > 0) {
        uint32_t logical_block = (uint32_t)(file_off / fs->block_size);
        uint32_t block_off     = (uint32_t)(file_off % fs->block_size);

        uint32_t phys = ext2_bmap(fs, &inode, logical_block);
        if (phys == 0) { spin_unlock(&fs->lock); return -1; }

        uint8_t block_buf[4096];  // fixed size, block_size ≤ 4096
        if (ext2_read_block(fs, phys, block_buf) != 0) {
            spin_unlock(&fs->lock); return -1;
        }

        uint32_t chunk = (uint32_t)(fs->block_size - block_off);
        if (chunk > remaining) chunk = (uint32_t)remaining;

        memcpy(out, block_buf + block_off, chunk);
        out       += chunk;
        file_off  += chunk;
        remaining -= chunk;
    }

    spin_unlock(&fs->lock);
    return (int)size;
}

// ── VFS readdir implementation ──────────────────────────
static int ext2_vfs_readdir(struct vfs_node *node, uint64_t index,
                             struct vfs_dirent *entry)
{
    if (!node || !entry) return -1;
    if (node->type != VFS_DIR) return -1;

    uint32_t ino = (uint32_t)(uintptr_t)node->fs_data;
    ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;

    spin_lock(&fs->lock);

    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, ino, &dir_inode) != 0) {
        spin_unlock(&fs->lock); return -1;
    }
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) {
        spin_unlock(&fs->lock); return -1;
    }

    uint8_t block_data[4096];
    uint64_t entry_idx = 0;

    for (uint32_t blk_idx = 0; ; blk_idx++) {
        uint32_t phys = ext2_bmap(fs, &dir_inode, blk_idx);
        if (phys == 0) break;

        if (ext2_read_block(fs, phys, block_data) != 0) break;

        uint32_t off = 0;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_data + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                if (entry_idx == index) {
                    size_t nlen = de->name_len;
                    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
                    memcpy(entry->name, de->name, nlen);
                    entry->name[nlen] = '\0';
                    entry->ino  = de->inode;
                    entry->size = 0;
                    entry->type = (de->file_type == 2) ? VFS_DIR : VFS_FILE;
                    spin_unlock(&fs->lock);
                    return 0;
                }
                entry_idx++;
            }
            off += de->rec_len;
        }
    }

    entry->name[0] = '\0';
    spin_unlock(&fs->lock);
    return 0;
}

// ── VFS operations table ────────────────────────────────
struct vfs_ops ext2_vfs_ops = {
    .flags   = 0,  // case-sensitive
    .read    = ext2_vfs_read,
    .write   = NULL,
    .readdir = ext2_vfs_readdir,
    .create  = NULL,
    .unlink  = NULL,
    .mkdir   = NULL,
    .rmdir   = NULL,
    .rename  = NULL,
    .truncate = NULL,
};

// ── ext2_init (mount) ───────────────────────────────────
int ext2_init(block_device_t *dev, ext2_fs_t **out_fs)
{
    *out_fs = NULL;
    if (!dev || !dev->present) return -1;

    ext2_fs_t *fs = calloc(1, sizeof(ext2_fs_t));
    if (!fs) return -1;
    fs->dev = dev;
    spin_init(&fs->lock);

    // Read superblock (byte 1024 = sector 2)
    uint8_t sb_buf[1024];
    if (block_device_read(dev, 2, 2, sb_buf) != 0) {
        kfree(fs); return -1;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) {
        debug_fs("ext2: bad magic %#x\n", sb->s_magic);
        kfree(fs); return -1;
    }

    fs->block_size   = 1024u << sb->s_log_block_size;
    if (fs->block_size > 4096) { kfree(fs); return -1; }
    fs->sectors_per_block = fs->block_size / 512;
    fs->blocks_per_group  = sb->s_blocks_per_group;
    fs->inodes_per_group  = sb->s_inodes_per_group;
    fs->num_block_groups  = (sb->s_blocks_count + fs->blocks_per_group - 1)
                            / fs->blocks_per_group;
    fs->inode_size = (sb->s_inode_size != 0) ? sb->s_inode_size : 128;

    // bgdesc table starts at block after superblock
    uint32_t sb_block     = (fs->block_size == 1024) ? 1u : 0u;
    uint32_t bgdesc_block = sb_block + 1;

    uint32_t table_size   = fs->num_block_groups * sizeof(ext2_bgdesc_t);
    uint32_t table_blocks = (table_size + fs->block_size - 1) / fs->block_size;
    fs->bgdesc_table = kmalloc(table_blocks * fs->block_size);
    if (!fs->bgdesc_table) { kfree(fs); return -1; }

    for (uint32_t i = 0; i < table_blocks; i++) {
        if (ext2_read_block(fs, bgdesc_block + i,
            (uint8_t *)fs->bgdesc_table + i * fs->block_size) != 0)
        {
            kfree(fs->bgdesc_table); kfree(fs); return -1;
        }
    }

    debug_fs("ext2: mounted — ino_size=%u blk_size=%u groups=%u\n",
             fs->inode_size, fs->block_size, fs->num_block_groups);
    *out_fs = fs;
    return 0;
}
```

- [ ] **Step 2: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success. No unused-function warnings (ext2_find_entry is not included — __vfs_lookup uses readdir iteration to find entries).

- [ ] **Step 3: Commit**

```bash
git add kernel/fs/ext2.c
git commit -m "feat(ext2): add read-only ext2 driver (mount, read, readdir)"
```

---

### Task 8: vfs_ops_t flags + vfs_dirent_t.ino → uint64_t

**Files:**
- Modify: `kernel/include/fs/vfs.h:21-48` (vfs_ops_t.flags), `vfs.h:64` (ino uint32_t → uint64_t)
- Modify: `kernel/fs/vfs.c:190` (__vfs_lookup name comparison)
- Modify: `kernel/fs/vfs.c:371` (vfs_getdents sort)
- Modify: `kernel/fs/fat.c` — ops .flags

- [ ] **Step 1: Change vfs_dirent_t.ino from uint32_t to uint64_t**

In `kernel/include/fs/vfs.h`, line 64:
```c
// Before:
    uint32_t ino;           // filesystem-specific id
// After:
    uint64_t ino;           // filesystem-specific id (must hold kernel pointer for tmpfs)
```

This is an ABI-safe change: `vfs_dirent_t` is only used inside the kernel, never passed to userspace directly (`vfs_getdents` converts to `linux_dirent64` which already uses `uint64_t d_ino`).

- [ ] **Step 2: Add flags to vfs_ops_t**

In `kernel/include/fs/vfs.h`, at the top of the vfs_ops struct:
```c
typedef struct vfs_ops {
    uint32_t flags;
    #define VFS_OPS_CASE_INSENSITIVE  (1 << 0)
    int (*read)(struct vfs_node *node, uint64_t offset,
                uint64_t size, void *buffer);
    // ... rest unchanged
```

- [ ] **Step 3: Update __vfs_lookup name comparison**

In `kernel/fs/vfs.c`, at the name comparison after `if (ret == 0)` (~line 190):
```c
            if (ret == 0) {
                int match;
                if (current->ops && (current->ops->flags & VFS_OPS_CASE_INSENSITIVE))
                    match = (vfs_name_cmp(entry.name, comp) == 0);
                else
                    match = (strcmp(entry.name, comp) == 0);

                if (match) {
```

- [ ] **Step 4: Update vfs_getdents sort comparison**

In `kernel/fs/vfs.c`, at the sort comparison (~line 371):
```c
            int cmp;
            if (dir->ops && (dir->ops->flags & VFS_OPS_CASE_INSENSITIVE))
                cmp = vfs_name_cmp(entries[j].name, entries[j + 1].name);
            else
                cmp = strcmp(entries[j].name, entries[j + 1].name);
            if (cmp > 0) {
```

Replace the existing `if (vfs_name_cmp(entries[j].name, entries[j + 1].name) > 0)` with the above.

- [ ] **Step 5: Add .flags to fat_vfs_ops**

In `kernel/fs/fat.c`, find `static struct vfs_ops fat_vfs_ops = {` and add `.flags = VFS_OPS_CASE_INSENSITIVE,` as the first initializer.

devfs_ops, procfs_ops, and (future) ext2_vfs_ops, tmpfs_vfs_ops all default `.flags = 0` = case-sensitive — correct.

- [ ] **Step 6: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success.

- [ ] **Step 7: Commit**

```bash
git add kernel/include/fs/vfs.h kernel/fs/vfs.c kernel/fs/fat.c
git commit -m "feat(vfs): per-fs case sensitivity + vfs_dirent_t.ino uint32_t→uint64_t"
```

Note: The `ino` change is critical for tmpfs (Task 10) — tmpfs stores `(uint64_t)(uintptr_t)tmpfs_node*` in `ino`.

---

### Task 9: fat32_mount → fat32_init refactor

**Files:**
- Modify: `kernel/include/fs/fat.h:99`
- Modify: `kernel/fs/fat.c` (fat32_mount implementation)

- [ ] **Step 1: Change fat32_mount signature in fat.h**

```c
// Before:
fat32_fs_t *fat32_mount(block_device_t *dev);
// After:
int fat32_init(block_device_t *dev, fat32_fs_t **out_fs);
```

- [ ] **Step 2: Update implementation in fat.c**

Find the `fat32_mount` function definition. Change signature, add `*out_fs = NULL;` at top. Change all `return NULL;` (error) → `return -1;`. Change `return fs;` (success) → `*out_fs = fs; return 0;`.

- [ ] **Step 3: DO NOT update main.c yet**

The main.c call site update will happen in Task 12 (integration). The old code `fat32_mount(dev)` + `vfs_mount("/", ...)` still compiles because `fat32_init` has a different name. Both old and new code paths coexist until Task 12 replaces the old one.

- [ ] **Step 4: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success (old main.c still calls old fat32_mount, which we just renamed the declaration but the implementation is renamed too, so link fails...).

Wait — if we rename the implementation to `fat32_init` but main.c still calls `fat32_mount`, the linker will fail. Options:
A) Keep old `fat32_mount` as a wrapper that calls `fat32_init` + `vfs_mount`
B) Just rename the implementation AND update main.c now

Since Task 12 will fully rewrite the storage section anyway, choose **B**: rename implementation, update main.c old call path minimally:

In `kernel/kernel/main.c`, replace lines 180-184:
```c
    if (block_device_count() > 0) {
        block_device_t *dev = block_device_get(0);
        fat32_fs_t *fs = fat32_mount(dev);
        if (fs)
            vfs_mount("/", dev, &fat_vfs_ops, fs);
    }
```
With:
```c
    if (block_device_count() > 0) {
        block_device_t *dev = block_device_get(0);
        fat32_fs_t *fs = NULL;
        if (0 == fat32_init(dev, &fs))
            vfs_mount("/", dev, &fat_vfs_ops, fs);
    }
```

This preserves the old single-FAT fallback (GPT-scan-failure path in Task 12 will also use this pattern).

- [ ] **Step 5: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success.

- [ ] **Step 6: Commit**

```bash
git add kernel/include/fs/fat.h kernel/fs/fat.c kernel/kernel/main.c
git commit -m "refactor(fat): fat32_mount → fat32_init — caller vfs_mount"
```

---

### Task 10: tmpfs — memory filesystem

**Files:**
- Create: `kernel/include/fs/tmpfs.h`
- Create: `kernel/fs/tmpfs.c`

- [ ] **Step 1: Create tmpfs.h**

```c
// kernel/include/fs/tmpfs.h
#ifndef _FS_TMPFS_H
#define _FS_TMPFS_H

void tmpfs_init(void);

#endif
```

- [ ] **Step 2: Create tmpfs.c — data structures**

```c
// kernel/fs/tmpfs.c
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <kernel/debug.h>
#include <kernel/pmm.h>    // alloc_4k_page, free_4k_page
#include <kernel/vmm.h>    // Phy_To_Virt, Virt_To_Phy
#include <string.h>
#include <stdlib.h>

#define TMPFS_CHILDREN_INIT_CAP 8

// Block metadata (kmalloc'd, 2 pointers + index)
typedef struct tmpfs_block_ptr {
    struct tmpfs_block_ptr *next;
    uint64_t                blk_idx;
    void                   *page;   // Phy_To_Virt(alloc_4k_page()) → 4096 bytes
} tmpfs_block_ptr_t;

// tmpfs inode
typedef struct tmpfs_node {
    char               name[256];
    uint8_t            type;
    uint64_t           size;
    struct tmpfs_node *parent;

    // File: block chain
    tmpfs_block_ptr_t *first_block;
    tmpfs_block_ptr_t *last_block;

    // Directory: children array
    struct tmpfs_node **children;
    int                 child_count;
    int                 child_cap;
} tmpfs_node_t;
```

- [ ] **Step 3: Helper functions**

```c
static tmpfs_block_ptr_t *tmpfs_find_block(tmpfs_node_t *tn, uint64_t blk_idx)
{
    tmpfs_block_ptr_t *bp = tn->first_block;
    while (bp) {
        if (bp->blk_idx == blk_idx) return bp;
        bp = bp->next;
    }
    return NULL;
}

static int tmpfs_grow_to(tmpfs_node_t *tn, uint64_t last_blk)
{
    for (uint64_t i = 0; i <= last_blk; i++) {
        if (tmpfs_find_block(tn, i)) continue;

        tmpfs_block_ptr_t *bp = kmalloc(sizeof(tmpfs_block_ptr_t));
        if (!bp) return -1;
        bp->blk_idx = i;
        uint64_t phys = alloc_4k_page();  // ★ returns uint64_t physical address
        if (!phys) { kfree(bp); return -1; }
        bp->page = (void *)Phy_To_Virt(phys);
        memset(bp->page, 0, 4096);
        bp->next = NULL;

        if (!tn->first_block) {
            tn->first_block = bp;
            tn->last_block  = bp;
        } else {
            tn->last_block->next = bp;
            tn->last_block       = bp;
        }
    }
    return 0;
}

static void tmpfs_free_blocks(tmpfs_node_t *tn)
{
    tmpfs_block_ptr_t *bp = tn->first_block;
    while (bp) {
        tmpfs_block_ptr_t *next = bp->next;
        if (bp->page) free_4k_page(Virt_To_Phy(bp->page));  // ★ free_4k_page(uint64_t phys)
        kfree(bp);
        bp = next;
    }
    tn->first_block = NULL;
    tn->last_block  = NULL;
}

static void tmpfs_truncate_blocks(tmpfs_node_t *tn, uint64_t new_size)
{
    uint64_t last_keep = (new_size > 0) ? ((new_size - 1) / 4096) : 0;
    tmpfs_block_ptr_t *prev = NULL;
    tmpfs_block_ptr_t *bp   = tn->first_block;

    while (bp) {
        if (bp->blk_idx > last_keep) {
            if (bp->page) free_4k_page(Virt_To_Phy(bp->page));
            tmpfs_block_ptr_t *next = bp->next;
            kfree(bp);
            if (prev) prev->next = next;
            else tn->first_block = next;
            if (bp == tn->last_block) tn->last_block = prev;
            bp = next;
        } else {
            prev = bp;
            bp   = bp->next;
        }
    }
}

static int tmpfs_find_child(tmpfs_node_t *dir, const char *name)
{
    for (int i = 0; i < dir->child_count; i++) {
        if (strcmp(dir->children[i]->name, name) == 0)
            return i;
    }
    return -1;
}
```

- [ ] **Step 4: VFS ops — read, write, readdir**

```c
static int tmpfs_vfs_read(struct vfs_node *node, uint64_t offset,
                           uint64_t size, void *buffer)
{
    tmpfs_node_t *tn = (tmpfs_node_t *)node->fs_data;
    if (!tn || !buffer || size == 0) return 0;
    if (offset >= tn->size) return 0;
    if (offset + size > tn->size) size = tn->size - offset;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t pos = offset;

    while (remaining > 0) {
        uint64_t blk_idx = pos / 4096;
        uint32_t blk_off = (uint32_t)(pos % 4096);
        tmpfs_block_ptr_t *bp = tmpfs_find_block(tn, blk_idx);
        if (!bp) return -1;

        uint32_t chunk = 4096 - blk_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        memcpy(out, (uint8_t *)bp->page + blk_off, chunk);
        out       += chunk;
        pos       += chunk;
        remaining -= chunk;
    }
    return (int)size;
}

static int tmpfs_vfs_write(struct vfs_node *node, uint64_t offset,
                            uint64_t size, void *buffer)
{
    tmpfs_node_t *tn = (tmpfs_node_t *)node->fs_data;
    if (!tn || !buffer || size == 0) return 0;

    uint64_t end_pos = offset + size;
    uint64_t last_blk = (end_pos > 0) ? ((end_pos - 1) / 4096) : 0;
    if (tmpfs_grow_to(tn, last_blk) != 0) return -1;
    if (end_pos > tn->size) tn->size = end_pos;

    uint8_t *in = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t pos = offset;

    while (remaining > 0) {
        uint64_t blk_idx = pos / 4096;
        uint32_t blk_off = (uint32_t)(pos % 4096);
        tmpfs_block_ptr_t *bp = tmpfs_find_block(tn, blk_idx);
        if (!bp) return -1;

        uint32_t chunk = 4096 - blk_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        memcpy((uint8_t *)bp->page + blk_off, in, chunk);
        in        += chunk;
        pos       += chunk;
        remaining -= chunk;
    }
    return (int)size;
}

static int tmpfs_vfs_readdir(struct vfs_node *node, uint64_t index,
                              struct vfs_dirent *entry)
{
    tmpfs_node_t *d = (tmpfs_node_t *)node->fs_data;
    if (!d || d->type != VFS_DIR) return -1;

    if (index == 0) {
        strcpy(entry->name, ".");
        entry->type = VFS_DIR;
        entry->ino  = (uint64_t)(uintptr_t)d;
        return 0;
    }
    if (index == 1) {
        strcpy(entry->name, "..");
        entry->type = VFS_DIR;
        entry->ino  = (uint64_t)(uintptr_t)(d->parent ? d->parent : d);
        return 0;
    }

    uint64_t child_idx = index - 2;
    if (child_idx >= (uint64_t)d->child_count) {
        entry->name[0] = '\0';
        return 0;
    }

    tmpfs_node_t *child = d->children[child_idx];
    size_t nlen = strlen(child->name);
    if (nlen >= 256) nlen = 255;
    memcpy(entry->name, child->name, nlen);
    entry->name[nlen] = '\0';
    entry->type = child->type;
    entry->size = child->size;
    entry->ino  = (uint64_t)(uintptr_t)child;  // ★ uint64_t ino can hold kernel pointer
    return 0;
}
```

The `ino = (uint64_t)(uintptr_t)child` cast is correct now because Task 8 changed `vfs_dirent_t.ino` to `uint64_t`. `__vfs_lookup` does `child->fs_data = (void *)(uintptr_t)entry.ino`, which correctly reconstructs the pointer.

- [ ] **Step 5: VFS ops — create, mkdir, unlink, rmdir, rename, truncate**

```c
static struct vfs_node *tmpfs_vfs_create(struct vfs_node *dir, const char *name)
{
    tmpfs_node_t *d = (tmpfs_node_t *)dir->fs_data;
    if (!d || d->type != VFS_DIR) return NULL;
    if (tmpfs_find_child(d, name) >= 0) return NULL;

    tmpfs_node_t *new_node = calloc(1, sizeof(tmpfs_node_t));
    if (!new_node) return NULL;
    strcpy(new_node->name, name);
    new_node->type   = VFS_FILE;
    new_node->parent = d;

    // Expand children array: kmalloc+memcpy+kfree (NEVER realloc — libc realloc breaks on kmalloc)
    if (d->child_count >= d->child_cap) {
        int new_cap = d->child_cap * 2;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) { kfree(new_node); return NULL; }
        memcpy(new_arr, d->children, d->child_count * sizeof(tmpfs_node_t *));
        memset(new_arr + d->child_count, 0,
               (new_cap - d->child_count) * sizeof(tmpfs_node_t *));
        kfree(d->children);
        d->children  = new_arr;
        d->child_cap = new_cap;
    }
    d->children[d->child_count++] = new_node;

    struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
    if (!vn) { d->children[--d->child_count] = NULL; kfree(new_node); return NULL; }
    strcpy(vn->name, name);
    vn->type    = VFS_FILE;
    vn->size    = 0;
    vn->fs_data = new_node;
    vn->ops     = &tmpfs_vfs_ops;
    vn->mount   = dir->mount;
    vn->parent  = dir;
    vn->refcount = 1;
    return vn;
}

static struct vfs_node *tmpfs_vfs_mkdir(struct vfs_node *dir, const char *name)
{
    tmpfs_node_t *d = (tmpfs_node_t *)dir->fs_data;
    if (!d || d->type != VFS_DIR) return NULL;
    if (tmpfs_find_child(d, name) >= 0) return NULL;

    tmpfs_node_t *new_node = calloc(1, sizeof(tmpfs_node_t));
    if (!new_node) return NULL;
    strcpy(new_node->name, name);
    new_node->type   = VFS_DIR;
    new_node->parent = d;

    if (d->child_count >= d->child_cap) {
        int new_cap = d->child_cap * 2;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) { kfree(new_node); return NULL; }
        memcpy(new_arr, d->children, d->child_count * sizeof(tmpfs_node_t *));
        kfree(d->children);
        d->children  = new_arr;
        d->child_cap = new_cap;
    }
    d->children[d->child_count++] = new_node;

    struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
    if (!vn) { d->children[--d->child_count] = NULL; kfree(new_node); return NULL; }
    strcpy(vn->name, name);
    vn->type    = VFS_DIR;
    vn->size    = 0;
    vn->fs_data = new_node;
    vn->ops     = &tmpfs_vfs_ops;
    vn->mount   = dir->mount;
    vn->parent  = dir;
    vn->refcount = 1;
    return vn;
}

static int tmpfs_vfs_unlink(struct vfs_node *dir, const char *name)
{
    tmpfs_node_t *d = (tmpfs_node_t *)dir->fs_data;
    if (!d || d->type != VFS_DIR) return -1;
    int idx = tmpfs_find_child(d, name);
    if (idx < 0) return -1;
    tmpfs_node_t *child = d->children[idx];
    tmpfs_free_blocks(child);
    kfree(child);
    d->children[idx] = d->children[d->child_count - 1];
    d->child_count--;
    return 0;
}

static int tmpfs_vfs_rmdir(struct vfs_node *dir, const char *name)
{
    tmpfs_node_t *d = (tmpfs_node_t *)dir->fs_data;
    if (!d) return -1;
    int idx = tmpfs_find_child(d, name);
    if (idx < 0) return -1;
    tmpfs_node_t *child = d->children[idx];
    if (child->child_count > 0) return -1;
    kfree(child);
    d->children[idx] = d->children[d->child_count - 1];
    d->child_count--;
    return 0;
}

static int tmpfs_vfs_rename(struct vfs_node *olddir, const char *oldname,
                             struct vfs_node *newdir, const char *newname)
{
    tmpfs_node_t *od = (tmpfs_node_t *)olddir->fs_data;
    tmpfs_node_t *nd = (tmpfs_node_t *)newdir->fs_data;
    if (!od || !nd) return -1;
    int idx = tmpfs_find_child(od, oldname);
    if (idx < 0) return -1;
    if (tmpfs_find_child(nd, newname) >= 0) return -1;

    tmpfs_node_t *child = od->children[idx];
    strcpy(child->name, newname);
    child->parent = nd;

    od->children[idx] = od->children[od->child_count - 1];
    od->child_count--;

    if (nd->child_count >= nd->child_cap) {
        int new_cap = nd->child_cap * 2;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) return -1;
        memcpy(new_arr, nd->children, nd->child_count * sizeof(tmpfs_node_t *));
        kfree(nd->children);
        nd->children  = new_arr;
        nd->child_cap = new_cap;
    }
    nd->children[nd->child_count++] = child;
    return 0;
}

static int tmpfs_vfs_truncate(struct vfs_node *node, uint64_t new_size)
{
    tmpfs_node_t *tn = (tmpfs_node_t *)node->fs_data;
    if (!tn) return -1;
    tmpfs_truncate_blocks(tn, new_size);
    tn->size = new_size;
    return 0;
}

static struct vfs_ops tmpfs_vfs_ops = {
    .flags    = 0,  // case-sensitive
    .read     = tmpfs_vfs_read,
    .write    = tmpfs_vfs_write,
    .readdir  = tmpfs_vfs_readdir,
    .create   = tmpfs_vfs_create,
    .unlink   = tmpfs_vfs_unlink,
    .mkdir    = tmpfs_vfs_mkdir,
    .rmdir    = tmpfs_vfs_rmdir,
    .rename   = tmpfs_vfs_rename,
    .truncate = tmpfs_vfs_truncate,
};
```

- [ ] **Step 6: tmpfs_init()**

```c
void tmpfs_init(void)
{
    tmpfs_node_t *root = calloc(1, sizeof(tmpfs_node_t));
    if (!root) return;
    root->type = VFS_DIR;
    strcpy(root->name, "/");
    root->children  = kmalloc(TMPFS_CHILDREN_INIT_CAP * sizeof(tmpfs_node_t *));
    root->child_cap = TMPFS_CHILDREN_INIT_CAP;
    root->parent    = NULL;

    vfs_mount("/tmp", NULL, &tmpfs_vfs_ops, root);
    debug_fs("tmpfs: mounted at /tmp\n");
}
```

- [ ] **Step 7: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -5`
Expected: Success.

- [ ] **Step 8: Commit**

```bash
git add kernel/fs/tmpfs.c kernel/include/fs/tmpfs.h
git commit -m "feat(tmpfs): add memory-based tmpfs with full VFS ops"
```

---

### Task 11: Update init paths (task.c + init.c)

**Files:**
- Modify: `kernel/sched/task.c:1386`
- Modify: `user/init.c:328-330`

- [ ] **Step 1: Fix spawn_user_task path**

In `kernel/sched/task.c`, line 1386:
```c
// Before:
    int64_t init_pid = spawn_user_task("/init.elf", NULL);
// After:
    int64_t init_pid = spawn_user_task("/bin/init", NULL);
```

- [ ] **Step 2: Fix user/init.c hardcoded paths**

In `user/init.c`, lines 328-330:
```c
// Before:
    add_action(ACT_RESPAWN, "", "/systest.elf");
    ...
    add_action(ACT_RESPAWN, "", "/busybox.elf sh");
// After:
    add_action(ACT_RESPAWN, "", "/bin/systest");
    ...
    add_action(ACT_RESPAWN, "", "/bin/busybox sh");
```

(Exact line numbers may shift — search for the strings.)

- [ ] **Step 3: Build kernel + user + verify**

Run: `make kernel/kernel.bin user 2>&1 | tail -5`
Expected: Success.

- [ ] **Step 4: Commit**

```bash
git add kernel/sched/task.c user/init.c
git commit -m "fix: update init paths /init.elf→/bin/init, /systest.elf→/bin/systest"
```

---

### Task 12: kernel_main.c — new multi-partition init sequence

**Files:**
- Modify: `kernel/kernel/main.c:176-212`

- [ ] **Step 1: Include new headers**

At the top of `kernel/kernel/main.c`, add:
```c
#include <fs/gpt.h>
#include <fs/ext2.h>
#include <fs/tmpfs.h>
```

- [ ] **Step 2: Replace storage section (~lines 176-210)**

Replace the existing storage+filesystem block (from `ahci_init()` through `procfs_init()`) with:

```c
    // ═══ 6. Storage + filesystem ════════════════════════════
    ahci_init();

    devfs_init();                   // mount /dev + register chrdev
                                    // ★ MUST be before any devfs_register_* call

    // Register physical disks in /dev
    for (int i = 0; i < block_device_count(); i++) {
        block_device_t *dev = block_device_get(i);
        devfs_register_blkdev(dev->name, dev);
    }

    vfs_init();                     // init mount table

    // Try GPT partition table scan
    gpt_info_t *gpt = (block_device_count() > 0)
                      ? gpt_scan(block_device_get(0)) : NULL;

    if (!gpt) {
        // Fallback: old single-FAT32 layout
        if (block_device_count() > 0) {
            block_device_t *dev = block_device_get(0);
            fat32_fs_t *fs = NULL;
            if (0 == fat32_init(dev, &fs))
                vfs_mount("/", dev, &fat_vfs_ops, fs);
        }
    } else {
        // Dual-partition layout:
        //   gpt->partitions[0] = hda1 (FAT32 ESP) → /boot
        //   gpt->partitions[1] = hda2 (ext2)      → /
        if (gpt->count >= 2) {
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
    }

    // /tmp → tmpfs (independent of disk)
    tmpfs_init();

    procfs_init();                  // /proc
```

Note: Remove the old `/dev/null` smoke test lines (now after the new sequence, or keep for debugging).

- [ ] **Step 3: Build**

Run: `make kernel/kernel.bin 2>&1 | tail -10`
Expected: Success.

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel/main.c
git commit -m "feat(main): GPT+ext2 dual-partition init with FAT32 fallback"
```

---

### Task 13: tools/mkdisk.c — disk image builder

**Files:**
- Create: `tools/mkdisk.c`
- Create: `tools/Makefile`

- [ ] **Step 1: Create tools/Makefile**

```makefile
# tools/Makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11

all: mkdisk

mkdisk: mkdisk.c
	$(CC) $(CFLAGS) -o $@ $<

check-deps:
	@missing=0; \
	for cmd in mkfs.vfat mcopy mmd mke2fs debugfs; do \
	    command -v $$cmd >/dev/null 2>&1 || { echo "ERROR: missing $$cmd (install dosfstools mtools e2fsprogs)"; missing=1; }; \
	done; \
	exit $$missing

clean:
	rm -f mkdisk
```

- [ ] **Step 2: Create mkdisk.c**

```c
// tools/mkdisk.c — GPT dual-partition disk image builder
// Build: make -C tools
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#define SECTOR_SIZE    512
#define FAT32_SIZE_MB  64
#define EXT2_SIZE_MB   128
#define ALIGN_LBA      2048
#define TOTAL_SIZE     ((FAT32_SIZE_MB + EXT2_SIZE_MB) * 1024 * 1024)
#define TOTAL_SECTORS  (TOTAL_SIZE / SECTOR_SIZE)

#define PART1_START    ALIGN_LBA
#define PART1_SECTORS  ((FAT32_SIZE_MB * 1024 * 1024) / SECTOR_SIZE)
#define PART1_END      (PART1_START + PART1_SECTORS - 1)

#define PART2_START    (((PART1_END + 1) + (ALIGN_LBA - 1)) / ALIGN_LBA * ALIGN_LBA)
#define PART2_SECTORS  ((EXT2_SIZE_MB * 1024 * 1024) / SECTOR_SIZE)
#define PART2_END      (PART2_START + PART2_SECTORS - 1)

// Standard reflected CRC32 (same as zlib, used by GPT)
static uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}

static void wr32(uint8_t *b, int off, uint32_t v) {
    for (int i = 0; i < 4; i++) b[off + i] = (v >> (i * 8)) & 0xFF;
}
static void wr64(uint8_t *b, int off, uint64_t v) {
    for (int i = 0; i < 8; i++) b[off + i] = (v >> (i * 8)) & 0xFF;
}

static void build_mbr(uint8_t *sector)
{
    memset(sector, 0, SECTOR_SIZE);
    sector[446 + 4] = 0xEE;  // GPT Protective
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static void build_gpt_header(uint8_t *hdr)
{
    memset(hdr, 0, 92);
    memcpy(hdr, "EFI PART", 8);
    wr32(hdr, 8,  0x00010000);
    wr32(hdr, 12, 92);
    wr64(hdr, 24, 1);
    wr64(hdr, 32, TOTAL_SECTORS - 1);   // alternate LBA (backup)
    wr64(hdr, 40, 34);
    wr64(hdr, 48, TOTAL_SECTORS - 34);
    wr64(hdr, 72, 2);
    wr32(hdr, 80, 128);
    wr32(hdr, 84, 128);
}

// ESP GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
static const uint8_t ESP_GUID[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8,0xD2,0x11,
    0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};

// Linux filesystem GUID: 0FC63DAF-8483-4772-8E79-3D69D8477DE4
static const uint8_t LINUX_FS_GUID[16] = {
    0xAF,0x3D,0xC6,0x0F, 0x83,0x84,0x72,0x47,
    0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4
};

static void gen_random_guid(uint8_t *out)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, out, 16); close(fd); }
    else {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (int i = 0; i < 16; i++) out[i] = (uint8_t)(rand() & 0xFF);
    }
}

static void write_gpt_name(uint8_t *entry, const char *name)
{
    size_t len = strlen(name);
    if (len > 36) len = 36;
    for (size_t i = 0; i < len; i++) {
        entry[56 + i * 2]     = (uint8_t)name[i];
        entry[56 + i * 2 + 1] = 0;
    }
}

static void build_partition_entry(uint8_t *entry, const uint8_t *type_guid,
                                   uint64_t start, uint64_t end, const char *name)
{
    memset(entry, 0, 128);
    memcpy(entry, type_guid, 16);
    gen_random_guid(entry + 16);
    wr64(entry, 32, start);
    wr64(entry, 40, end);
    write_gpt_name(entry, name);
}

static int run_cmd(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("  [cmd] %s\n", buf);
    return system(buf);
}

int main(int argc, char **argv)
{
    const char *efi_path    = NULL;
    const char *kernel_path = NULL;
    const char *rootfs_dir  = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--efi") && i+1 < argc)    efi_path    = argv[++i];
        else if (!strcmp(argv[i], "--kernel") && i+1 < argc) kernel_path = argv[++i];
        else if (!strcmp(argv[i], "--rootfs") && i+1 < argc)  rootfs_dir  = argv[++i];
    }
    if (!efi_path || !kernel_path || !rootfs_dir) {
        fprintf(stderr, "Usage: mkdisk --efi BOOTX64.EFI --kernel kernel.bin --rootfs fsroot/\n");
        return 1;
    }

    printf("Building disk.img: %dMB ESP + %dMB ext2 root\n", FAT32_SIZE_MB, EXT2_SIZE_MB);
    printf("  Part1 (ESP):  LBA %lu - %lu\n", (unsigned long)PART1_START, (unsigned long)PART1_END);
    printf("  Part2 (ext2): LBA %lu - %lu\n", (unsigned long)PART2_START, (unsigned long)PART2_END);

    // ── Phase 1: GPT structure ───────────────────────────
    FILE *f = fopen("disk.img", "wb");
    if (!f) { perror("fopen"); return 1; }

    uint8_t mbr[SECTOR_SIZE];
    build_mbr(mbr);
    fwrite(mbr, SECTOR_SIZE, 1, f);

    uint8_t gpt_hdr[92];
    build_gpt_header(gpt_hdr);

    uint8_t entries[128 * 128];
    memset(entries, 0, sizeof(entries));
    build_partition_entry(entries,       ESP_GUID,      PART1_START, PART1_END, "ESP");
    build_partition_entry(entries + 128, LINUX_FS_GUID, PART2_START, PART2_END, "rootfs");

    uint32_t entries_crc = crc32(entries, sizeof(entries));
    wr32(gpt_hdr, 88, entries_crc);

    wr32(gpt_hdr, 16, 0);
    uint32_t hdr_crc = crc32(gpt_hdr, 92);
    wr32(gpt_hdr, 16, hdr_crc);

    fwrite(gpt_hdr, 92, 1, f);
    fwrite(entries, sizeof(entries), 1, f);

    // Pad to PART1_START
    uint64_t pos = 2 + (sizeof(entries) / SECTOR_SIZE);
    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);
    while (pos < PART1_START) { fwrite(zero, SECTOR_SIZE, 1, f); pos++; }
    fclose(f);

    // ── Phase 2: Build + inject ESP ──────────────────────
    printf("Building ESP...\n");
    run_cmd("dd if=/dev/zero of=/tmp/_mkdisk_esp.img bs=1M count=%d 2>/dev/null", FAT32_SIZE_MB);
    run_cmd("mkfs.vfat -F 32 /tmp/_mkdisk_esp.img 2>/dev/null");
    run_cmd("mmd -i /tmp/_mkdisk_esp.img ::/EFI 2>/dev/null");
    run_cmd("mmd -i /tmp/_mkdisk_esp.img ::/EFI/BOOT 2>/dev/null");
    run_cmd("mcopy -i /tmp/_mkdisk_esp.img %s ::/EFI/BOOT 2>/dev/null", efi_path);
    run_cmd("mcopy -i /tmp/_mkdisk_esp.img %s ::/ 2>/dev/null", kernel_path);
    run_cmd("dd if=/tmp/_mkdisk_esp.img of=disk.img bs=512 seek=%lu conv=notrunc 2>/dev/null",
            (unsigned long)PART1_START);

    // ── Phase 3: Build + inject ext2 ─────────────────────
    printf("Building ext2 root...\n");
    run_cmd("dd if=/dev/zero of=/tmp/_mkdisk_rootfs.img bs=1M count=%d 2>/dev/null", EXT2_SIZE_MB);
    run_cmd("mke2fs -t ext2 -I 128 -b 4096 /tmp/_mkdisk_rootfs.img 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /bin\" 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /home\" 2>/dev/null");
    run_cmd("debugfs -w /tmp/_mkdisk_rootfs.img -R \"mkdir /etc\" 2>/dev/null");

    // Copy fsroot/bin/* to /bin/
    {
        char glob_cmd[1024];
        snprintf(glob_cmd, sizeof(glob_cmd),
                 "for f in %s/bin/*; do "
                 "  base=$(basename \"$f\"); "
                 "  debugfs -w /tmp/_mkdisk_rootfs.img -R \"write $f /bin/$base\" 2>/dev/null; "
                 "done", rootfs_dir);
        system(glob_cmd);
    }

    run_cmd("dd if=/tmp/_mkdisk_rootfs.img of=disk.img bs=512 seek=%lu conv=notrunc 2>/dev/null",
            (unsigned long)PART2_START);

    // ── Write backup GPT at end of disk ──────────────────
    {
        // Backup GPT header at last LBA
        wr64(gpt_hdr, 24, TOTAL_SECTORS - 1);  // my_lba
        wr64(gpt_hdr, 32, 1);                   // alternate_lba points to primary
        wr64(gpt_hdr, 72, TOTAL_SECTORS - 33);  // partition entry array now at end-32 sectors
        wr32(gpt_hdr, 16, 0);
        hdr_crc = crc32(gpt_hdr, 92);
        wr32(gpt_hdr, 16, hdr_crc);

        f = fopen("disk.img", "r+b");
        fseek(f, (long)(TOTAL_SECTORS - 1) * SECTOR_SIZE, SEEK_SET);
        fwrite(gpt_hdr, 92, 1, f);
        fseek(f, (long)(TOTAL_SECTORS - 33) * SECTOR_SIZE, SEEK_SET);
        fwrite(entries, sizeof(entries), 1, f);
        fclose(f);
    }

    // ── Cleanup ──────────────────────────────────────────
    unlink("/tmp/_mkdisk_esp.img");
    unlink("/tmp/_mkdisk_rootfs.img");

    // ── Self-check ───────────────────────────────────────
    printf("Self-check...\n");
    {
        uint8_t buf[1024];
        f = fopen("disk.img", "rb");
        if (f) {
            fseek(f, SECTOR_SIZE, SEEK_SET);
            fread(buf, 1, 92, f);
            printf("  GPT header: %s\n",
                   memcmp(buf, "EFI PART", 8) == 0 ? "OK" : "FAIL");

            fseek(f, PART2_START * 512LL + 1024, SEEK_SET);
            fread(buf, 1, 2, f);
            printf("  ext2 magic: %s\n",
                   (buf[56] == 0x53 && buf[57] == 0xEF) ? "OK" : "FAIL");

            fseek(f, PART1_START * 512LL + 510, SEEK_SET);
            fread(buf, 1, 2, f);
            printf("  FAT32 BB: %s\n",
                   (buf[0] == 0x55 && buf[1] == 0xAA) ? "OK" : "FAIL");

            fclose(f);
        }
    }

    printf("Done: disk.img (%lu MB)\n",
           (unsigned long)((TOTAL_SECTORS * SECTOR_SIZE) / 1024 / 1024));
    return 0;
}
```

- [ ] **Step 3: Build mkdisk**

Run: `make -C tools`
Expected: `mkdisk` binary created.

- [ ] **Step 4: Commit**

```bash
git add tools/mkdisk.c tools/Makefile
git commit -m "feat(tools): add mkdisk — GPT dual-partition disk builder with backup GPT"
```

Note: Partition index naming uses a local `part_idx` counter (1-based) passed to `block_device_create_partition`. Partition 1 = "ESP" = hda1, Partition 2 = "rootfs" = hda2.

---

### Task 14: Root Makefile + .gitignore

**Files:**
- Modify: `Makefile:97-112`
- Modify: `.gitignore`

- [ ] **Step 1: Replace disk.img target**

Replace the current `disk.img:` target with:

```makefile
disk.img: boot/uefi/BOOTX64.EFI lib kernel.bin user build/x86_64/user/busybox.elf
	@mkdir -p config/fsroot/bin config/fsroot/home config/fsroot/etc
	@cp build/x86_64/user/init.elf          config/fsroot/bin/init
	@cp build/x86_64/user/busybox.elf        config/fsroot/bin/busybox
	@cp build/x86_64/user/spin.elf           config/fsroot/bin/spin
	@cp build/x86_64/user/sigtest.elf        config/fsroot/bin/sigtest
	@cp build/x86_64/user/poweroff.elf       config/fsroot/bin/poweroff
	@cp build/x86_64/user/systest.elf        config/fsroot/bin/systest
	@cp build/x86_64/user/test_mmap.elf      config/fsroot/bin/test_mmap
	@cp build/x86_64/user/test_fork_mmap.elf config/fsroot/bin/test_fork_mmap
	@cp build/x86_64/user/test_cow.elf       config/fsroot/bin/test_cow
	$(MAKE) -C tools check-deps
	$(MAKE) -C tools
	tools/mkdisk disk.img \
	    --efi boot/uefi/BOOTX64.EFI \
	    --kernel kernel.bin \
	    --rootfs config/fsroot/
```

Note: The old `mcopy -i $@ config/config.txt ::/` line is removed — config.txt is not read by current init code (init.c's `parse_inittab` is an empty stub).

- [ ] **Step 2: Update .gitignore**

Add to `.gitignore`:
```
config/fsroot/
```

- [ ] **Step 3: Build disk.img**

Run: `make disk.img 2>&1 | tail -15`
Expected: `tools/mkdisk` builds, ESP + ext2 partitions filled, self-check OK, `disk.img` created (~192 MB).

- [ ] **Step 4: Commit**

```bash
git add Makefile .gitignore
git commit -m "feat(build): switch to GPT dual-partition disk.img via tools/mkdisk"
```

---

### Task 15: Integration test — full boot

**Files:**
- None (test only)

- [ ] **Step 1: Full clean build**

```bash
make clean && make disk.img 2>&1 | tail -20
```

Expected: Successful build.

- [ ] **Step 2: Boot in QEMU**

```bash
make run
```

Expected serial output:
```
AHCI: port 0: MODEL=... SECTORS=...
block: registered hda
devfs: mounted at /dev
devfs: registered blkdev hda
gpt: found 2 partitions
devfs: registered blkdev hda1
devfs: registered blkdev hda2
ext2: mounted — ino_size=128 blk_size=4096 groups=...
VFS: mounted '/'
VFS: mounted '/boot'
VFS: mounted '/tmp'
→ init.elf starts (from /bin/init)
```

- [ ] **Step 3: Verify /dev contents**

Once userland is running:
```
ls /dev
# Expected: null zero random serial tty keyboard fb hda hda1 hda2
```

- [ ] **Step 4: Verify /tmp is writable**

```
echo "hello" > /tmp/test.txt && cat /tmp/test.txt
# Expected: "hello"
```

- [ ] **Step 5: Verify /boot is FAT32 (case-insensitive lookup)**

```
ls /boot
# Expected: kernel.bin (and any other FAT32 directory entries)
```

- [ ] **Step 6: Commit any fixes**

If boot succeeds, this task is complete without code changes. If minor issues are found, fix and commit.

---

### Task 16: SELFTEST — kernel self-tests

**Files:**
- Modify: `kernel/fs/ext2.c` — add SELFTEST blocks
- Modify: `kernel/fs/tmpfs.c` — add SELFTEST blocks
- Modify: `kernel/fs/gpt.c` — add SELFTEST blocks

- [ ] **Step 1: Add ext2 SELFTEST**

In `kernel/fs/ext2.c`, at bottom:

```c
#ifdef KERNEL_SELFTEST
#include <kernel/selftest.h>

SELFTEST(test_ext2_magic)
{
    // EXT2_MAGIC must be 0xEF53
    if (EXT2_MAGIC != 0xEF53) return -1;
    return 0;
}

SELFTEST(test_ext2_struct_sizes)
{
    // Verify struct sizes match expected on-disk layout.
    // ext2_inode_t = 116 bytes (19 × 4B + 3 × 2B + 15×4B + 4×4B)
    //   i_block[15] = 60B, total = 116. 128B on-disk, but C struct is shorter.
    if (sizeof(ext2_inode_t) != 116) return -1;

    // ext2_bgdesc_t = 32 bytes
    if (sizeof(ext2_bgdesc_t) != 32) return -1;

    // ext2_dirent_t = 8 bytes + flexible array
    // (sizeof reports the non-flexible-array portion)
    if (sizeof(ext2_dirent_t) != 8) return -1;

    return 0;
}
#endif
```

- [ ] **Step 2: Add tmpfs SELFTEST**

In `kernel/fs/tmpfs.c`:

```c
#ifdef KERNEL_SELFTEST
#include <kernel/selftest.h>

SELFTEST(test_tmpfs_mounted)
{

    struct vfs_node *tmp = vfs_lookup("/tmp");
    if (!tmp) return -1;
    if (tmp->type != VFS_DIR) { vfs_node_put(tmp); return -1; }
    vfs_node_put(tmp);
    return 0;
}
#endif
```

Note: `vfs_lookup` must be available — it's in vfs.c, which is always linked.

- [ ] **Step 3: Add GPT CRC32 SELFTEST**

In `kernel/fs/gpt.c`:

```c
#ifdef KERNEL_SELFTEST
#include <kernel/selftest.h>

SELFTEST(test_gpt_crc32)
{
    // CRC32("123456789") = 0xCBF43926
    const char *test = "123456789";
    uint32_t crc = gpt_crc32((const uint8_t *)test, 9);
    if (crc != 0xCBF43926) return -1;
    return 0;
}
#endif
```

Note: `gpt_crc32` is `static` — move it to file scope (not static) or duplicate a test-only copy. Simpler: just keep it static and put the test IN the same file as `gpt_crc32` (already done — gpt.c). The SELFTEST code can see file-static functions.

- [ ] **Step 4: Build with KERNEL_SELFTEST=1**

```bash
make KERNEL_SELFTEST=1 kernel/kernel.bin 2>&1 | tail -5
```

Expected: Success. Selftest ran on boot.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/ext2.c kernel/fs/tmpfs.c kernel/fs/gpt.c
git commit -m "test: add SELFTEST for ext2 structs, tmpfs mount, GPT CRC32"
```

---

## Spec Coverage Check

| Spec Section | Covered By |
|---|---|
| §1.1-1.4 GPT partition scan | Tasks 1, 2, 5, 12 |
| §2.1-2.4 Devfs blkdev | Tasks 3, 4, 12 |
| §3.1-3.8 ext2 driver | Tasks 6, 7 |
| §4.1-4.5 tmpfs | Task 10 |
| §5.1-5.5 Build system | Tasks 13, 14 |
| §6 Boot sequence | Tasks 11, 12 |
| §7.1-7.4 VFS (already correct) | Verified in place |
| §7.5 Case sensitivity | Task 8 |
| §7.6 config.txt discard | Task 14 (removes mcopy line) |
| §8 File list | All tasks |
| §9.1 SELFTEST | Task 16 |
| §9.3 Integration | Task 15 |

## Review Fixes Summary (v2 → final)

| # | Issue | Fix |
|---|-------|-----|
| 1 | `spinlock_t` → `spinlock_T` | Corrected in Task 6, declared with note |
| 2 | `alloc_pages(0)` wrong API | Tasks 10: `alloc_4k_page()` → `Phy_To_Virt(phys)` |
| 3 | `free_pages(Virt_To_Phy(...), 0)` wrong | Tasks 10: `free_4k_page(uint64_t phys)` |
| 4 | `ASSERT` macro doesn't exist | Tasks 16: `if (!(x)) return -1;` |
| 5 | `ino uint32_t` truncates tmpfs pointers | Task 8: `uint32_t` → `uint64_t` in vfs_dirent_t |
| 6 | Task 12 (main.c) before ext2_vfs_ops extern | Merged: ext2_vfs_ops is extern from Task 6, used in Task 12; no separate task needed |
| 7 | Missing init path updates | Task 11: `/init.elf→/bin/init`, `/systest.elf→/bin/systest`, `/busybox.elf→/bin/busybox` |
| 8 | Partition naming "hda2" instead of "hda1" | Tasks 5: local `part_idx` counter (1-based), passed to `block_device_create_partition` |
| 9 | sizeof assertions wrong | Task 16: ext2_inode_t=116, ext2_bgdesc_t=32, ext2_dirent_t=8 |
| 10 | ext2_find_entry dead code | Removed — __vfs_lookup uses readdir to find entries |
| 11 | vfs_getdents sort not updated | Task 8 step 4: updated with per-fs flags |
| 12 | Missing backup GPT | Task 13: writes backup header + entries at disk end |
