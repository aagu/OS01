# ARCH-5：VFS 抽象补强设计（多阶段总控 Spec）

日期：2026-10-24

状态：用户已确认设计方向与三节分阶段契约；本文档定义 ARCH-5 的四阶段目标架构与验收准则，不表示代码已落地。

---

## 1. 目标与范围

对应 [`docs/roadmap.md`](../../roadmap.md) 的 **ARCH-5 VFS 抽象补强**：

- **需要解决的 5 项架构缺陷**：
  1. **无文件系统类型注册表**：`boot.c`、`tmpfs.c`、`devfs.c`、`procfs.c` 直接调用 `vfs_mount(path, dev, &xxx_vfs_ops, fs_data)`，`fat_vfs_ops` 与 `ext2_vfs_ops` 为跨编译单元全局 `extern` 符号，`boot.c` 手工调用 `ext2_init()` / `fat32_init()`。
  2. **操作表未分离且缺 `lookup`/`getattr`**：`vfs_ops_t` 将目录/节点名字空间操作（`create`/`unlink`/`mkdir`/`rmdir`/`rename`/`truncate`/`symlink`/`readlink`）与打开文件流操作（`read`/`write`/`readdir`/`mmap`/`open_file`/`read_file`/`release_file`）混为一体；因缺少 `lookup`，`__vfs_lookup_raw` 靠循环调用 `vfs_readdir(cur, idx, &entry)` 线性扫描（底层 `ext2`/`fat32` 每次从头扫描目录块，单分量复杂度退化为 $O(n^2)$，且受 `max_iter = 256` 硬上限截断）。
  3. **`vfs_node_t` 缺元数据**：仅有 `name`/`size`/`type`/`mount`/`fs_data`/`parent`/`ops`/`refcount`；`vfs_stat()` 硬编码权限位（`0755`/`0600`/`0777`）、`uid=0`、`gid=0`、`nlink=1`、无时间戳，无法透出 `ext2` 磁盘 inode 上已有的 `i_mode`/`i_uid`/`i_gid`/`i_links_count`/`i_atime`/`i_mtime`/`i_ctime`。
  4. **挂载按路径字符串前缀匹配**：`find_mount(path)` 对全局 `mount_list` 做字符串前缀匹配，`vfs_getdents()` 仅在枚举根挂载点时靠 `vfs_is_child_mount()` 字符串比对注入子挂载项，挂载拓扑未绑定在 `vfs_node_t` 上。
  5. **无 Dentry Cache**：每次路径查找均走到底层 FS 并 `calloc` 全新的瞬态 `vfs_node_t`，无目录项缓存与失效机制。

- **明确边界（Non-Goals）**：
  - **不拆分 `vfs_dentry_t` 与 `vfs_inode_t` 指针句柄**：保持 `vfs_node_t *` 作为 VFS 唯一对外节点句柄，避免改动 `file_t->node`、`sys_fs.c`、`elf.c`、`vma.c` 等全内核调用签名。
  - **本期不引入 `permission` 钩子与多用户权限拦截**：`inode_operations` 仅新增 `lookup` 与 `getattr`；权限检查钩子留待 P5 多用户凭证模型落地时引入。
  - **不承担 ARCH-6（FS 并发锁与块缓存）或 ARCH-11（devfs 子目录化）**：本设计保证自身新增注册表、挂载树与 `dcache` 的并发安全，不代劳 `fat.c`/`tmpfs.c` 内部锁或块层缓存。

---

## 2. 现状源码依据

