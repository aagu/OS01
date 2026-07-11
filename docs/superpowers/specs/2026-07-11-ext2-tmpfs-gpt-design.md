# Ext2 + Tmpfs + GPT 分区文件系统 — 设计规格

**Date:** 2026-07-11
**Scope:** ext2 只读驱动、tmpfs 内存文件系统、GPT 分区表、/dev 块设备、VFS 多挂载点、disk.img 双分区构建
**Status:** proposed

## 目标

将文件系统从单 FAT32（所有文件在根目录）升级为标准 Unix 布局：
- `/boot` → FAT32 ESP 分区 (sda1)，供 UEFI 读取 `BOOTX64.EFI` + `kernel.bin`
- `/` → ext2 分区 (sda2)，绑定 `/bin`, `/home`, `/etc` 等目录
- `/tmp` → tmpfs 内存文件系统
- `/dev` → devfs 注册块设备节点 (`/dev/sda`, `/dev/sda1`, `/dev/sda2`)

## 架构

```
disk.img (GPT)
├── Partition 1 (sda1): FAT32 ESP  →  VFS 挂载 /boot
└── Partition 2 (sda2): ext2       →  VFS 挂载 /

tmpfs (纯内存)                     →  VFS 挂载 /tmp

启动链:
  UEFI firmware → ESP (FAT32) → BOOTX64.EFI → 从 FAT32 读 kernel.bin
  → kernel 启动 → AHCI → block_device_t sda
  → gpt_scan() 解析分区表 → sda1, sda2
  → vfs_mount("/boot", sda1, &fat_vfs_ops, ...)
  → vfs_mount("/",    sda2, &ext2_vfs_ops, ...)
  → vfs_mount("/tmp", NULL, &tmpfs_vfs_ops, ...)
  → spawn_user_task("/bin/init")
```

### 设计原则

- **Bootloader 不碰 ext2**：`BOOTX64.EFI` 只用 FAT32 加载 `kernel.bin`，无需 ext2 代码
- **`BOOT_INFO` 不变**：内核自己解析 GPT，不依赖 bootloader 传递分区信息
- **分区是 block device wrapper**：`sda1` 是一个 `block_device_t`，read/write 时对 LBA 加偏移，限制在分区范围内
- **VFS 多挂载已支持**：`mount_table[8]` + `find_mount` 最长前缀匹配 — 无需改动 VFS 核心

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
  1. 读 LBA 1 (GPT header)
     验证签名 "EFI PART" (8 bytes)
     验证 header_size, revision
  2. 读 Partition Entry Array (LBA 2, 通常 128 条 × 128B)
  3. 遍历 entries:
     - type_guid == zero → skip (empty entry)
     - 提取 start_lba, end_lba, name (UTF-16 → ASCII)
     - 调用 block_device_create_partition(parent, start, end)
       创建分区 wrapper
     - 调用 devfs_register_blkdev(name, partition_dev)
       在 /dev 下注册节点
  4. 返回 gpt_info_t
```

### 1.4 分区块设备

分区是 block_device_t 的轻量 wrapper，无需新结构体：

```c
typedef struct partition_ctx {
    block_device_t *parent;
    uint64_t        offset_lba;   // 分区起始 LBA
    uint64_t        length;       // 扇区数
} partition_ctx_t;
```

`block_device_t` 的 `.read/.write` 函数指针指向分区实现的 read/write，内部对 LBA 加 `offset_lba`，并检查越界，然后委托给 `parent->read/parent->write`。

---

## 2. Devfs 块设备支持

### 2.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/fs/devfs.c` | 修改 | 新增 `devfs_register_blkdev()` (~30 行增量) |

### 2.2 接口

```c
// 注册块设备节点到 /dev/<name>
int devfs_register_blkdev(const char *name, block_device_t *dev);
```

与 `devfs_register_chrdev` 镜像，区别：`type = VFS_BLKDEV`，`fs_data` 指向 `block_device_t *`。`devfs_read/devfs_write` 对 `VFS_BLKDEV` 节点直接走 `block_device_read/block_device_write` 进行扇区级读写。

### 2.3 注册顺序

`kernel_main()` 中：
```
block_device_init()       → sda（AHCI port 0 注册为 block device + /dev/sda）
devfs_init()              → /dev 挂载 + 字符设备注册（null, zero, random, serial, tty）
                          → devices[] 数组零初始化完成
gpt_scan(sda)             → 内部调用 devfs_register_blkdev("sda1", ...),
                            devfs_register_blkdev("sda2", ...)
```

