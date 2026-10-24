# ARCH-5：VFS 抽象补强设计（多阶段总控 Spec）

日期：2026-10-24

状态：修订稿 v5（基于 v4 评审修订）；本文档定义 ARCH-5 的四阶段目标架构与验收准则，不表示代码已落地或通过运行时验证。

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
  - **不承担 ARCH-6（FS 并发锁与块缓存）或 ARCH-11（devfs 子目录化）**：本设计保证自身新增注册表、挂载树与 `dcache` 的并发安全，包含保障节点查找、登记、删除和最终回收所必需的后端生命周期锁；完整数据路径并发优化与块缓存仍属于 ARCH-6。

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

    // 可选钩子：当 vfs_node_put() 将 node->refcount 减至 0 时（释放 node 内存前）调用，
    // 后端在生命周期锁内摘除 active_nodes 登记，并决定唯一回收权；
    // 返回后通用层才释放 node/name 并 put(parent)，不得提前摘除登记。
    void             (*evict_node)(struct vfs_node *node);
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
#define VFS_NODE_ORPHANED    (1u << 2)  // 节点已被 unlink (nlink==0)，待最后 1 个 vfs_node_put 时由 evict_node 回收底层块/inode/簇
#define VFS_NODE_STALE       (1u << 3)  // 不可恢复 I/O/对象验证失败的失效节点；普通 unlink 不使已打开文件失效

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
    struct vfs_node       *mnt_active_next; // 挂载实例 mp->active_nodes 活跃节点追踪单链表
} vfs_node_t;

int vfs_generic_getattr(vfs_node_t *node, struct stat *buf);

// ── 稳定目录身份（Stable Directory Identity） ─────────────
// 由于 Phase 3 尚无 dcache、Phase 4 允许 LRU 淘汰，不同次路径查找可能为同一底层
// 目录分配不同的 vfs_node_t* 实例。所有跨次查找的挂载点匹配、getdents 子挂载注入
// 与 dcache 失效均以 (node->mount, vfs_node_stable_ino(node)) 作为唯一目录身份，
// 严禁依赖 vfs_node_t* 指针相等性。
#define VFS_SYNTH_ROOT_INO       1ULL
#define VFS_MOUNT_STUB_INO_BASE  0x8000000000000000ULL

