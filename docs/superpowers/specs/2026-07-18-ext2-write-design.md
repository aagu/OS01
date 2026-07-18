# ext2 文件系统写入 — 设计方案

> **日期**: 2026-07-18
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
| block 映射 | `ext2_bmap` (direct + single indirect) | `kernel/fs/ext2.c:50` |
| inode 读 | `ext2_read_inode` (group/index → inode table) | `kernel/fs/ext2.c:25` |
| mount | `ext2_init` — superblock 解析、bgdesc 加载、lock 初始化 | `kernel/fs/ext2.c:197` |
| block 设备写 | `block_device_write` → AHCI 写 | `kernel/block/blockdev.c:103` |
| FAT32 完整写 | `fat32_alloc_cluster` + `fat32_write_data` + VFS ops | `kernel/fs/fat.c` |

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

底层缺少：block/inode 分配/释放、inode 回写、superblock 回写、目录项插入/删除。

---

## 2. 总体架构

### 2.1 分层

```
VFS ops (7 个): write / truncate / create / unlink / mkdir / rmdir / rename
       │
       ▼
原语层 (9 个): alloc_block / free_block / alloc_inode / free_inode
              write_inode / write_block / write_superblock
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
uint32_t         bgdesc_block; // bgdesc 表起始 block 号 (cache 避免重算)
```

`ext2_init` 读取 superblock 后将其拷贝到 `fs->sb_raw`，后续 `alloc_block/free_block/alloc_inode/free_inode` 更新其中的计数字段，`ext2_write_superblock` 将其整体序列化回磁盘。

### 3.1 ext2_write_block

```
int ext2_write_block(ext2_fs_t *fs, uint32_t block, const void *buf)

lba = block * sectors_per_block
→ block_device_write(dev, lba, sectors_per_block, buf)
```

`ext2_read_block` 的写镜像。调用者负责传入完整 block 大小的 buffer。

### 3.2 ext2_write_inode

```
int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode)

group = (ino - 1) / inodes_per_group
index = (ino - 1) % inodes_per_group
table_start = bgdesc_table[group].bg_inode_table
→ 读目标 block → memcpy 修改 inode 槽 → ext2_write_block 回写
```

与 `ext2_read_inode` 共享 group/index/offset 计算逻辑，方向反向：从 struct 写入磁盘。

### 3.3 ext2_write_superblock

```
int ext2_write_superblock(ext2_fs_t *fs)

→ 将缓存的 superblock struct（含更新后的 s_free_blocks_count 等计数字段）
  序列化为 1024 字节 → block_device_write(offset=1024, 2 sectors)
→ 同时回写所有 bgdesc 条目（table_blocks 个 block，从 bgdesc_block 起）
```

### 3.4 alloc_block / free_block

**alloc_block**:
```
遍历 block group descriptor → 跳过 bg_free_blocks_count==0 的 group
→ 读 block bitmap → 扫描: 按字节找 != 0xFF → __builtin_ctz 定位 free bit → 置 1
→ 回写 bitmap block（ext2_write_block）
→ bgdesc.bg_free_blocks_count--
→ superblock.s_free_blocks_count--
→ ext2_write_superblock（同时回写 bgdesc table）
→ 将新分配的 block 用 ext2_write_block 全零写回（zero 化）
→ 返回 block number
```

**free_block**:
```
定位 block 所属 group → 读 bitmap
→ 对应 bit 置 0
→ bgdesc.bg_free_blocks_count++
→ superblock.s_free_blocks_count++
→ ext2_write_superblock
```

### 3.5 alloc_inode / free_inode

**alloc_inode(fs, mode)**:
```
扫描 inode bitmap → 找到 free bit → 置 1
→ 回写 bitmap block
→ 递减 s_free_inodes_count / bg_free_inodes_count
→ ext2_write_superblock
→ 读 inode table → 初始化: i_mode=mode, i_links_count=1, i_blocks=0,
  i_size=0, i_block[0..14]清零, i_atime=i_ctime=i_mtime=0
  （内核当前无 RTC 时间戳接口，填 0）
→ ext2_write_inode 回写
→ 返回 inode number
```

**free_inode(fs, ino)**:
```
bitmap 对应 bit 置 0 → 递增 s_free_inodes_count / bg_free_inodes_count
→ ext2_write_superblock
```

### 3.6 dirent_add / dirent_del

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
    → 读下一个目录块（ext2_bmap 逻辑块号 + 1）
    → 检查是否需要 alloc_block 扩展目录
    → 新块: 新 dirent 占用整个块 (rec_len = block_size)