| 位置 | 当前行为 | ARCH-5 目标行为 |
|------|----------|-----------------|
| `kernel/include/fs/vfs.h` | 单一 `vfs_ops_t`；`vfs_node_t` 8 个字段（无 `ino`/`mode`/`uid`/`gid`/`nlink`/时间戳/`mounted_here`） | 新增 `vfs_fs_type_t`；拆分为 `vfs_inode_ops_t` + `vfs_file_ops_t`；补齐 `vfs_node_t` 元数据与 `mounted_here` |
| `kernel/fs/vfs.c` | `find_mount(path)` 字符串前缀匹配；`__vfs_lookup_raw` 循环 256 次调 `vfs_readdir`；`vfs_stat` 硬编码属性 | 按节点挂载树逐分量步进（含 `..` 跨挂载回退）；调 `i_ops->lookup` + `dcache`；`vfs_stat` 走 `i_ops->getattr` 或 `vfs_generic_getattr` |
| `kernel/fs/boot.c` | `fs_boot_prepare()` 在 `/` 挂载前先调 `devfs_init()`；`mount_partitioned_disk()` 直接调 `ext2_init`/`fat32_init` 与全局 `&ext2_vfs_ops`/`&fat_vfs_ops` | 通过 `vfs_mount_fs("ext2"\|"vfat"\|"tmpfs"\|"devfs"\|"procfs", ...)` 挂载；支持 `/dev` 预挂载延迟绑定到 `/` |
| `kernel/fs/ext2.c` | 已有内部 `ext2_find_dirent()` 与 `ext2_read_inode()`，但 VFS 查找仍被迫走 `ext2_vfs_readdir` | 实现 `ext2_iop_lookup`（直接 `ext2_find_dirent` + `ext2_read_inode` 填充完整元数据）与 `ext2_iop_getattr` |
| `kernel/fs/fat.c` | `fat_readdir` 按下标从头遍历簇链；`fat_vfs_ops` 全局暴露 | 实现 `fat_iop_lookup` 单趟簇链查找 + 大小写无关匹配；收敛 `fat_fs_type` |
| `kernel/fs/tmpfs.c` | 已有 `tmpfs_find_child()`，但 VFS 查找走 `tmpfs_vfs_readdir` | 实现 `tmpfs_iop_lookup` 直接调用 `tmpfs_find_child` 并填充节点元数据 |
| `kernel/fs/devfs.c` | `devfs_readdir` 遍历 `devices[]`；`devfs_open_node`/`tty_magic_open` 手工设置 `node->ops` | 实现 `devfs_iop_lookup` 按名查 `devices[]`；标记 `VFS_FS_NO_DCACHE`（因运行期动态注册设备） |
| `kernel/fs/procfs.c` | `procfs_readdir` 承担枚举与查找；无 `stat` 钩子 | 实现 `procfs_iop_lookup`（解析固定项、`/proc/<pid>`、`/proc/<pid>/fd/<n>`）；标记 `VFS_FS_NO_DCACHE` |
| `tools/mkdisk.c` | 仅在根盘创建 `/bin`、`/home`、`/etc`、`/opt`，盘面无 `/dev`、`/proc`、`/tmp`、`/boot` 目录 | VFS 挂载树支持内存虚拟挂载桩（Synthetic Mountpoint Stub），不强制要求底层磁盘预存挂载点目录 |

---

## 3. 目录结构与文件布局

严格遵守 `AGENTS.md` 目录对称规范（`kernel/fs/*.c` ↔ `kernel/include/fs/*.h`）：

| 源文件 | 公开头文件 | 职责 |
|--------|------------|------|
| `kernel/fs/vfs.c` | `kernel/include/fs/vfs.h` | VFS 核心：FS 类型注册表、挂载树管理、路径解析、通用 `vfs_*` 入口与 `vfs_generic_getattr` |
| `kernel/fs/dcache.c`（Phase 4 新增） | `kernel/include/fs/dcache.h`（Phase 4 新增） | 有界 Dentry Cache：`(parent, name)` 哈希表、LRU 淘汰、精确失效与統計计数 |
| `kernel/fs/boot.c` | `kernel/include/fs/boot.h` | 启动期 FS 类型注册触发、块设备注册与根/子文件系统挂载编排 |
| `kernel/fs/{ext2,fat,tmpfs,devfs,procfs}.c` | `kernel/include/fs/{ext2,fat,tmpfs,devfs,procfs}.h` | 各文件系统后端的 `vfs_fs_type_t`、`vfs_inode_ops_t`、`vfs_file_ops_t` 实现 |

---

## 4. 核心数据结构与接口契约

### 4.1 文件系统类型注册（`vfs_fs_type_t`）

