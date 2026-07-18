# ext2 文件系统写入 — 设计方案

> **日期**: 2026-07-18
> **修订**: 2026-07-18 — 16 条 (v2) + 5 条 (v3) 评审意见全量修复
> **范围**: ext2 全部 7 个 VFS 写入操作 + 底层分配/管理原语
> **方案**: 共享原语 + VFS op 组合（方案 2）
> **依赖**: 现有 ext2 只读路径、block_device_write、FAT32 写路径参考

---

## 1. 现状分析

### 1.1 已有能力

| 能力 | 实现 | 文件 |
|------|------|------|
| ext2 只读 | `ext2_vfs_read` + `ext2_vfs_readdir` 完整实现 | `kernel/fs/ext2.c` |
| block 读 | `ext2_read_block` (block → block_device_read) | `kernel/fs/ext2.c:18` |
| block 映射 | `ext2_bmap` (direct + single indirect, 只读) | `kernel/fs/ext2.c:50` |
| inode 读 | `ext2_read_inode` (group/index → inode table) | `kernel/fs/ext2.c:25` |
| mount | `ext2_init` — superblock 解析、bgdesc 加载、lock 初始化 | `kernel/fs/ext2.c:197` |
| block 设备写 | `block_device_write` → AHCI 写 | `kernel/block/blockdev.c:103` |
| FAT32 完整写 | `fat32_alloc_cluster` + `fat32_write_data` + VFS ops | `kernel/fs/fat.c` |
| 常用辅助 | `ext2_node_ino(node)` — node→fs_data 解析为 ino；root 节点 fs_data=NULL 返回 2 | `kernel/fs/ext2.c:11` |

### 1.2 缺失项

VFS ops table 中全部写操作均为 NULL：

| ops 槽位 | 当前值 | 需要实现的操作 |
|----------|--------|---------------|
| `.write` | NULL | 向已有文件写入数据，必要时分配新 block |
| `.truncate` | NULL | 扩展/收缩文件大小 |
| `.create` | NULL | 创建普通文件（alloc inode + dirent） |
| `.unlink` | NULL | 删除文件（dirent → inode → 回收 block） |
| `.mkdir` | NULL | 创建目录（alloc inode + "." / ".." dirent） |
| `.rmdir` | NULL | 删除空目录 |
| `.rename` | NULL | 重命名/移动文件或目录 |

底层缺少：block/inode 分配/释放、inode 回写、superblock 回写、目录项插入/删除、写入路径的 block 映射分配变体。

---

## 2. 总体架构

### 2.1 分层

```
VFS ops (7 个): write / truncate / create / unlink / mkdir / rmdir / rename
       │
       ▼
原语层 (11 个): alloc_block / free_block / alloc_inode / free_inode
               write_inode / write_block / write_superblock
               ext2_bmap_alloc / ext2_find_dirent
               dirent_add / dirent_del
       │
       ▼
设备层: block_device_read / block_device_write → AHCI → 磁盘
```

所有原语在调用者持有 `ext2_fs_t.lock` 的前提下运行，不内部加锁。

### 2.2 参考实现模式

FAT32 已有相同的分层结构（`fat32_alloc_cluster` / `fat32_write_fat_entry` / `fat32_write_data` → VFS ops），ext2 写实现沿用此模式。ext2 读路径已有的 `ext2_read_block` / `ext2_read_inode` / `ext2_bmap` 为写路径的原语提供对称框架。

---

## 3. 底层原语

### 3.0 前置改动: ext2_fs_t 扩展

在 `kernel/include/fs/ext2.h` 的 `ext2_fs_t` 中新增两个字段:

```c
ext2_superblock_t sb_raw;    // 缓存 superblock 用于回写
uint32_t         bgdesc_block; // bgdesc 表起始 block 号
uint32_t         bgdesc_table_blocks; // bgdesc 表占用的 block 数
```