注意：`block_device_init()` 不仅要注册 `sda` 到 block device 子系统，还要调用 `devfs_register_blkdev("sda", dev)` 将其注册到 `/dev/sda`。这确保了 `gpt_scan()` 调用 `devfs_register_blkdev` 时 `devices[]` 已经是零初始化状态。

---

## 3. Ext2 只读驱动

### 3.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/fs/ext2.c` | 新增 | ext2 只读驱动 (~350 行) |
| `kernel/include/fs/ext2.h` | 新增 | on-disk 结构体 + `ext2_mount()` 声明 |

### 3.2 On-disk 结构

```c
// superblock：offset 1024 (sector 2 byte 0), 共 1024 bytes
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;     // reserved blocks
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;   // 0 = block_size >= 1024, 1 = block_size == 1024
    uint32_t s_log_block_size;     // block_size = 1024 << this
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              // 0xEF53
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // ext2 revision 1 额外字段 (从 offset 84 开始, 只读到 s_rev_level==1 时使用):
    uint32_t s_first_ino;          // 第一个非保留 inode (通常是 11)
    uint16_t s_inode_size;         // inode 结构大小 (通常 128)
    uint16_t s_block_group_nr;     // 此 superblock 副本所在的 block group
    uint32_t s_feature_compat;     // 兼容特性位图
    uint32_t s_feature_incompat;   // 不兼容特性位图
    uint32_t s_feature_ro_compat;  // 只读兼容特性位图
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // (后续字段 + padding 到 1024 bytes)
    uint8_t  _pad[0];              // struct 用 sizeof 验证总大小 = 1024
} ext2_superblock_t;

// Block Group Descriptor (32 bytes)
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

// Inode (128 bytes)
typedef struct __attribute__((packed)) {
    uint16_t i_mode;        // S_IFREG=0x8000, S_IFDIR=0x4000, perms
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
    uint32_t i_osd1;        // OS-specific
    uint32_t i_block[15];   // [0..11]=direct, [12]=single indirect, [13]=double, [14]=triple
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;       // fragment address (不使用)
    // ... osd2 等填充到 128 字节
} ext2_inode_t;

// Directory Entry (变长, rec_len 链表)
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;       // 到下一个 entry 的偏移
    uint8_t  name_len;
    uint8_t  file_type;     // 0=unknown, 1=reg, 2=dir
    char     name[];        // name_len bytes, 后跟 '\0' (非标准, 但常见)
} ext2_dirent_t;
```

### 3.3 内存态结构

```c
typedef struct {
    block_device_t   *dev;
    uint32_t          block_size;
    uint32_t          sectors_per_block;
    uint32_t          inodes_per_group;
    uint32_t          blocks_per_group;
    uint32_t          num_block_groups;
    ext2_bgdesc_t    *bgdesc_table;    // kmalloc 的 bgdesc 数组
    uint8_t          *block_buf;       // block_size 字节临时缓冲
} ext2_fs_t;
```

### 3.4 核心函数

```
ext2_mount(block_device_t *dev):
  1. ext2_fs_t *fs = calloc(1, sizeof(ext2_fs_t))
  2. 读 superblock (sector 2, 1024 bytes)
  3. 验证 s_magic == 0xEF53
  4. block_size = 1024 << sb.s_log_block_size
  5. sectors_per_block = block_size / 512
  6. blocks_per_group = sb.s_blocks_per_group
  7. inodes_per_group = sb.s_inodes_per_group
  8. num_block_groups = (sb.s_blocks_count + blocks_per_group - 1) / blocks_per_group
  9. 读 bgdesc table (紧接 superblock 的 block)
  10. fs->block_buf = kmalloc(block_size)
  11. vfs_mount("/", dev, &ext2_vfs_ops, fs)
```

```
ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out):
  1. group = (ino - 1) / inodes_per_group
  2. index = (ino - 1) % inodes_per_group
  3. table_start = bgdesc_table[group].bg_inode_table
  4. inodes_per_block = block_size / 128  (inode_size = 128)
  5. block_off = index / inodes_per_block
  6. inode_off = (index % inodes_per_block) * 128
  7. ext2_read_block(fs, table_start + block_off, fs->block_buf)
  8. memcpy(out, fs->block_buf + inode_off, 128)
```

```
ext2_bmap(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t logical_block):
  映射 logical block number → physical block number
  实现 direct blocks [0..11] + single indirect [12]
  double/triple indirect 返回 0 (暂不实现)
```

