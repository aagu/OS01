# ARCH-11 devfs 子目录化 — 设计规格

- 日期：2026-11-02
- 状态：已评审批准，待实施
- 对应 roadmap：`docs/roadmap.md` 的 ARCH-11「devfs 子目录化」
- 前置：ARCH-5（VFS 抽象补强）✅、ARCH-10（静态容量与工程卫生）✅

---

## 1. 目标与范围

### 1.1 问题

`devfs_register_chrdev(name, …)` 只接受 ≤32 字节的**单段**名字，`devfs_devices[]` 是一张拍平的链表
（ARCH-10 已把它改成动态链表，但**仍然是拍平的**）。任何 `/dev/<a>/<b>` 形态的层级接口在今天只能：

- 退化为 `/dev/<a>-<b>` 的拍平命名，或
- 要求重构 VFS 的查找路径。

roadmap ARCH-11 的原始描述把「VFS `find_mount`/`__vfs_lookup_raw` 不支持多段解析」列为需要一并解决的
约束，因此目标是：**devfs 注册接受 `/` 分隔的 path 段，devfs 内部改为按组件组织的目录结构，
VFS 查找路径支持多段解析**。

### 1.2 现状复核结论（关键）

实施前对 VFS 查找路径逐行复核，结论与 roadmap 的原始假设**不同**：

`kernel/fs/vfs.c` 的 `__vfs_lookup_raw()`（第 941–1157 行）**已经**实现了逐分量迭代：

```c
while ((comp = next_component(&ptr)) != NULL) {
    ...
    child = dcache_lookup_begin(cur, comp, &lookup_gen);
    if (!child) {
        lrc = cur->i_ops->lookup(cur, comp, &child);   // ← 逐段回调
        ...
    }
    cur = child;
}
```

即：VFS 层的 `find_mount` / `__vfs_lookup_raw` **本来就支持多段路径解析**，只要被查找的文件系统
在遇到中间分量时返回一个 `VFS_DIR` 节点即可继续下降。

因此 **ARCH-11 不需要修改 VFS 查找路径**。全部改动收敛在 `kernel/fs/devfs.c` +
`kernel/include/fs/devfs.h`。本设计把这一复核结论作为第一约束，避免对 VFS 做无谓的重构。

### 1.3 目标

1. devfs 注册接受 `/` 分隔的多段 path，注册后用户态可见 `/dev/<a>/<b>/<c>`。
2. devfs 内部存储从拍平链表改为**按组件组织的目录树**，容量按需（不再有固定表）。
3. devfs 的 `lookup` / `readdir` 支持层级：中间分量返回目录节点，每级目录枚举自己的直接孩子。
4. 现存拍平命名（`null` / `zero` / `random` / `urandom` / `serial` / `tty` / `tty0` / `ptmx` /
   `pts<N>` / `fb` / `keyboard` / `mouse` / `gfx0` / `hda*`）**完全兼容**，行为不变。
5. 旧的 `devfs_register_chrdev("foo", …)` 单段调用自动当作单段路径注册。
6. 新增显式多段 API `devfs_register_path()`。
7. 以 hosttest 验证「注册 → 多级查找 → 读取 → 重挂载后仍可枚举」全链。

### 1.4 非目标（明确不做）

- **不实现** devfs 显式 `mkdir` / 空目录 API（隐式建目录已覆盖全部当前需求）。
- **不实现** 运行期 unregister / 热卸载（延续 ARCH-10「记录存活至关机」契约）。
- **不实现** devfs 内的符号链接语义。
- **不修改** `boot_context` v2 ABI、`kernel/include/uapi/` 的任何 syscall 号或跨边界结构。
- **不实现** 真实的 `/dev/pci/<BDF>/<attr>`（属 ARCH-12 lspci）。
- **不修改** VFS 的查找路径、dcache、挂载树（§1.2 已说明其已具备多段能力）。
- **不引入** 新的内核日志通道或裸 `serial_printk`（延续日志规范）。