`ext2_init` 变更: 在解析 superblock 后，将其 memcpy 到 `fs->sb_raw` 供后续回写使用。当前代码（`ext2.c:208`）将 superblock 读到栈上的 `uint8_t sb_buf[1024]` 后直接当 `ext2_superblock_t*` 使用，需在此处添加：

```c
memcpy(&fs->sb_raw, sb_buf, sizeof(ext2_superblock_t));
```

同时保存 `bgdesc_block` 和 `bgdesc_table_blocks`（当前代码中已经计算了 `bgdesc_block` 和 `table_blocks`，但只在栈上存在——写到 `fs` 中即可）。

### 3.1 ext2_write_block

```
int ext2_write_block(ext2_fs_t *fs, uint32_t block, const void *buf)

lba = block * sectors_per_block
→ block_device_write(dev, lba, sectors_per_block, buf)
```

`ext2_read_block` 的写镜像。调用者负责传入完整 block 大小的 buffer。

### 3.2 ext2_bmap_alloc — block 映射的分配变体

```
int ext2_bmap_alloc(ext2_fs_t *fs, ext2_inode_t *inode,
                    uint32_t logical_block)

// 已有物理块?
if (logical_block < 12) {
    if (inode->i_block[logical_block] != 0)
        return inode->i_block[logical_block];  // 已分配
    // 分配新块 → 写入 i_block[logical_block] → 递增 i_blocks(扇区计数)
    uint32_t phys = alloc_block(fs);
    inode->i_block[logical_block] = phys;
    inode->i_blocks += block_size / 512;  // i_blocks 以 512B 为单位
    return phys;
}

uint32_t ptrs_per_block = block_size / sizeof(uint32_t);
if (logical_block < 12 + ptrs_per_block) {
    // 确保 indirect block 存在
    if (inode->i_block[12] == 0) {
        uint32_t indirect_blk = alloc_block(fs);
        inode->i_block[12] = indirect_blk;
        inode->i_blocks += block_size / 512;
    }
    // 读 indirect block
    uint32_t indirect[1024];
    ext2_read_block(fs, inode->i_block[12], indirect);
    uint32_t idx = logical_block - 12;
    if (indirect[idx] == 0) {
        indirect[idx] = alloc_block(fs);
        inode->i_blocks += block_size / 512;
        ext2_write_block(fs, inode->i_block[12], indirect);
    }
    return indirect[idx];
}

return 0;  // double/triple indirect 不支持
```

`ext2_bmap` 的分配变体。保证 `logical_block` 有对应的物理块，不存在则分配。是 `ext2_vfs_write` 和 `ext2_vfs_truncate`（扩展路径）的核心依赖。

**注意**: 本函数只修改内存中的 `inode` 结构体（`i_block[]`、`i_blocks`、以及可能的 indirect block 磁盘数据）。调用方通过 `ext2_write_inode` 负责将修改后的 inode 持久化到磁盘。

### 3.3 ext2_write_inode

```
int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode)

group = (ino - 1) / inodes_per_group
index = (ino - 1) % inodes_per_group
table_start = bgdesc_table[group].bg_inode_table
→ 读目标 block → memcpy 修改 inode 槽 → ext2_write_block 回写
```

与 `ext2_read_inode` 共享 group/index/offset 计算逻辑，方向反向：从 struct 写入磁盘。

### 3.4 ext2_write_superblock

```
int ext2_write_superblock(ext2_fs_t *fs)

→ 将 fs->sb_raw 序列化为 1024 字节 → block_device_write(offset=1024, 2 sectors)
→ 同时回写所有 bgdesc 条目: 从 bgdesc_block 起，连续写 bgdesc_table_blocks 个 block
```

`bgdesc_table_blocks = ceil(num_block_groups * sizeof(ext2_bgdesc_t) / block_size)`，在 `ext2_init` 中已计算，此处复用缓存的 `fs->bgdesc_table_blocks`。

**性能注意事项**: 每次 alloc/free 都会回写整个 bgdesc 表。对于 N 个 block group 的磁盘，每次元数据变更需要写 `ceil(N * 32 / block_size)` 个 block。4KB block、1024 group (16GB 磁盘) 时，每次分配写 8 个 block。当前开发环境下可接受（磁盘镜像小，操作频率低）；详见 §8 已知限制。