```c
// 文件系统特性标志
#define VFS_FS_REQUIRES_DEV      (1u << 0)  // 必须提供非 NULL block_device_t (ext2, vfat)
#define VFS_FS_CASE_INSENSITIVE  (1u << 1)  // 路径分量大小写不敏感 (vfat)
#define VFS_FS_NO_DCACHE         (1u << 2)  // 动态合成目录，禁止 dcache 缓存 (procfs, devfs)

typedef struct vfs_fs_type {
    const char         *name;      // 唯一类型名："ext2", "vfat", "tmpfs", "devfs", "procfs"
    uint32_t            fs_flags;  // VFS_FS_* 位掩码
    // 挂载构造回调：初始化 mp->fs_data 与 mp->root（含 i_ops/f_ops/mode/ino）
    // 成功返回 0，失败返回负数 -errno（如 -EINVAL / -ENOMEM / -EIO）
    int               (*mount)(struct vfs_mount *mp, block_device_t *dev, void *data);
    void              (*unmount)(struct vfs_mount *mp); // 可选清理回调
    struct vfs_fs_type *next;      // 注册表内部链表指针
} vfs_fs_type_t;

int                 vfs_register_filesystem(vfs_fs_type_t *fs_type);
int                 vfs_unregister_filesystem(const char *name);
const vfs_fs_type_t *vfs_find_filesystem(const char *name);
int                 vfs_mount_fs(const char *type_name, const char *path,
                                 block_device_t *dev, void *data);
```

- **契约与错误处理**：
  - `vfs_register_filesystem`：若 `!fs_type || !fs_type->name || !fs_type->name[0] || !fs_type->mount` 返回 `-EINVAL`；若同名类型已注册返回 `-EEXIST`；成功返回 `0`。
  - `vfs_mount_fs`：
    - 若 `!type_name || !path || path[0] != '/'` 返回 `-EINVAL`；
    - 查找 `type_name` 未命中返回 `-ENODEV`；
    - 若 `(fs_type->fs_flags & VFS_FS_REQUIRES_DEV) && !dev` 返回 `-EINVAL`；
    - 调用 `fs_type->mount(mp, dev, data)` 失败时释放 `mp` 并原样传播负数 `-errno`。

### 4.2 `vfs_inode_ops_t` 与 `vfs_file_ops_t` 分离

```c
typedef struct vfs_inode_ops {
    // 在目录 dir 下查找单分量 name（不含 '/'，且已排除 "." 与 ".."）。
    // 成功返回 0 并设置 *out_node（refcount=1，parent=vfs_node_get(dir)）；
    // 未找到返回 -ENOENT；I/O 或分配失败返回 -EIO / -ENOMEM。
    int (*lookup)(struct vfs_node *dir, const char *name, struct vfs_node **out_node);

    // 填充 stat 结构。若为 NULL，vfs_stat() 自动调用 vfs_generic_getattr()。
    int (*getattr)(struct vfs_node *node, struct stat *buf);

    struct vfs_node *(*create)(struct vfs_node *dir, const char *name);
    int              (*unlink)(struct vfs_node *dir, const char *name);
    struct vfs_node *(*mkdir)(struct vfs_node *dir, const char *name);
    int              (*rmdir)(struct vfs_node *dir, const char *name);
    int              (*rename)(struct vfs_node *olddir, const char *oldname,
                               struct vfs_node *newdir, const char *newname);
    int              (*truncate)(struct vfs_node *node, uint64_t new_size);
    int              (*symlink)(struct vfs_node *parent, const char *name,
                                const char *target);
    int              (*readlink)(struct vfs_node *node, char *buf, size_t size);
} vfs_inode_ops_t;

typedef struct vfs_file_ops {
    int     (*read)(struct vfs_node *node, uint64_t offset,
                    uint64_t size, void *buffer);
    int     (*write)(struct vfs_node *node, uint64_t offset,
                     uint64_t size, void *buffer);
    int     (*readdir)(struct vfs_node *node, uint64_t index,
                       struct vfs_dirent *entry);
    int     (*mmap)(struct vfs_node *node, struct vma *vma);

    // per-open 快照生命周期钩子（procfs 统计文件等使用）
    int     (*open_file)(struct file *f);
    int64_t (*read_file)(struct file *f, uint64_t offset,
                         uint64_t size, void *buffer);
    void    (*release_file)(struct file *f);
} vfs_file_ops_t;
```

