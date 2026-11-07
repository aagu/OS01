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

## FS Concurrency & Block Cache (ARCH-6, `kernel/block/bcache.c`, `kernel/include/block/bcache.h`)

### Per-mount locking (Phase 1)

- Each of ext2, FAT32 and tmpfs guards its whole VFS-op operation bodies with a
  **scheduler-backed per-mount `mutex_t`** (`kernel/sync/mutex.c`); tmpfs keeps
  its mutex plus root node in a per-mount `tmpfs_mount_ctx_t` (mount->fs_data).
  Boot-time mounting (pre-SMP, single core) skips the mutex via
  `vfs_sched_ready()` — the same `vfs_bootstrap` exemption `vfs_ns_lock` uses;
  host tests opt in with `vfs_test_set_sched_ready(true)`.
- Lock order: `FS mutex → mount->active_lock (node bookkeeping only) → bcache
  metadata lock (leaf)`. The bcache metadata lock never nests with the
  per-buffer data locks, the wait-queue lock, or the AHCI port lock; all I/O,
  sleeping and allocation happen outside cache spinlocks.

### Block cache (Phase 2)

- `block_device_read/write` on a **physical disk** (`dev->parent == NULL`) go
  through the sector-granular cache (2048 × 512 B slots, (dev, LBA) hash).
  **Partition devices never enter the cache**: their ops forward to the
  parent's wrapper, so a physical block has exactly one key.
- Write-back: writes update the cached sector and mark it `VALID|DIRTY`
  without touching the backend; reads are served from the cache
  (read-your-writes). Consistency contract: the cache is the single source of
  truth for resident blocks — **no module may bypass the wrapper** and issue
  raw `ops->read/write` against a published device (cache write-back itself
  excepted).
- Durability: `block_device_flush(dev)` snapshots the dirty (dev, LBA) keys,
  reserves each with `BUSY` (blocking new pins), drains in-flight pins, writes
  back outside all locks and clears `DIRTY` only on success; a failing
  write-back keeps key/data/DIRTY and returns the raw error. `SYS_sync`
  flushes every registered `BLOCK_DISK` (first error wins, remaining disks
  still flushed). This is backend visibility, **not a power-loss
  guarantee** (the AHCI flush hook is a no-op).
- Concurrency: per-buffer `refcnt` pins, an exclusive `BUSY` reservation for
  maintenance (fill / flush drain / evict), a per-buffer `data_lock`
  (irqsave) making single-sector copies atomic, and an ARM→recheck wait
  handshake on a global wait queue (a waiter that can progress reserves in
  the same critical section; a vanished key is "no future producer" and never
  schedules).
- Eviction: free list first, then the LRU tail that is unpinned and not BUSY;
  a dirty victim writes back under its old key's BUSY reservation before the
  slot is transformed, keeping key/data/DIRTY on failure. Pool exhaustion
  waits instead of failing.
- Device retirement (`block_device_unregister_boot`) and the test reset hook
  silently **drop** the retired device's entries (no write-back — the ops are
  gone).

### SMP acceptance

- `make KERNEL_SELFTEST=1 test-kernel-selftest` runs the late
  `bcache_smp_coherence` case (two pinned kthreads: same-sector atomicity,
  concurrent first miss, adjacent-sector isolation, flush behind a held pin,
  exhaustion wait, backend visibility, no leaks).
- `make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress` boots the
  four-CPU stress variant: four workers concurrently create / write / verify /
  rename / cross-read / delete files on ext2, tmpfs and vfat plus a shared
  directory race window, with fail-closed marker parsing.

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

### 子目录与多级路径（ARCH-11）

devfs 的注册名是**相对挂载点 `/dev`** 的路径，可以包含 `/` 分隔的多级分量：

```c
devfs_register_path("pci/0000:00:1f.2/vendor", priv, &ops);
```