---

## 2. 存储模型

### 2.1 从拍平链表到目录树

现状（ARCH-10 后）：

```c
typedef struct devfs_device {
    uint64_t id;
    char name[DEVFS_NAME_MAX];   // 拍平名
    uint8_t type;
    const struct devfs_ops *ops;
    void *private_data;
    struct devfs_device *next;   // 全局单链表
} devfs_device_t;
```

改为**单棵目录树**，节点既可表示目录也可表示设备：

```c
typedef enum {
    DEVFS_KIND_DIR,
    DEVFS_KIND_CHR,
    DEVFS_KIND_BLK,
} devfs_kind_t;

typedef struct devfs_node {
    char                    name[DEVFS_NAME_MAX];  // 单段名（≤31 字节）
    uint64_t                id;                    // 稳定、非零、单调
    devfs_kind_t            kind;
    const struct devfs_ops *ops;                   // 仅设备
    void                   *private_data;          // 仅设备
    struct devfs_node      *parent;                // 目录的父；根为 NULL
    struct devfs_node      *first_child;
    struct devfs_node      *last_child;
    struct devfs_node      *next_sibling;
    struct devfs_node      *prev_sibling;
} devfs_node_t;
```

### 2.2 不变量

| 编号 | 不变量 |
|------|--------|
| D1 | 根是唯一的静态 `devfs_root`（`DEVFS_KIND_DIR`），`parent == NULL`。 |
| D2 | 每个节点的 `parent` 与 `first_child/last_child`/`next_sibling/prev_sibling` 双向一致。 |
| D3 | `id` 稳定、非零、单调递增，由 `next_device_id` 统一分配（目录与设备共用一个计数器）。 |
| D4 | 同一目录内，节点名唯一（一个名字要么是设备，要么是目录，不兼有）。 |
| D5 | 节点一经发布**永不释放**（延续 ARCH-10 §7.3「运行期已发布对象存活至关机」）。 |
| D6 | 设备节点的 `name` 是叶子分量（不是全路径）；全路径由树结构隐含表达。 |

### 2.3 与 ARCH-10 契约的衔接

ARCH-10 §7.3 末句写明：「ARCH-11 后续只需将单段名字索引替换为目录结构；记录对象、ID 和已打开文件
的引用方式无需再次改变。」本设计严格遵守：

- 记录对象语义（稳定 id、`node->fs_data` 存记录指针、统一 accessor 校验归属）不变；
- id 分配与 ino/rdev 一致性不变；
- 已打开文件的引用方式不变（`file_t->node` 仍持有 VFS 节点引用）。

变化仅限于：**记录的存储拓扑**（全局单链表 → 树）、**`name` 的含义**（拍平名 → 叶子分量）、
**多出目录记录**这一类节点。

### 2.4 容量

不再有任何固定容量上限（`DEVFS_MAX_DEVICES` 已在 ARCH-10 移除）。每个节点按需 `calloc`，
目录的孩子用兄弟链表组织，容量随注册数增长，无静态表。

---

## 3. 路径语义

### 3.1 路径是相对的

devfs 注册路径**相对于 devfs 的挂载点**（`/dev`）。注册 `"pci/0000:00:1f.2/vendor"`
对应用户态路径 `/dev/pci/0000:00:1f.2/vendor`。

这与 Linux devtmpfs 的惯例一致：驱动注册 `"null"`，用户看到 `/dev/null`。

### 3.2 语法

```
path      := component ( '/' component )*
component := 1..31 字节，任意非 '/' 非 NUL 字节
```

- 分隔符：`'/'`
- 单段长度：`< DEVFS_NAME_MAX`（32），即 1..31 字节
- 段数：1..`DEVFS_MAX_DEPTH`（16）
- 绝对用户路径长度：`strlen("/dev/") + strlen(path) < VFS_NAME_MAX`（256），否则 `-ENAMETOOLONG`

