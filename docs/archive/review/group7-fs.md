# 架构评审 — Group 7: 文件系统 + I/O

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/fs/vfs.c`, `vfs.h`, `file.c`, `elf.c`, `tmpfs.c`, `devfs.c`, `procfs.c`, `fat.c`, `ext2.c`, `kernel/block/blockdev.c`, `kernel/include/fs/*.h`, `kernel/include/block/blockdev.h`

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P1 | VFS | `__vfs_lookup` 每次查找都 `calloc` 新 `vfs_node_t`，无缓存 | 待处理 |
| 2 | P1 | VFS | VFS 层无锁（mount list、node 创建/读取、refcount 修改在 SMP 下全无保护） | 待处理 |
| 3 | P1 | tmpfs | tmpfs 所有操作无锁，并发访问损坏 inode 树 | 待处理 |
| 4 | P1 | tmpfs | `tmpfs_vfs_mkdir`/`tmpfs_vfs_create` 不递增 `dir->refcount`，mount root 节点过早释放 | 已修复 |
| 5 | P2 | VFS | `vfs_node_put` 递归释放 `parent`，深目录树可能栈溢出 | 待处理 |
| 6 | P2 | VFS | `vfs_lookup` 路径 $`VFS\_NAME\_MAX` 栈缓冲限制（256 字节） | 待处理 |
| 7 | P2 | tmpfs | `tmpfs_vfs_rmdir` 不检查 "." 和 ".." 安全 | 待处理 |
| 8 | P2 | block | 块设备表无锁，AHCI 无 I/O 调度/并发保护 | 待处理 |
| 9 | P2 | file | `vfs_getdents` 每次系统调用堆分配 32KB+ 数据 | 待处理 |
| 10 | P2 | vfs | `vfs_node_put` 的 `--node->refcount` 非原子 | 待处理 |

---

### [P1] 1. `__vfs_lookup` 每次创建新 `vfs_node_t`

- **位置**: `kernel/fs/vfs.c:222`
- **现象**: 每次 `vfs_lookup` 调用都会为每个路径组件分配新的 `vfs_node_t`（`calloc`），即使该文件已经存在节点。不同的查找操作得到不同的 `vfs_node_t` 实例，指向相同的物理数据。`fs_data` 从 `entry.ino` 填充，每个文件系统自行解释该值。这导致：
  - 每个查找都分配/释放节点，产生 GC 压力
  - `vfs_node_t.refcount` 无法追踪多个路径别名（如硬链接）
  - 不同 `vfs_node_t` 实例同时修改可能导致数据不一致（如在 tmpfs 上同时读写同一文件）
- **建议**: 实现 VFS 缓存（inode cache），对同一物理文件的查找返回相同节点

### [P1] 2. VFS 层在 SMP 下完全无锁

- **位置**: `kernel/fs/vfs.c` 全线
- **现象**:
  - `mount_list` 无锁遍历/修改
  - `__vfs_lookup` 在创建/释放节点时无锁递增/递减 `refcount`
  - `vfs_node_put` 递归释放链无锁保护
  - 两个 CPU 同时 `vfs_lookup("/tmp/foo")` 会创建两个独立的 `vfs_node_t`，然后同时 `vfs_node_put` 递减 refcount
- **建议**: 为 VFS 添加全局锁或精细粒度的 per-mount 锁

### [P1] 3. tmpfs 无锁

- **位置**: `kernel/fs/tmpfs.c` 全线
- **现象**: `tmpfs_node_t` 的所有操作（`children` 数组的增删、`first_block` 链表的修改）全无锁保护。SMP 下并发 `create`/`unlink`/`rename` 会：
  - 损坏 `children` 数组（并发 `kmalloc` + `memcpy` 替换）
  - 损坏 `first_block` 链表
  - 双重释放节点
- **建议**: 为每个 tmpfs mount 添加 spinlock，在所有 VFS ops 入口处加锁

### [P1] 4. `tmpfs_vfs_mkdir`/`tmpfs_vfs_create` 不递增 `dir->refcount`

- **位置**: `kernel/fs/tmpfs.c:281, 328`
- **现象**: `tmpfs_vfs_mkdir` 和 `tmpfs_vfs_create` 在将新节点添加到 `dir->children` 后不执行 `dir->refcount++`。当 `vfs_node_put` 被调用返回 mount root 时（如 `vfs_lookup` 返回后），`dir`（mount root）的 refcount 可能降到 0 导致过早 kfree。ext2 在 `ext2_vfs_mkdir`/`ext2_vfs_create` 中正确执行 `dir->refcount++`。
- **直接后果**: `rmdir`/`unlink`/`rename` on `/tmp`（tmpfs）全部失败——3 个 systest 用例失败，修复后从 **117/122 → 122/122**。
- **修复**: `c14d2a3` — 在 `tmpfs_vfs_mkdir:281` 和 `tmpfs_vfs_create:328` 中添加 `dir->refcount++`。

### [P2] 5. `vfs_node_put` 递归释放

- **位置**: `kernel/fs/vfs.c:324-333`
- **现象**: `vfs_node_put` 在 refcount 归零时 kfree 节点并递归调用 `vfs_node_put(parent)`。对于深度目录树（如 `/a/b/c/d/e/f/g/h`），递归深度可达路径组件数级，内核栈溢出
- **建议**: 改为迭代释放（将 parent 暂存到局部变量，释放当前节点后循环处理 parent）

### [P2] 6. `vfs_lookup` 路径长度限制

- **位置**: `kernel/fs/vfs.c:153, 273`
- **现象**: `__vfs_lookup` 使用栈缓冲 `char path_copy[VFS_NAME_MAX]`（256 字节）。路径超过 256 字节时返回 NULL
- **建议**: 增加 `VFS_NAME_MAX` 或使用动态分配

### [P2] 7. `tmpfs_vfs_rmdir` 不检查 "." 和 ".."

- **位置**: `kernel/fs/tmpfs.c:356-368`
- **现象**: `tmpfs_vfs_rmdir` 不验证 `name` 是否为 "." 或 ".."。如果用户调用 `rmdir("/tmp/.")`，会移除当前目录的父目录（错误行为）
- **建议**: 在开头的有效性检查中拒绝 "." 和 ".."

### [P2] 8. 块设备层无并发保护

- **位置**: `kernel/block/blockdev.c`, `kernel/driver/ahci.c`
- **现象**: `block_device_read`/`block_device_write` 直接调用 AHCI 的 DMA 读写，无 I/O 调度或命令排队。两个 CPU 同时对同一块设备读写会导致 AHCI command list 相互覆盖
- **建议**: 为每个块设备添加 per-device 锁，或实现 minimal I/O scheduler

### [P2] 9. `vfs_getdents` 堆分配开销

- **位置**: `kernel/fs/vfs.c:411-416`
- **现象**: 每次 `getdents64` 系统调用分配 `sizeof(vfs_dirent_t) * 64 + VFS_NAME_MAX * 64` = (64+64*256)≈16KB + 16KB = 32KB 堆内存。用完后立即释放
- **建议**: 使用线程局部缓存或减少 `VFS_GETDENTS_SORT_MAX` 到更实际的值（如 32）

### [P2] 10. `vfs_node_put` 的非原子 refcount 递减

- **位置**: `kernel/fs/vfs.c:327`
- **现象**: `if (--node->refcount == 0)` — 非原子递减。SMP 下两个 CPU 同时调用 `vfs_node_put` 会导致 refcount 少减一次，或两个 CPU 同时看到 refcount==1 后递减到 0 并双重释放节点
- **建议**: 使用 `__sync_fetch_and_sub` 做原子递减，并验证返回值