### 4.3 增强型 `vfs_node_t` 与元数据规则

```c
// node_flags 位定义
#define VFS_NODE_NO_DCACHE   (1u << 0)  // 禁止缓存此目录的子项
#define VFS_NODE_MOUNT_STUB  (1u << 1)  // VFS 合成的虚拟挂载点目录桩

typedef struct vfs_node {
    char                  *name;          // 堆分配节点名（kfree 释放）
    uint64_t               ino;           // inode 编号
    uint64_t               size;          // 文件字节数
    uint32_t               mode;          // S_IFMT 文件类型位 | 权限位 (07777)
    uint32_t               uid;           // 所有者 UID
    uint32_t               gid;           // 所有者 GID
    uint32_t               nlink;         // 硬链接数
    uint64_t               rdev;          // 设备号（VFS_CHRDEV / VFS_BLKDEV）
    uint64_t               atime;         // 最后访问时间（秒）
    uint64_t               mtime;         // 最后修改时间（秒）
    uint64_t               ctime;         // 最后状态变更时间（秒）
    uint8_t                type;          // vfs_node_type_t
    uint8_t                node_flags;    // VFS_NODE_*
    struct vfs_mount      *mount;         // 所属挂载实例
    struct vfs_mount      *mounted_here;  // 挂载在此节点上的子文件系统（非挂载点为 NULL）
    void                  *fs_data;       // FS 私有指针/整数句柄
    struct vfs_node       *parent;        // 父目录节点（持有 refcount）
    const vfs_inode_ops_t *i_ops;         // Inode 操作表
    const vfs_file_ops_t  *f_ops;         // File 操作表
    uint32_t               refcount;      // 原子引用计数
} vfs_node_t;

int vfs_generic_getattr(vfs_node_t *node, struct stat *buf);
```

- **`vfs_generic_getattr(node, buf)` 填充规范**：
  1. `st_ino = node->ino ? node->ino : (uint64_t)(uintptr_t)node->fs_data`；
  2. `st_size = (int64_t)node->size`；
  3. `st_mode`：若 `node->mode != 0`，直接使用 `node->mode`（若未含 `S_IFMT` 位则按 `node->type` 自动或上对应 `S_IFREG`/`S_IFDIR`/`S_IFCHR`/`S_IFBLK`/`S_IFLNK`）；若 `node->mode == 0`，按现有默认规则回退（`VFS_FILE`→`S_IFREG|0755`，`VFS_DIR`→`S_IFDIR|0755`，`VFS_CHRDEV`→`S_IFCHR|0600`，`VFS_BLKDEV`→`S_IFBLK|0600`，`VFS_SYMLINK`→`S_IFLNK|0777`）；
  4. `st_uid = node->uid`、`st_gid = node->gid`；
  5. `st_nlink = node->nlink ? node->nlink : (node->type == VFS_DIR ? 2 : 1)`；
  6. `st_rdev = node->rdev`；
  7. `st_blksize = 512`（或 `ext2` 在自定义 `getattr` 中填 `fs->block_size`），`st_blocks = (node->size + 511) / 512`；
  8. `st_atime = node->atime`、`st_mtime = node->mtime`、`st_ctime = node->ctime`。

### 4.4 节点级挂载结构（`vfs_mount_t`）

```c
typedef struct vfs_mount {
    block_device_t        *dev;
    const vfs_fs_type_t   *fs_type;       // 关联的文件系统类型
    uint32_t               mnt_flags;     // 继承自 fs_type->fs_flags
    const char            *path;          // 规范化挂载路径（堆拷贝，供 vfs_resolve_path 诊断与兼容）
    char                   mount_name[VFS_NAME_MAX]; // 在父目录中的分量名（如 "dev", "proc"）
    struct vfs_node       *root;          // 本挂载文件系统的根节点
    struct vfs_node       *mountpoint;    // 被覆盖的父目录节点（根挂载 "/" 为 NULL）
    struct vfs_mount      *parent_mount;  // 父挂载实例（根挂载 "/" 为 NULL）
    const vfs_inode_ops_t *i_ops;         // 挂载默认 i_ops
    const vfs_file_ops_t  *f_ops;         // 挂载默认 f_ops
    void                  *fs_data;       // FS 私有超块结构
    struct vfs_mount      *next;          // 全局挂载链表
} vfs_mount_t;
```