注册后用户态可见 `/dev/pci/0000:00:1f.2/vendor`。中间目录
（`/dev/pci`、`/dev/pci/0000:00:1f.2`）**隐式创建**，并作为真实的目录出现在
`ls /dev` 与 `ls /dev/pci` 中。`devfs_register_chrdev()` /
`devfs_register_blkdev()` 是 `devfs_register_path()` /
`devfs_register_blkdev_path()` 的历史拼写，同样接受多级路径；单分量调用
（`devfs_register_chrdev("null", …)`）行为与之前完全一致。

路径规则：

| 规则 | 说明 |
|------|------|
| 相对路径 | 相对 `/dev`；前导 `/` 被拒绝（`-EINVAL`） |
| 分隔符 | `/`；空分量（`a//b`）与尾随 `/` 被拒绝 |
| 分量长度 | `< DEVFS_NAME_MAX`（32），超长不截断，直接 `-EINVAL` |
| 深度 | `<= DEVFS_MAX_DEPTH`（16） |
| 总量 | 绝对路径 `/dev/<path>` 必须 `< VFS_NAME_MAX`（256），否则 `-ENAMETOOLONG` |
| 遍历语义 | `.` 与 `..` 分量被拒绝（devfs 不做 `..` 解析） |

冲突语义：同路径重复注册返回 `-EEXIST`；中间分量已被设备占用返回 `-ENOTDIR`；
中间分量已是目录则复用该目录。注册失败时已建立的中间目录保留——它们不承载
任何外部可见状态。

内部结构：devfs 用一棵树保存所有节点，目录与设备同构（`devfs_node_t`）。
目录与设备共用同一个单调递增的 id 计数器，`ino` / `rdev` / `readdir` 的 `ino`
保持一致且非零。记录一经发布不释放（unmount 只释放 VFS 节点）。

VFS 侧的路径解析本身已支持多级分量（`__vfs_lookup_raw()` 逐分量调用
`i_ops->lookup()`），devfs 只需对中间分量返回 `VFS_DIR` 节点即可，**无需修改 VFS**。

### PCI 设备命名空间（ARCH-12）

内核在 `fs_boot_prepare()` 之后（devfs 已挂载、PCI 设备已在 subsys phase 6
枚举完毕）把每个已枚举的 PCI 设备发布成 `/dev/pci/<BDF>/<attr>`：

| 路径 | 内容 |
|------|------|
| `/dev/pci/0000:00:1f.2/vendor` | vendor id，文本 `"0x8086\n"` |
| `/dev/pci/0000:00:1f.2/device` | device id，文本 `"0x2922\n"` |
| `/dev/pci/0000:00:1f.2/class` | 24-bit class code，文本 `"0x010601\n"` |
| `/dev/pci/0000:00:1f.2/config` | 256 字节原始 config space（小端，逐字节原样） |
| `/dev/pci/0000:00:1f.2/resource` | BAR 列表，每行 `Region N: <kind> at 0x<addr>` |
| `/dev/pci/0000:00:1f.2/irq` | 路由后的 GSI，文本 `"16\n"`；取不到时 `"none\n"` |
| `/dev/pci/0000:00:1f.2/driver` | 绑定的驱动名；未绑定时 `"none\n"` |

- `<BDF>` 是 `domain:bus:slot.function`（如 `0000:00:1f.2`）。
- `resource` 的 kind 取值：`PCI_BAR_IO` → `I/O ports`；`PCI_BAR_MMIO32` /
  `PCI_BAR_MMIO64` → `Memory`；无效 BAR 与 64 位 BAR 的上半段不出现。
- `resource` **只报地址与类型，不报 BAR 容量**——BAR 的无损 sizing 不可得
  （ARCH-9 驱动模型契约：BAR 的 "length" 是驱动已知窗口而非 BAR 容量），
  编造 size 会给出误导性输出。
- `config` 的内容长度固定 256 字节；每次 read 都要跑 64 次
  `pci_config_read32()`（每次一对 CF8/CFC 端口 I/O），与 Linux sysfs 同款行为。