```
ext2_find_entry(ext2_fs_t *fs, ext2_inode_t *dir_inode, const char *name):
  遍历目录数据块（通过 ext2_bmap 获取 physical block）
  逐条 ext2_dirent_t 链（rec_len 跳转）
  匹配 name → 返回 inode number
```

```
ext2_vfs_read(vfs_node_t *node, offset, size, buffer):
  1. 从 node->fs_data 获取 inode number
  2. ext2_read_inode() 获取 inode
  3. 通过 ext2_bmap 映射逻辑块 → 物理块
  4. ext2_read_block → memcpy 到 buffer

ext2_vfs_readdir(vfs_node_t *node, index, entry):
  1. 从 node->fs_data 获取 inode number
  2. ext2_read_inode() 获取目录 inode
  3. 遍历 ext2_dirent_t 链到 index 位置
  4. 填充 vfs_dirent_t (name, type, size, ino)
```

### 3.5 VFS Ops 表

```c
static vfs_ops_t ext2_vfs_ops = {
    .read     = ext2_vfs_read,
    .write    = NULL,      // 返回 -1（只读）
    .readdir  = ext2_vfs_readdir,
    .create   = NULL,      // 返回 NULL（只读）
    .unlink   = NULL,      // 返回 -errno（只读）
    .mkdir    = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .truncate = NULL,
};
```

写操作 NULL 指针由 VFS 层检查：`vfs_write()` 中 `node->ops->write == NULL` 返回 -1。对 ext2 上文件的 write syscall 会返回 -EROFS（只读文件系统），无需在 ext2 内部处理。

### 3.6 未实现

- **Double/Triple 间接块**：只实现 direct (12) + single indirect (1)。12 个 direct 块 + (block_size/4) 个 single-indirect 条目，覆盖 ~4MB + (block_size/4)*block_size 范围，对 `/bin` 下的程序完全足够
- **Symlink 解析**：ext2 目录支持，VFS 不做 symlink 追踪（后续任务）
- **权限检查**：不检查 uid/gid/permission bits
- **Journal replay**：ext2 本身无 journal，不涉及

### 3.7 参考

Aquila ext2 驱动（221 行只读核心），本实现扩展至 350 行是因为包含完整的间接块映射和 readdir 处理。

---

## 4. Tmpfs 内存文件系统

### 4.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `kernel/fs/tmpfs.c` | 新增 | tmpfs 实现 (~200 行) |
| `kernel/include/fs/tmpfs.h` | 新增 | 接口声明 |

### 4.2 数据结构

```c
// 数据块：4KB 固定，链表连接
typedef struct tmpfs_block {
    struct tmpfs_block *next;
    uint64_t            blk_idx;
    uint8_t             data[4096];
} tmpfs_block_t;

// 节点：文件或目录
#define TMPFS_CHILDREN_INIT_CAP  8

typedef struct tmpfs_node {
    char              name[VFS_NAME_MAX];
    uint8_t           type;           // VFS_FILE or VFS_DIR
    uint64_t          size;

    // 文件：数据块链表
    tmpfs_block_t    *first_block;
    tmpfs_block_t    *last_block;     // 追加优化

    // 目录：子节点动态数组
    struct tmpfs_node **children;
    int                child_count;
    int                child_cap;
} tmpfs_node_t;
```

### 4.3 核心函数

```
tmpfs_find_block(node, blk_idx):
  遍历 tmpfs_block 链表，定位到 blk_idx 位置
  若 blk_idx 超过已分配块 → return NULL

tmpfs_grow_to(node, blk_idx):
  从 last_block 开始，分配新 tmpfs_block_t (kmalloc)
  追加到链表尾部直到 blk_idx
  更新 size

tmpfs_vfs_read(node, offset, size, buffer):
  blk_idx = offset / 4096
  定位 first_block → 遍历到 blk_idx
  从块内偏移 (offset % 4096) 开始 memcpy
  跨块时推进到下一个块继续复制
  直到 size 字节读完或到达 size 边界

tmpfs_vfs_write(node, offset, size, buffer):
  tmpfs_grow_to(node, (offset + size - 1) / 4096)
  同 read 的逆操作，从 buffer 写回数据块

tmpfs_vfs_readdir(dir, index, entry):
  . (index 0) → ino=(uintptr_t)dir, type=DIR
  .. (index 1) → parent 信息
  children[index-2] (index >= 2) → 子节点的 name/type/size/ino

tmpfs_vfs_create(dir, name):
  分配新 tmpfs_node_t
  dir->children 数组扩容（realloc 或 kmalloc 更大数组 + memcpy）
  追加到 children[child_count++]

tmpfs_vfs_mkdir(dir, name):
  同 create，但 type = VFS_DIR

tmpfs_vfs_unlink(dir, name):
  在 children[] 中查找 name → 释放节点 + 数据块
  数组缩容（把末尾元素移到删除位置，child_count--）

tmpfs_vfs_rmdir(dir, name):
  同 unlink，但检查子目录为空（child_count == 0）

tmpfs_vfs_truncate(node, new_size):
  释放 new_size 之后的块
  调整 size
```