---

## 5. 四阶段详细设计

### 5.1 Phase 1：`file_system_type` 注册表 + Mount by Type

1. **初始化与注册顺序**：
   - `vfs_init()` 初始化 `mount_list`、`fs_type_list` 及各自的读写锁，随后立即调用各内建 FS 的类型注册函数：
     - `devfs_register_fs()` → 注册 `"devfs"`（`fs_flags = VFS_FS_NO_DCACHE`）
     - `ext2_register_fs()` → 注册 `"ext2"`（`fs_flags = VFS_FS_REQUIRES_DEV`）
     - `fat_register_fs()` → 注册 `"vfat"`（`fs_flags = VFS_FS_REQUIRES_DEV | VFS_FS_CASE_INSENSITIVE`）
     - `tmpfs_register_fs()` → 注册 `"tmpfs"`（`fs_flags = 0`）
     - `procfs_register_fs()` → 注册 `"procfs"`（`fs_flags = VFS_FS_NO_DCACHE`）
2. **各后端 `mount` 回调职责**：
   - `ext2_fs_mount(mp, dev, data)`：内部调用 `ext2_init(dev, &fs)`，设置 `mp->fs_data = fs`，分配并初始化 `mp->root`（`ino = EXT2_ROOT_INO (2)`，`type = VFS_DIR`，从磁盘 inode 2 读取真实 `mode`/`uid`/`gid`/`nlink`/时间戳）。
   - `fat_fs_mount(mp, dev, data)`：内部调用 `fat32_init(dev, &fs)`，设置 `mp->fs_data = fs` 与 `mp->root`。
   - `tmpfs_fs_mount(mp, dev, data)`：分配 `tmpfs_node_t` 根节点并绑定 `mp->fs_data = root`。
   - `devfs_fs_mount` / `procfs_fs_mount`：绑定各自的根节点与操作表。
3. **`boot.c` 改造**：
   - `mount_partitioned_disk()` 改为调用 `vfs_mount_fs("ext2", "/", gpt->partitions[1].dev, NULL)` 与 `vfs_mount_fs("vfat", "/boot", gpt->partitions[0].dev, NULL)`（无 GPT 回退分支调用 `vfs_mount_fs("vfat", "/", disk, NULL)`），保留原有日志字符串以通过 `phase-0` 启动日志断言。

### 5.2 Phase 2：`i_ops`/`f_ops` 分离 + `lookup`/`getattr` + 元数据补齐

