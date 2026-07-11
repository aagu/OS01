# Ext2 + Tmpfs + GPT 分区文件系统 — 设计规格

**Date:** 2026-07-11
**Revision:** v3 (second code-review feedback)
**Scope:** ext2 只读驱动、tmpfs 内存文件系统、GPT 分区表、/dev 块设备、VFS 多挂载点、disk.img 双分区构建
**Status:** proposed

## 目标

将文件系统从单 FAT32（所有文件在根目录）升级为标准 Unix 布局：
- `/boot` → FAT32 ESP 分区 (hda1)，供 UEFI 读取 `BOOTX64.EFI` + `kernel.bin`
- `/` → ext2 分区 (hda2)，绑定 `/bin`, `/home`, `/etc` 等目录
- `/tmp` → tmpfs 内存文件系统
- `/dev` → devfs 注册块设备节点 (`/dev/hda`, `/dev/hda1`, `/dev/hda2`)

## 架构

```
disk.img (GPT)
├── Partition 1 (hda1): FAT32 ESP  →  VFS 挂载 /boot
└── Partition 2 (hda2): ext2       →  VFS 挂载 /

tmpfs (纯内存)                      →  VFS 挂载 /tmp

启动链:
  UEFI firmware → ESP (FAT32) → BOOTX64.EFI → 从 FAT32 读 kernel.bin
  → kernel 启动 → ahci_init() → block device "hda"
  → gpt_scan() 解析分区表 → hda1, hda2
  → vfs_mount("/boot", hda1, &fat_vfs_ops, ...)
  → vfs_mount("/",    hda2, &ext2_vfs_ops, ...)
  → vfs_mount("/tmp", NULL, &tmpfs_vfs_ops, ...)
  → spawn_user_task("/bin/init")
```

**磁盘命名**: AHCI 当前使用 `hda`/`hdb` (ahci.c:265)。保持此命名，物理磁盘 `/dev/hda`，分区 `/dev/hda1`、`/dev/hda2`。若需改为 Linux 风格 `sda`，将 ahci.c 中 `'h'` 改为 `'s'` 即可（一行改动），不影响方案其他部分。

### 设计原则

- **Bootloader 不碰 ext2**：`BOOTX64.EFI` 只用 FAT32 加载 `kernel.bin`，无需 ext2 代码
- **`BOOT_INFO` 不变**：内核自己解析 GPT，不依赖 bootloader 传递分区信息
- **分区是 block device wrapper**：`hda1` 是一个自定义 `block_device_t`，read/write 时对 LBA 加偏移，限制在分区范围内
- **VFS `find_mount` 已是最长前缀匹配**：vfs.c:101-121 已有 `best_len` 追踪 + 边界检查，`/boot` 和 `/` 两个挂载点无需额外修改。会由回归测试覆盖确认

---

## 1. GPT 分区解析

### 1.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/fs/gpt.c` | 新增 | GPT 扫描 + 分区注册 (~150 行) |
| `kernel/include/fs/gpt.h` | 新增 | 结构体 + 接口声明 |

### 1.2 数据结构

```c
#define GPT_PARTITION_MAX  16

typedef struct gpt_partition {
    char     name[40];          // UTF-16 → ASCII, 36 字符上限
    uint8_t  type_guid[16];     // 分区类型 GUID
    uint64_t start_lba;
    uint64_t end_lba;           // inclusive
    block_device_t *parent;     // 所属物理磁盘
    block_device_t *dev;        // 分区包装的 block device
} gpt_partition_t;

typedef struct gpt_info {
    gpt_partition_t partitions[GPT_PARTITION_MAX];
    int              count;
} gpt_info_t;
```

### 1.3 流程

```
gpt_scan(block_device_t *disk):
  1. 读 LBA 1 (GPT header, ≥ 512 bytes)
     验证签名 "EFI PART" (8 bytes)
     验证 revision (= 0x00010000)
  2. ★ 从 header 读取动态参数（不要硬编码 LBA 2 / 128 / 128!）:
     uint32_t header_size         = *((uint32_t*)(header + 12))  // 通常 92, ≥ 92
     uint64_t partition_entry_lba = *((uint64_t*)(header + 72)) // 通常 2
     uint32_t num_entries         = *((uint32_t*)(header + 80)) // 通常 128
     uint32_t entry_size          = *((uint32_t*)(header + 84)) // 通常 128, ≥ 128
     // 表损坏时这些值不可信, 需 sanity check:
     if (num_entries * entry_size > 1MB) return NULL;  // 上限保护
  3. 计算 header_crc32 (crc 字段置 0 → 计算 → 与 header[16..19] 比对)
  4. 读 Partition Entry Array:
     array_size = num_entries * entry_size
     array_sectors = (array_size + 511) / 512
     分配 buf = kmalloc(array_sectors * 512)
     block_device_read(disk, partition_entry_lba, array_sectors, buf)
  5. 计算+比对 partition_entries_crc32
  6. 遍历 entries (每条 entry_size 字节):
     - type_guid == zero → skip (empty entry)
     - 提取 starting_lba (entry[32..39]), ending_lba (entry[40..47]),
       name (UTF-16LE → ASCII, entry[56..127])
     - 调用 block_device_create_partition(parent, starting_lba, ending_lba)
     - 调用 devfs_register_blkdev(name, partition_dev)
  7. kfree(buf); 返回 gpt_info_t
```

失败处理：若 GPT 签名不匹配或 CRC 校验失败，返回 NULL，调用方 fallback 到旧单分区 FAT32 布局（见 §6）。

### 1.4 分区块设备（partition wrapper）

分区 block device **绝对不能复用 `block_device_register()`**。原因：blockdev.c:52-53 无条件执行：

```c
dev->read = default_ahci_read;   // ← 会覆盖自定义的 partition_read
dev->write = default_ahci_write; // ← 会覆盖自定义的 partition_write
```

即使先设 custom hook 再调 `block_device_register()`，也会被冲掉。且 `port_num` 参数对分区无意义。

新增独立注册路径 `block_device_register_raw()`：

