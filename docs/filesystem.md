# OS01 Filesystem Stack

## Overview

OS01 implements a layered filesystem stack: a VFS layer dispatches to concrete filesystem drivers (FAT32, ext2, tmpfs, devfs, procfs), which sit atop a block device abstraction backed by AHCI. GPT partition tables are parsed to discover the partition layout.

---

## Block Device Layer (`kernel/block/blockdev.c`, `kernel/include/block/blockdev.h`)

`block_device_t` represents a disk or partition with sector-based read/write:

```c
typedef struct block_device {
    char     name[BLOCKDEV_NAME_MAX];   // "hda", "hda1", ...
    uint32_t port_num;                   // AHCI port number
    uint64_t sector_count;
    uint32_t sector_size;                // always 512
    int      present;
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    void *private_data;                  // partition_ctx_t* or NULL
} block_device_t;
```

- `block_device_register(name, port, sectors)` — register a raw AHCI disk with default read/write wrappers
- `block_device_register_raw(name, sectors, private_data)` — register without AHCI defaults; caller sets .read/.write
- `block_device_read/write` — bounds-checked sector I/O dispatching to the device's function pointers
- `block_device_get(i)` / `block_device_count()` — enumerate registered devices

A static table of up to `BLOCKDEV_MAX` (8) devices is maintained.

---

## VFS Layer (`kernel/fs/vfs.c`, `kernel/include/fs/vfs.h`)

### Key Structures

**`vfs_node_t`** — the in-memory inode:

| Field | Purpose |
|-------|---------|
| `name[256]` | File/directory name |
| `size` | File size in bytes |
| `type` | `VFS_FILE` / `VFS_DIR` / `VFS_CHRDEV` / `VFS_BLKDEV` |
| `mount` | Pointer to the `vfs_mount_t` this node belongs to |
| `fs_data` | Filesystem-private data (cluster number, inode number, or tmpfs_node_t pointer) |
| `parent` | Parent directory node |
| `ops` | Pointer to the `vfs_ops_t` function table |
| `refcount` | Reference count (see below) |

**`vfs_mount_t`** — a mounted filesystem instance:

```c
typedef struct vfs_mount {
    block_device_t *dev;
    const char     *path;       // mount point path (e.g. "/", "/boot", "/dev")
    struct vfs_node *root;      // root node
    struct vfs_ops  *ops;
    void           *fs_data;    // filesystem-private (e.g. fat32_fs_t*, tmpfs_node_t*)
} vfs_mount_t;
```

**`vfs_ops_t`** — the driver interface (struct of function pointers):

| Member | Purpose |
|--------|---------|
| `flags` | `VFS_OPS_CASE_INSENSITIVE` for FAT32 |
| `read` | Read from file at offset |
| `write` | Write to file at offset |
| `readdir` | Read one directory entry by index |
| `create` | Create a regular file |
| `unlink` | Delete a file |
| `mkdir` | Create a directory |
| `rmdir` | Remove an empty directory |
| `rename` | Rename across directories |
| `truncate` | Truncate a file to new size |

### Mount Table

A static array of up to `VFS_MOUNTPOINT_MAX` (8) entries. `vfs_mount(path, dev, ops, fs_data)` adds an entry, allocates a root `vfs_node_t` (type `VFS_DIR`, name `/`), and stores the filesystem's root in `mount->fs_data`. `find_mount()` locates the deepest matching mount point by prefix comparison.

### Path Resolution

`vfs_lookup(path)` resolves an absolute path:

1. If path is `/`, return the root mount's root node (refcount incremented)
2. `find_mount()` — find the deepest mount covering this path
3. Tokenize the remaining path component-by-component via `next_component()`
4. For each component, call `current->ops->readdir()` sequentially until the matching name is found
5. Allocate a new `vfs_node_t` child, store `entry.ino` in `node->fs_data`, and walk down

Supports `.` (no-op) and `..` (move to parent). Case-insensitive matching when `VFS_OPS_CASE_INSENSITIVE` is set (FAT32). `vfs_lookup_from(path, cwd)` adds relative path support.

### File Operations

`vfs_read/vfs_write/vfs_readdir` delegate to `node->ops->read/write/readdir`. `vfs_getdents` collects entries from the underlying filesystem, injects VFS mount points that are direct children of the directory, sorts case-insensitively, and streams to a `linux_dirent64` buffer. `vfs_stat` fills a `struct stat` from node fields.

### Refcounting

`vfs_node_get()` increments refcount; `vfs_node_put()` decrements and frees the node when it reaches zero. Lookup returns a node with refcount already incremented.

---

## FAT32 (`kernel/fs/fat.c`, `kernel/include/fs/fat.h`)

Read-write filesystem driver for FAT32 partitions. `fat32_init(dev, &fs)` reads the BIOS Parameter Block (BPB), validates the signature (`0xAA55`), and initializes a `fat32_fs_t` with FAT geometry. Exposes `fat_vfs_ops`:

- **Directory traversal**: Walks cluster chains via `fat32_next_cluster()`, converting cluster numbers to LBAs with `fat32_cluster_to_sector()`. Each cluster contains short (8.3) directory entries; long filename (LFN) entries are skipped.
- **Read/write**: Reads/writes sectors through the block device layer, supporting file offset → cluster → LBA mapping.
- **Flags**: Sets `VFS_OPS_CASE_INSENSITIVE` for case-insensitive name matching.
- **Mount point**: Mounted at `/boot` (ESP partition in GPT layout) or at `/` (fallback single-partition layout).