### 3.5 alloc_block / free_block

**alloc_block**:
```
遍历 block group descriptor → 跳过 bg_free_blocks_count==0 的 group
→ 读 block bitmap → 扫描: 按字节找 != 0xFF → __builtin_ctz 定位 free bit → 置 1
→ 回写 bitmap block（ext2_write_block）
→ bgdesc.bg_free_blocks_count--
→ fs->sb_raw.s_free_blocks_count--
→ ext2_write_superblock（同时回写 superblock + 全部 bgdesc）
→ 将新分配的 block 用 ext2_write_block 全零写回（zero 化）
→ 返回 block number
```

**free_block**:
```
定位 block 所属 group → 读 bitmap
→ 对应 bit 置 0 → 回写 bitmap block
→ bgdesc.bg_free_blocks_count++
→ fs->sb_raw.s_free_blocks_count++
→ ext2_write_superblock
```

### 3.6 alloc_inode / free_inode

**alloc_inode(fs, mode)**:
```
扫描 inode bitmap → 找到 free bit → 置 1
→ 回写 bitmap block
→ fs->sb_raw.s_free_inodes_count-- / bg_free_inodes_count--
→ ext2_write_superblock
→ 读 inode table → 初始化: i_mode=mode, i_links_count=1, i_blocks=0,
  i_size=0, i_block[0..14]清零, i_atime=i_ctime=i_mtime=0
  （内核当前无 RTC 时间戳接口，填 0）
→ ext2_write_inode 回写
→ 返回 inode number
```

**free_inode(fs, ino)**:
```
定位 ino 的 bitmap → 对应 bit 置 0 → 回写 bitmap block
→ fs->sb_raw.s_free_inodes_count++ / bg_free_inodes_count++
→ ext2_write_superblock
```

### 3.7 ext2_find_dirent

```
int ext2_find_dirent(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                     uint32_t *out_ino, uint8_t *out_file_type,
                     uint32_t *out_block, uint32_t *out_off)

// 遍历目录 inode 的 dirent 链表，匹配 name
// 成功: 填充 *out_ino, *out_file_type, *out_block(物理块号), *out_off(块内偏移), 返回 0
// 未找到: 返回 -ENOENT
```

`unlink`、`rmdir`、`rename` 共享的查找辅助函数。避免三处各自遍历 dirent 链表。

**生命周期说明**: 本函数内部使用栈上的 `ext2_dirent_t` 缓冲区，遍历时通过 `ext2_read_block` 逐块读取。返回值 `out_ino`/`out_file_type`/`out_block`/`out_off` 是标量值，不持有指针依赖，调用方无需关心内部缓冲区生命周期。不可返回指向内部 `de.name[]` 的指针。

### 3.8 dirent_add / dirent_del

**dirent_add(fs, dir_ino, name, new_ino, file_type)**:
```
计算新 dirent 所需空间 (aligned):
  new_len = align4(sizeof(ext2_dirent_t) + name_len)
  （ext2 dirent 按 4 字节对齐: align4(x) = (x + 3) & ~3）

读目录 inode → 遍历目录块:
  对每条 dirent:
    实际占用 = align4(sizeof(ext2_dirent_t) + 该条目的 name_len)
    if dirent.rec_len - 实际占用 >= new_len:
      → "分裂"此处: 将本条 rec_len 缩小到实际占用
      → 新 dirent 放在实际占用之后: rec_len = 原 rec_len - 实际占用
      → 填充 inode/name_len/file_type → ext2_write_block 回写
      → return 0

  如果遍历完当前块无合适空隙:
    → 尝试 ext2_bmap(logical_blk+1): 存在则读下一个目录块继续遍历
    → 不存在则 alloc_block 分配新块 → 新 dirent 占用整个块
      (rec_len = block_size) → 更新目录 i_size += block_size

→ 更新父 inode.i_size (如扩展: i_size = original_size + new_block_size),
  i_mtime → ext2_write_inode(dir)
```