```c
// partition_ctx_t — 分区 block device 的私有数据
typedef struct partition_ctx {
    block_device_t *parent;
    uint64_t        offset_lba;   // 分区起始 LBA
    uint64_t        length;       // 扇区数
} partition_ctx_t;

// 分区专用 read/write — 对 LBA 加偏移、检查越界、委托 parent
static int partition_read(block_device_t *dev, uint64_t lba,
                          uint32_t count, void *buf) {
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (lba + count > ctx->length) return -1;
    return ctx->parent->read(ctx->parent, ctx->offset_lba + lba, count, buf);
}
static int partition_write(block_device_t *dev, uint64_t lba,
                           uint32_t count, const void *buf) {
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (lba + count > ctx->length) return -1;
    return ctx->parent->write(ctx->parent, ctx->offset_lba + lba, count, buf);
}

// block_device_register_raw — 不绑 AHCI hook 的注册
// 新增于 blockdev.c, 直接写入 block_devices[]数组。
// 与 block_device_register() 的区别: 不设置 dev->read/dev->write,
// 不依赖 port_num — 调用方自行设置这些字段。
block_device_t *block_device_register_raw(const char *name,
                                           uint64_t sector_count,
                                           void *private_data)
{
    if (block_device_count_val >= BLOCKDEV_MAX) return NULL;
    block_device_t *dev = &block_devices[block_device_count_val];
    memset(dev, 0, sizeof(block_device_t));
    strcpy((char *)dev->name, name);
    dev->sector_count = sector_count;
    dev->sector_size  = 512;
    dev->present      = 1;
    dev->port_num     = 0;  // 分区无 AHCI port
    dev->private_data = private_data;
    block_device_count_val++;    // 递增后 block_device_count() 返回新计数
    return dev;
}

// block_device_create_partition — 组装分区 block device
// 流程: 分配 ctx → block_device_register_raw() → 手动设 read/write
block_device_t *block_device_create_partition(
    block_device_t *parent, uint64_t offset_lba, uint64_t length)
{
    partition_ctx_t *ctx = kmalloc(sizeof(partition_ctx_t));
    ctx->parent     = parent;
    ctx->offset_lba = offset_lba;
    ctx->length     = length;

    char name[16];
    snprintf(name, sizeof(name), "%s%d", parent->name,
             block_device_count() + 1);  // "hda1", "hda2" ...

    block_device_t *dev = block_device_register_raw(name, length, ctx);
    if (!dev) { kfree(ctx); return NULL; }

    // ★ 关键：在 register_raw 之后设置 hook，不会被覆盖
    dev->read  = partition_read;
    dev->write = partition_write;
    return dev;
}
```

需要在 `block_device_t` 中新增 `void *private_data` 字段。`block_devices[]` 当前是 blockdev.c 中的 `static`，`block_device_register_raw()` 作为 blockdev.c 中的新公开函数直接访问。

**ABI 注意**：`block_device_register()` 也需修改 — 在 `dev->write = default_ahci_write;` 后加 `dev->private_data = NULL;`。否则 hda 的 private_data 是未初始化垃圾值（`block_devices[]` 是 BSS/静态数组，不保证 mtime 局部清零）。`block_device_register_raw()` 内部有 `memset(dev, 0, ...)`，已覆盖。

**注册顺序保证**：物理盘 hda 通过 `block_device_register()` 先注册，分区 hda1/hda2 通过 `block_device_register_raw()` 后注册。`block_device_get(0)` 仍是 hda。BLOCKDEV_MAX=8，3 槽够用。**phys first, partitions second — 不要乱序**。

---

## 2. Devfs 块设备支持

### 2.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/fs/devfs.c` | 修改 | 新增加载 blkdev dispatch + `devfs_register_blkdev()` (~40 行增量) |
| `kernel/include/fs/devfs.h` | 修改 | 新增 `devfs_register_blkdev()` 声明 |

### 2.2 设计 — 避免 fs_data 语义冲突

**现状**: devfs 用 `node->fs_data` 存数组索引（devfs.c:123: `idx = (int)(uintptr_t)node->fs_data`），`devfs_readdir` 设 `entry->ino = i`（devfs.c:157），`__vfs_lookup` 把 `entry->ino` 写回 `child->fs_data`（vfs.c:198）。即 `fs_data` 语义是 **devices[] 整数索引**，不是指针。

**方案**: 保持 `fs_data` 为索引不变。把 `block_device_t *` 存入 `devices[idx].private_data`，在 `devfs_read/devfs_write` 中按 `devices[idx].type` 分支 dispatch：

```c
// devfs_device_t 新增字段
typedef struct devfs_device {
    char     name[DEVFS_NAME_MAX];
    uint8_t  type;       // VFS_CHRDEV or VFS_BLKDEV
    int (*read)(vfs_node_t *, uint64_t, uint64_t, void *);
    int (*write)(vfs_node_t *, uint64_t, uint64_t, void *);
    void    *private_data;  // chrdev: 自定义; blkdev: block_device_t *
    int      registered;
} devfs_device_t;

// devfs_read/dispatch 中新增 blkdev 分支
static int devfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer) {
    int idx = (int)(uintptr_t)node->fs_data;
    if (idx < 0 || idx >= DEVFS_MAX_DEVICES || !devices[idx].registered)
        return -1;

    if (devices[idx].type == VFS_BLKDEV) {
        // 扇区级读写：offset 必须按 sector_size 对齐
        block_device_t *bdev = (block_device_t *)devices[idx].private_data;
        uint32_t lba   = (uint32_t)(offset / 512);
        uint32_t count = (uint32_t)((size + 511) / 512);
        uint8_t *tmp = kmalloc(count * 512);  // 临时缓冲
        int ret = block_device_read(bdev, lba, count, tmp);
        if (ret == 0) memcpy(buffer, tmp + (offset % 512), size);
        kfree(tmp);
        return (ret == 0) ? (int)size : -1;
    }

    // chrdev path (existing)
    if (devices[idx].read)
        return devices[idx].read(node, offset, size, buffer);
    return -1;
}
// devfs_write 同理
```

### 2.3 接口

```c
int devfs_register_blkdev(const char *name, block_device_t *dev);
```

实现：同 `devfs_register_chrdev`，但 `type = VFS_BLKDEV`，`private_data = dev`，`read/write = NULL`（dispatch 走 blkdev 分支）。

`devfs_readdir` (devfs.c:155) 已有 `entry->type = devices[i].type`，所以 blkdev 节点的 d_type 自动正确 → `DT_BLK`。无需修改 readdir。

### 2.4 注册顺序 — ★ 关键：devfs_init 必须最先

`devfs_init()` (devfs.c:182) 做 `memset(devices, 0, sizeof(devices)); device_count = 0;`。**任何 devfs 注册必须在 devfs_init 之后**，否则注册数据会被清零。