`DEVFS_MAX_DEPTH` 是递归深度上界，防止畸形路径导致深递归；16 段对
`/dev/usb/xhci/1-1/1-1:1.0/vendor` 这类形态绰绰有余。

### 3.3 校验与错误码

| 输入 | 返回 | 说明 |
|------|------|------|
| `NULL` | `-EINVAL` | |
| `""`（空串） | `-EINVAL` | |
| 前导 `'/'`（`"/leading"`） | `-EINVAL` | 路径必须是相对的 |
| 尾随 `'/'`（`"trailing/"`） | `-EINVAL` | 末段为空 |
| 空分量（`"a//b"`） | `-EINVAL` | |
| `"."` 或 `".."` 分量 | `-EINVAL` | devfs 不做路径遍历语义 |
| 单段 ≥ `DEVFS_NAME_MAX` 字节 | `-EINVAL` | 不截断为潜在同名 |
| 段数 > `DEVFS_MAX_DEPTH` | `-EINVAL` | |
| 绝对路径 ≥ `VFS_NAME_MAX` | `-ENAMETOOLONG` | |
| 与已有**完全相同**的路径 | `-EEXIST` | |
| 某个中间分量已是**设备**（不是目录） | `-ENOTDIR` | 如 `"null/foo"`，`null` 是字符设备 |
| 某个中间分量已是**目录** | 成功（复用） | 隐式建目录 |
| 节点分配失败 | `-ENOMEM` | 已建成的中间目录不回滚，见 §6.4 |

> **与 ARCH-10 的差异**：ARCH-10 §7.3 要求「单段名称不得为空、含 `/` 或达到 `DEVFS_NAME_MAX`；
> 返回 `-EINVAL`」。本项按 §3.3 放宽了「含 `/`」这一条——含 `/` 的名字现在被解释为多段路径而**不是**
> 非法名。`test_devfs_capacity.c` 中 `devfs_register_chrdev("sub/dev", …)` 的断言从 `-EINVAL`
> 改为 `0`，其余断言（空串 / `NULL` / 前导 `/` / 尾随 `/` / 超长段 / 重复名）**保持不变**。

### 3.4 冲突与复用

- **完全同路径**再次注册 → `-EEXIST`（不覆盖已有记录）。
- **同名但类型不同**（例如先注册设备 `"pci"`，再注册 `"pci/x"`）→ `-ENOTDIR`，
  因为 `"pci"` 是设备不是目录。
- **反向**（先注册 `"pci/x"` 隐式建出目录 `"pci"`，再注册设备 `"pci"`）→ `-EEXIST`，
  因为 `"pci"` 这个名字已被目录占用（D4）。
- 同一目录下两个不同路径共享前缀 → 共享中间目录，互不影响。

---

## 4. 对外 API

### 4.1 新增与保留

```c
// 多段 path 注册（字符设备）——本项新增的显式多段入口
int devfs_register_path(const char *path, void *private_data,
                         const struct devfs_ops *ops);

// 多段 path 注册（块设备）
int devfs_register_blkdev_path(const char *path, block_device_t *dev);

// 既有 API，语义扩展为「接受 '/' 分隔的多段 path」
int devfs_register_chrdev(const char *name, void *private_data,
                           const struct devfs_ops *ops);
int devfs_register_blkdev(const char *name, block_device_t *dev);
```

`devfs_register_chrdev()` 与 `devfs_register_path()` **行为完全等价**，前者是后者的历史拼写，
保留是为了不破坏现有调用点与外部驱动；`devfs_register_blkdev()` 同理对应
`devfs_register_blkdev_path()`。两对函数共用同一个内部核心 `devfs_register_node()`。

单段调用（`devfs_register_chrdev("null", …)`）是单段路径的退化情形，语义与 ARCH-10 完全一致。

### 4.2 查找 API