**dirent_del(fs, dir_ino, name)**:
```
ext2_find_dirent 找到目标(获取 out_ino, out_file_type, block, off)
→ 将目标 inode 清零: 直接写回 block data → *(uint32_t*)(block_data + off) = 0
  （此举让 readdir 的 de->inode != 0 检查自然跳过此条目）
→ 将前一条 dirent 的 rec_len += 本条 de.rec_len（合并回前一条）
→ ext2_write_block 回写目录块
→ ext2_write_inode(dir)
```

---

## 4. 节点创建模式

VFS 层没有通用的 `vfs_create_node()` 辅助函数。fat32 的 `fat_create`（`fat.c:1154`）展示了标准模式：

```c
vfs_node_t *node = calloc(1, sizeof(vfs_node_t));
node->name = name;          // 调用者传入的字符串（持久化或 shallow copy）
node->type = VFS_FILE;      // 或 VFS_DIR
node->mount = parent->mount;
node->ops = parent->ops;
node->fs_data = (void *)(uintptr_t)new_ino;
node->refcount = 1;
```

ext2 的 create/mkdir 复用此模式。不引入额外的 `vfs_make_node` 包装。

---

## 5. VFS 操作

所有 VFS op 入口通过 `ext2_node_ino(node)` 获取 inode number，通过 `node->mount->fs_data` 获取 `ext2_fs_t*`。

### 5.1 ext2_vfs_write

```
Write(node, offset, size, buffer):
  读 inode
  → extent 检查: if (inode.i_flags & 0x00080000) return -ENOTSUP
     （EXT4_EXTENTS_FL — 当前 ext2_bmap/ext2_bmap_alloc 都无法处理 extent 树）
  → 确保 offset+size 范围内每个 logical_block 有物理映射:
    对 logical_blk in [offset/blk_size .. (offset+size-1)/blk_size]:
      ext2_bmap_alloc(fs, inode, logical_blk)
  → 逐块 RMW: ext2_read_block → memcpy 修改部分 → ext2_write_block
  → if offset+size > i_size: i_size = offset+size
  → i_mtime = 0 → ext2_write_inode
  → 返回写入字节数
```

### 5.2 ext2_vfs_truncate

```
Truncate(node, new_size):
  读 inode
  → extent 检查: if (inode.i_flags & 0x00080000) return -ENOTSUP

  if new_size > i_size:
    new_blocks = ceil(new_size / block_size)
    old_blocks = ceil(i_size / block_size)
    → for logical_blk in [old_blocks .. new_blocks-1]:
        ext2_bmap_alloc(fs, inode, logical_blk)
    → i_size = new_size
    → write_inode

  else (new_size < i_size):
    new_blocks = ceil(new_size / block_size)
    old_blocks = ceil(i_size / block_size)
    → for logical_blk in [new_blocks .. old_blocks-1]:
        获取物理块: ext2_bmap(fs, inode, logical_blk)
        free_block(phys)
        清除 i_block[slot]（direct: i_block[logical_blk]=0;
              indirect: indirect[idx]=0, 回写 indirect block,
              如果 indirect block 已空则 free_block(i_block[12])）
        i_blocks -= block_size / 512
    → i_size = new_size
    → write_inode
```

`i_blocks` 字段以 **512 字节扇区** 为单位（ext2 规范），不是 block 数。alloc_block 时 `i_blocks += block_size / 512`，free_block 时 `i_blocks -= block_size / 512`。

### 5.3 ext2_vfs_create

```
Create(dir, name, mode=0644):
  → alloc_inode(EXT2_S_IFREG | mode)
  → dirent_add(dir_ino, name, new_ino, EXT2_FT_REG_FILE)
  → ext2_write_inode(dir)  // 更新 mtime
  → 按 §4 模式包装 vfs_node_t 返回
```

父目录的 `i_links_count` 仅子目录创建时递增（因 ".."），普通文件创建不增加。

### 5.4 ext2_vfs_unlink