正确顺序（详见 §6）：
```
ahci_init()                                    → block_device_register("hda", port, sectors)
                                                  （这只是 block 子系统注册，不碰 devfs）
devfs_init()                                   → ★ memset(devices, 0, …) + mount /dev + chrdev
// 以下所有 devfs_register_* 在 devfs_init 之后
devfs_register_blkdev("hda", block_device_get(0))  → /dev/hda
gpt_scan(block_device_get(0))                  → 内部:
    block_device_register_raw("hda1", …)           → block 子系统
    devfs_register_blkdev("hda1", …)               → /dev/hda1
    block_device_register_raw("hda2", …)
    devfs_register_blkdev("hda2", …)               → /dev/hda2
```

物理磁盘 `hda` 的 devfs 注册由 `main.c` 在 `ahci_init()` + `devfs_init()` 之后显式调用 `devfs_register_blkdev("hda", block_device_get(0))`。分区 hda1/hda2 的 devfs 注册由 `gpt_scan()` 内部完成。

---

## 3. Ext2 只读驱动

### 3.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/fs/ext2.c` | 新增 | ext2 只读驱动 (~350 行) |
| `kernel/include/fs/ext2.h` | 新增 | on-disk 结构体 + `ext2_init()` 声明 |

### 3.2 On-disk 结构

```c
// ── constants ─────────────────────────────────────────
#define EXT2_SB_OFFSET     1024    // superblock byte offset (bytes)
#define EXT2_SB_SIZE       1024    // superblock 总大小
#define EXT2_MAGIC         0xEF53
#define EXT2_INODE_SIZE_OLD 128   // ext2 默认 inode 大小
#define EXT2_ROOT_INO      2      // 根目录 inode

#define EXT2_S_IFREG       0x8000
#define EXT2_S_IFDIR       0x4000

// ── superblock (offset 1024, 1024 bytes) ──────────────
// 只读到 s_algo_bitmap；不映射 trailing padding。
// 读 1024 bytes 进栈缓冲，memcpy 前 264 bytes 到此 struct。
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
    // ext2 revision 1 额外字段 (offset 84-263):
    uint32_t s_first_ino;
    uint16_t s_inode_size;         // ★ 必须读取, 用于 inode 偏移计算
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // 后续 preseed block / journal UUID 等字段不使用。
    // 此 struct 映射 264 bytes; padding 到 1024 bytes 不在 struct 中。
} ext2_superblock_t;

// ── Block Group Descriptor (32 bytes) ─────────────────
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

// ── Inode (128 bytes, 或 s_inode_size) ─────────────────
// 结构体只映射前 128 字节(12 data block ptrs 之后)。
// 若 s_inode_size > 128, 在 read 时读取完整的 s_inode_size 字节到栈缓冲,
// 然后 memcpy 前 128 字节到此 struct。
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
    uint32_t i_blocks;      // 512B sector count
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];   // [0..11]=direct, [12]=single indirect, [13]=double, [14]=triple
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    // 若 s_inode_size > 128, 这之后还有 osd2 / extra 字段, 不影响读取。
} ext2_inode_t;

// ── Directory Entry (变长, rec_len 链接) ──────────────
// name 是 name_len 字节, 标准 ext2 不保证 NUL 结尾！
// 比较文件名时必须用 name_len, 不能用 strcmp。
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;       // 到下一个 entry 的偏移 (字节)
    uint8_t  name_len;      // name 的字节数
    uint8_t  file_type;     // 0=unknown, 1=reg, 2=dir
    char     name[];        // name_len bytes, 无 NUL 结尾
} ext2_dirent_t;
```

### 3.3 内存态结构

```c
typedef struct {
    block_device_t   *dev;
    uint32_t          block_size;         // 1024, 2048, 4096
    uint32_t          sectors_per_block;  // block_size / 512
    uint32_t          inodes_per_group;
    uint32_t          blocks_per_group;
    uint32_t          num_block_groups;
    uint32_t          inode_size;         // ★ 从 s_inode_size 读取
    ext2_bgdesc_t    *bgdesc_table;       // kmalloc 的完整 bgdesc 数组
    spinlock_t        lock;               // SMP 并发保护 (见 §3.7)
} ext2_fs_t;
```

**注意**：不保留全局 `block_buf`（见 §3.7 SMP 并发分析）。

### 3.4 核心函数

**ext2_read_block — 栈分配缓冲, 无共享状态**:

```c
static int ext2_read_block(ext2_fs_t *fs, uint32_t block, void *buf) {
    uint64_t lba = (uint64_t)block * fs->sectors_per_block;
    return block_device_read(fs->dev, lba, fs->sectors_per_block, buf);
}
```

**ext2_mount**:

```
ext2_init(block_device_t *dev, ext2_fs_t **out_fs):
  1. *out_fs = NULL
  2. ext2_fs_t *fs = calloc(1, sizeof(ext2_fs_t))
  3. spin_init(&fs->lock)

  4. 读 superblock 到栈缓冲:
     uint8_t sb_buf[1024];
     block_device_read(dev, 2, 2, sb_buf);  // sector 2+3 = 1024 bytes
     验证 sb_buf 中 magic == 0xEF53

  5. 计算关键参数:
     block_size = 1024 << sb.s_log_block_size
     if (block_size > 4096) { kfree(fs); return -1; }  // 栈缓冲 4096B 不够
     sectors_per_block = block_size / 512
     blocks_per_group = sb.s_blocks_per_group
     inodes_per_group = sb.s_inodes_per_group
     num_block_groups = (sb.s_blocks_count + blocks_per_group - 1) / blocks_per_group
     fs->inode_size = (sb.s_inode_size != 0) ? sb.s_inode_size : 128
        // mke2fs -I 128 → s_inode_size = 128; 默认 256 → 读 256 后只映射前 128B

  6. bgdesc 起始块 (★ 取决于 block_size):
     - block_size == 1024: SB 在 block 1 (byte 1024), bgdesc 在 block 2
     - block_size == 2048: SB 在 block 0 (byte 1024), bgdesc 在 block 1
     - block_size == 4096: SB 在 block 0 (byte 1024), bgdesc 在 block 1
     公式: sb_block    = (block_size == 1024) ? 1 : 0
          bgdesc_block = sb_block + 1

  7. 读 bgdesc table:
     table_size = num_block_groups * 32 (bgdesc = 32 bytes)
     table_blocks = (table_size + block_size - 1) / block_size
     fs->bgdesc_table = kmalloc(table_blocks * block_size)
     循环 for i in 0..table_blocks-1:
       ext2_read_block(fs, bgdesc_block + i, (uint8_t*)fs->bgdesc_table + i * block_size)

  8. *out_fs = fs; return 0
```