该命名空间由 `kernel/bus/pci/pci_devfs.c` 发布。**零新增 syscall、零 UAPI 变更**：
用户态用 `opendir` / `readdir` 枚举 `/dev/pci` 与 `/dev/pci/<BDF>/`，
再逐个读属性文件。对应的用户态命令是 `/bin/lspci`。

注册失败**不阻断启动**：单个属性注册失败只跳过该节点并 `log_warn` 记录；
重复注册（`-EEXIST`）视为幂等，不记日志。

### USB 设备命名空间（ARCH-14）

内核在 `fs_boot_prepare()` 之后（devfs 已挂载且 xHCI 控制器与设备枚举配置完毕）
调用 `usb_devfs_register_all()`，将所有已枚举配置的 USB 设备发布成 `/dev/usb/<bus>-<port>/<attr>`：

| 路径 | 内容 |
|------|------|
| `/dev/usb/1-1/bus` | 总线号，文本 `"1\n"` |
| `/dev/usb/1-1/devnum` | 设备号（分配的 slot_id），文本 `"1\n"` |
| `/dev/usb/1-1/port` | 根集线器端口号，文本 `"1\n"` |
| `/dev/usb/1-1/vendor` | Vendor ID，文本 `"0x0627\n"` |
| `/dev/usb/1-1/product` | Product ID，文本 `"0x0001\n"` |
| `/dev/usb/1-1/class` | 设备类别代码，文本 `"0x00\n"` |
| `/dev/usb/1-1/subclass` | 子类别代码，文本 `"0x00\n"` |
| `/dev/usb/1-1/protocol` | 协议代码，文本 `"0x00\n"` |
| `/dev/usb/1-1/speed` | 协商速度，文本 `"super\n"` / `"high\n"` / `"full\n"` / `"low\n"` |
| `/dev/usb/1-1/descriptors` | 原始二进制描述符切片（对齐 Linux `/sys/bus/usb/devices/.../descriptors`） |

- `<bus>-<port>` 表示 USB 拓扑路径（例如总线 1 端口 1 为 `1-1`）。
- `descriptors` 节点为原始二进制描述符流：先拼接 18 字节标准 Device Descriptor，紧跟完整 Configuration 描述符层次树（含 Configuration、Interface、HID、Endpoint 描述符等）。
- `descriptors` 的读操作由 `usb_attr_read()` 实现，支持任意 `offset` 与 `count` 的切片读取与 EOF 截断。
- 该命名空间由 `kernel/driver/usb_devfs.c` 驱动实现并接入 ARCH-11 多级 devfs 路径体系。**零新增 syscall、零 UAPI 变更**。
- 对应的用户态工具是 `/bin/lsusb`（`user/lsusb.c`）：
  - 默认模式：枚举 `/dev/usb/`，按 bus 和 port 确定性升序排序并输出单行摘要（例如 `Bus 001 Device 001: ID 0627:0001 QEMU USB Tablet`）。
  - `-s [[bus]:][devnum]`：过滤指定总线或设备。
  - `-v`：读取 `descriptors` 属性并反序列化打印完整的 USB 描述符树。
- 注册失败**不阻断启动**：usb_devfs 为诊断和设备信息服务，注册出错仅记录 `log_warn`，不影响系统正常引导。

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

## Init Sequence (`kernel/core/main.c:170-218`)

1. `vfs_init()` — zero the mount table
2. `devfs_init()` — mount `/dev`, register built-in char devices (null, zero, random, serial, tty)
3. Register `keyboard` and `fb` char devices; register physical disks as block devices in `/dev`
4. `gpt_scan(block_device_get(0))` — try GPT on the first block device
5. **If GPT found**: mount ext2 partition at `/`, FAT32 at `/boot`
6. **If no GPT** (fallback): mount FAT32 at `/` directly
7. `tmpfs_init()` — mount `/tmp`
8. `procfs_init()` — mount `/proc`