```
Unlink(dir, name):
  → ext2_find_dirent(fs, dir_ino, name, &target_ino, &file_type, &block, &off)
  → dirent_del(dir_ino, name)
  → 读目标 inode → i_links_count--
  → if i_links_count == 0:
      遍历 i_block[0..11] → free_block 每个非零块
      if i_block[12] != 0:
        读 indirect block → free_block 每个非零条目
        → free_block indirect block 本身
      free_inode(target_ino)
    else:
      ext2_write_inode(target_ino)  // 只更新 i_links_count
```

### 5.5 ext2_vfs_mkdir

```
Mkdir(dir, name, mode=0755):
  → alloc_inode(EXT2_S_IFDIR | mode)
  → alloc_block → 初始化目录块:
    dirent[0]: inode=新ino, name=".", name_len=1, file_type=2, rec_len=12
    dirent[1]: inode=父ino, name="..", name_len=2, file_type=2,
               rec_len=block_size-12（吞掉剩余全部空间）
  → 父 group bg_used_dirs_count++ → fs->sb_raw 对应字段 + ext2_write_superblock
  → 父 inode.i_links_count++（因为子目录的 ".." 引用父目录）
  → 新 inode: i_links_count=2 (. 和来自父目录的引用),
    i_blocks = block_size / 512（已分配一个目录块）
  → dirent_add(dir_ino, name, new_ino, EXT2_FT_DIR)
  → ext2_write_inode(new) + ext2_write_inode(dir)
  → 按 §4 模式包装 vfs_node_t 返回
```

### 5.6 ext2_vfs_rmdir

```
Rmdir(dir, name):
  → ext2_find_dirent(fs, dir_ino, name, &target_ino, &file_type, &block, &off)
  → 读目标 inode
  → 检查目标目录是否为空: 遍历 dirent 链表
      有效条目 = de.inode != 0（已删除 dirent 跳过）
      且 de.inode != target_ino（"." 跳过）
      且 de.inode != dir_ino（".." 跳过，dir_ino 是真实父目录 ino）
    → 若仍有有效条目 → 返回 -ENOTEMPTY
  → dirent_del(dir_ino, name)
  → 父目录 i_links_count--（因为子目录的 ".." 随之消失）
  → ext2_write_inode(dir)
  → 父 group bg_used_dirs_count-- → ext2_write_superblock
  → 释放目录全部数据块:
      遍历 i_block[0..11] → free_block 每个非零块
      if i_block[12] != 0:
        读 indirect block → free_block 每个非零条目
        → free_block indirect block 本身
  → free_inode(target_ino)
```

**空目录检查**: ".." 必须指向 `dir_ino`（真实父目录），不允许其他值。如果 ".." 指向别处，说明目录已被另一次 rename 移走或 fs 损坏——应返回 -ENOTEMPTY 而非继续删除。

### 5.7 ext2_vfs_rename

```
Rename(olddir, oldname, newdir, newname):
  → ext2_find_dirent(olddir, oldname) → 获取 target_ino, file_type
  → 读旧目录 inode

// 注: 当前不支持硬链接 (§10)，因此 oldname 和 newname 指向同一 inode 的情况
// 不会出现。若后续引入 link()，需在此时添加 if (target_ino == existing_ino) return 0
// 的空操作检测（POSIX 要求同 inode rename 是空操作）。

  → 预处理目标:
    在新目录中 ext2_find_dirent(newdir, newname)
    → 如果目标存在且为目录:
        检查是否为空（同 rmdir）→ 非空: 返回 -ENOTEMPTY
        (POSIX: rename 不允许替换非空目录)
    → 如果目标存在且为文件:
        先 unlink 替换（POSIX: 静默替换文件）

  → if olddir == newdir:
      dirent_del(olddir_ino, oldname) + dirent_add(olddir_ino, newname, target_ino, type)
  → else:
      dirent_add(newdir_ino, newname, target_ino, type)
      dirent_del(olddir_ino, oldname)

  → 更新被移动的 inode.i_ctime → ext2_write_inode(target_ino)

  → 如果是目录跨目录移动:
      读目标目录的第一个数据块 → 更新 ".." dirent 指向 newdir_ino
      → ext2_write_block 回写
      → 调整 olddir/newdir 的 i_links_count:
        olddir.i_links_count--, newdir.i_links_count++
      → olddir group bg_used_dirs_count--, newdir group bg_used_dirs_count++
      → ext2_write_superblock
```