调用方（main.c）负责调用 `vfs_mount("/", dev, &ext2_vfs_ops, fs)`。与 `fat32_init` 风格一致：驱动返回 fs 指针，调用方挂载。

**ext2_read_inode**:

```
ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out):
  1. group = (ino - 1) / fs->inodes_per_group
  2. index = (ino - 1) % fs->inodes_per_group
  3. table_start = fs->bgdesc_table[group].bg_inode_table
  4. inode_size = fs->inode_size  ★ 使用 fs 中存储的值
  5. inodes_per_block = fs->block_size / inode_size
  6. block_off = index / inodes_per_block
  7. inode_off = (index % inodes_per_block) * inode_size
  8. // 栈分配或 kmalloc 临时缓冲 (inode 最大 256B, 栈用 256B 数组)
     uint8_t buf[256];  // 足够覆盖 256B inode
     ext2_read_block(fs, table_start + block_off, buf);
  9. memcpy(out, buf + inode_off, sizeof(ext2_inode_t));
     // 只拷贝 struct 映射的前 128 bytes
```

`buf` 是栈变量（最大 256B），不共享 → 无 SMP 竞争。

**ext2_bmap**:

```
ext2_bmap(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical_block):
  映射 logical block number → physical block number
  Direct blocks [0..11]: inode->i_block[logical_block]
  Single indirect [12]: 读 single-indirect block → 栈分配 uint32_t 数组 → 返回条目
  Double/triple indirect: 返回 0 (暂不实现, 见 §3.6)
```

**ext2_find_entry — 用 name_len 匹配, 不用 strcmp**:

```
ext2_find_entry(ext2_fs_t *fs, ext2_inode_t *dir_inode, const char *name):
  1. 获取 name_len = strlen(name)
  2. 遍历目录数据块（通过 ext2_bmap 获取 physical block）
  3. 对每个块:
     - 栈分配 uint8_t block_data[4096]  ★ 固定大小, block_size ≤ 4096; 不共享
     - ext2_read_block(fs, phys_block, block_data)
     - 遍历 ext2_dirent_t 链（rec_len 跳转）
     - 对每个 entry:
        * inode == 0 → 跳过(已删除)
        * dir_entry->name_len != name_len → 跳过
        * memcmp(dir_entry->name, name, name_len) == 0 → 返回 dir_entry->inode
  4. 返回 0 (not found)
```

**ext2_vfs_read / ext2_vfs_readdir**:

```
ext2_vfs_read(vfs_node_t *node, offset, size, buffer):
  1. uint32_t ino = (uint32_t)(uintptr_t)node->fs_data
  2. spin_lock(&fs->lock)  // 保护 block_device I/O 顺序（多核并发）
  3. ext2_inode_t inode; ext2_read_inode(fs, ino, &inode)
  4. 通过 ext2_bmap 映射逻辑块 → 物理块
  5. 栈分配 uint8_t block_buf[4096]; ext2_read_block(fs, phys_block, block_buf)
     ★ 固定 4096, block_size ≤ 4096; 不够则在 ext2_mount 里拒绝 block_size > 4096

ext2_vfs_readdir(vfs_node_t *node, index, entry):
  1. uint32_t ino = (uint32_t)(uintptr_t)node->fs_data
  2. spin_lock(&fs->lock)
  3. ext2_inode_t dir_inode; ext2_read_inode(fs, ino, &dir_inode)
  4. 遍历 ext2_dirent_t 链到 index 位置
  5. 填充 vfs_dirent_t (name, type, size, ino)
  6. spin_unlock(&fs->lock)
```

**node->fs_data 存储 inode number**：ext2 驱动将 `(void *)(uintptr_t)ino` 存入 `fs_data`（与 FAT32 存储 cluster number 的方式一致）。`__vfs_lookup` 生成 child node 时，`ino` 来自 `ext2_vfs_readdir` 的 `entry->ino`。

### 3.5 VFS Ops 表

```c
static vfs_ops_t ext2_vfs_ops = {
    .read     = ext2_vfs_read,
    .write    = NULL,      // 返回 -1（只读）
    .readdir  = ext2_vfs_readdir,
    .create   = NULL,
    .unlink   = NULL,
    .mkdir    = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .truncate = NULL,
};
```

写操作 NULL 指针已由 VFS 层检查：`vfs_write()` 中 `!node->ops->write` → `return -1`。

### 3.6 未实现

- **Double/Triple 间接块**：只实现 direct (12) + single indirect (1)。覆盖 ~(12 + block_size/4) 个块，对 `/bin` 下的程序足够
- **Symlink 解析**：后续任务
- **权限检查**：不检查
- **Journal replay**：ext2 无 journal

### 3.7 SMP 并发安全

当前内核默认 `-smp 2`，多核可能同时调用 ext2 VFS ops。风险：

- **block device I/O 非原子**：两个 CPU 同时做 `block_device_read` 共享同一个 AHCI port 的 DMA 缓冲区 → 数据错乱
- **ext2_fs_t 元数据读取**：`ext2_read_inode` 计算 bgdesc→block 后读出，若并发则无数据竞争（输出在调用方栈上），但 block device 层不一定是线程安全的

**方案**：`ext2_fs_t` 中加一个 `spinlock_t lock`。`ext2_vfs_read` / `ext2_vfs_readdir` 在开始读 block device 之前 `spin_lock(&fs->lock)`，完成后 `spin_unlock`。这序列化了所有 ext2 I/O。

**不在 ext2_fs_t 中保留共享的 `block_buf`**：每个函数在自己的栈上（`uint8_t buf[256]` 或 `uint8_t block_buf[4096]`）分配固定大小的临时缓冲。block_size 最大 4096，加上 inode buffer 256B，栈开销 ~4352B — 在内核栈（`STACK_SIZE` 通常 16KB）的安全范围内。

**已知不完整**（标注，不阻塞本次实现）：
- ext2 lock 只保护 ext2 自身的 I/O 序列化。若两个 CPU 分别读 hda1 (FAT) 和 hda2 (ext2) — 同一物理盘不同分区 — 它们竞争 AHCI port 的共享 DMA 缓冲区，lock 在此不生效。
- FAT32 / devfs blkdev 读同样裸奔。
- 正确的修复是将 lock 下沉到 `block_device_t`（或 AHCI port）层面，统一序列化对同一物理 disk 的所有 I/O。这是一个独立任务，等 ext2 + tmpfs 落地后再处理。

### 3.8 参考