### 4.4 初始化

```c
void tmpfs_init(void) {
    // 创建 tmpfs 根节点 (目录类型)
    tmpfs_node_t *root = calloc(1, sizeof(tmpfs_node_t));
    root->type = VFS_DIR;
    root->children = kmalloc(TMPFS_CHILDREN_INIT_CAP * sizeof(void *));
    root->child_cap = TMPFS_CHILDREN_INIT_CAP;

    vfs_mount("/tmp", NULL, &tmpfs_vfs_ops, root);
    // dev=NULL 表示无后备 block device，VFS 已有此路径
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

### 5.1 文件

| 文件 | 类型 | 说明 |
|------|------|------|
| `tools/mkdisk.c` | 新增 | disk.img 构建工具 (~200 行宿主 C) |
| `tools/Makefile` | 新增 | 宿主编译规则 |
| `Makefile` | 修改 | `disk.img` target 改用 `tools/mkdisk` |
| `config/fsroot/` | 新增 | ext2 内容源目录 |

### 5.2 mkdisk 流程

```
tools/mkdisk disk.img \
    --efi boot/uefi/BOOTX64.EFI \
    --kernel kernel.bin \
    --rootfs config/fsroot/

输入:
  config/fsroot/
    ├── bin/
    │   ├── init       → 来自 build/x86_64/user/init.elf
    │   ├── busybox    → 来自 build/x86_64/user/busybox.elf
    │   ├── spin       → 来自 build/x86_64/user/spin.elf
    │   ├── sigtest    → 来自 build/x86_64/user/sigtest.elf
    │   ├── poweroff   → 来自 build/x86_64/user/poweroff.elf
    │   ├── systest    → 来自 build/x86_64/user/systest.elf
    │   ├── test_mmap  → 来自 build/x86_64/user/test_mmap.elf
    │   ├── test_fork_mmap → 来自 build/x86_64/user/test_fork_mmap.elf
    │   └── test_cow   → 来自 build/x86_64/user/test_cow.elf
    ├── home/
    └── etc/

流程:
  1. 计算总大小
     FAT32 = 64MB (给 BOOTX64.EFI + kernel.bin 留有足够空间)
     ext2  = 128MB (可变, 至少 64MB)
     total = 64MB + 128MB

  2. 创建 disk.img (fallocate 或 seek write)

  3. 写入 Protective MBR (LBA 0, 只有一个 Type=0xEE entry 的 DOS 分区表)

  4. 写入 GPT header (LBA 1):
     "EFI PART" signature
     header_size = 92
     my_lba = 1
     first_usable_lba = 34
     last_usable_lba = total_sectors - 34
     partition_entry_lba = 2
     num_partition_entries = 128
     size_of_partition_entry = 128
     计算 + 写入 CRC32

  5. 写入 Partition Entry Array (LBA 2..33):
     Entry 1:
        type_guid = C12A7328-F81F-11D2-BA4B-00A0C93EC93B (EFI System Partition)
        name = "ESP"
        start_lba = 2048
        end_lba   = 2048 + (64MB / 512) - 1
     Entry 2:
        type_guid = 0FC63DAF-8483-4772-8E79-3D69D8477DE4 (Linux filesystem data)
        name = "rootfs"
        start_lba = entry1_end + 1
        end_lba   = entry2_start + (128MB / 512) - 1

  6. 填充 FAT32 ESP 分区:
     dd if=/dev/zero of=/tmp/esp.img bs=1M count=64
     mkfs.vfat -F 32 /tmp/esp.img
     mmd -i /tmp/esp.img ::/EFI
     mmd -i /tmp/esp.img ::/EFI/BOOT
     mcopy -i /tmp/esp.img BOOTX64.EFI ::/EFI/BOOT
     mcopy -i /tmp/esp.img kernel.bin ::/
     dd if=/tmp/esp.img of=disk.img bs=512 seek=2048 conv=notrunc

  7. 填充 ext2 分区:
     dd if=/dev/zero of=/tmp/rootfs.img bs=1M count=128
     mke2fs -t ext2 -I 128 -b 4096 /tmp/rootfs.img
     使用 debugfs -w /tmp/rootfs.img:
       mkdir /bin
       mkdir /home
       mkdir /etc
       mkdir /tmp      # mount tmpfs 前的占位目录
       write init.elf /bin/init
       write busybox /bin/busybox
       ... (其他文件)
     dd if=/tmp/rootfs.img of=disk.img bs=512 seek=<ext2_start> conv=notrunc

  8. 清理临时镜像