---

## 6. 写入顺序与 Superblock 同步

### 6.1 每条操作路径的写入顺序

```
创建文件:   data blocks → inode → bitmap → superblock+bgdesc → dirent
删除文件:   dirent → inode(i_links_count) → bitmap → superblock+bgdesc → data blocks
扩展文件:   data blocks → bitmap → superblock+bgdesc → inode(i_size)
truncate:   inode(i_size) → bitmap → superblock+bgdesc → data blocks
mkdir:      data block("." / "..") → inode → bitmap → superblock+bgdesc → dirent
```

### 6.2 Superblock 同步

每次元数据变更后立即回写 superblock 和全部 bgdesc。触发点：
- alloc_block → fs->sb_raw.s_free_blocks_count--, bg_free_blocks_count--
- free_block → fs->sb_raw.s_free_blocks_count++, bg_free_blocks_count++
- alloc_inode → fs->sb_raw.s_free_inodes_count--, bg_free_inodes_count--
- free_inode → fs->sb_raw.s_free_inodes_count++, bg_free_inodes_count++

正常关机/重启下元数据一致。崩溃场景下 bitmap 是权威来源（fsck 可修）。

---

## 7. 测试

### 7.1 内核自测 (KERNEL_SELFTEST)

复用现有 AHCI 磁盘上的 ext2 分区，每个测试前后做 save/restore。

**save/restore 范围**（每个测试的精确保存范围，不是"目标 block"的粗略估算）：

| 测试 | 保存范围 | 测试操作 | 恢复 |
|------|---------|---------|------|
| `ext2_selftest_block_alloc` | 目标 bitmap block + 目标 group 的 bgdesc block + superblock (2 sectors) | alloc → 验证非零 → free → 验证 free 后 alloc 得不同块 | 逐个写回 bitmap → bgdesc → superblock |
| `ext2_selftest_inode_alloc` | inode bitmap block + bgdesc block + superblock + inode table block（将被写入的那个） | alloc → 验证 ino>0 → free | 逐个写回 |
| `ext2_selftest_dirent_roundtrip` | 父目录的数据 block(s) + 父 inode table block + inode bitmap block + bgdesc block + superblock + 新 inode 所在 inode table block（dirent_add 需要先 alloc_inode） | alloc_inode → dirent_add → readdir 验证 → dirent_del → free_inode → 验证消失 | 逐个写回 |
| `ext2_selftest_write_read` | 目标文件的数据 block(s) + 目标 inode table block | write → read 回读验证 | 逐个写回数据 + inode |

**`ext2_selftest_write_read` 前提条件**: 测试假设写入范围在文件的当前 i_size 之内，不触发 `ext2_bmap_alloc`（不分配新块）。如果文件已有大小不足以容纳测试写入，则测试应当 `SKIP` 或选择一个更大的已有文件。此约束确保 save/restore 范围不含 bitmap/superblock。

**`ext2_selftest_dirent_roundtrip` 依赖说明**: 必须走完整的 `alloc_inode → dirent_add` 路径，因为目录项需要一个有效的 inode 编号。因此保存范围包含 inode bitmap 和 inode table block，结束前 `free_inode` 回收。

如果内核自测阶段 AHCI 设备尚未初始化，则所有 ext2 写自测标记为 `SKIP` 而非 `FAIL`。

### 7.2 用户态 systest