```c
int devfs_lookup_node(const char *name, struct vfs_node **out);
```

扩展为接受**多段相对路径**（如 `"pts/0"`）。单段调用行为不变。
该函数返回的节点与 `devfs_iop_lookup()` 一样，`fs_data` 指向 devfs 记录指针，
但 `parent` 为 `NULL`（保持既有「独立借用节点」语义）。

### 4.3 不变的 API 与语义

`devfs_ops` 结构体布局、`devfs_open_node()` / `devfs_ioctl_node()` / `devfs_ioctl_file()` /
`devfs_poll()` / `devfs_poll_file()` / `devfs_release_file()` / `devfs_get_private()` /
`devfs_get_file_ops()` 的签名与语义**全部不变**。

`ops->open(path, out)` 收到的 `path` 仍是**完整用户路径**（`/dev/pci/.../vendor`），
因为它来自 `sys_open` 的 `path_copy`。既有设备（`tty_magic_open` 比较 `"/dev/tty"`）
不受影响；新增的层级设备可按需解析该路径。

---

## 5. VFS 集成

### 5.1 lookup

`devfs_iop_lookup(dir, name, out)` 是 VFS 的 `i_ops->lookup` 实现。`dir` 是当前所在的
devfs 目录节点，`name` 是**单个**路径分量（VFS 负责逐段迭代）。

```
1. dir->type != VFS_DIR            → -ENOTDIR
2. 取目录记录：dir->fs_data
     - 非 NULL → 用它（子目录节点由 devfs 自己创建时设置）
     - NULL    → 回退到根记录 &devfs_root（mount 时会设置 mp->root->fs_data，
                 保留回退以防 root 节点被替换）
     该记录的 kind 必须是 DEVFS_KIND_DIR，否则 → -ENOTDIR
     （防御性检查：VFS_DIR 节点不应指向设备记录）
3. 在该目录的孩子链表里按 name 精确匹配（devfs 不区分大小写：
   mount 未设 VFS_FS_CASE_INSENSITIVE）
4. 未命中                              → -ENOENT
5. 命中的是设备 → 建设备 VFS 节点     → 0
   命中的是目录 → 建目录 VFS 节点     → 0
```

节点构造：

- 设备节点：`type = VFS_CHRDEV|VFS_BLKDEV`，`ino = rdev = 记录 id`，
  `mode = (S_IFCHR|S_IFBLK) | 0666`，`nlink = 1`，`fs_data = 记录`，
  `parent = vfs_node_get(dir)`，`i_ops = &devfs_iops`，`f_ops = &devfs_fops`，
  `name = strndup(叶子分量)`。
- 目录节点：`type = VFS_DIR`，`ino = 目录 id`，`rdev = 0`，`mode = S_IFDIR | 0755`，
  `nlink = 2`，`fs_data = 目录记录`，`parent = vfs_node_get(dir)`，
  `i_ops = &devfs_iops`，`f_ops = &devfs_fops`，`name = strndup(目录分量)`。

### 5.2 readdir

`devfs_readdir(node, index, entry)` 枚举 `node->fs_data` 指向的目录的**直接孩子**
（子目录 + 设备），按注册顺序（兄弟链表顺序）：

- `entry->name` = 孩子的单段名（长度 < `VFS_NAME_MAX`，无需截断）；
- `entry->type` = `VFS_DIR` / `VFS_CHRDEV` / `VFS_BLKDEV`；
- `entry->ino` = 孩子的 id（与该孩子 lookup 出来的节点的 `ino`/`rdev` 一致）；
- `entry->size` = 0；
- 越过末尾 → `entry->name[0] = '\0'`，返回 0。

root 节点的 `fs_data` 在 `devfs_fs_mount()` 中设置为 `&devfs_root`，
这样 `ls /dev` 能同时看到设备与一级子目录。

### 5.3 归属校验 accessor

`devfs_device_from_node(node)`（`node->fs_data` → 记录指针的统一 accessor）语义不变：