→ 更新父 inode.i_size (如扩展), i_mtime → ext2_write_inode(dir)
```

**dirent_del(fs, dir_ino, name)**:
```
读目录 inode → 遍历 dirent 找到匹配 name 的条目
→ 将前一条的 rec_len += 本条 rec_len（合并回前一条）
→ 可选: memset 覆盖旧数据防残留
→ ext2_write_block 回写目录块
→ ext2_write_inode(dir)
```

---

## 4. VFS 操作

### 4.1 ext2_vfs_write

```
Write(file, offset, size, buffer):
  读 inode → 确保 offset+size 范围内 block 已分配
  → 不足: alloc_block + 更新 i_block[] + write_inode
  → 逐块 RMW: ext2_read_block → memcpy 修改部分 → ext2_write_block
  → 更新 i_size if needed, i_mtime
  → ext2_write_inode
  → 返回写入字节数
```

### 4.2 ext2_vfs_truncate

```
Truncate(node, new_size):
  if new_size > i_size:
    → alloc_block 扩展 → 更新 i_block[] → i_size = new_size
  else:
    → 计算多余 block（从 new_size 向上取整到 block 边界之后的）
    → free_block 逐个释放（含 single indirect 块）
    → i_size = new_size / i_blocks 重新计算
  → write_inode
```

### 4.3 ext2_vfs_create

```
Create(dir, name, mode=0644):
  → alloc_inode(EXT2_S_IFREG | mode)
  → dirent_add(dir_ino, name, new_ino, EXT2_FT_REG_FILE)
  → ext2_write_inode(dir)  // 更新 mtime
  → 返回包装为 vfs_node_t 的新文件节点
```

父目录的 `i_links_count` 仅子目录创建时递增（因 ".."），普通文件创建不增加。

### 4.4 ext2_vfs_unlink

```
Unlink(dir, name):
  → 读目录 inode → 遍历找到目标 dirent → 获取目标 ino
  → dirent_del(dir_ino, name)
  → 读目标 inode → i_links_count--
  → if i_links_count == 0:
      遍历 i_block[0..14] → free_block 每个非零块
      if i_block[12] != 0: 读 indirect block → free_block 每个条目
                           → free_block indirect block 本身
      free_inode(ino)
    else:
      write_inode(ino)  // 只更新 i_links_count
```

### 4.5 ext2_vfs_mkdir

```
Mkdir(dir, name, mode=0755):
  → alloc_inode(EXT2_S_IFDIR | mode)
  → alloc_block → 初始化目录块:
    dirent[0]: inode=新ino, name=".", name_len=1, file_type=2, rec_len=12
    dirent[1]: inode=父ino, name="..", name_len=2, file_type=2,
               rec_len=block_size-12（吞掉剩余全部空间）
  → 父 group bg_used_dirs_count++ → ext2_write_bgdesc（内嵌于 write_superblock）
  → 父 inode.i_links_count++（因为子目录的 ".." 引用）
  → 新 inode: i_links_count=2 (. 和来自父目录的引用)
  → dirent_add(dir_ino, name, new_ino, EXT2_FT_DIR)
  → ext2_write_inode(new) + ext2_write_inode(dir)
  → 返回 vfs_node_t
```

### 4.6 ext2_vfs_rmdir

```
Rmdir(dir, name):
  → 读目标 inode
  → 检查目标目录是否为空: 遍历 dirent 链表，除 "." (ino==自身) 和
    ".." (ino==父或已删除的父) 外不应有其他有效条目（de->inode != 0）
  → 非空: 返回 -ENOTEMPTY
  → dirent_del(dir_ino, name)
  → 父目录 i_links_count--（因为子目录的 ".." 随之消失）
  → ext2_write_inode(dir)
  → 递减父 group bg_used_dirs_count → ext2_write_superblock
  → free_block(目录的数据块)
  → free_inode(目标 ino)
```

### 4.7 ext2_vfs_rename

```
Rename(olddir, oldname, newdir, newname):
  → 在 olddir 中查找 oldname，获取目标 ino 和 file_type
  → if olddir == newdir:
      直接 dirent_del(olddir, oldname) + dirent_add(olddir, newname, ino, type)
      （修改 dirent name 字段在原位置不划算——ext2 dirent 是变长记录，
       不如 del+add 干净。同目录 rename 是低频操作。）
  → else:
      检查 newdir 中是否已有 newname → 有则先 unlink/rmdir
      dirent_add(newdir, newname, ino, type)
      dirent_del(olddir, oldname)
  → 更新被移动的 inode.i_ctime → ext2_write_inode(ino)
  → 如果是目录跨目录移动:
      更新 ".." dirent 指向 newdir → 调整 olddir/newdir 的 i_links_count
      和 bg_used_dirs_count