Aquila ext2 驱动（221 行只读核心）。本实现扩展至 ~350 行因包含完整间接块映射、s_inode_size 动态读取、SMP 锁和 name_len 比较。

---

## 4. Tmpfs 内存文件系统

### 4.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/fs/tmpfs.c` | 新增 | tmpfs 实现 (~200 行) |
| `kernel/include/fs/tmpfs.h` | 新增 | 接口声明 |

### 4.2 数据结构

```c
// 数据块：使用 alloc_pages(0) 获取整页 (4KB)
// 以 Phy_To_Virt 转换后访问。元数据（next + blk_idx）用 kmalloc。
// 选择 alloc_pages 而非 kmalloc(>4096) 的原因：slab 分配器对超页大小对象效率低，
// alloc_pages 返回的 4KB 页与 tmpfs 逻辑块大小天然对齐。

typedef struct tmpfs_block {
    struct tmpfs_block *next;       // kmalloc(sizeof(tmpfs_block_ptr))
    uint64_t            blk_idx;
    uint8_t            *data;       // alloc_pages(0) → Phy_To_Virt → 4096 bytes
} tmpfs_block_t;

// 节点：文件或目录
#define TMPFS_CHILDREN_INIT_CAP  8

typedef struct tmpfs_node {
    char              name[VFS_NAME_MAX];
    uint8_t           type;           // VFS_FILE or VFS_DIR
    uint64_t          size;

    // 目录树：parent 指针供 .. 和 __vfs_lookup 的 node->parent 使用
    struct tmpfs_node *parent;

    // 文件：数据块链表
    tmpfs_block_t    *first_block;
    tmpfs_block_t    *last_block;     // 追加 O(1)

    // 目录：子节点动态数组
    struct tmpfs_node **children;
    int                child_count;
    int                child_cap;
} tmpfs_node_t;
```

### 4.3 核心函数

**VFS node ↔ tmpfs_node 绑定**：

tmpfs 把 `tmpfs_node_t *` 存为 `vfs_node_t->fs_data`（不使用 devfs 的索引模式）。所有 ops 从 `(tmpfs_node_t *)node->fs_data` 取回节点。

`__vfs_lookup` 创建子节点时设置 `child->parent`，所以 tmpfs 不负责 VFS 层的 parent 管理。但 tmpfs 内部的 `tmpfs_node_t *parent` 用于读取 `..` 的信息。

```
tmpfs_vfs_read(node, offset, size, buffer):
  tmpfs_node_t *tn = node->fs_data
  blk_idx = offset / 4096
  遍历块链表到 blk_idx → 从块内偏移 memcpy
  跨块时推进到下一个块，直到 size 读完或 size 边界

tmpfs_vfs_write(node, offset, size, buffer):
  tmpfs_node_t *tn = node->fs_data
  tmpfs_grow_to(tn, last_blk = (offset + size - 1) / 4096)
  同 read 逆操作，从 buffer 写回数据块

tmpfs_vfs_readdir(dir, index, entry):
  tmpfs_node_t *d = dir->fs_data
  . (index 0) → ino=(uintptr_t)d, type=DIR
  .. (index 1) → 若 d->parent 非 null, 填 ino=(uintptr_t)d->parent, type=DIR
                 否则填 ino=(uintptr_t)d, type=DIR (在根目录时 .. 就是 .)
  children[index-2] (index >= 2) → 子节点的 name/type/size, ino=(uintptr_t)子 tmpfs_node

tmpfs_vfs_create(dir, name):
  tmpfs_node_t *d = dir->fs_data
  分配新 tmpfs_node_t → 设置 new->parent = d ★
  d->children 数组扩容 → 追加到 children[child_count++]
  ★ 扩容必须用 kmalloc+memcpy+kfree, 绝对不能用 realloc
     (libc realloc 会读 kmalloc 返回内存的伪造 header → 垃圾数据)

tmpfs_vfs_mkdir(dir, name):  同 create, type = VFS_DIR, new->parent = d
tmpfs_vfs_unlink(dir, name):  在 children[] 中查找 → 释放节点+数据块 → 缩容(移动末尾元素)
tmpfs_vfs_rmdir(dir, name):   同 unlink, 检查子目录 child_count == 0
tmpfs_vfs_truncate(node, sz): 释放 new_size 之后的块, 调整 size
tmpfs_vfs_rename(olddir, oldname, newdir, newname):
  从旧父目录 children[] 移除 → 挂到新父目录 → 更新 node->parent = newdir
```

### 4.4 初始化

```c
void tmpfs_init(void) {
    tmpfs_node_t *root = calloc(1, sizeof(tmpfs_node_t));
    root->type = VFS_DIR;
    strcpy(root->name, "/");
    root->children = kmalloc(TMPFS_CHILDREN_INIT_CAP * sizeof(void *));
    root->child_cap = TMPFS_CHILDREN_INIT_CAP;

    vfs_mount("/tmp", NULL, &tmpfs_vfs_ops, root);
}
```

### 4.5 VFS Ops 表

```c
static vfs_ops_t tmpfs_vfs_ops = {
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

---

## 5. 构建系统

### 5.1 职责边界

`tools/mkdisk.c` 是一个宿主 C 程序，职责分两层：

- **自己实现的部分**（~200 行 C）：Protective MBR、GPT header、Partition Entry Array（含 CRC32）、磁盘布局计算、校验
- **调用宿主工具的部分**（`system()` 或 `fork+exec`）：`mkfs.vfat` + `mtools`（FAT32 分区）、`mke2fs` + `debugfs`（ext2 分区）

`tools/Makefile` 明确列出依赖检测，缺工具时输出友好报错：

```makefile
REQUIRED := mkfs.vfat mcopy mmd mke2fs debugfs
check-deps:
	@for cmd in $(REQUIRED); do \
	    command -v $$cmd >/dev/null 2>&1 || { echo "ERROR: missing $$cmd"; exit 1; }; \
	done
```

### 5.2 mkdisk 详细流程

```
tools/mkdisk disk.img --efi BOOTX64.EFI --kernel kernel.bin --rootfs config/fsroot/

参数:
  disk.img        输出文件
  --efi           BOOTX64.EFI 路径
  --kernel        kernel.bin 路径
  --rootfs        ext2 内容源目录 (config/fsroot/)

  FAT32_ESP_SIZE = 64 * 1024 * 1024   (64 MB)
  EXT2_SIZE      = 128 * 1024 * 1024  (128 MB)
  TOTAL_SIZE     = FAT32_ESP_SIZE + EXT2_SIZE
  SECTOR_SIZE    = 512
  ALIGN_LBA      = 2048               (standard GPT partition alignment)