依赖: mkfs.vfat (dosfstools), mke2fs (e2fsprogs), mtools, debugfs (e2fsprogs)
```

### 5.3 Makefile 集成

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
	$(MAKE) -C tools
	tools/mkdisk disk.img \
	    --efi boot/uefi/BOOTX64.EFI \
	    --kernel kernel.bin \
	    --rootfs config/fsroot/
```

---

## 6. 启动顺序

`kernel_main()` 中的文件系统初始化调整为：

```c
// 1. 块设备
block_device_init();          // AHCI → sda, 注册 /dev/sda

// 2. devfs 挂载 + 字符设备注册（初始化 devices[] 数组）
devfs_init();                 // /dev: null, zero, random, serial, tty

// 3. 分区发现（此时 devices[] 已可用）
gpt_scan(block_device_get(0)); // 解析 GPT → /dev/sda1, /dev/sda2

// 4. VFS 文件系统挂载
vfs_init();                   // 初始化 mount table
fat32_mount(sda1, "/boot");   // /boot → FAT32 ESP (sda1)
ext2_mount(sda2);             // / → ext2 (sda2)
tmpfs_init();                 // /tmp → tmpfs (纯内存)

// 5. 信息文件系统
procfs_init();                // /proc

// 6. 启动用户态
spawn_user_task("/bin/init");
```

注：
- `fat32_mount` 当前签名是 `int fat32_mount(block_device_t *dev, fat32_fs_t **out_fs)`，内部硬编码 `vfs_mount("/", dev, ...)`。需改为 `int fat32_mount(block_device_t *dev, const char *mount_path, fat32_fs_t **out_fs)`，将 mount_path 传给 `vfs_mount`。
- `ext2_mount(block_device_t *dev)` 内部调用 `vfs_mount("/", dev, &ext2_vfs_ops, fs)` 挂载到 `/`。
- `block_device_get(0)` 返回 sda（第一个注册的 block device）。`gpt_scan` 内部通过 `block_device_t` 的 sector read 读取 GPT 表，并为每个分区创建 `partition_ctx_t` wrapper block device。

---

## 7. VFS 改动点

### 7.1 `find_mount` 改为最长前缀匹配

当前 `find_mount` 遍历 `mount_table`，对路径做 `strncmp(mp->path, path, mp_len) == 0` 检查，返回第一个匹配的 mount。当同时存在 `/` 和 `/boot` 两个挂载点时，`/` 会先匹配 `/boot/kernel.bin`（因为 `/` 也是其前缀）。

需要改为**最长前缀匹配**：
```c
static vfs_mount_t *find_mount(const char *path) {
    vfs_mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < mount_count; i++) {
        size_t mp_len = strlen(mount_table[i].path);
        if (strncmp(mount_table[i].path, path, mp_len) == 0) {
            // path is "/" or path starts with mount path followed by '/' or NUL
            if (path[mp_len] == '/' || path[mp_len] == '\0') {
                if (mp_len > best_len) {
                    best = &mount_table[i];
                    best_len = mp_len;
                }
            }
        }
    }
    return best;
}
```

- `/boot/kernel.bin` → 匹配 `/boot`（len=5）和 `/`（len=1），选最长 → `/boot` ✓
- `/bin/init` → 只匹配 `/` → `/` ✓
- 对于 `/boot` 挂载下的路径，`__vfs_lookup` 跳过 `/boot` 前缀后正常查找

### 7.2 `/` + `/boot` 两个 mount 的路径解析

`__vfs_lookup` 中 `find_mount` 已改为最长前缀匹配：