- 节点类型必须是 `VFS_CHRDEV` / `VFS_BLKDEV`（目录节点被自动排除）；
- `fs_data` 必须指向 devfs 树中真实存在的记录（遍历树校验指针身份）；
- 否则返回 `NULL`，调用方按既有约定返回 `-ENODEV` / `-1`。

代价：从「拍平链表 O(n) 扫描」变为「树 O(n) 遍历」，同阶，无回归。

### 5.4 与 VFS 的其它交互

- **dcache**：devfs mount 带 `VFS_FS_NO_DCACHE`，`dcache_lookup_begin()` /
  `dcache_insert_validated()` 对 devfs 直接短路（`kernel/fs/dcache.c:169`、`:220`），
  目录节点不会被缓存，语义与现有设备节点一致。
- **挂载**：`vfs_bind_submount_locked()` 通过 `parent_dir->i_ops->lookup()` 找挂载点。
  devfs 的 lookup 现在能返回目录节点，因此**在 `/dev/pci` 上挂载文件系统是可行的**
  （本项不实现该用例，但接口已就位）。
- **活动节点注册**：`__vfs_lookup_raw()` 对每个 child 调 `vfs_active_node_register()`；
  目录节点在 `vfs_node_put()` 归零时经 `vfs_active_node_unregister()` 摘除，
  与现有设备节点路径一致，无泄漏。

---

## 6. 并发、生命周期与错误处理

### 6.1 锁

沿用既有 `devfs_lock`（`spinlock_T`，irqsave 语义），保护整棵树。

- **注册**：路径解析与节点 `calloc` 在锁外完成；在锁内复查冲突并挂接。
  若在锁内发现冲突，释放本次已分配的节点并返回错误码。
- **查找**：在锁内定位目标记录指针；**在锁外**创建 VFS 节点（分配动作不进锁）。
  由于 D5（记录不释放），锁外持有的记录指针始终有效。
- **readdir**：在锁内遍历并把名字拷出，锁外写 `entry`（沿用现有实现的写法）。

### 6.2 设备回调不进锁

`ops->read/write/ioctl/poll/mmap/open` 一律在锁外调用（沿用现状）。
`devfs_device_from_node()` 自身会取锁，属于既有行为。

### 6.3 记录生命周期

- 记录（设备与目录）一经发布**永不释放**，与 ARCH-10 §7.3 一致：
  「运行期已发布对象存活至关机，因此读者离开注册表锁后仍能安全持有指针」。
- devfs 卸载（`devfs_fs_unmount()`）只释放 VFS 节点，**不销毁** devfs 记录；
  重新挂载后整棵层级树仍可完整枚举（`test_devfs_capacity.c` 已有重挂载用例，本项扩展为层级）。

### 6.4 错误处理

- 分配失败（节点或名字）→ `-ENOMEM`，并回收本次临时分配的节点/名字。
- **注册过程中间目录已建立、后续步骤失败**：已建立的中间目录**保留**（不回滚）。
  理由：目录记录无外部可见状态，回滚需要额外的引用计数与并发协议，
  收益低于复杂度；已建立的空目录对用户无害。
  这一点必须在头文件注释中写明，避免调用方误以为失败注册不留痕迹。
- **设备记录已成功登记后**，后续独立的 lookup 失败（`-ENOMEM`）**不回滚**该登记
  （沿用 ARCH-10 §7.3 的既有约定）。

### 6.5 日志

新增/修改的内核代码一律使用 `log_err` / `log_warn` / `log_info` / `debug_fs()`，
不使用裸 `serial_printk` / `color_printk`（例外白名单不含本次改动）。

注册失败**不在 devfs 内部记日志**：注册 API 把精确 errno 交回调用方，
严重级别由调用方决定（启动必需设备失败是 error，可选设备失败是 warning），
devfs 内部再记一次既重复，又会把 logger 拉进每一个 devfs 消费方
（`test_poll_*` 等 host case 因此链接失败）。成功路径用 `debug_fs()` 留痕。