流程:
  ── Phase 1: 磁盘布局 ──────────────────────────────────
  1. total_sectors = TOTAL_SIZE / 512
  2. Create disk.img (ftruncate 或 lseek+write 1 byte)
  3. Write Protective MBR (LBA 0):
     一个 DOS 分区条目: bootable=0, type=0xEE,
     start_lba=1, size_lba=0xFFFFFFFF (clamped)
  4. Compute partition boundaries:
     part1_start  = ALIGN_LBA           (2048)
     part1_sectors = FAT32_ESP_SIZE / 512
     part1_end    = part1_start + part1_sectors - 1
     part2_start  = ALIGN_UP(part1_end + 1, ALIGN_LBA)
     part2_sectors = EXT2_SIZE / 512
     part2_end    = part2_start + part2_sectors - 1

  ── Phase 2: GPT header + partition entries ─────────────
  5. Write GPT header (LBA 1):
     uint8_t hdr[92] 零初始化
     hdr[0..7]   = "EFI PART"
     hdr[8..11]  = 0x00010000           (revision 1.0)
     hdr[12..15] = 92                   (header_size)
     hdr[24..31] = 1                    (my_lba)
     hdr[32..39] = total_sectors - 1    (alternate_lba, = backup GPT header LBA)
     hdr[40..47] = 34                   (first_usable_lba)
     hdr[48..55] = total_sectors - 34   (last_usable_lba)
     hdr[72..79] = 2                    (partition_entry_lba)
     hdr[80..83] = 128                  (num_partition_entries)
     hdr[84..87] = 128                  (size_of_partition_entry)
     // hdr[16..19] = header_crc32 — 填 0 后计算 (见 CRC32 节)
     // hdr[88..91] = reserved (0)

  6. Build Partition Entry Array (LBA 2..33):
     128 entries × 128 bytes per entry = 16384 bytes (32 sectors)
     Fill Entry 1 (at offset 0×0):
       type_guid = C12A7328-F81F-11D2-BA4B-00A0C93EC93B  (EFI System Partition)
       unique_guid = random (read /dev/urandom 16 bytes)
       starting_lba = part1_start
       ending_lba   = part1_end
       attributes   = 0
       name: UTF-16LE "ESP" + \0 padding → 72 bytes
     Fill Entry 2 (at offset 0×80):
       type_guid = 0FC63DAF-8483-4772-8E79-3D69D8477DE4  (Linux filesystem)
       unique_guid = random
       starting_lba = part2_start
       ending_lba   = part2_end
       attributes   = 0
       name: UTF-16LE "rootfs" + \0 padding → 72 bytes
     Remaining 126 entries: zero-filled

  7. Compute partition_entries_crc32 over the 16384-byte array.
     Write to GPT header offset 88 (header->partition_entries_crc32).

  8. Compute header_crc32 (header_crc32 field = 0 during computation).
     Write to GPT header offset 16.

  9. Write GPT header + partition entry array to disk:
     lseek to LBA 1 → write 92-byte header
     lseek to LBA 2 → write 16384-byte partition entry array

  ── Phase 3: Fill FAT32 ESP partition ───────────────────
  10. Build esp.img via system():
      dd if=/dev/zero of=esp.img bs=1M count=64
      mkfs.vfat -F 32 esp.img
      mmd -i esp.img ::/EFI
      mmd -i esp.img ::/EFI/BOOT
      mcopy -i esp.img BOOTX64.EFI ::/EFI/BOOT
      mcopy -i esp.img kernel.bin ::/
  11. dd if=esp.img of=disk.img bs=512 seek=<part1_start_lba> conv=notrunc

  ── Phase 4: Fill ext2 partition ────────────────────────
  12. Build rootfs.img via system():
      dd if=/dev/zero of=rootfs.img bs=1M count=128
      mke2fs -t ext2 -I 128 -b 4096 rootfs.img
      debugfs -w rootfs.img:
        mkdir /bin
        mkdir /home
        mkdir /etc
        # 注意: 不创建 /boot /tmp /dev /proc！
        # 这些是 mount points, 由 VFS mount 注入提供可见性
        # (vfs_getdents phase 2 mount-point injection).
        # __vfs_lookup("/boot") 直接命中 /boot mount, 不需要 ext2 里存在该条目.
        # 创建它们反而会导致 getdents 重复 (挂载注入 + ext2 真实条目).
        for f in bin/*; do write $f /bin/$(basename $f); done
        # 如果有 etc/ 或 home/ 内容, 同样写入
  13. dd if=rootfs.img of=disk.img bs=512 seek=<part2_start_lba> conv=notrunc

  ── Phase 5: Cleanup ────────────────────────────────────
  14. rm esp.img rootfs.img

  ── Phase 6: Self-check ─────────────────────────────────
  15. Read back disk.img GPT header (LBA 1) → verify "EFI PART"
  16. Read partition entries → CRC32 verify
  17. Read ESP sector 0 → verify FAT magic bytes (0x55AA at offset 510)
  18. Read rootfs superblock → verify ext2 magic 0xEF53
```

### 5.3 CRC32 算法

GPT 使用标准反射 CRC32（与 zlib 一致）：

```
多项式: 0xEDB88320 (反射 0x04C11DB7)
初始值: 0xFFFFFFFF
输出值: XOR 0xFFFFFFFF
不附加 trailing bytes

// 计算 (同 zlib 的 crc32()):
uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}
```

两处 CRC 使用：
1. **header_crc32** (GPT header offset 16)：计算范围 = header 的全部 92 字节，其中 CRC 字段自身置 0
2. **partition_entries_crc32** (GPT header offset 88)：计算范围 = 全部 partition entry array（128×128=16384 bytes）

kernel 端 `gpt_scan()` 也需要同样的 CRC32 函数用于校验（~30 行复制），放到 `kernel/fs/gpt.c` 内部。

### 5.4 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `tools/mkdisk.c` | 新增 | disk 构建工具 (~250 行, 含 CRC + GPT + system() 编排) |
| `tools/Makefile` | 新增 | 宿主编译 + 依赖检测 |
| `Makefile` (root) | 修改 | `disk.img` target |
| `config/fsroot/` | 新增 | ext2 内容源目录（bin/、home/、etc/，不含 boot/tmp/dev/proc） |

### 5.5 Makefile 集成

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
注：旧的 `mcopy -i $@ config/config.txt ::/` 行移除 — config.txt 当前无代码读取，待 init inittab 实现后重新引入。

---

## 6. 启动顺序

`kernel_main()` 中的文件系统初始化调整为：

```c
// ═══ 1-5. CPU + Memory + APIC + Timers + Device IRQs (不变) ═══

// ═══ 6. Storage + filesystem ═══════════════════════════════
ahci_init();                    // PCI scan → AHCI enable → port init
                                //   → block_device_register("hda", port, sectors)
                                //   → block_device_register("hdb", port, sectors) ... (if present)

devfs_init();                   // /dev 挂载 + devices[] 零初始化 + chrdev 注册
                                // (null, zero, random, serial, tty)

// 注册物理磁盘到 /dev
for (int i = 0; i < block_device_count(); i++)
    devfs_register_blkdev(block_device_get(i)->name, block_device_get(i));
// → /dev/hda, /dev/hdb ... (在 devfs readdir 中可见)

// 解析 GPT → 分区 block device + devfs 注册
gpt_info_t *gpt = gpt_scan(block_device_get(0));  // scan hda
if (!gpt) {
    // Fallback: 旧单分区 FAT32 布局 (/ 挂载 hda)
    debug_block("GPT: scan failed, falling back to single-FAT layout\n");
    vfs_init();
    fat32_init(block_device_get(0), &fs);
    if (fs) vfs_mount("/", block_device_get(0), &fat_vfs_ops, fs);
} else {
    // 新双分区布局
    vfs_init();

    // / → ext2 (hda2 = gpt->partitions[1].dev)
    ext2_fs_t *ext2_fs;
    if (0 == ext2_init(gpt->partitions[1].dev, &ext2_fs))
        vfs_mount("/", gpt->partitions[1].dev, &ext2_vfs_ops, ext2_fs);
    else
        serial_printk("EXT2: mount failed — / not available\n");

    // /boot → FAT32 ESP (hda1 = gpt->partitions[0].dev)
    fat32_fs_t *fat_fs;
    if (0 == fat32_init(gpt->partitions[0].dev, &fat_fs))
        vfs_mount("/boot", gpt->partitions[0].dev, &fat_vfs_ops, fat_fs);
    else
        serial_printk("FAT32: /boot mount failed\n");
}

// /tmp → tmpfs (不依赖磁盘分区)
tmpfs_init();                   // vfs_mount("/tmp", NULL, &tmpfs_vfs_ops, root)

// ═══ then: procfs_init(), TTY, SMP, scheduler, spawn_user_task ═══
```

**关键改动**：
1. `ahci_init()` 在 `devfs_init()` 之前 — 保证 block device 已注册，devfs 的 devices[] 数组可在注册 block device 到 /dev 时使用
2. `devfs_init()` 在 `gpt_scan()` 之前 — 保证 `devices[]` 数组零初始化，分区注册 blkdev 可用
3. GPT scan 失败 → fallback 到旧单 FAT32 布局 — 避免内核因找不到分区而静默空转
4. `fat32_init` 签名变更：`int fat32_init(block_device_t *dev, fat32_fs_t **out_fs)` — 不再内部调用 `vfs_mount`，由调用方决定挂载点。需同步修改 `fat.c` 中的旧 `fat32_mount` 实现和 `fat.h` 声明
5. 磁盘命名：物理磁盘 "hda"（AHCI 现有命名），分区 "hda1"/"hda2"

---

## 7. VFS 已有保证（无需修改）

### 7.1 `find_mount` — 已实现最长前缀匹配 ✓

`kernel/fs/vfs.c:101-121` 的 `find_mount` 当前即为最长前缀实现：

```c
static vfs_mount_t *find_mount(const char *path)
{
    vfs_mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < mount_count; i++) {
        size_t len = strlen(mount_table[i].path);
        if (strncmp(path, mount_table[i].path, len) == 0) {
            if (len == 1 && mount_table[i].path[0] == '/') {
                if (len > best_len) { best_len = len; best = &mount_table[i]; }
            } else if (path[len] == '\0' || path[len] == '/') {
                if (len > best_len) { best_len = len; best = &mount_table[i]; }
            }
        }
    }
    return best;
}
```

- `/boot/kernel.bin` → 匹配 `/boot`（len=5）和 `/`（len=1），选最长 → `/boot` ✓
- `/bin/init` → 只匹配 `/` → `/` ✓

### 7.2 `__vfs_lookup` 跳过 mount 前缀 — 已实现 ✓

`vfs.c:147-153` 中，当 mount point 非 `/` 时（如 `/boot`），`ptr = path_copy + mp_len` 跳过前缀，然后 `while (*ptr == '/') ptr++` 跳过前导斜杠。剩余路径（如 `kernel.bin`）在 mount root node 的 ops->readdir 中正常查找。

### 7.3 getdents 挂载点注入 + 去重

`vfs.c:411-432`（Phase 2 mount-point injection）在目录 ≠ mount root 时不触发注入。当前逻辑：`dir->mount && dir == dir->mount->root` 条件保证只有 mount root 节点会注入子挂载。

去重问题：方案 §5.2 step 12 明确**不在 ext2 根目录创建 boot/tmp/dev/proc 等挂载点目录**。因此：
- `/`（ext2 root）包含 bin、home、etc → readdir 返回这些真实目录
- `/boot`、`/tmp`、`/dev`、`/proc` 只通过 mount injection 出现在 `/` 的 getdents 中
- ext2 中无同名目录 → 无重复

`VFS_GETDENTS_SORT_MAX = 64`：ext2 顶层目录条目（~5-10 个）+ mount 注入（~4 个）<< 64，不触发截断。

### 7.4 `fat32_init` 签名变更

`fat.h:99` 的声明和 `fat.c` 的实现改为：
```c
int fat32_init(block_device_t *dev, fat32_fs_t **out_fs);
```
不再内部调用 `vfs_mount`，调用方（main.c）在返回后按需挂载到任意路径。

### 7.5 Per-fs 大小写敏感性 — ★ 需要 VFS 改动

**问题**：当前 `vfs_name_cmp`（vfs.c:15）是全局大小写不敏感的（为 FAT32 设计）。ext2 和 tmpfs 是 Unix 文件系统，要求大小写敏感。

`vfs_name_cmp` 被两处调用：
- `__vfs_lookup` (vfs.c:174): `vfs_name_cmp(entry.name, comp)` — 路径查找的名字匹配 ← **需要按 flags 分支**
- `vfs_getdents` (vfs.c:371): `vfs_name_cmp(a->name, b->name)` — 排序用 ← **保持现状**（排序用大小写敏感/不敏感只影响显示顺序，不影响正确性；统一用 case-insensitive 避免改动扩大化）

**方案**：给 `vfs_ops_t` 新增一个 flags 字段，只在 `__vfs_lookup` 路径使用：

```c
typedef struct vfs_ops {
    uint32_t flags;
    #define VFS_OPS_CASE_INSENSITIVE  (1 << 0)  // FAT32
    int (*read)(...);
    int (*write)(...);
    int (*readdir)(...);
    ...
} vfs_ops_t;
```

`__vfs_lookup` 和 `vfs_getdents` 中根据 `dir->ops->flags & VFS_OPS_CASE_INSENSITIVE` 选择比较函数：

```c
// In __vfs_lookup:
int match = (current->ops->flags & VFS_OPS_CASE_INSENSITIVE)
    ? (vfs_name_cmp(entry.name, comp) == 0)          // FAT32: case-insensitive
    : (strcmp(entry.name, comp) == 0);               // ext2/tmpfs: case-sensitive
```

各 fs 的 ops 定义：
```c
static vfs_ops_t fat_vfs_ops  = { .flags = VFS_OPS_CASE_INSENSITIVE, ... };
static vfs_ops_t ext2_vfs_ops = { .flags = 0, ... };  // case-sensitive
static vfs_ops_t tmpfs_vfs_ops = { .flags = 0, ... };  // case-sensitive
// devfs, procfs 同样 flags=0
```

此改动影响 VFS 核心（vfs.c 的 `__vfs_lookup` 和 `vfs_getdents`），~10 行修改。

### 7.6 `config.txt` 处理 — 本次直接丢弃

验证 `user/init.c:308-313`：`parse_inittab()` 是空实现，当前 init 使用 hardcoded fallback（`/systest.elf` / `/busybox.elf sh`）。`config/config.txt` 目前没有任何代码读取。本次不引入 `/etc/config.txt` — 待 init 实现真正的 inittab 解析时再补。

原 Makefile 中的 `mcopy -i $@ config/config.txt ::/` 行移除。§5.5 的 Makefile 示例无需包含 config.txt 的 cp。

---

## 8. 文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `kernel/fs/ext2.c` | 新增 | ext2 只读驱动 (~350 行) |
| `kernel/include/fs/ext2.h` | 新增 | ext2 on-disk 结构体 + `ext2_init()` |
| `kernel/fs/tmpfs.c` | 新增 | tmpfs (~200 行) |
| `kernel/include/fs/tmpfs.h` | 新增 | `tmpfs_init()` 声明 |
| `kernel/fs/gpt.c` | 新增 | GPT 扫描 + CRC32 (~180 行) |
| `kernel/include/fs/gpt.h` | 新增 | partition_t + `gpt_scan()` 声明 |
| `kernel/block/blockdev.c` | 修改 | 新增 `block_device_register_raw()` + `private_data` 字段 (~30 行) |
| `kernel/include/block/blockdev.h` | 修改 | 新增 `private_data` + `block_device_register_raw()` 声明 |
| `kernel/fs/devfs.c` | 修改 | `devfs_device_t` 新增 `private_data` + blkdev dispatch (~50 行) |
| `kernel/include/fs/devfs.h` | 修改 | 新增 `devfs_register_blkdev()` |
| `kernel/fs/vfs.c` | 修改 | `__vfs_lookup` + `vfs_getdents` 使用 ops->flags 选择大小写比较 (~10 行) |
| `kernel/include/fs/vfs.h` | 修改 | `vfs_ops_t` 新增 `uint32_t flags` 字段 + `VFS_OPS_CASE_INSENSITIVE` |
| `kernel/fs/fat.c` | 修改 | `fat32_mount` → `fat32_init`，移除内部 vfs_mount (~5 行)；ops 设 flags |
| `kernel/include/fs/fat.h` | 修改 | 更新 fat32_init 签名 |
| `kernel/kernel/main.c` | 修改 | 新挂载顺序 (~20 行增量) |
| `kernel/Makefile` | 修改 | 新增 .c 文件 |
| `tools/mkdisk.c` | 新增 | disk 构建工具 (~250 行) |
| `tools/Makefile` | 新增 | 宿主工具编译 + 依赖检测 |
| `config/fsroot/bin/` | 新增 | ext2 /bin 内容源 |
| `config/fsroot/home/` | 新增 | ext2 /home (空目录) |
| `config/fsroot/etc/` | 新增 | ext2 /etc (config.txt 等) |
| `Makefile` (root) | 修改 | disk.img target |

**总计**：~950 行新代码 + ~120 行修改。

---

## 9. 测试策略

### 9.1 内核自测 (SELFTEST)

- **ext2 superblock 验证**：读 hda2 的 sector 1024/512，验证 magic=0xEF53
- **ext2 root inode**：inode 2 的 i_mode has S_IFDIR
- **ext2 s_inode_size 读取**：验证从 superblock 偏移 88 读到的值 = 128 (mke2fs -I 128)
- **ext2 readdir root**：/ 下有 bin、home、etc（不含 boot/tmp/dev/proc）
- **ext2 file read**：读取 `/bin/init` 前 4 字节 → ELF magic `\x7fELF`
- **ext2 dirent name_len 匹配**：用 name_len 比较文件名，验证正确（创建一个带有非 NUL 后缀的测试文件）
- **tmpfs create+read+write**：挂载后创建文件 → 写入 → 读回
- **GPT scan**：扫描 hda → partition count = 2, 签名 "EFI PART"
- **GPT CRC32 校验**：计算 partition entry array CRC → 验证与 header 一致
- **分区 block device 转发**：通过 hda2 读 sector 0 → 与通过 hda 在 offset_lba 处读出的内容对比一致
- **分区越界检查**：对 hda2 读超过 length 的 LBA → 返回 -1

### 9.2 现有测试

`make test` 中 `test_vfs_basic.c`（in-memory fake filesystem）不受影响，保持通过。

### 9.3 集成验证

```bash
make run   # QEMU 启动
# 期望输出:
#   "AHCI: port 0: MODEL=... SECTORS=..."
#   "block: registered hda (...)"
#   "devfs: mounted at /dev"
#   "gpt: found 2 partitions"
#   "devfs: registered blkdev hda"
#   "devfs: registered blkdev hda1"
#   "devfs: registered blkdev hda2"
#   "EXT2: inode_size=128 block_size=4096 ..."
#   "VFS: mounted '/'"
#   "VFS: mounted '/boot'"
#   "VFS: mounted '/tmp'"
#   → init.elf 成功启动
```