- `/boot/kernel.bin` → `find_mount("/boot/kernel.bin")` → 匹配 `/boot` (len=5) 和 `/` (len=1)，选最长 → 返回 `/boot` mount ✓
- `/bin/init` → `find_mount("/bin/init")` → 只匹配 `/` → 返回 `/` mount ✓

`__vfs_lookup` 中需要确认：当 mount point 是 `/boot` 时，`mp_len > 1`，prefix skip 逻辑正确去掉 `/boot` 前缀后查找剩余路径（即 `/kernel.bin`）。

### 7.3 补充：`block_device_init` 注册物理磁盘到 devfs

`block_device_init()` 需要在 AHCI port 发现后调用 `devfs_register_blkdev("sda", dev)`，将物理磁盘也暴露到 `/dev`。后续 `gpt_scan` 创建的 partition wrapper block device 再注册为 `sda1`、`sda2` 等。

分区 block device 创建函数（`block_device_create_partition`）：
```c
// 创建一个分区 block device wrapper
// parent: 物理磁盘或上层 block device
// offset_lba: 分区起始扇区
// length: 分区扇区数
block_device_t *block_device_create_partition(block_device_t *parent,
                                               uint64_t offset_lba,
                                               uint64_t length);
```
内部结构见 1.4 节的 `partition_ctx_t`。此函数分配 `block_device_t` + `partition_ctx_t`，设置 `.read/.write` 为分区转发函数，调用 `block_device_register` 注册到全局列表，并返回。分区设备不再次注册到 devfs — 由 `gpt_scan` 中决定哪些分区需要注册到 `/dev`。

### 7.4 getdents 挂载点注入

`/` 下的 getdents 应显示 `boot/`, `bin/`, `home/`, `etc/`, `tmp/`, `dev/`, `proc/`。其中 `boot/`, `tmp/`, `dev/`, `proc/` 是 mount points。当前已有 mount point 注入逻辑（`b5cc904`），需确认在新布局下正确工作。

---

## 8. 文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `kernel/fs/ext2.c` | 新增 | ext2 只读驱动 (~350 行) |
| `kernel/include/fs/ext2.h` | 新增 | ext2 on-disk 结构体定义 |
| `kernel/fs/tmpfs.c` | 新增 | tmpfs (~200 行) |
| `kernel/include/fs/tmpfs.h` | 新增 | tmpfs 接口声明 |
| `kernel/fs/gpt.c` | 新增 | GPT 分区表解析 (~150 行) |
| `kernel/include/fs/gpt.h` | 新增 | partition_t + gpt_scan 声明 |
| `kernel/fs/devfs.c` | 修改 | 新增 `devfs_register_blkdev()` (~30 行) |
| `kernel/fs/vfs.c` | 修改 | `find_mount` 最长前缀匹配确认修复 |
| `kernel/fs/fat.c` | 修改 | `fat32_mount` 接受 mount_path 参数 |
| `kernel/kernel/main.c` | 修改 | 新挂载顺序 (~10 行) |
| `kernel/Makefile` | 修改 | 新增 .c 文件 |
| `tools/mkdisk.c` | 新增 | disk 构建工具 (~200 行) |
| `tools/Makefile` | 新增 | 宿主工具编译 |
| `config/fsroot/` | 新增 | ext2 内容源目录 |
| `Makefile` (root) | 修改 | disk.img target 改动 |

**总计**：~950 行新代码 + ~50 行修改。

---

## 9. 测试策略

### 9.1 内核自测 (SELFTEST)

- ext2 superblock 验证：magic=0xEF53 → mount 成功
- ext2 root inode 读取：inode 2 readinode → i_mode has S_IFDIR
- ext2 readdir root：/ 下有目录条目（. .. bin home etc）
- ext2 file read：读取 `/bin/init` 的前 4 字节 → ELF magic `\x7fELF`
- tmpfs create + read + write：挂载后创建文件 → 写入 → 读回验证
- GPT 解析：扫描 sda → partition count > 0

### 9.2 现有测试

`make test` 中 VFS 测试 (`test_vfs_basic.c`) 基于 in-memory fake filesystem，不受 ext2 引入影响，应保持通过。

### 9.3 集成验证

```bash
make run   # QEMU 启动
# 期望：
#   "gpt: found 2 partitions"
#   "devfs: registered blkdev sda1"
#   "devfs: registered blkdev sda2"
#   "VFS: mounted '/boot'"  
#   "VFS: mounted '/'"
#   "VFS: mounted '/tmp'"
#   → init.elf 成功启动
```