在 `/opt/test` 目录下测试（该目录在 ext2 分区上）：
- `open(O_CREAT|O_RDWR)` → `write` → `lseek(SEEK_SET)` → `read` 验证
- `unlink` → `open` 应返回 ENOENT
- `mkdir` → `stat` 验证 → `rmdir`
- 同目录 `rename` + 跨目录 `rename`
- `truncate` 扩展 + 收缩
- 多 block 文件（写入 > 4KB 验证 indirect block 分配）

### 7.3 Shell 冒烟测试

```sh
echo hello > /opt/test/smoke && cat /opt/test/smoke && rm /opt/test/smoke
mkdir /opt/test/d && rmdir /opt/test/d
```

---

## 8. 实现阶段

| 阶段 | 内容 | 文件 |
|------|------|------|
| 1. Block 写基础 + struct 扩展 | `ext2_fs_t` 增加 `sb_raw`/`bgdesc_block`/`bgdesc_table_blocks`；`ext2_init` 缓存 superblock；`ext2_write_block`；`ext2_write_inode`；`ext2_write_superblock` | `kernel/fs/ext2.c`, `kernel/include/fs/ext2.h` |
| 2. 分配/释放原语 | `alloc_block/free_block`, `alloc_inode/free_inode`, `ext2_bmap_alloc`, `ext2_find_dirent`, `dirent_add/del` | `kernel/fs/ext2.c` |
| 3. VFS ops | write, truncate, create, unlink, mkdir, rmdir, rename | `kernel/fs/ext2.c` |
| 4. 测试 | 内核自测 + 用户态 systest + shell 冒烟 | `kernel/fs/ext2.c`, `user/systest.c` |

每个阶段结束时进行 `make clean && make && make test` 验证无回归。

---

## 9. 风险与注意事项

- **bitmap 跨块扫描**: 每个 block group 的 bitmap 占 1 个 block，大容量磁盘下需跨多 group 扫描
- **dirent rec_len 可变长度管理**: 插入/删除时 rec_len 调整是 ext2 写入最易出错的点——必须保证 align4 计算和 rec_len 拆分正确
- **single indirect block 管理**: alloc/free 时需处理 i_block[12] 指向的间接块；`ext2_bmap_alloc` 负责分配 indirect block 本身
- **extent inode 保护**: 所有写入入口（write/truncate）在操作 inode 前检查 `inode.i_flags & 0x00080000`（EXT4_EXTENTS_FL），拒绝 extent 格式的 inode，防止在不支持的映射格式上损坏数据
- **`make clean`**: 修改 ext2.h 中结构体后必须 clean build（无 header deps）
- **spinlock 持有期间不阻塞**: 所有原语内部不调用 schedule/sleep。当前架构下 IRQ handler 不接触 `ext2_fs_t`，`spin_lock/spin_unlock`（非中断安全变体）足够
- **i_blocks 扇区计数**: ext2 规范中 `i_blocks` 以 512 字节扇区为单位。每次 alloc_block 后 `i_blocks += block_size / 512`；free_block 后 `i_blocks -= block_size / 512`

## 10. 已知限制

- **superblock 回写性能**: 每次 alloc/free 写入整个 bgdesc 表（`ceil(num_groups * 32 / block_size)` 个 block）。4KB block + 1024 groups (16GB) 时，每次元数据变更多写 8 个 block。未来可优化为仅回写变更的 bgdesc（`bgdesc_table[group]` 写入对应 block 的对应偏移），但需在 `ext2_init` 中缓存 bgdesc 表的物理 block 号映射
- **double/triple indirect**: 不支持——与读路径 `ext2_bmap` 一致。16GB 以下文件系统不受影响（4KB block × 12 direct + 1024 indirect = 4.2MB 单文件上限）。超出此大小的文件将在 `ext2_bmap_alloc` 中返回 0 导致 write 报错
- **时间戳**: `i_atime/i_ctime/i_mtime` 全部填 0（无 RTC 接口）。不影响正确性，但 `ls -l` 显示 1970-01-01
- **不支持硬链接**: `i_links_count` 只在 create/unlink/rmdir 中做 ±1 操作，不提供 `link()` 系统调用对应的 VFS op。ext2 本身支持硬链接但当前无需暴露