```

---

## 5. 写入顺序与 Superblock 同步

### 5.1 每条操作路径的写入顺序

```
创建文件:   data blocks → inode → bitmap → superblock → dirent
删除文件:   dirent → inode(i_links_count) → bitmap → superblock → data blocks
扩展文件:   data blocks → bitmap → superblock → inode(i_size)
truncate:   inode(i_size) → bitmap → superblock → data blocks
mkdir:      data block("." / "..") → inode → bitmap → superblock → dirent
```

### 5.2 Superblock 同步

每次元数据变更后立即回写 superblock 和对应的 bgdesc。触发点：
- alloc_block → s_free_blocks_count--, bg_free_blocks_count--
- free_block → s_free_blocks_count++, bg_free_blocks_count++
- alloc_inode → s_free_inodes_count--, bg_free_inodes_count--
- free_inode → s_free_inodes_count++, bg_free_inodes_count++

正常关机/重启下元数据一致。崩溃场景下 bitmap 是权威来源（fsck 可修）。

---

## 6. 测试

### 6.1 内核自测 (KERNEL_SELFTEST)

在 `ext2.c` selftest 区新增。复用现有 AHCI 磁盘上的 ext2 分区，每个测试前后做 save/restore：

```
// save/restore 粒度: 将要修改的 block 先读到临时 buf
// 测试操作 → 验证 → 从临时 buf 写回原 block
//
// 对于 bitmap/inode/dirent 测试: save 目标 block → 操作 → 验证 → restore
// 对于 write/read 测试: save 数据 block + inode → write → read 验证 → restore
```

| 测试 | 内容 |
|------|------|
| `ext2_selftest_block_alloc` | alloc(FREE_ON_DISK) → 验证返回值有效且未变 0 → 立即 free → restore 不变 |
| `ext2_selftest_inode_alloc` | alloc → 验证 ino > 0 → free → restore |
| `ext2_selftest_dirent_roundtrip` | dirent_add → readdir 验证存在 → dirent_del → 验证消失 → restore |
| `ext2_selftest_write_read` | write 到已有文件 → read 回读验证 → restore |

**restore 策略**: 由于 bitmap / inode / dirent 操作都只接触特定 block，save 时读取要被修改的 block 到临时缓冲区，测试结束后写回。不涉及 block alloc/free 的测试（如 write_read）只需 restore inode + data blocks 的内容，不恢复 bitmap。

如果内核自测阶段 AHCI 设备尚未初始化，则这些自测标记为 `SKIP` 而非 `FAIL`。

### 6.2 用户态 systest

在 `/opt/test` 目录下测试（该目录在 ext2 分区上）：
- `open(O_CREAT|O_RDWR)` → `write` → `lseek(SEEK_SET)` → `read` 验证
- `unlink` → `open` 应返回 ENOENT
- `mkdir` → `openat`/`stat` 验证 → `rmdir`
- 跨目录 `rename`
- `truncate` 扩展 + 收缩
- 多 block 文件（写入 > 4KB 验证 indirect block 分配）

### 6.3 Shell 冒烟测试

```sh
echo hello > /opt/test/smoke && cat /opt/test/smoke && rm /opt/test/smoke
mkdir /opt/test/d && rmdir /opt/test/d
```

---

## 7. 实现阶段

| 阶段 | 内容 | 文件 |
|------|------|------|
| 1. Block 写基础 | `ext2_write_block`, `ext2_write_inode`, `ext2_write_superblock` | `kernel/fs/ext2.c` |
| 2. 分配/释放原语 | `alloc_block/free_block`, `alloc_inode/free_inode`, `dirent_add/del` | `kernel/fs/ext2.c` |
| 3. VFS ops | write, truncate, create, unlink, mkdir, rmdir, rename | `kernel/fs/ext2.c` |
| 4. 测试 | 内核自测 + 用户态 systest + shell 冒烟 | `kernel/fs/ext2.c`, `user/systest.c` |

每个阶段结束时进行 make + make test 验证无回归。

---

## 8. 风险与注意事项

- **bitmap 跨块扫描**: 每个 block group 的 bitmap 占 1 个 block，大容量磁盘下需跨多 group 扫描
- **dirent rec_len 可变长度管理**: 插入/删除时 rec_len 调整是 ext2 写入最易出错的点
- **single indirect block 管理**: alloc/free 时需处理 i_block[12] 指向的间接块
- **`make clean`**: 修改 ext2.h 中结构体后必须 clean build
- **spinlock 持有期间不阻塞**: 所有原语内部不调用 schedule/sleep