1. **拆分策略与调用点更新**：
   - `vfs_read` / `vfs_write` / `vfs_readdir` / `vfs_file_open` / `vfs_file_read` / `vfs_file_release` 以及 `vma.c` 的 `mmap`、`sys_fs.c` 的 `SYS_access` 写能力检查统一走 `node->f_ops`。
   - `vfs_unlink` / `vfs_mkdir` / `vfs_rmdir` / `vfs_rename` / `vfs_truncate` 以及 `sys_fs.c` 的 `sys_symlink` / `sys_readlink` / `open(O_CREAT|O_TRUNC)`、[`vfs_lookup_resolved`](file:///home/aagu/OS01/kernel/fs/vfs.c#L659) 的 `readlink` 统一走 `node->i_ops`。
2. **路径解析核心替换为 `i_ops->lookup`**：
   - 在 `__vfs_lookup_raw` 的分量循环中，处理完 `"."` 与 `".."` 后：
     - 若 `cur->i_ops && cur->i_ops->lookup` 存在，直接调用 `rc = cur->i_ops->lookup(cur, comp, &child)`；
     - 若某测试桩（如旧单测）仅提供 `f_ops->readdir` 而未提供 `i_ops->lookup`，由 `vfs_generic_lookup_by_readdir(cur, comp, &child)` 兜底（保持单测兼容）。
   - 各后端 `lookup` 实现要点：
     - **`ext2_iop_lookup`**：调 `ext2_find_dirent(fs, dir_ino, name, &child_ino, &ft, &blk, &off)` → 未找到返回 `-ENOENT`；找到后调 `ext2_read_inode(fs, child_ino, &disk_ino)`，分配 `vfs_node_t`，填充 `ino = child_ino`、`size = disk_ino.i_size`、`mode = disk_ino.i_mode`、`uid = disk_ino.i_uid`、`gid = disk_ino.i_gid`、`nlink = disk_ino.i_links_count`、`atime = disk_ino.i_atime`、`mtime = disk_ino.i_mtime`、`ctime = disk_ino.i_ctime`。
     - **`fat_iop_lookup`**：单趟遍历目录簇链，使用大小写不敏感匹配找到目标 8.3 目录项，一步构造 `vfs_node_t`（`ino = start_cluster`，`mode = (is_dir ? S_IFDIR|0755 : S_IFREG|0644)`）。
     - **`tmpfs_iop_lookup`**：`tmpfs_node_t` 增补 `mode`/`uid`/`gid`/`mtime` 字段，`tmpfs_find_child` 命中后直接构造 `vfs_node_t`（`ino = (uint64_t)(uintptr_t)tn`）。
     - **`devfs_iop_lookup`**：在 `devices[]` 中按名查找 `registered` 项，构造 `vfs_node_t`（`ino = idx + 1`、`rdev = idx + 1`、`mode = (type == VFS_CHRDEV ? S_IFCHR : S_IFBLK) | 0666`）。
     - **`procfs_iop_lookup`**：将原 `procfs_readdir` 中的三层结构（根 `/proc`、进程目录 `/proc/<pid>`、文件描述符目录 `/proc/<pid>/fd`）提炼为按名精确解析函数，时间复杂度从遍历 toàn 表降为 $O(1)$ PID 定位。

### 5.3 Phase 3：节点级挂载树（`mounted_here` + 虚拟挂载桩 + 延迟绑定）

1. **挂载点绑定与虚拟桩（Synthetic Stub）算法**：
   - 当挂载路径为 `"/"` 时：记为全局 `root_mount`，随后遍历 `pending_submounts` 队列，将所有在 `"/"` 之前注册的子挂载（如 `"/dev"`）逐个绑定到 `root_mount`。
   - 当挂载路径为子路径（如 `"/dev"`、 `"/boot"`、 `"/tmp"`、 `"/proc"` 或多级挂载路径 `"/mnt/usb"`）时：
     - 若 `root_mount == NULL`：将 `mp` 加入 `pending_submounts`（此时直接访问 `"/dev/..."` 仍可通过早期预挂载入口定位到 `mp->root`）。
     - 若 `root_mount != NULL`：
       1. 用 `vfs_split_parent(mp->path, "/", parent_path)` 拆出父目录路径 `parent_path` 与挂载点基名 `mp->mount_name`；
       2. 解析 `parent_path` 得到 `parent_dir` 节点；
       3. 在 `parent_dir` 下查找 `mp->mount_name`：
          - 若底层文件系统存在同名目录节点 `target_node`，则复用该节点（由 `mp->mountpoint` 持久持有 1 个 `refcount`）；
          - 若底层文件系统返回 `-ENOENT`（如盘面无 `/dev`、`/proc`、`/tmp`、`/boot`），则分配一个内存虚拟目录桩节点 `stub`（`type = VFS_DIR`，`mode = S_IFDIR | 0755`，`node_flags = VFS_NODE_MOUNT_STUB`，`parent = vfs_node_get(parent_dir)`，`mount = parent_dir->mount`）；
       4. 设置 `mp_node->mounted_here = mp`、`mp->mountpoint = mp_node`、`mp->parent_mount = parent_dir->mount`。
2. **逐分量步进中的跨挂载转换**：
   - **起点**：从 `root_mount->root` 开始（若启动早期 `root_mount == NULL`，则匹配 `pending_submounts` 中的预挂载根）。
   - **向下跨越（进入子挂载）**：
     - 在目录 `cur` 下查找分量 `comp` 时，先检查 `cur` 是否有绑定的子挂载节点 `mp_node`（其 `mount_name` 与 `comp` 匹配且 `mp_node->mounted_here != NULL`）；
     - 若匹配，直接令 `child = vfs_node_get(mp_node->mounted_here->root)`，跳过底层磁盘的 `i_ops->lookup`（实现挂载遮蔽与虚拟挂载点透明步进）；
     - 若通过普通 `lookup` 拿到的 `child` 自身满足 `child->mounted_here != NULL`，同样自动穿透为 `vfs_node_get(child->mounted_here->root)` 并释放原 `child` 引用。
   - **向上跨越（`".."` 退出子挂载）**：
     - 当遇到分量 `".."` 时，若 `cur == cur->mount->root` 且 `cur->mount->mountpoint != NULL`，先将 `cur` 切换为 `cur->mount->mountpoint`，再跳转到 `cur->parent`（若已在全局根 `"/"` 则原地不动）。
3. **`vfs_getdents` 子挂载合并与去重**：
   - 枚举目录 `dir` 时，第一阶段通过 `dir->f_ops->readdir` 收集底层目录项；
   - 第二阶段检查所有 `mp->mountpoint != NULL && mp->mountpoint->parent == dir`（或当 `dir == dir->mount->root` 且 `mp->parent_mount == dir->mount` 的单层子挂载），若 `entries[]` 中尚未出现同名项（防止磁盘已有 `/dev` 目录时重复列出），则注入该挂载点目录项。
   - 彻底移除 `find_mount(path)` 字符串前缀扫描。

### 5.4 Phase 4：简单有界 Dentry Cache（`dcache`）

```c
#define VFS_DCACHE_BUCKETS   64
#define VFS_DCACHE_CAPACITY  256

void        dcache_init(void);
vfs_node_t *dcache_lookup(vfs_node_t *parent, const char *name);
void        dcache_insert(vfs_node_t *parent, const char *name, vfs_node_t *child);
void        dcache_invalidate(vfs_node_t *parent, const char *name);
void        dcache_invalidate_node(vfs_node_t *node);
void        dcache_flush_mount(vfs_mount_t *mp);
void        dcache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions);
```

1. **身份与键匹配问题（同一目录的指针等价性）**：
   - 哈希与相等性判定以 `(parent->mount, parent_ino_or_ptr, normalized_name)` 为键（其中 `parent_ino_or_ptr = parent->ino ? parent->ino : (uint64_t)(uintptr_t)parent`），这样即便两次查找持有的 `parent` 结构体指针不同但代表同一挂载下的同一目录 inode，也能正确命中与失效！
   - 若 `parent->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE`，哈希计算与名字比较统一转大写后进行。
2. **生命周期与锁顺序**：
   - 每个 `dcache_entry_t` 持有 `vfs_node_get(child)` 的 1 个引用计数；
   - `dcache_lookup` 命中时，在 `dcache_lock` 内将该项移至 LRU 头部并执行 `vfs_node_get(entry->node)`，解锁后返回给调用者；
   - 当容量达到 `VFS_DCACHE_CAPACITY (256)` 需要淘汰 LRU 尾部项，或调用 `dcache_invalidate*` 移除缓存项时，**先在 `dcache_lock` 内摘除条目并保存待释放的 `vfs_node_t *victim`，解锁后再调用 `vfs_node_put(victim)`**，严禁在持有自旋锁期间触发 `kfree` 或递归父节点释放。
3. **绕过与失效钩子**：
   - 若 `parent->node_flags & VFS_NODE_NO_DCACHE` 或 `parent->mount->mnt_flags & VFS_FS_NO_DCACHE`（`procfs`、`devfs`），`dcache_lookup` 直接返回 `NULL`，`dcache_insert` 直接 no-op；
   - `vfs_unlink(path, cwd)` 与 `vfs_rmdir(path, cwd)` 在底层 `i_ops->unlink/rmdir` 成功后调用 `dcache_invalidate(parent, name)`；
   - `vfs_rename(oldpath, newpath, cwd)` 在底层 `i_ops->rename` 成功后调用 `dcache_invalidate(olddir, oldname)` 与 `dcache_invalidate(newdir, newname)`；
   - `vfs_truncate` 或 `vfs_write` 修改文件大小后，若该文件节点在 `dcache` 中存在缓存副本，同步更新缓存节点的 `size`（或由 `vfs_stat` 通过 `i_ops->getattr` 读取最新大小）。

---

## 6. 错误处理与边界条件

| 场景 | 期望返回码 / 行为 |
|------|-------------------|
| 未注册的 FS 类型调用 `vfs_mount_fs("unknown", ...)` | `-ENODEV` |
| `VFS_FS_REQUIRES_DEV` 类型传入 `dev == NULL` | `-EINVAL` |
| 重复注册同名 `vfs_fs_type_t` | `-EEXIST` |
| 目录项超过 256 个时查找第 300 个文件 | Phase 2 起通过 `i_ops->lookup` 正常返回 `0`，不再因 `max_iter=256` 误报 `-ENOENT` |
| 对不支持写/创建的只读 FS（如 `procfs`）调用 `unlink`/`mkdir`/`rmdir`/`rename`/`truncate` | `i_ops` 对应函数指针为 `NULL`，VFS 统一返回 `-EROFS`（`symlink`/`readlink` 保持返回 `-EOPNOTSUPP` 以兼容现有 systest） |
| 跨挂载点 `rename`（`olddir->mount != newdir->mount`） | VFS 层在调用 `olddir->i_ops->rename` 前拦截并返回 `-EXDEV` |
| 跨挂载点 `".."` 解析（如 `/dev/..`、`/boot/..`、`/tmp/..`） | 沿着 `mp->mountpoint->parent` 退回父挂载目录 `"/"` |
| 根挂载 `"/"` 上解析 `".."`（如 `/..`、`/../../bin`） | 停留在 `"/"`，不越界 |
| Symlink 循环与 `LOOKUP_NOFOLLOW` | 保持现有 `MAXSYMLINKS = 8` 返回 `-ELOOP`，尾部分量 `LOOKUP_NOFOLLOW` 返回 symlink 自身节点 |

---

## 7. 分阶段测试与验收矩阵

每个阶段独立合入前必须通过以下三级门禁：

1. **宿主单元测试（`make test-host`）**：
   - **Phase 1**：新增 `hosttests/cases/test_vfs_fs_registry.c`，覆盖 `vfs_register_filesystem`、重复注册 `-EEXIST`、非法参数 `-EINVAL`、`vfs_mount_fs` 的 `REQUIRES_DEV` 检查与未知类型 `-ENODEV`。
   - **Phase 2**：新增 `hosttests/cases/test_vfs_iops_fops.c`，覆盖 `i_ops->lookup`（含 300 项大目录查找无截断）、`i_ops->getattr` 自定义与 `vfs_generic_getattr` 回退、`ext2`/`tmpfs` 元数据字段透出、跨挂载 `rename` 返回 `-EXDEV`。
   - **Phase 3**：新增 `hosttests/cases/test_vfs_mount_tree.c`，覆盖 `/dev` 先于 `/` 挂载的延迟绑定、虚拟挂载桩（Synthetic Stub）透明步进、`/dev/..` 跨挂载回退、`vfs_getdents` 子挂载注入与同名目录去重。
   - **Phase 4**：新增 `hosttests/cases/test_vfs_dcache.c`，覆盖正缓存命中计数、FAT32 大小写不敏感键命中、256 项容量上限 LRU 驱逐与 `refcount` 平衡、`unlink`/`rmdir`/`rename` 失效、`VFS_FS_NO_DCACHE` 绕过。
2. **静态与构建审计（`make test-static`）**：
   - `x86_64-clang` 与 `aarch64-clang` 构建零警告通过。
3. **QEMU 集成与系统调用回归**：
   - `make test-qemu SUITE=phase-0`
   - `make KERNEL_SELFTEST=1 test-kernel-selftest`（验证 `ext2_selftest_*`、`tmpfs_selftest_*`、`symlink_selftest` 全部 PASS）
   - `make OS01_SYSTEST=1 test-syscall`（228 项用户态 syscall 套件零回退）