---

## ext2 (`kernel/fs/ext2.c`, `kernel/include/fs/ext2.h`)

Read-only filesystem driver for ext2. `ext2_init(dev, &fs)` reads the superblock from offset 1024, validates the ext2 magic (`0xEF53`), and builds block group descriptor metadata. Exposes `ext2_vfs_ops`:

- **Inode reading**: Computes block group from inode number, reads the inode table within that group, supports the `i_block[15]` block map (12 direct + single indirect; double/triple indirect not implemented).
- **Directory iteration**: Reads directory entries from inode blocks; each `ext2_dirent_t` is a variable-length record linked by `rec_len`.
- **Read-only**: No write/create/unlink/rename/truncate operations.
- **Mount point**: Mounted at `/` in the GPT dual-partition layout.

---

## tmpfs (`kernel/fs/tmpfs.c`, `kernel/include/fs/tmpfs.h`)

In-memory filesystem (~430 lines). `tmpfs_init()` allocates a root `tmpfs_node_t` and mounts at `/tmp`. All data lives in 4KB pages allocated via the PMM subpage pool:

- **Block chain**: Files store a singly-linked list of `tmpfs_block_ptr_t` entries, each pointing to a 4KB page. Pages are allocated on demand (`alloc_4k_page` + `Phy_To_Virt`).
- **Directories**: Store children in a dynamically-grown array (`kmalloc`/`memcpy`/`kfree`, no `realloc`).
- **Full VFS ops**: Supports read, write, create, unlink, mkdir, rmdir, rename, and truncate.
- **Case-sensitive** (`flags = 0`).

---

## devfs (`kernel/fs/devfs.c`, `kernel/include/fs/devfs.h`)

Virtual filesystem for device nodes, mounted at `/dev`. Maintains an internal table of up to `DEVFS_MAX_DEVICES` devices:

### Character Devices

| Device | Read | Write |
|--------|------|-------|
| `/dev/null` | Returns EOF (0 bytes) | Discards all data |
| `/dev/zero` | Returns zero-filled buffer | Discards all data |
| `/dev/serial` | Blocking read from COM1 | Writes to COM1 via `write_serial` |
| `/dev/tty` | TTY canonical mode input | TTY output (framebuffer + serial) |
| `/dev/fb` | — | Writes to framebuffer via `color_printk` |
| `/dev/random` | Pseudorandom `rdtsc()` data | Discards all data |
| `/dev/keyboard` | Keyboard input | — |

### Block Devices

Block devices in devfs (`/dev/hda`, `/dev/hda1`, etc.) dispatch reads/writes through the `block_device_t` layer at sector granularity, with read-modify-write for unaligned offsets.

### Registration

- `devfs_register_chrdev(name, private_data, read_fn, write_fn)` — register a character device
- `devfs_register_blkdev(name, block_device_t*)` — register a block device

All devfs nodes share a single `vfs_ops_t` vector; the device index is stored in `node->fs_data` and dispatched to the correct handler at runtime.

---

## procfs (`kernel/fs/procfs.c`, `kernel/include/fs/procfs.h`)

Virtual filesystem for process information, mounted at `/proc`. Content is generated on-the-fly per read call:

| Entry | Type | Content |
|-------|------|---------|
| `/proc/self/` | Directory | Symlink to current process directory |
| `/proc/self/status` | File | Process status for the reading task |
| `/proc/meminfo` | File | Memory statistics (total/free/used via PMM zones) |
| `/proc/<pid>/` | Directory | Per-process directory (user tasks only, skips kernel threads) |
| `/proc/<pid>/status` | File | Name, Pid, PPid, State, CPU, Priority |

`fs_data` encodes both type and PID via `PROCFS_ENCODE(type, pid)` macros. The task list is traversed locklessly via `find_task_by_pid()`.

---

## GPT Partition Table (`kernel/fs/gpt.c`, `kernel/include/fs/gpt.h`)

`gpt_scan(dev)` reads and validates a GPT header:

1. Reads LBA 1 (GPT header), checks `"EFI PART"` signature and revision `0x00010000`
2. Validates header CRC32 (computes over header with CRC field zeroed)
3. Reads the partition entry array at `entry_lba`, validates its CRC32
4. For each non-zero entry, creates a partition wrapper `block_device_t` (via `block_device_create_partition`) with offset-adjusted read/write that delegates to the parent device
5. Registers each partition block device in devfs (e.g. `/dev/hda1`, `/dev/hda2`)
6. Returns `gpt_info_t` with up to `GPT_PARTITION_MAX` (16) partitions

The dual-partition layout expects partition 0 = FAT32 ESP, partition 1 = ext2 root.

---

## Init Sequence (`kernel/kernel/main.c:170-218`)

1. `vfs_init()` — zero the mount table
2. `devfs_init()` — mount `/dev`, register built-in char devices (null, zero, random, serial, tty)
3. Register `keyboard` and `fb` char devices; register physical disks as block devices in `/dev`
4. `gpt_scan(block_device_get(0))` — try GPT on the first block device
5. **If GPT found**: mount ext2 partition at `/`, FAT32 at `/boot`
6. **If no GPT** (fallback): mount FAT32 at `/` directly
7. `tmpfs_init()` — mount `/tmp`
8. `procfs_init()` — mount `/proc`