static inline uint64_t vfs_node_stable_ino(const vfs_node_t *node)
{
    if (!node) return 0;
    if (node->ino != 0) return node->ino;
    if (node->mount && node == node->mount->root) return VFS_SYNTH_ROOT_INO;
    return (uint64_t)(uintptr_t)node->fs_data;
}
```

- **各后端稳定 `ino` 分配契约（保证任意目录 `vfs_node_stable_ino(dir) != 0` 且跨次查找恒定）**：
  - `ext2`：磁盘 inode 号（根为 `EXT2_ROOT_INO = 2`，子项为 `dirent.inode`）；
  - `fat32`：目录使用起始簇号（根显式设置 `root->ino = fs->root_cluster`）；普通文件使用挂载内单调分配且不复用的 `fat_file_object.object_id`（从 `1ULL << 32` 开始，与 FAT32 目录簇号空间分离），不能使用首簇作为文件身份。对象存活期间，包括空文件、首次写入、truncate(0)、rename 和 unlink，`ino` 均不变；无任何句柄/缓存引用后允许重建对象并获得新编号，本期不承诺重挂载后文件编号稳定。
  - `tmpfs`：持久内存节点地址 `(uint64_t)(uintptr_t)tmpfs_node`（含根节点）；
  - `devfs` / `procfs`：根节点 `ino = VFS_SYNTH_ROOT_INO (1)`；`procfs` 子目录为确定性的 `PROCFS_ENCODE(type, pid)`；
  - 合成挂载桩（`VFS_NODE_MOUNT_STUB`）：分配单调递增的 `VFS_MOUNT_STUB_INO_BASE | stub_id`。

- **`vfs_generic_getattr(node, buf)` 填充规范**：
  1. `st_ino = vfs_node_stable_ino(node)`；
  2. `st_size = (int64_t)node->size`；
  3. `st_mode`：若 `node->mode != 0`，直接使用 `node->mode`（若未含 `S_IFMT` 位则按 `node->type` 自动或上对应 `S_IFREG`/`S_IFDIR`/`S_IFCHR`/`S_IFBLK`/`S_IFLNK`）；若 `node->mode == 0`，按现有默认规则回退（`VFS_FILE`→`S_IFREG|0755`，`VFS_DIR`→`S_IFDIR|0755`，`VFS_CHRDEV`→`S_IFCHR|0600`，`VFS_BLKDEV`→`S_IFBLK|0600`，`VFS_SYMLINK`→`S_IFLNK|0777`）；
  4. `st_uid = node->uid`、`st_gid = node->gid`；
  5. 所有正式后端和合成桩在构造时显式设置 `nlink`；`st_nlink = node->nlink`，保留已删除但仍打开节点的合法值 `0`，不得用默认值覆盖；
  6. `st_rdev = node->rdev`；
  7. `st_blksize = 512`（或 `ext2` 在自定义 `getattr` 中填 `fs->block_size`），`st_blocks = (node->size + 511) / 512`；
  8. `st_atime = node->atime`、`st_mtime = node->mtime`、`st_ctime = node->ctime`。

### 4.4 节点级挂载结构（`vfs_mount_t`）

```c
typedef struct vfs_mount {
    block_device_t        *dev;
    const vfs_fs_type_t   *fs_type;         // 关联的文件系统类型
    uint32_t               mnt_flags;       // 继承自 fs_type->fs_flags
    const char            *path;            // 规范化挂载路径（堆拷贝，供 vfs_resolve_path 诊断与兼容）
    char                   mount_name[VFS_NAME_MAX]; // 在父目录中的分量名（如 "dev", "usb"）
    struct vfs_node       *root;            // 本挂载文件系统的根节点
    struct vfs_node       *mountpoint;      // 被覆盖的挂载点目录节点（持有至根的 parent 链供 ".." 回退；根挂载 "/" 为 NULL）
    struct vfs_mount      *parent_mount;    // 父目录所属的 vfs_mount（根挂载 "/" 为 NULL）
    uint64_t               parent_dir_ino;  // 父目录的稳定身份 vfs_node_stable_ino(parent_dir)（根挂载 "/" 为 0）
    const vfs_inode_ops_t *i_ops;           // 挂载默认 i_ops
    const vfs_file_ops_t  *f_ops;           // 挂载默认 f_ops
    void                  *fs_data;         // FS 私有超块结构
    spinlock_T             active_lock;     // 保护 active_nodes 链表与孤儿状态标记
    struct vfs_node       *active_nodes;    // 已登记的非根节点，含 refcount 已归零但尚未执行 evict 的待摘除节点
    struct vfs_mount      *next;            // 全局挂载链表
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
   - `vfs_unlink` / `vfs_mkdir` / `vfs_rmdir` / `vfs_rename` / `vfs_truncate` 以及 `sys_fs.c` 的 `sys_symlink` / `sys_readlink` / `open(O_CREAT|O_TRUNC)`、`vfs_lookup_resolved` 的 `readlink` 统一走 `node->i_ops`。
2. **路径解析核心替换为 `i_ops->lookup` 与底层对象生命周期契约**：
   - 在 `__vfs_lookup_raw` 的分量循环中，处理完 `"."` 与 `".."` 后：
     - 若 `cur->i_ops && cur->i_ops->lookup` 存在，直接调用 `rc = cur->i_ops->lookup(cur, comp, &child)`；
     - 若某测试桩（如旧单测）仅提供 `f_ops->readdir` 而未提供 `i_ops->lookup`，由 `vfs_generic_lookup_by_readdir(cur, comp, &child)` 兜底（保持单测兼容）。
   - **原子登记与失败回滚（所有节点构造入口共同遵守）**：
     - `lookup` / `create` / `mkdir` / symlink 后续查找，以及后端直接返回节点的辅助入口，均须在后端生命周期锁内完成「定位或发布目录项 → 固定底层资源/获取引用 → 初始化节点 → 在 `mp->active_lock` 下登记」，登记完成后才能释放后端锁、向调用者返回。ext2 使用现有 `fs->lock`；FAT 增加挂载私有 `fat_lifecycle_lock`；tmpfs 使用 `tmpfs_lock`。不得仅保护 lookup 而让 create/mkdir 在解锁后构造与登记。
     - 可在锁外预分配候选内存，进入后端锁后重新校验目录项；若采用预留底层引用后锁外构造，预留必须作为可被 unlink 看见的资源引用计入同一生命周期协议，不能只保存在局部裸指针中。未完成构造不发布半初始化节点。
     - 删除定位、目录项摘除、引用计数/孤儿状态变更与登记使用同一后端锁；FAT 的查找与删除不得只靠事后的 `active_lock` 扫描。分配失败时撤销登记/引用并回滚尚未成功发布的 create/mkdir；已摘除目录项但清理 I/O 失败时保留待回收记录，不得重新发布路径或提前复用资源。
   - **最终摘除与唯一回收权**：
     - `vfs_node_put` 原子减至零后，直接调用 `i_ops->evict_node(node)`，通用层不得预先摘除 active 登记。节点为零引用后禁止复活，但在 evict 完成前内存与登记仍存在；扫描者只能在锁内读取状态，不能对零引用节点调用 `vfs_node_get`。
     - ext2/FAT 的 evict 按「后端生命周期锁 → `mp->active_lock`」摘除自己，并在同一临界区判定是否是同资源的最后一个登记/预留。只有该次最后摘除且资源已孤儿化的调用取得唯一回收权；其他 evict 不得解锁后再次以“列表为空”决定释放。
     - 回收者释放 `active_lock`，仍在后端生命周期锁下执行 inode/块/簇回收并完成一次性状态转移，再释放后端锁。若 unlink 在后端锁内发现没有任何登记/预留，它自己成为唯一回收者。零引用但尚未 evict 的登记必须计入，因此 unlink 与最终 put 不会各自回收一次。I/O 失败的待回收状态保留资源占用并可重试，禁止第二个调用重复清理已成功释放的部分。
     - 不持有 `active_lock` 或 `dcache_lock` 执行磁盘 I/O、分配/释放内存、递归 `put(parent)`。evict 返回后通用层才释放 VFS 节点与名字，再释放 parent；父节点递归 evict 因而不会递归获取仍持有的后端锁。动态伪文件系统与合成桩使用通用摘除逻辑，不对合成桩调用磁盘 inode 回收函数。
   - **各后端 `lookup` 与延迟回收实现**：
     - **ext2**：在 `fs->lock` 内 `ext2_find_dirent`、读取 inode、构造并登记；create/mkdir 同样在目录项发布到解锁之间完成登记。unlink/rmdir/rename 覆盖目标导致 `i_links_count` 降为零时，在同一锁内删除名字并将全部对应登记节点标记 `VFS_NODE_ORPHANED`、`nlink=0`；有登记/预留则保留 inode 与数据块，无登记则执行唯一回收。evict 遵守上述最后摘除协议。保留未链接 inode 的实时大小，旧句柄继续 read/write/getattr；硬链接尚存时不置孤儿，不回收 inode。
     - **FAT 共享文件对象（后端私有，不新增 VFS 对外 inode 句柄）**：
       - 普通文件 `node->fs_data` 指向 `fat_file_object`，至少含不变 `object_id`、当前 `first_cluster`、`size`、目录项定位 `(parent_cluster, lba, offset)`、`linked/orphaned/reclaiming` 状态及登记/预留引用。目录仍使用目录簇号，由节点类型选择解析方式，不能把普通文件 `fs_data` 强转为簇号。
       - 在 `fat_lifecycle_lock` 下按已链接目录项定位查找或创建唯一共享对象，再构造登记节点。多个空文件拥有不同 object_id，同一文件所有同时存活句柄共享同一对象；根、lookup、create、read/write、truncate、getattr、rename、unlink、evict 及目录项更新辅助函数须一起迁移。
       - 首次分配簇或 truncate(0) 只修改共享对象的 `first_cluster/size`，保持 object_id；所有句柄读取共享对象中的当前簇链，禁止使用各自旧簇副本。截断释放簇后，旧句柄从共享对象读取零首簇，不能访问被其他文件复用的簇。read/write/truncate/getattr 从读取共享对象到最后一次簇链访问均持有 `fat_lifecycle_lock`；不能先取出首簇再解锁执行 I/O，防止在途 read 使用已由 truncate 释放并被其他文件复用的簇。
       - unlink 在同一后端锁内将目录项置 `0xE5`，把对象从“已链接目录项定位索引”摘除并置 orphaned，随后禁止其所有句柄回写该目录项槽位；新文件即使复用同一槽也得到新 object_id。已打开的孤儿文件（含未分配首簇的空文件）仍可 read/write/truncate/getattr，在共享对象内分配或释放簇且不回写目录项；最后一个登记/预留释放时唯一回收剩余簇链和对象。
       - rename 成功时在同一后端锁内原子更新对象的目录项定位索引；覆盖目标先执行目标对象的孤儿协议。零句柄且仍 linked 的对象可移出索引并释放内存，目录项与簇链继续存在；重建对象从当前目录项读取状态。
     - **tmpfs**：`tmpfs_node_t` 挂入 `children[]` 时持有一个目录引用。在 `tmpfs_lock` 内定位子项、获取底层引用并完成 VFS 节点登记；create/mkdir 的发布与登记也在同一锁内。unlink/rmdir 在锁内摘除目录引用、更新 `nlink`，保留已打开节点；rename/覆盖及 children 数组变更同样服从此锁。evict 在后端锁内摘除 active 登记、递减底层引用，最后引用在释放锁后回收块和内存。候选分配失败必须回滚引用与未发布目录项。
     - **`devfs_iop_lookup`**：在 `devices[]` 中按名查找 `registered` 项，构造 `vfs_node_t`（`ino = idx + 1`、`rdev = idx + 1`、`mode = (type == VFS_CHRDEV ? S_IFCHR : S_IFBLK) | 0666`）。
     - **`procfs_iop_lookup`**：将原 `procfs_readdir` 中的三层结构（根 `/proc`、进程目录 `/proc/<pid>`、文件描述符目录 `/proc/<pid>/fd`）提炼为按名精确解析函数，时间复杂度从遍历全表降为 $O(1)$ PID 定位。
3. **写入与截断后的元数据（`size`/`mtime`/`blocks`）一致性契约**：
   - **后端实时 `getattr`**：
     - `tmpfs_iop_getattr(node, buf)` 直接从底层 `tmpfs_get_node(node)`（`tmpfs_node_t *tn`）读取实时 `tn->size`、`tn->mtime` 并同步回 `node->size = tn->size`，再调用 `vfs_generic_getattr(node, buf)`；
     - `ext2_iop_getattr(node, buf)` 直接调用 `ext2_read_inode(fs, node->ino, &disk_ino)` 读取最新磁盘 `i_size`、`i_blocks`、`i_links_count`、`i_mtime` 并同步回 `node->size`，再填充 `buf`；
     - `fat_iop_getattr(node, buf)` 读取最新大小并填充 `buf`。
   - **VFS 写/截断同步**：
     - `vfs_write` / `vfs_truncate` 在后端生命周期锁内完成底层变更后，从权威 inode/共享对象取得实际大小并同步所有同身份的登记节点（含缓存持有节点）。即使后端已更新当前 `node->size`，也不得跳过其他节点的同步；不得在释放后端锁后用旧写入的 `offset+ret` 覆盖并发 truncate 的结果。部分成功随后报错的写入同样以底层实际大小刷新；
     - 实时 getattr 也在后端锁内读取并同步 `size/nlink/mtime/blocks`；FAT 从共享对象读取，孤儿文件不得尝试重新按旧目录项定位读取。直接消费 `node->size` 的 lseek/ELF 等调用点必须使用同一受保护的大小读取接口或实时 getattr。
     - 由此保证：**缓存节点、已打开的独立 `vfs_node_t` 句柄、以及从缓存淘汰后重新查找构造的节点**三者在 `write`/`truncate` 前后执行 `vfs_stat` 始终获得一致的最新文件大小。

### 5.3 Phase 3：节点级挂载树（稳定父目录身份索引 + 虚拟挂载桩 + 延迟绑定）

1. **挂载点绑定、稳定父目录身份与虚拟桩（Synthetic Stub）算法**：
   - 当挂载路径为 `"/"` 时：记为全局 `root_mount`（`parent_mount = NULL`，`parent_dir_ino = 0`），随后遍历 `pending_submounts` 队列，将所有在 `"/"` 之前注册的子挂载（如 `"/dev"`）逐个绑定到 `root_mount`。
   - 当挂载路径为子路径（如 `"/dev"`、 `"/boot"`、 `"/tmp"`、 `"/proc"` 或多级挂载路径 `"/mnt/usb"`）时：
     - 若 `root_mount == NULL`：将 `mp` 加入 `pending_submounts`（此时直接访问 `"/dev/..."` 仍可通过早期预挂载入口定位到 `mp->root`）。
     - 若 `root_mount != NULL`：
       1. 用 `vfs_split_parent(mp->path, "/", parent_path)` 拆出父目录路径 `parent_path` 与挂载点基名 `mp->mount_name`；
       2. 解析 `parent_path` 得到 `parent_dir` 节点（若 `parent_dir` 不存在或非 `VFS_DIR`，返回 `-ENOENT` / `-ENOTDIR`）；
       3. 在 `parent_dir` 下查找 `mp->mount_name`：
          - 若底层文件系统存在同名目录节点 `target_node`，则复用该节点（由 `mp->mountpoint` 持久持有 1 个 `refcount`，从而锚定从该挂载点向上直到 `/` 的完整 `parent` 链）；
          - 若底层文件系统返回 `-ENOENT`（如盘面无 `/dev`、`/proc`、`/tmp`、`/boot`），则分配一个内存虚拟目录桩节点 `stub`（`type = VFS_DIR`，`ino = VFS_MOUNT_STUB_INO_BASE | next_stub_id`，`mode = S_IFDIR | 0755`，`node_flags = VFS_NODE_MOUNT_STUB`，`parent = vfs_node_get(parent_dir)`，`mount = parent_dir->mount`）；
       4. 记录**稳定父目录身份**与挂载绑定：
          - `mp->parent_mount = parent_dir->mount`
          - `mp->parent_dir_ino = vfs_node_stable_ino(parent_dir)`
          - `mp->mountpoint = mp_node`
          - `mp_node->mounted_here = mp`
2. **逐分量步进中的跨挂载转换（不依赖 `vfs_node_t*` 指针相等）**：
   - **起点**：从 `root_mount->root` 开始（若启动早期 `root_mount == NULL`，则匹配 `pending_submounts` 中的预挂载根）。
   - **向下跨越（进入子挂载）**：
     - 在目录 `cur` 下查找分量 `comp` 时，先调用 `vfs_find_submount(cur, comp)`，在 `mount_lock` 读锁下按**稳定目录身份**匹配：
       `mp->parent_mount == cur->mount && mp->parent_dir_ino == vfs_node_stable_ino(cur) && vfs_mount_name_eq(cur->mount, mp->mount_name, comp)`；
     - 由于匹配只依赖 `(cur->mount, vfs_node_stable_ino(cur), comp)`，无论 `cur`（例如 `/mnt`）是首次查找新分配的节点、Phase 3 无缓存下重新查找分配的新节点、还是 Phase 4 从 `dcache` 淘汰后重建的节点，均能确定性地命中子挂载 `mp`（如 `/mnt/usb`），直接令 `child = vfs_node_get(mp->root)` 并跳过底层磁盘的 `i_ops->lookup`；
     - 同一路径已有挂载则返回 `-EBUSY`；本期不支持 overmount，避免挂载栈的额外拓扑与回退契约。
   - **向上跨越（`".."` 退出子挂载）**：
     - 当遇到分量 `".."` 时，若 `cur == cur->mount->root && cur->mount->mountpoint != NULL`，循环将 `cur` 切换为 `cur->mount->mountpoint`（其 `parent` 链在挂载时已被持久锚定），再跳转到 `cur->parent`（若已在全局根 `"/"` 则原地不动）。
3. **挂载点及其祖先目录的 `rename`/`rmdir` 保护与 `vfs_namespace_lock` 串行化契约**：
   - 由于 `mp->mountpoint` 持久锚定了挂载时的祖先链 `mp->mountpoint → ... → root`，若允许移动或删除挂载点自身或其任何祖先目录（例如挂载 `/a/d/usb` 后执行 `rename("/a/d", "/b/d")` 或 `rename("/a", "/x")`），会导致 `mp->mountpoint` 的向上 `".."` 链与实际目录树位置脱节。
   - **全局名字空间串行化锁 `mutex_t vfs_namespace_lock`**（独立于保护 `mount_list` 读遍历的 `mount_lock`，且普通路径查找 `vfs_lookup*` **不获取** `vfs_namespace_lock`，因此挂载内部调用 `vfs_lookup` 无递归死锁风险）：
     - **挂载发布区间**：`vfs_mount_fs` / `vfs_mount` 在持有 `vfs_namespace_lock` 期间完成「解析 `parent_dir` → 查找/创建 `mp_node` → 绑定 `mp->mountpoint` 与 `parent_dir_ino` → 写入 `mount_list`」全过程；
     - **目录变更区间**：`vfs_rename` 与 `vfs_rmdir` 在解析父目录路径前，**获取并持有同一把 `vfs_namespace_lock` 直至底层 `i_ops->rename/rmdir` 及 `dcache` 失效全部完成**：
       1. 在锁内解析父目录并校验仍 linked，禁止用锁外解析所得的陈旧祖先链执行变更；
       2. 在锁内调用 `bool vfs_is_mount_or_ancestor(vfs_node_t *dir, const char *name)`，遍历 `mount_list` 中所有 `mp->mountpoint != NULL` 的挂载项，沿 `for (vfs_node_t *anc = mp->mountpoint; anc && anc->parent; anc = anc->parent)` 向上检查：
          `anc->parent->mount == dir->mount && vfs_node_stable_ino(anc->parent) == vfs_node_stable_ino(dir) && vfs_mount_name_eq(dir->mount, anc->name, name)`；
       3. 若命中（`vfs_rmdir` 目标命中，或 `vfs_rename` 源/目标任一命中），立即释放 `vfs_namespace_lock` 并返回 **`-EBUSY`**；
       4. 若未命中，在保持持有 `vfs_namespace_lock` 的状态下完成 `dcache_mutation_begin()` → `i_ops->rename/rmdir` → `dcache_mutation_end_*()`，最后释放 `vfs_namespace_lock`。
     - 由此保证“挂载祖先检查 → 目录变更完成”与“新子挂载解析及发布”严格互斥，绝无并发挂载插入检查与变更之间的竞争窗口。
4. **`vfs_getdents` 子挂载合并与去重（统一稳定父目录身份判定）**：
   - 枚举目录 `dir` 时，第一阶段通过 `dir->f_ops->readdir` 收集底层目录项；
   - 第二阶段在 `mount_lock` 读锁下遍历 `mount_list`，仅当子挂载满足**单一精确谓词**：
     `mp->parent_mount == dir->mount && mp->parent_dir_ino == vfs_node_stable_ino(dir)`
     时，才将该子挂载的 `mp->mount_name` 视为 `dir` 的直接子挂载（例如 `/mnt/usb` 的 `parent_dir_ino` 是 `/mnt` 的 `ino` 而非 `/` 的 `ino`，因此 `ls /` 绝不会错误列出 `usb`，而 `ls /mnt` 精确列出一次 `usb`）；
   - 若 `entries[]` 中尚未出现同名项（防止磁盘已有 `/dev` 目录时重复列出），则注入该挂载点目录项。
   - 彻底移除 `find_mount(path)` 字符串前缀扫描与 `vfs_is_child_mount` 路径字符串比对。

### 5.4 Phase 4：简单有界 Dentry Cache（`dcache`）

```c
#define VFS_DCACHE_BUCKETS   64
#define VFS_DCACHE_CAPACITY  256

void        dcache_init(void);
vfs_node_t *dcache_lookup_begin(vfs_node_t *parent, const char *name,
                                uint64_t *out_gen);
void        dcache_insert_validated(vfs_node_t *parent, const char *name,
                                    vfs_node_t *child, uint64_t lookup_gen);
void        dcache_mutation_begin(void); // 仅提升代次与 active，不承担按名失效
void        dcache_mutation_end_invalidate(vfs_node_t *parent, const char *name);
void        dcache_mutation_end_rename(vfs_node_t *olddir, const char *oldname,
                                       vfs_node_t *newdir, const char *newname);
void        dcache_flush_mount(vfs_mount_t *mp);
void        dcache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions);
```

1. **键结构与父链一致性（解决 `rename` 后代 `parent` 链陈旧与父节点淘汰重建问题）**：
   - **索引键**：以 `(parent->mount, vfs_node_stable_ino(parent), normalized_name)` 哈希和匹配（若 `parent->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE`，名字统一转大写后计算哈希与比较）。
   - **命中时的父指针强校验**：`dcache_lookup_begin(parent, name, &out_gen)` 命中缓存条目 `entry` 时，额外检查 `entry->node->parent == parent`：
     - 若相等，移动至 LRU 头部、`vfs_node_get(entry->node)` 并返回命中节点；
     - 若 `entry->node->parent != parent`（说明父目录节点曾被淘汰重建或发生过目录迁移），立即将该陈旧条目从缓存摘除（锁外 `vfs_node_put`）并按未命中处理，由调用方基于当前 `parent` 重新构造子节点。这保证了任何从 `dcache` 返回的节点其 `node->parent` 链自顶向下严格一致，且 `node->parent` 在节点生命周期内保持只读不变，无需对 `vfs_resolve_path` 或 `".."` 加额外锁。
   - **失效键**：`dcache_mutation_end_invalidate(dir, name)` 仅按 `(dir->mount, vfs_node_stable_ino(dir), normalized_name)` 匹配并摘除条目，不要求传入的 `dir` 指针与缓存时的 `parent` 指针相同。
2. **并发 Lookup 与 Unlink/Rmdir/Rename 竞态防护（代次屏障 + 锁内取引用 + 孤儿延迟回收三重保护）**：
   - **第一重保护（防止旧结果回填 `dcache`）**：`dcache` 内部维护受 `dcache_lock` 保护的 `uint64_t dcache_mut_gen`（初值为 1，0 保留为禁止插入标记；溢出时清空缓存并使在途 token 全部失效）与 `uint32_t dcache_mut_active`（活跃目录变更计数）：
     1. **查找开始**：`dcache_lookup_begin(parent, name, &lookup_gen)` 未命中时，在 `dcache_lock` 内若 `dcache_mut_active == 0` 则记录 `*lookup_gen = dcache_mut_gen`，否则置 `*lookup_gen = 0`。
     2. **变更围栏**：`vfs_unlink` / `vfs_rmdir` / `vfs_rename` 在调用底层 `i_ops->unlink/rmdir/rename` **之前**调用 `dcache_mutation_begin()`（`dcache_mut_active++; dcache_mut_gen++;`）；随后在调用后端前以独立的按名失效步骤摘除对应缓存项（rename 清理源和目标，必要时清空该挂载）并在锁外 put；在底层操作返回**之后**调用 `dcache_mutation_end_invalidate(...)` 或 `dcache_mutation_end_rename(...)`（再次清理并执行 `dcache_mut_gen++; dcache_mut_active--;`）。
     3. **验证插入**：`dcache_insert_validated(parent, name, child, lookup_gen)` 仅当 `lookup_gen != 0 && dcache_mut_active == 0 && dcache_mut_gen == lookup_gen` 时才允许写入缓存；否则放弃入缓存。
   - **第二重保护（`tmpfs` 锁内原子获取引用）**：
     - `tmpfs_iop_lookup` 在 `tmpfs_lock` 保护下完成目录查找与 `tmpfs_node_get(child)`，与 `tmpfs_vfs_unlink` 摘除目录项互斥，不存在读出指针后尚未增加引用即被释放的窗口。
   - **第三重保护（`ext2`/`fat32` 孤儿资源延迟回收与失效保护）**：
     - 如 §5.2.2 所述，当已打开或并发查找持有的 `vfs_node_t` 遭遇 `unlink` 时，后端在生命周期锁内将对应节点/共享对象标记为 `VFS_NODE_ORPHANED` 或 orphaned，`ext2` 的磁盘 inode/数据块与 `fat32` 的已分配簇链均推迟到唯一最后摘除者执行 `evict_node` 时才真正释放回位图/FAT 表，因此后续 `create(B)` 绝不会复用正在被旧句柄使用的 inode 或簇链，彻底杜绝交叉写坏新文件。
3. **Rename 子树与后代 `parent` 链失效策略**：
   - 当执行 `vfs_rename(oldpath, newpath, cwd)` 时（在 `vfs_namespace_lock` 保护下通过 §5.3.3 的 `-EBUSY` 挂载点及祖先检查后），被移动目录的所有已缓存后代（如 `/a/d/sub`）其 `parent` 链和祖先 `name` 均已失效；
   - 由于 `VFS_DCACHE_CAPACITY` 固定为 256 项，`dcache_mutation_end_rename` 在 `dcache_lock` 内直接调用 `dcache_flush_mount_locked(olddir->mount)` 清空该挂载下的缓存项（固定 256 槽扫描，耗时须实测，不承诺微秒上限），并在解锁后批量 `vfs_node_put` 被摘除的节点，从根源上杜绝任何深度的后代残留旧 `/a/d` 父链。
4. **锁外释放原则与绕过规则**：
   - 所有淘汰（LRU 满 256 项）、失效（`invalidate` / `flush_mount`）及父指针不匹配摘除操作，均在 `dcache_lock` 内将条目从哈希桶和 LRU 链表摘下放入局部待释放数组，**释放 `dcache_lock` 后再调用 `vfs_node_put`**，严禁在自旋锁内触发 `kfree` 或递归父引用释放。
   - 若 `parent->node_flags & VFS_NODE_NO_DCACHE` 或 `parent->mount->mnt_flags & VFS_FS_NO_DCACHE`（`procfs`、`devfs`），`dcache_lookup_begin` 直接返回 `NULL`（`*out_gen = 0`），`dcache_insert_validated` 直接 no-op。

---

## 5.5 锁顺序、注册寿命与分阶段过渡

- **锁顺序**：运行期 `vfs_namespace_lock`（可睡眠 mutex）→ 后端生命周期锁 → `active_lock`。`mount_lock` / `dcache_lock` 仅保护短时内存操作，不跨后端调用、I/O、分配或 put；从这些锁获得的节点引用在解锁后使用。不得从后端 evict 再获取 namespace mutex。ext2 沿用现有后端锁，FAT 新增生命周期锁，完整锁序需在实现中静态审计。
- **启动期**：调度器就绪前由单一 BSP 顺序完成挂载，可用显式 `vfs_bootstrap` 分支绕过 namespace mutex；必须在放行并发 VFS 调用前结束 bootstrap，后续统一使用 mutex。不能在无 current 的早期路径调用依赖任务 owner 的 mutex。
- **名字空间写操作**：mount、create、mkdir、symlink、unlink、rmdir、rename 均由最外层 VFS/系统调用适配入口获取同一 namespace mutex，锁内解析父路径并执行变更；嵌套内部 helper 使用 `_locked` 形式，不重复加锁。`vfs_lookup*` 及后端 lookup 不取该 mutex。当前 `sys_fs.c` 中绕过 VFS 的直接 create/symlink 调用必须迁移到相同入口。后端生命周期锁仍保护 lookup 与删除之间的原子登记。
- **FS 类型寿命**：内建 `vfs_fs_type_t` 及操作表为静态存储，返回指针不允许调用者释放；unregister 只摘除注册表，有已发布或正在构造的挂载时返回 `-EBUSY`。挂载构造先在注册表锁内预留使用计数，再解锁调用 mount；失败清理全部私有分配和根节点并放下预留，成功转换为挂载引用。本期无运行期卸载与动态模块释放。
- **阶段门禁**：Phase 1 保留 `vfs_ops_t` 与旧 mount 签名作过渡，注册表回调填旧 ops；Phase 2 才统一切换 `i_ops/f_ops` 和元数据、生命周期协议并删除旧 ops；Phase 3 才切换挂载树；Phase 4 才增加 dcache。Phase 2/3 的缓存失效 helper 为 no-op，不得引用尚不存在的 dcache 对象；通用生命周期和 namespace 锁在 Phase 2 引入，Phase 3 扩展挂载发布保护。每次节点/挂载/后端结构变更均先对两个 profile 执行 `make clean`，再独立构建和验收。
- **挂载限制**：本期同一路径重复挂载返回 `-EBUSY`，不支持 overmount；挂载目标为已有非目录返回 `-ENOTDIR`。pending 挂载按父路径深度绑定，失败不发布半绑定项；根挂载在所有 pending 绑定成功后统一发布，失败回滚整个此次发布。早期 pending 查找保留独立入口，支持绑定前已注册 `/dev` 的访问。

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
| 对活跃挂载点或其祖先目录执行 `rename` / `rmdir`（或与 `vfs_mount` 并发竞争） | `vfs_namespace_lock` 串行化挂载发布与目录变更，`vfs_is_mount_or_ancestor` 命中返回 `-EBUSY`，保护挂载点锚定的向上 `".."` 链不被破坏 |
| 多级子挂载 `/mnt/usb` 重新查找父目录 `/mnt` 或缓存淘汰后再查找 | 通过 `(cur->mount, vfs_node_stable_ino(cur), "usb")` 稳定匹配进入 `usb` 挂载根 |
| 多级子挂载 `/mnt/usb` 下对 `/` 与 `/mnt` 执行 `vfs_getdents` | `/` 的目录列表中**不出现** `usb`；`/mnt` 的目录列表中**精确出现一次** `usb` |
| `open(A) → unlink(A) → create(B) → write(old_fd)`（含 `tmpfs`/`ext2`/`fat32`） | `A` 标记为 `VFS_NODE_ORPHANED`，其 `tmpfs_node_t` / `ext2` inode+块 / `fat32` 簇链延迟至最后登记/预留释放（唯一回收者）才释放，`B` 分配全新资源，互不污染 |
| 文件写入或截断后对缓存节点、打开句柄、淘汰重建节点执行 `vfs_stat` | 通过后端锁内同步所有登记节点及 `i_ops->getattr` 实时读取，三者返回一致的最新 `st_size` |
| 目录 `rename`（`/a/d` → `/b/d`）后解析 `/b/d/sub/../..` 与 `vfs_resolve_path` | 缓存子树随 `dcache_flush_mount` 失效重建，`/b/d/sub/../..` 落到 `/b`，`vfs_resolve_path` 返回 `/b/d/sub` |
| 跨挂载点 `".."` 解析（如 `/dev/..`、`/boot/..`、`/mnt/usb/..`） | 沿着 `mp->mountpoint->parent` 退回父挂载对应目录（`/` 或 `/mnt`） |
| 根挂载 `"/"` 上解析 `".."`（如 `/..`、`/../../bin`） | 停留在 `"/"`，不越界 |
| Symlink 循环与 `LOOKUP_NOFOLLOW` | 保持现有 `MAXSYMLINKS = 8` 返回 `-ELOOP`，尾部分量 `LOOKUP_NOFOLLOW` 返回 symlink 自身节点 |

---

## 7. 分阶段测试与验收矩阵

每个阶段独立合入前必须通过以下三级门禁：

1. **宿主单元测试（`make test-host`）**：
   - **Phase 1**：新增 `hosttests/cases/test_vfs_fs_registry.c`，覆盖 `vfs_register_filesystem`、重复注册 `-EEXIST`、非法参数 `-EINVAL`、`vfs_mount_fs` 的 `REQUIRES_DEV` 检查与未知类型 `-ENODEV`。
   - **Phase 2**：新增 `hosttests/cases/test_vfs_iops_fops.c`，覆盖：
     - `i_ops->lookup`（含 300 项大目录查找无截断）；
     - `i_ops->getattr` 自定义与 `vfs_generic_getattr` 回退、`ext2`/`tmpfs` 元数据字段透出；
     - `tmpfs` 锁内取引用与 `evict_node` 生命周期：验证查找加引用与 `unlink` 摘除受 `tmpfs_lock` 互斥保护、分配 `vfs_node_t` 失败时引用正确回滚，以及持有 `vfs_node_t` 期间 `unlink` 不释放底层块、`vfs_node_put` 后计数归零回收；
     - 所有后端 lookup/create/mkdir 的登记前确定性交错：删除必须等待登记完成，或查找返回 `-ENOENT`；不得返回已回收资源；覆盖 ext2 create 在目录项发布之后、登记之前暂停，以及 FAT lookup 定位之后暂停。
     - 两个独立同身份 VFS 节点在 unlink 后同时最终 put：强制交错摘除与清理，资源回收函数恰好执行一次；另测 unlink 与零引用待 evict 节点竞争。
     - FAT 多空文件身份隔离、同文件多句柄首次写入与 truncate(0) 后读写、目录项槽复用、rename 后定位更新、孤儿空文件写入、失败回滚；另在 read 已取得首簇后暂停，验证 truncate(0) 与其他文件分配等待本次簇链访问结束，不能读到复用后的数据。
     - `ext2`/`fat32` 孤儿节点延迟回收（`open(A) → unlink(A) → create(B) → write(old_A)`）：验证 `A` 被 `unlink` 后仍有登记/预留时，`create(B)` 不复用 A 正持有的 inode/簇链（空 FAT 文件比较 object_id），向 `A` 写入绝不污染 `B`，且 `vfs_node_put(A)` 后 `A` 的底层块/inode/簇被 `evict_node` 完整回收；
     - `write`/`truncate` 后对同一打开节点与新查找节点执行 `vfs_stat` 的大小一致性；
     - 跨挂载 `rename` 返回 `-EXDEV`。
   - **Phase 3**：新增 `hosttests/cases/test_vfs_mount_tree.c`，覆盖：
     - `/dev` 先于 `/` 挂载的延迟绑定；
     - 虚拟挂载桩（Synthetic Stub）透明步进；
     - 多级挂载 `/mnt/usb` 在每次重新分配 `/mnt` 节点（无 dcache）时仍能通过稳定目录身份进入子挂载；
     - `/dev/..` 与 `/mnt/usb/..` 跨挂载回退；
     - `vfs_namespace_lock` 串行化与挂载祖先保护：验证挂载 `/a/d/usb` 后尝试 `rename("/a/d", "/b/d")`、`rename("/a", "/x")` 或 `rmdir("/a/d")` 均返回 `-EBUSY`，以及 `vfs_mount` 与 `vfs_rename`/`vfs_rmdir` 在同一把 `vfs_namespace_lock` 下互斥执行；
     - `vfs_getdents` 子挂载注入、同名目录去重，以及多级挂载 `/mnt/usb` 下枚举 `/` **不出现** `usb`、枚举 `/mnt` **只出现一次** `usb`。
   - **Phase 4**：新增 `hosttests/cases/test_vfs_dcache.c`，覆盖：
     - 正缓存命中计数、FAT32 大小写不敏感键命中；
     - 256 项容量上限 LRU 驱逐与 `refcount` 平衡、父目录 `/mnt` 从缓存淘汰后再查找 `/mnt/usb` 仍正确跨越挂载点；
     - 确定性并发交错用例：`dcache_lookup_begin` 未命中并完成底层 `lookup` 后，穿插 `dcache_mutation_begin` / `unlink` / `dcache_mutation_end_invalidate`，验证 `dcache_insert_validated` 拒绝回填旧节点，且调用者手头的 `child` 在最终 `vfs_node_put` 前可安全读取、put 时安全释放（put 后不得再访问）（`evict_node` 延迟回收）；
     - 元数据一致性用例：缓存节点先 `vfs_stat`、再通过同一或另一句柄 `vfs_write`/`vfs_truncate`、再对缓存节点/已打开节点/淘汰重建节点分别 `vfs_stat`，均读到最新 `st_size`；
     - 目录 `rename`（`/a/d` → `/b/d`）后先前的缓存后代 `/a/d/sub` 被失效，重新查找 `/b/d/sub` 后其 `../..` 指向 `/b` 且 `vfs_resolve_path` 输出 `/b/d/sub`；
     - `VFS_FS_NO_DCACHE` 绕过。
2. **静态与构建审计（`make test-static`）**：
   - 另行执行 `make PROFILE=x86_64-clang kernel` 与 `make PROFILE=aarch64-clang kernel`，两者构建零警告；`test-static` 本身不代替双架构构建。
3. **QEMU 集成与系统调用回归**：
   - `make test-qemu SUITE=phase-0`
   - `make KERNEL_SELFTEST=1 test-kernel-selftest`（验证 `ext2_selftest_*`、`tmpfs_selftest_*`、`symlink_selftest` 全部 PASS）
   - `make OS01_SYSTEST=1 test-syscall`（228 项用户态 syscall 套件零回退）