---

## 7. 测试

### 7.1 新增 hosttest：`hosttests/cases/test_devfs_hierarchy.c`

用例：

| 用例 | 验证内容 |
|------|----------|
| `test_devfs_hierarchy_register_and_lookup` | 注册 `"pci/0000:00:1f.2/vendor"` → `/dev/pci`、`/dev/pci/0000:00:1f.2`、`/dev/pci/0000:00:1f.2/vendor` 三级 lookup 全部命中；叶子节点 `type == VFS_CHRDEV`，`ino == rdev != 0`；目录节点 `type == VFS_DIR`。 |
| `test_devfs_hierarchy_readdir_per_level` | 三级 readdir 各自枚举出正确的孩子名与类型（`/dev` 列出 `pci` 为 `VFS_DIR`；`/dev/pci` 列出 `0000:00:1f.2` 为 `VFS_DIR`；`/dev/pci/0000:00:1f.2` 列出 `vendor` 为 `VFS_CHRDEV`）；readdir 的 `ino` 与该名字 lookup 出来的节点 `ino`/`rdev` 一致。 |
| `test_devfs_hierarchy_read_data` | 打开层级设备并读出 `ops->read` 提供的 payload（证明 `fs_data` → 记录 → ops 的链路在层级下成立）。 |
| `test_devfs_hierarchy_legacy_single_segment` | 单段注册 `"plain_dev"` → `/dev/plain_dev` 可查可读，行为与 ARCH-10 一致。 |
| `test_devfs_hierarchy_conflicts` | 同路径重复 → `-EEXIST`；`"null/foo"`（`null` 是设备）→ `-ENOTDIR`；先 `"pci/x"` 后 `"pci"` → `-EEXIST`；先 `"pci"` 后 `"pci/x"` → `-ENOTDIR`。 |
| `test_devfs_hierarchy_invalid_paths` | `NULL` / `""` / 前导 `/` / 尾随 `/` / `"a//b"` / `"."` / `".."` / 超长段 → `-EINVAL`；超长总路径 → `-ENAMETOOLONG`；超深路径 → `-EINVAL`。 |
| `test_devfs_hierarchy_remount` | 重挂载 devfs 后，层级路径仍可 lookup / readdir / 读取。 |

注册方式：按仓库约定在 `hosttests/mk/fs.mk` 用 `register_host_test()` 注册，并把可执行文件加入
`TEST_BINS`（与 `test_devfs_capacity` 同处），不产生孤儿测试。

### 7.2 修改既有 hosttest：`hosttests/cases/test_devfs_capacity.c`

`test_devfs_name_validation()` 中：

```c
// 旧：'/' 在名字里非法
// assert_eq(-EINVAL, devfs_register_chrdev("sub/dev", NULL, &s_test_cap_ops));
// 新：'/' 分隔多段路径，注册成功，且 /dev/sub 成为目录
assert_eq(0, devfs_register_chrdev("sub/dev", NULL, &s_test_cap_ops));
vfs_node_t *sub_dir = vfs_lookup("/dev/sub");
assert_not_null(sub_dir);
assert_eq(VFS_DIR, sub_dir->type);
vfs_node_put(sub_dir);
```

其余断言（`NULL` / `""` / `"/leading"` / `"trailing/"` / 32 字符名 / 重复名）**保持不变**。

### 7.3 QEMU 回归

按评审决定，QEMU 侧**只做无回归检查**，不为本项引入临时测试设备：

- 正常启动，`/dev/null`、`/dev/zero`、`/dev/tty`、`/dev/serial`、`/dev/ptmx`、`/dev/pts0` 可用；
- `ls /dev` 输出与改动前一致（无新增项、无缺失项）；
- `cat /dev/urandom` 正常。

真实 `/dev/pci/<BDF>/<attr>` 属 ARCH-12。

### 7.4 静态审计

- `make test-static`：runtime / layout / canary / link-order 审计必须全绿。
- `make test-host`：hosttest 全绿。
- 新增/修改的 kernel C 代码的日志调用符合 `docs/log/log.md` 规范。

---

## 8. 改动清单

| 文件 | 改动 |
|------|------|
| `kernel/include/fs/devfs.h` | 新增 `devfs_register_path()` / `devfs_register_blkdev_path()` 声明；`devfs_register_chrdev()` / `devfs_register_blkdev()` 注释更新为「接受 '/' 分隔的多段 path」；新增 `DEVFS_MAX_DEPTH`；`devfs_lookup_node()` 注释更新。 |
| `kernel/fs/devfs.c` | 拍平链表 → 目录树；`devfs_register_node()` 核心；路径解析与校验；`devfs_iop_lookup()` 支持目录；`devfs_readdir()` 逐级枚举；`devfs_lookup_node()` 支持多段；`devfs_device_from_node()` 改为树校验；`devfs_fs_mount()` 设置 `mp->root->fs_data`。 |
| `hosttests/cases/test_devfs_hierarchy.c` | 新增。 |
| `hosttests/mk/fs.mk` | 注册新 hosttest + 加入 `TEST_BINS`。 |
| `hosttests/cases/test_devfs_capacity.c` | 更新 `"sub/dev"` 断言。 |
| `docs/fs/filesystem.md` | 新增「devfs 子目录与多级路径」章节。 |
| `docs/roadmap.md` | ARCH-11 标记闭环；更新「当前状态」行。 |
| `docs/changelog.md` | 闭环条目。 |
| `docs/superpowers/specs/2026-11-02-arch11-devfs-hierarchy-design.md` | 本文档。 |
| `docs/superpowers/plans/2026-11-02-arch11-devfs-hierarchy.md` | 实施计划。 |

**不改动**：`kernel/fs/vfs.c`、`kernel/fs/dcache.c`、`kernel/include/fs/vfs.h`、
任何 `kernel/include/uapi/` 文件、任何现有 devfs 驱动调用点。

---

## 9. 风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| 现有驱动依赖「设备名等于注册名」 | `ops->open` 收到的是完整用户路径，与现状一致 | §4.3 已核实 `sys_open` 传 `path_copy`；既有设备均不解析该串或只比较 `/dev/tty` |
| `devfs_device_from_node()` 从链表扫描改为树遍历 | 每次 read/write 多一层遍历开销 | 同阶 O(n)，且 ARCH-10 已把表改为动态；n 实际为几十量级 |
| 目录节点引入新的 `fs_data` 语义 | `devfs_device_from_node()` 若不做类型区分会误判 | 已有 `node->type != VFS_CHRDEV && != VFS_BLKDEV → NULL` 的类型门禁，目录被自动排除 |
| 隐式目录残留（注册失败后） | 目录树里留下空目录 | §6.4 明确保留；空目录无外部可见状态 |
| `test_devfs_capacity.c` 断言变更 | 与 ARCH-10 的既有契约冲突 | §3.3 已显式记录差异；ARCH-11 的目标正是解除「单段名」约束 |

---

## 10. 验收标准

1. `make test-host` 全绿，含新增 `test_devfs_hierarchy` 与修改后的 `test_devfs_capacity`。
2. `make test-static` 全绿。
3. QEMU 正常启动，`/dev` 下既有设备全部可用，`ls /dev` 无回归。
4. `docs/fs/filesystem.md` 有独立的「devfs 子目录与多级路径」章节。
5. `docs/roadmap.md` 的 ARCH-11 标记闭环并在「当前状态」行体现。
6. `docs/changelog.md` 有对应闭环条目。
7. VFS 查找路径（`kernel/fs/vfs.c`）**未发生任何改动**——这一点在评审记录中作为架构约束保留。
