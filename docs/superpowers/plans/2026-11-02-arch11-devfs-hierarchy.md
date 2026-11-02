# ARCH-11 devfs 子目录化 — 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 devfs 支持 `/` 分隔的多级注册路径（`/dev/pci/<BDF>/<attr>` 形态），内部存储从拍平链表改为按组件组织的目录树。

**Architecture:** devfs 内部用一棵树（`devfs_node_t`）保存所有节点，目录与设备同构。注册时按 `/` 切分路径，隐式创建中间目录。查找时 devfs 的 `i_ops->lookup` 对中间分量返回 `VFS_DIR` 节点，由 VFS 现有的逐分量遍历继续下降——**VFS 侧零改动**。

**Tech Stack:** C（kernel，`clang` + `lld`）、kernel 自旋锁、repo 内 hosttest 框架（`hosttests/cases/` + `hosttests/mk/fs.mk`）。

**规格文档:** `docs/superpowers/specs/2026-11-02-arch11-devfs-hierarchy-design.md`

## Global Constraints

以下约束适用于**每一个** Task，实施时不得违反：

- **`kernel/fs/vfs.c` 与 `kernel/fs/dcache.c` 不得修改。** VFS 的 `__vfs_lookup_raw()` 已经逐分量遍历路径（kernel/fs/vfs.c:1024-1148），多段解析能力已存在；本项只改 devfs。
- **不得修改 `kernel/include/uapi/` 下任何文件**，不新增/重排 syscall 号，不改 `boot_context` v2 ABI。
- **新增/修改的内核代码一律用 `log_err` / `log_warn` / `log_info` / `debug_fs()`**；禁止裸 `serial_printk` / `color_printk`（白名单例外不含本项改动）。
- **源目录 ↔ 头目录对称**：`kernel/fs/devfs.c` ↔ `kernel/include/fs/devfs.h`（AGENTS.md 目录规范 §1）。本项不新增文件到 kernel/。
- **记录一经发布永不释放**（延续 ARCH-10 §7.3）。devfs 卸载只释放 VFS 节点，不销毁记录。
- **`node->fs_data` 存 devfs 记录指针**，由 `devfs_device_from_node()` 统一 accessor 校验节点种类与归属。
- **`ino` / `rdev` / readdir 的 `ino` 必须一致且非零**；目录与设备共用同一个单调 id 计数器。
- **`devfs_ops` 结构体布局不得改动**（设备 ABI）。
- **devfs mount 保持 `VFS_FS_NO_DCACHE`**，不启用 dcache，不预建设备节点。
- **每个新增 hosttest case 必须在同一个 commit 里**完成：`hosttests/mk/fs.mk` 的 `register_host_test()` 注册 + `TEST_BINS` 条目 + 构建规则，三者缺一即为孤儿测试。
- **禁止在 kernel 源码里出现 `TBD` / `TODO` / 占位实现。**
- 每完成一个 Task 立即 commit，不要攒到最后。

## 参考命令

```bash
# 构建单个 hosttest 二进制
make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/mk/profiles/x86_64-clang.mk \
     PROFILE=x86_64-clang /home/aagu/OS01/build/x86_64-clang/host-test/<name>.elf

# 直接运行单个 hosttest
/home/aagu/OS01/build/x86_64-clang/host-test/<name>.elf

# 全量 hosttest + 静态审计
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static

# 内核构建（确认没打破内核编译）
make PROFILE=x86_64-clang kernel.bin
```

---

## Task 1: devfs 目录树存储模型（行为等价重构）

把拍平链表换成目录树，**但本 Task 不引入多段路径**——所有现有单段注册仍然落在根目录下，注册 API 签名与返回值完全不变。这是纯重构，必须由既有 hosttest 全绿证明无回归。

**Files:**
- Modify: `kernel/include/fs/devfs.h`（新增 `DEVFS_MAX_DEPTH`；更新 `devfs_register_chrdev()` / `devfs_register_blkdev()` / `devfs_lookup_node()` 的注释，说明名字可以是单段）
- Modify: `kernel/fs/devfs.c`（第 23-62 行的记录结构与全局状态；`devfs_device_from_node()`；`devfs_create_node_for_device()`；`devfs_readdir()`；`devfs_iop_lookup()`；`devfs_lookup_node()`；`devfs_register_device()`；`devfs_validate_name()`；`tty_magic_open()`）
- Test: `hosttests/cases/test_devfs_capacity.c`（不改动，仅作回归门禁）

**Interfaces:**
- Consumes: 现有 `struct devfs_ops`（不变）、`vfs_node_t`、`VFS_NAME_MAX`(256)、`DEVFS_NAME_MAX`(32)。
- Produces:
  - `kernel/include/fs/devfs.h`：`#define DEVFS_MAX_DEPTH 16`
  - `kernel/fs/devfs.c` 内部类型：
    ```c
    typedef enum { DEVFS_KIND_DIR, DEVFS_KIND_CHR, DEVFS_KIND_BLK } devfs_kind_t;

    typedef struct devfs_node {
        char                    name[DEVFS_NAME_MAX];  // single path component
        uint64_t                id;                    // stable, non-zero, monotonic
        devfs_kind_t            kind;
        const struct devfs_ops *ops;                   // devices only
        void                   *private_data;          // devices only
        struct devfs_node      *parent;
        struct devfs_node      *first_child;
        struct devfs_node      *last_child;
        struct devfs_node      *next_sibling;
        struct devfs_node      *prev_sibling;
    } devfs_node_t;
    ```
  - `kernel/fs/devfs.c` 内部函数：
    ```c
    static devfs_node_t *devfs_find_child(devfs_node_t *dir, const char *name);
    static void          devfs_link_child(devfs_node_t *dir, devfs_node_t *node);
    static bool          devfs_subtree_contains(devfs_node_t *dir, const devfs_node_t *needle);
    static devfs_node_t *devfs_dir_record(const vfs_node_t *dir);
    static vfs_node_t   *devfs_create_node_for_record(devfs_node_t *rec, vfs_node_t *dir);
    static vfs_node_t   *devfs_create_dir_node(devfs_node_t *rec, vfs_node_t *dir);
    static devfs_node_t *devfs_find_device_by_private(devfs_node_t *dir, void *priv);
    ```

- [ ] **Step 1: 在 `kernel/include/fs/devfs.h` 加 `DEVFS_MAX_DEPTH` 并更新注释**

在 `#define DEVFS_NAME_MAX    32` 之后加入：

```c
#define DEVFS_NAME_MAX    32

// Maximum number of path components in a single devfs registration
// path ("pci/0000:00:1f.2/vendor" is 3).  Bounds the recursion of
// the tree walk; generous enough for /dev/usb/<bus>/<port>/<iface>/…
#define DEVFS_MAX_DEPTH   16
```

把 `devfs_register_chrdev()` 的注释改为：

```c
// Register a character device that will appear under /dev/.
// `name` is a path RELATIVE to the devfs mount point: it may be a
// single component ("null") or a '/'-separated multi-component path
// ("pci/0000:00:1f.2/vendor").  Intermediate directories are created
// implicitly.  A leading '/' is rejected — the path is relative.
int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops);
```

`devfs_register_blkdev()` 的注释同样加一句「`name` 是相对 /dev 的路径，可含 '/' 分段」。

- [ ] **Step 2: 在 `kernel/fs/devfs.c` 替换记录结构与全局状态**

把第 23-37 行（`devfs_device_t` 定义 + `devfs_lock` / `devfs_devices` / `devfs_devices_tail` / `next_device_id`）整块替换为：

```c
// ── Internal device tree ────────────────────────────────────
//
// Every registered node lives in one tree rooted at devfs_root.  A
// node is either a directory (DEVFS_KIND_DIR) or a device
// (DEVFS_KIND_CHR / DEVFS_KIND_BLK).  A device registered under a
// '/'-separated path ("pci/0000:00:1f.2/vendor") gets the
// intermediate directories created implicitly.
//
// Records are never freed once published (ARCH-10 §7.3: "运行期已发布
// 对象存活至关机"), so a record pointer stays valid after the registry
// lock is dropped.

typedef enum {
    DEVFS_KIND_DIR,
    DEVFS_KIND_CHR,
    DEVFS_KIND_BLK,
} devfs_kind_t;

typedef struct devfs_node {
    char                    name[DEVFS_NAME_MAX];  // single path component
    uint64_t                id;                    // stable, non-zero, monotonic
    devfs_kind_t            kind;
    const struct devfs_ops *ops;                   // devices only
    void                   *private_data;          // devices only
    struct devfs_node      *parent;
    struct devfs_node      *first_child;
    struct devfs_node      *last_child;
    struct devfs_node      *next_sibling;
    struct devfs_node      *prev_sibling;
} devfs_node_t;

static spinlock_T devfs_lock = { 1 };
static devfs_node_t devfs_root = {
    .name = "",
    .id   = 0,
    .kind = DEVFS_KIND_DIR,
};
static uint64_t next_device_id = 1;
```

- [ ] **Step 3: 替换 `devfs_device_from_node()` 并加入树辅助函数**

把原 `devfs_device_from_node()`（第 39-62 行）替换为下面这一整块（树辅助函数 + 新 accessor）：

```c
// Find a direct child of `dir` named `name`.  Caller holds devfs_lock.
static devfs_node_t *devfs_find_child(devfs_node_t *dir, const char *name)
{
    if (!dir || !name) return NULL;
    for (devfs_node_t *cur = dir->first_child; cur; cur = cur->next_sibling) {
        if (strcmp(cur->name, name) == 0)
            return cur;
    }
    return NULL;
}

// Append `node` as the last child of `dir`.  Caller holds devfs_lock.
static void devfs_link_child(devfs_node_t *dir, devfs_node_t *node)
{
    node->parent       = dir;
    node->prev_sibling = dir->last_child;
    node->next_sibling = NULL;
    if (dir->last_child)
        dir->last_child->next_sibling = node;
    else
        dir->first_child = node;
    dir->last_child = node;
}

// Does the subtree rooted at `dir` contain `needle`?
static bool devfs_subtree_contains(devfs_node_t *dir, const devfs_node_t *needle)
{
    if (!dir || !needle) return false;
    for (devfs_node_t *cur = dir->first_child; cur; cur = cur->next_sibling) {
        if (cur == needle) return true;
        if (devfs_subtree_contains(cur, needle)) return true;
    }
    return false;
}

// Resolve a VFS directory node to the devfs directory record behind
// it.  The mount root gets its fs_data in devfs_fs_mount(); the NULL
// fallback keeps lookups working if that root node is ever replaced.
static devfs_node_t *devfs_dir_record(const vfs_node_t *dir)
{
    if (!dir) return NULL;
    devfs_node_t *rec = (devfs_node_t *)dir->fs_data;
    if (!rec) return &devfs_root;
    if (rec->kind != DEVFS_KIND_DIR) return NULL;
    return rec;
}

// First device record whose private_data matches `priv`.
// Caller holds devfs_lock.
static devfs_node_t *devfs_find_device_by_private(devfs_node_t *dir, void *priv)
{
    if (!dir) return NULL;
    for (devfs_node_t *cur = dir->first_child; cur; cur = cur->next_sibling) {
        if (cur->kind != DEVFS_KIND_DIR) {
            if (cur->private_data == priv)
                return cur;
        } else {
            devfs_node_t *hit = devfs_find_device_by_private(cur, priv);
            if (hit) return hit;
        }
    }
    return NULL;
}

static devfs_node_t *devfs_device_from_node(const vfs_node_t *node)
{
    if (!node) return NULL;
    if (node->type != VFS_CHRDEV && node->type != VFS_BLKDEV)
        return NULL;
    if (!node->fs_data) return NULL;
#ifndef OS01_HOST_TEST
    if ((uint64_t)node->fs_data < 0xffff800000000000ULL)
        return NULL;
#endif

    devfs_node_t *candidate = (devfs_node_t *)node->fs_data;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    bool found = devfs_subtree_contains(&devfs_root, candidate);
    spin_unlock_irqrestore(&devfs_lock, flags);
    return found ? candidate : NULL;
}
```

- [ ] **Step 4: 把 `devfs_create_node_for_device()` 改成 `devfs_create_node_for_record()` 并新增目录节点构造**

把原 `devfs_create_node_for_device()`（第 430-454 行）替换为：

```c
static vfs_node_t *devfs_create_node_for_record(devfs_node_t *rec, vfs_node_t *dir)
{
    vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!child) return NULL;
    child->name = strndup(rec->name, VFS_NAME_MAX - 1);
    if (!child->name) {
        kfree(child);
        return NULL;
    }
    child->type = (rec->kind == DEVFS_KIND_BLK) ? VFS_BLKDEV : VFS_CHRDEV;
    child->ino = rec->id;
    child->rdev = rec->id;
    child->mode = (child->type == VFS_CHRDEV ? S_IFCHR : S_IFBLK) | 0666;
    child->nlink = 1;
    child->size = 0;
    if (dir) {
        child->mount = dir->mount;
        child->parent = vfs_node_get(dir);
    }
    child->fs_data = rec;
    child->i_ops = &devfs_iops;
    child->f_ops = &devfs_fops;
    child->refcount = 1;
    return child;
}

// Synthetic directory node for a devfs sub-directory.  Its fs_data
// carries the directory record so that the next lookup() call knows
// which directory it is descending into.
static vfs_node_t *devfs_create_dir_node(devfs_node_t *rec, vfs_node_t *dir)
{
    vfs_node_t *node = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!node) return NULL;
    node->name = strndup(rec->name, VFS_NAME_MAX - 1);
    if (!node->name) {
        kfree(node);
        return NULL;
    }
    node->type = VFS_DIR;
    node->ino = rec->id;
    node->rdev = 0;
    node->mode = S_IFDIR | 0755;
    node->nlink = 2;
    node->size = 0;
    if (dir) {
        node->mount = dir->mount;
        node->parent = vfs_node_get(dir);
    }
    node->fs_data = rec;
    node->i_ops = &devfs_iops;
    node->f_ops = &devfs_fops;
    node->refcount = 1;
    return node;
}
```

- [ ] **Step 5: 改 `devfs_readdir()` 为按目录枚举孩子**

把原 `devfs_readdir()`（第 397-423 行）替换为：

```c
static int devfs_readdir(vfs_node_t *node, uint64_t index, vfs_dirent_t *entry)
{
    if (!node || node->type != VFS_DIR || !entry) return -1;

    devfs_node_t *rec = devfs_dir_record(node);
    if (!rec) return -1;

    uint64_t cur_idx = 0;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *cur = rec->first_child;
    while (cur) {
        if (cur_idx == index) {
            size_t nlen = strlen(cur->name);
            if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
            memcpy(entry->name, cur->name, nlen);
            entry->name[nlen] = '\0';
            entry->type = (cur->kind == DEVFS_KIND_DIR)  ? VFS_DIR :
                          (cur->kind == DEVFS_KIND_BLK)  ? VFS_BLKDEV :
                                                           VFS_CHRDEV;
            entry->size = 0;
            entry->ino = cur->id;
            spin_unlock_irqrestore(&devfs_lock, flags);
            return 0;
        }
        cur = cur->next_sibling;
        cur_idx++;
    }
    spin_unlock_irqrestore(&devfs_lock, flags);

    entry->name[0] = '\0';  // end of directory
    return 0;
}
```

- [ ] **Step 6: 改 `devfs_iop_lookup()` 与 `devfs_lookup_node()`**

把原 `devfs_iop_lookup()`（第 456-481 行）替换为：

```c
static int devfs_iop_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dir || !name) return -EINVAL;
    if (dir->type != VFS_DIR) return -ENOTDIR;

    devfs_node_t *rec = devfs_dir_record(dir);
    if (!rec) return -ENOTDIR;

    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *target = devfs_find_child(rec, name);
    spin_unlock_irqrestore(&devfs_lock, flags);

    if (!target) return -ENOENT;

    vfs_node_t *child = (target->kind == DEVFS_KIND_DIR)
                        ? devfs_create_dir_node(target, dir)
                        : devfs_create_node_for_record(target, dir);
    if (!child) return -ENOMEM;
    *out = child;
    return 0;
}
```

把原 `devfs_lookup_node()`（第 483-507 行）替换为（Task 1 先只支持单段，Task 4 再扩展为多段）：

```c
int devfs_lookup_node(const char *name, struct vfs_node **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!name) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *target = devfs_find_child(&devfs_root, name);
    spin_unlock_irqrestore(&devfs_lock, flags);

    if (!target) return -ENOENT;

    vfs_node_t *child = (target->kind == DEVFS_KIND_DIR)
                        ? devfs_create_dir_node(target, NULL)
                        : devfs_create_node_for_record(target, NULL);
    if (!child) return -ENOMEM;
    *out = child;
    return 0;
}
```

- [ ] **Step 7: 改 `devfs_validate_name()` 与 `devfs_register_device()`**

把原 `devfs_validate_name()`（第 762-771 行）替换为（Task 1 只做单段校验，Task 2 换成多段解析）：

```c
// Validate a single-component device name.
static int devfs_validate_name(const char *name)
{
    if (!name || name[0] == '\0')
        return -EINVAL;
    if (strlen(name) >= DEVFS_NAME_MAX)
        return -EINVAL;
    if (strchr(name, '/'))
        return -EINVAL;
    return 0;
}
```

（保持原样，不改。）

把原 `devfs_register_device()`（第 773-818 行）替换为：

```c
static int devfs_register_device(const char *name, uint8_t type,
                                 void *private_data, const struct devfs_ops *ops)
{
    int val_rc = devfs_validate_name(name);
    if (val_rc != 0)
        return val_rc;

    devfs_kind_t kind = (type == VFS_BLKDEV) ? DEVFS_KIND_BLK : DEVFS_KIND_CHR;

    devfs_node_t *dev = (devfs_node_t *)calloc(1, sizeof(devfs_node_t));
    if (!dev)
        return -ENOMEM;

    strncpy(dev->name, name, DEVFS_NAME_MAX - 1);
    dev->name[DEVFS_NAME_MAX - 1] = '\0';
    dev->kind = kind;
    dev->ops = ops;
    dev->private_data = private_data;

    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    if (devfs_find_child(&devfs_root, name)) {
        spin_unlock_irqrestore(&devfs_lock, flags);
        kfree(dev);
        return -EEXIST;
    }

    if (next_device_id == UINT64_MAX) {
        // Refuse rather than wrap into a live id.
        spin_unlock_irqrestore(&devfs_lock, flags);
        kfree(dev);
        return -ENOSPC;
    }

    dev->id = next_device_id++;
    devfs_link_child(&devfs_root, dev);
    spin_unlock_irqrestore(&devfs_lock, flags);

    debug_fs("devfs: registered '%s' (type %u, id %lu)\n", name, (unsigned)type, (unsigned long)dev->id);
    return 0;
}
```

- [ ] **Step 8: 改 `tty_magic_open()` 里的设备查找与节点构造**

把 `tty_magic_open()` 中这一段：

```c
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_device_t *cur = devfs_devices;
    devfs_device_t *matched_dev = NULL;
    while (cur) {
        if (cur->private_data == target && cur->type == VFS_CHRDEV) {
            matched_dev = cur;
            break;
        }
        cur = cur->next;
    }
    spin_unlock_irqrestore(&devfs_lock, flags);
```

替换为：

```c
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *matched_dev = devfs_find_device_by_private(&devfs_root, target);
    if (matched_dev && matched_dev->kind != DEVFS_KIND_CHR)
        matched_dev = NULL;
    spin_unlock_irqrestore(&devfs_lock, flags);
```

并把该函数里的

```c
        vfs_node_t *node = devfs_create_node_for_device(matched_dev, NULL);
```

替换为

```c
        vfs_node_t *node = devfs_create_node_for_record(matched_dev, NULL);
```

- [ ] **Step 9: 改 `devfs_read()` / `devfs_write()` / `devfs_mmap()` / `devfs_poll()` 等对 `dev->ops` / `dev->type` / `dev->private_data` 的引用**

这些函数里的 `dev` 现在是 `devfs_node_t *`。逐个确认字段访问仍然合法：

- `dev->type == VFS_BLKDEV` → 改为 `dev->kind == DEVFS_KIND_BLK`
- `dev->ops`、`dev->private_data` → 字段名不变，直接可用

`devfs_read()` 里的块设备分支改为：

```c
    // Block device path: sector-level read via block_device_t
    if (dev->kind == DEVFS_KIND_BLK) {
```

`devfs_write()` 里的块设备分支同样改为 `if (dev->kind == DEVFS_KIND_BLK) {`。

其余 `dev->ops->read` / `dev->ops->write` / `dev->ops->mmap` / `dev->ops->poll` /
`dev->ops->ioctl` / `dev->ops->release_file` / `dev->ops->read_file` / `dev->ops->poll_file`
引用**不需要改**。

- [ ] **Step 10: 改 `devfs_get_private()` 与 `devfs_get_file_ops()`**

这两个函数的 `dev->private_data` / `dev->ops` 字段名未变，**不需要改**。但 `devfs_device_from_node()`
的返回类型从 `devfs_device_t *` 变成了 `devfs_node_t *`，所以这两个函数里 `devfs_device_t *dev`
的声明要改成 `devfs_node_t *dev`。同样地，`devfs_open_node()`、`devfs_ioctl_node()`、
`devfs_ioctl_file()`、`devfs_release_file()`、`devfs_read_file()` 里的
`devfs_device_t *dev` 全部改成 `devfs_node_t *dev`。

用一次全文件搜索确认没有遗漏：

```bash
cd /home/aagu/OS01 && grep -n "devfs_device_t" kernel/fs/devfs.c
```

期望输出为空。

- [ ] **Step 11: 编译并跑既有 hosttest 验证无回归**

```bash
cd /home/aagu/OS01 && make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/mk/profiles/x86_64-clang.mk \
     PROFILE=x86_64-clang /home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_capacity.elf
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_capacity.elf
```

期望：输出 `>>> ALL TESTS PASSED <<<`，且末尾行为 `[TEST] END suite=hosttests total=6 passed=6 failed=0 skipped=0`（6 = 用例数；`Total:` 那一行统计的是断言数，数值随断言数量变化，不作硬性要求）。

再编译内核，确认生产构建也过：

```bash
cd /home/aagu/OS01 && make PROFILE=x86_64-clang kernel.bin
```

期望：编译成功，无 warning（warning 视为失败，需要修掉）。

- [ ] **Step 12: 提交**

```bash
cd /home/aagu/OS01 && git add kernel/fs/devfs.c kernel/include/fs/devfs.h && \
git commit -m "refactor(devfs): store devices in a per-component tree

Replace the flat devfs_device list with a tree of devfs_node records
so that a record can carry a directory or a device role.  Single
component registrations keep their exact previous behaviour: same
names, same ids, same ino/rdev, same lookup and readdir results.

No user-visible change yet -- multi-component registration paths
land in the next commit."
```

---

## Task 2: 多段路径注册（解析 + 隐式目录 + 新 API）

**Files:**
- Modify: `kernel/include/fs/devfs.h`（新增 `devfs_register_path()` / `devfs_register_blkdev_path()` 声明）
- Modify: `kernel/fs/devfs.c`（新增 `devfs_split_path()` / `devfs_register_node()`；`devfs_register_chrdev()` / `devfs_register_blkdev()` 改为委托；`devfs_register_device()` 删除）
- Create: `hosttests/cases/test_devfs_hierarchy.c`
- Modify: `hosttests/mk/fs.mk`（注册 + `TEST_BINS` + 构建规则）

**Interfaces:**
- Consumes: Task 1 的 `devfs_node_t`、`devfs_find_child()`、`devfs_link_child()`、`devfs_root`。
- Produces:
  - `kernel/include/fs/devfs.h`：
    ```c
    int devfs_register_path(const char *path, void *private_data,
                             const struct devfs_ops *ops);
    int devfs_register_blkdev_path(const char *path, struct block_device *dev);
    ```
  - `kernel/fs/devfs.c` 内部：
    ```c
    static int devfs_split_path(const char *path,
                                 char comps[DEVFS_MAX_DEPTH][DEVFS_NAME_MAX],
                                 int *out_count);
    static int devfs_register_node(const char *path, devfs_kind_t kind,
                                    void *private_data,
                                    const struct devfs_ops *ops);
    ```

- [ ] **Step 1: 先写失败测试——创建 `hosttests/cases/test_devfs_hierarchy.c`**

整个文件内容（脚手架沿用 `hosttests/cases/test_devfs_capacity.c` 的同款 stub）：

```c
/*
 * test_devfs_hierarchy.c — ARCH-11: devfs sub-directory support
 *
 * Verifies:
 *  - A '/'-separated registration path creates intermediate directories
 *    and the full path is reachable through vfs_lookup().
 *  - Readdir enumerates each level's direct children with the right type.
 *  - A device under a sub-directory reads back its own payload.
 *  - Single-component registration keeps working unchanged.
 *  - Path conflicts and malformed paths return the documented errno.
 *  - Remounting devfs preserves the whole hierarchy.
 */

#include "test_framework.h"
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <block/blockdev.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static task_t s_test_task = {
    .pid = 1,
    .addr_limit = 0x00007fffffffffffULL,
};
task_t *vfs_test_current = &s_test_task;

int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }

int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -EIO;
}

int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -EIO;
}

uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

void get_random_bytes(void *b, size_t n) { if (b && n) memset(b, 0x42, n); }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int  user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }
tty_t *get_dev_tty(void) { return NULL; }

static const vfs_inode_ops_t s_dummy_proc_iops = {0};
static const vfs_file_ops_t  s_dummy_proc_fops = {0};
static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    mp->i_ops = &s_dummy_proc_iops;
    mp->f_ops = &s_dummy_proc_fops;
    mp->root->i_ops = &s_dummy_proc_iops;
    mp->root->f_ops = &s_dummy_proc_fops;
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0555;
    mp->root->nlink = 2;
    return 0;
}
static vfs_fs_type_t s_host_procfs_type = {
    .name     = "procfs",
    .fs_flags = VFS_FS_NO_DCACHE,
    .mount    = host_procfs_mount,
};
int procfs_register_fs(void) { return vfs_register_filesystem(&s_host_procfs_type); }

file_t *file_alloc(void)
{
    file_t *f = (file_t *)calloc(1, sizeof(file_t));
    if (f) {
        f->refcount = 1;
        f->type = FD_NONE;
    }
    return f;
}

void file_free(file_t *f)
{
    if (!f) return;
    if (f->type == FD_DEV) {
        devfs_release_file(f);
    }
    if (f->node) {
        vfs_node_put(f->node);
        f->node = NULL;
    }
    free(f);
}

// The device read returns its own private payload as a uint64_t.
static int hier_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)offset;
    if (!node || !buffer || size < sizeof(uint64_t)) return -EINVAL;
    void *priv = devfs_get_private(node);
    if (!priv) return -EIO;
    uint64_t id = (uint64_t)(uintptr_t)priv;
    memcpy(buffer, &id, sizeof(id));
    return (int)sizeof(id);
}

static const struct devfs_ops s_hier_ops = {
    .read = hier_read,
};

/* ── Helpers ─────────────────────────────────────────────────── */

// Look up one entry of a directory node by name through readdir.
static bool readdir_has(vfs_node_t *dir, const char *name, uint8_t type)
{
    char name_buf[VFS_NAME_MAX];
    vfs_dirent_t entry;
    uint64_t idx = 0;
    while (1) {
        memset(&entry, 0, sizeof(entry));
        entry.name = name_buf;
        if (dir->f_ops->readdir(dir, idx++, &entry) != 0) return false;
        if (entry.name[0] == '\0') return false;
        if (strcmp(entry.name, name) == 0) return type == 0 || entry.type == type;
    }
}

/* ── Test Cases ────────────────────────────────────────────── */

TEST_FUNC(test_devfs_hierarchy_register_and_lookup) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("pci/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x1111, &s_hier_ops));

    vfs_node_t *d1 = vfs_lookup("/dev/pci");
    assert_not_null(d1);
    assert_eq(VFS_DIR, d1->type);

    vfs_node_t *d2 = vfs_lookup("/dev/pci/0000:00:1f.2");
    assert_not_null(d2);
    assert_eq(VFS_DIR, d2->type);

    vfs_node_t *leaf = vfs_lookup("/dev/pci/0000:00:1f.2/vendor");
    assert_not_null(leaf);
    assert_eq(VFS_CHRDEV, leaf->type);
    assert_true(leaf->ino != 0);
    assert_eq(leaf->ino, leaf->rdev);

    vfs_node_put(leaf);
    vfs_node_put(d2);
    vfs_node_put(d1);
}

TEST_FUNC(test_devfs_hierarchy_readdir_per_level) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("pci/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x2222, &s_hier_ops));

    vfs_node_t *root = vfs_lookup("/dev");
    assert_not_null(root);
    assert_true(readdir_has(root, "pci", VFS_DIR));

    vfs_node_t *d1 = vfs_lookup("/dev/pci");
    assert_not_null(d1);
    assert_true(readdir_has(d1, "0000:00:1f.2", VFS_DIR));

    vfs_node_t *d2 = vfs_lookup("/dev/pci/0000:00:1f.2");
    assert_not_null(d2);
    assert_true(readdir_has(d2, "vendor", VFS_CHRDEV));

    vfs_node_put(d2);
    vfs_node_put(d1);
    vfs_node_put(root);
}

TEST_FUNC(test_devfs_hierarchy_read_data) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("pci/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x3333, &s_hier_ops));

    vfs_node_t *leaf = vfs_lookup("/dev/pci/0000:00:1f.2/vendor");
    assert_not_null(leaf);
    if (leaf) {
        uint64_t val = 0;
        assert_eq((int)sizeof(val), leaf->f_ops->read(leaf, 0, sizeof(val), &val));
        assert_eq(0x3333, val);
        vfs_node_put(leaf);
    }
}

TEST_FUNC(test_devfs_hierarchy_legacy_single_segment) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_chrdev("plain_dev", (void *)(uintptr_t)0x4444,
                                       &s_hier_ops));

    vfs_node_t *node = vfs_lookup("/dev/plain_dev");
    assert_not_null(node);
    if (node) {
        assert_eq(VFS_CHRDEV, node->type);
        uint64_t val = 0;
        assert_eq((int)sizeof(val), node->f_ops->read(node, 0, sizeof(val), &val));
        assert_eq(0x4444, val);
        vfs_node_put(node);
    }
}

TEST_FUNC(test_devfs_hierarchy_conflicts) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("pci/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x5555, &s_hier_ops));

    // Exact duplicate path
    assert_eq(-EEXIST, devfs_register_path("pci/0000:00:1f.2/vendor",
                                           (void *)(uintptr_t)0x6666, &s_hier_ops));

    // "null" is a built-in character device, not a directory
    assert_eq(-ENOTDIR, devfs_register_path("null/foo",
                                             (void *)(uintptr_t)0x7777, &s_hier_ops));

    // "pci" is now a directory — the name is taken
    assert_eq(-EEXIST, devfs_register_path("pci",
                                           (void *)(uintptr_t)0x8888, &s_hier_ops));
}

TEST_FUNC(test_devfs_hierarchy_invalid_paths) {
    vfs_init();
    devfs_init();

    assert_eq(-EINVAL, devfs_register_path(NULL, NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("/leading", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("trailing/", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("a//b", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("./x", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("../x", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("a/./b", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("01234567890123456789012345678901",
                                           NULL, &s_hier_ops));
}

TEST_FUNC(test_devfs_hierarchy_deep_and_long_paths) {
    vfs_init();
    devfs_init();

    // 16 components is the maximum depth
    char deep[DEVFS_MAX_DEPTH * 16];
    int n = 0;
    for (int i = 0; i < DEVFS_MAX_DEPTH; i++)
        n += snprintf(deep + n, sizeof(deep) - n, "%sd%d", i ? "/" : "", i);
    assert_eq(0, devfs_register_path(deep, (void *)(uintptr_t)0x9999, &s_hier_ops));

    // 17 components exceeds DEVFS_MAX_DEPTH
    char too_deep[DEVFS_MAX_DEPTH * 16];
    n = 0;
    for (int i = 0; i < DEVFS_MAX_DEPTH + 1; i++)
        n += snprintf(too_deep + n, sizeof(too_deep) - n, "%sd%d", i ? "/" : "", i);
    assert_eq(-EINVAL, devfs_register_path(too_deep, NULL, &s_hier_ops));

    // A path whose absolute form would not fit in VFS_NAME_MAX
    char toolong[VFS_NAME_MAX * 2];
    memset(toolong, 'x', sizeof(toolong));
    toolong[0] = 'l';
    toolong[sizeof(toolong) - 1] = '\0';
    assert_eq(-ENAMETOOLONG, devfs_register_path(toolong, NULL, &s_hier_ops));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_devfs_hierarchy_register_and_lookup),
    TEST_ENTRY(test_devfs_hierarchy_readdir_per_level),
    TEST_ENTRY(test_devfs_hierarchy_read_data),
    TEST_ENTRY(test_devfs_hierarchy_legacy_single_segment),
    TEST_ENTRY(test_devfs_hierarchy_conflicts),
    TEST_ENTRY(test_devfs_hierarchy_invalid_paths),
    TEST_ENTRY(test_devfs_hierarchy_deep_and_long_paths),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: 在 `hosttests/mk/fs.mk` 注册新 case**

在 `$(eval $(call register_host_test,test_devfs_capacity,fs,protocol-or-legacy))` 之后加一行：

```make
$(eval $(call register_host_test,test_devfs_hierarchy,fs,protocol-or-legacy))
```

在 `TEST_BINS` 列表里，`$(TEST_BLD)/test_devfs_capacity.elf` 之后加：

```make
    $(TEST_BLD)/test_devfs_hierarchy.elf \
```

在 `$(TEST_BLD)/test_devfs_capacity.elf:` 规则之后加：

```make
$(TEST_BLD)/test_devfs_hierarchy.o: $(TEST_CASES)/test_devfs_hierarchy.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TESTS_DIR)/kernel/include/fs/devfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_devfs_hierarchy.elf: $(TEST_BLD)/test_devfs_hierarchy.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=kmalloc -o $@ $^
```

- [ ] **Step 3: 运行测试，确认它失败**

```bash
cd /home/aagu/OS01 && make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/mk/profiles/x86_64-clang.mk \
     PROFILE=x86_64-clang /home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf
```

期望：**编译失败**，报 `devfs_register_path` 未声明（隐式声明错误）。

- [ ] **Step 4: 在 `kernel/include/fs/devfs.h` 声明新 API**

在 `devfs_register_chrdev()` 声明之后加：

```c
// Register a character device under a '/'-separated path relative to
// the devfs mount point ("pci/0000:00:1f.2/vendor").  Intermediate
// directories are created implicitly.  Behaviourally identical to
// devfs_register_chrdev(), which is the historical spelling kept for
// source compatibility.
int devfs_register_path(const char *path, void *private_data,
                         const struct devfs_ops *ops);
```

在 `devfs_register_blkdev()` 声明之后加：

```c
// Block-device counterpart of devfs_register_path().
int devfs_register_blkdev_path(const char *path, struct block_device *dev);
```

- [ ] **Step 5: 在 `kernel/fs/devfs.c` 实现路径解析与注册核心**

把原 `devfs_validate_name()` 与 `devfs_register_device()`（第 762-818 行那一整块）替换为：

```c
// Split a relative '/'-separated path into components.
//
// Returns 0 and fills `comps` / `*out_count` on success, or:
//   -EINVAL      NULL / empty / absolute / empty component / "." / ".."
//                / component >= DEVFS_NAME_MAX bytes / too many components
//   -ENAMETOOLONG  the absolute user path would not fit in VFS_NAME_MAX
//
// `comps` must have room for DEVFS_MAX_DEPTH entries of DEVFS_NAME_MAX bytes.
static int devfs_split_path(const char *path,
                             char comps[DEVFS_MAX_DEPTH][DEVFS_NAME_MAX],
                             int *out_count)
{
    if (!path || path[0] == '\0') return -EINVAL;

    // devfs paths are relative to the mount point (/dev).
    if (path[0] == '/') return -EINVAL;

    // The absolute user path is "/dev/" + path and must fit in VFS_NAME_MAX.
    size_t plen = strlen(path);
    if (plen + strlen("/dev/") >= VFS_NAME_MAX) return -ENAMETOOLONG;

    int count = 0;
    const char *p = path;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0) return -EINVAL;                // "", "a//b", "trailing/"
        if (len >= DEVFS_NAME_MAX) return -EINVAL;   // too long — never truncate
        if (count >= DEVFS_MAX_DEPTH) return -EINVAL;

        // No traversal semantics in devfs.
        if ((len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.'))
            return -EINVAL;

        memcpy(comps[count], p, len);
        comps[count][len] = '\0';
        count++;

        if (!slash) break;
        p = slash + 1;
    }

    *out_count = count;
    return 0;
}

// Register a node at `path`, creating intermediate directories.
//
// Nodes are allocated BEFORE the registry lock is taken (allocation
// must not happen under a spinlock).  One candidate node is prepared
// per path component; a component that already exists in the tree
// reuses the existing node and its candidate is discarded.
//
// Error codes:
//   -EINVAL        malformed path (see devfs_split_path)
//   -ENAMETOOLONG  the absolute path would not fit in VFS_NAME_MAX
//   -EEXIST        the leaf name is already taken in its directory
//   -ENOTDIR       an intermediate component is a device, not a directory
//   -ENOMEM        allocation failed
//   -ENOSPC        the id space is exhausted
//
// Intermediate directories that were already created stay in the tree
// when a later step fails — they carry no externally visible state and
// rolling them back would need extra reference counting.
static int devfs_register_node(const char *path, devfs_kind_t kind,
                                void *private_data,
                                const struct devfs_ops *ops)
{
    char (*comps)[DEVFS_NAME_MAX] =
        (char (*)[DEVFS_NAME_MAX])calloc(DEVFS_MAX_DEPTH, DEVFS_NAME_MAX);
    if (!comps) return -ENOMEM;

    int count = 0;
    int rc = devfs_split_path(path, comps, &count);
    if (rc != 0) {
        kfree(comps);
        return rc;
    }

    // One candidate node per component.  slots[i] is used only when
    // component i does not already exist in the tree.
    devfs_node_t *slots[DEVFS_MAX_DEPTH];
    uint32_t used = 0;
    for (int i = 0; i < count; i++) {
        slots[i] = (devfs_node_t *)calloc(1, sizeof(devfs_node_t));
        if (!slots[i]) {
            for (int j = 0; j < i; j++)
                if (!(used & (1u << j))) kfree(slots[j]);
            kfree(comps);
            return -ENOMEM;
        }
        strncpy(slots[i]->name, comps[i], DEVFS_NAME_MAX - 1);
        slots[i]->name[DEVFS_NAME_MAX - 1] = '\0';
        slots[i]->kind = (i == count - 1) ? kind : DEVFS_KIND_DIR;
    }

    int result = 0;
    devfs_node_t *leaf = NULL;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);

    devfs_node_t *cur_dir = &devfs_root;
    for (int i = 0; i < count - 1; i++) {
        devfs_node_t *child = devfs_find_child(cur_dir, comps[i]);
        if (!child) {
            // Implicit directory.
            if (next_device_id == UINT64_MAX) { result = -ENOSPC; goto out; }
            slots[i]->id = next_device_id++;
            devfs_link_child(cur_dir, slots[i]);
            used |= (1u << i);
            child = slots[i];
        } else if (child->kind != DEVFS_KIND_DIR) {
            // A device occupies this name — it cannot be a directory.
            result = -ENOTDIR;
            goto out;
        }
        cur_dir = child;
    }

    // Leaf: the name must be free in its directory.
    if (devfs_find_child(cur_dir, comps[count - 1])) {
        result = -EEXIST;
        goto out;
    }
    if (next_device_id == UINT64_MAX) { result = -ENOSPC; goto out; }

    leaf = slots[count - 1];
    leaf->id = next_device_id++;
    leaf->kind = kind;
    leaf->ops = ops;
    leaf->private_data = private_data;
    devfs_link_child(cur_dir, leaf);
    used |= (1u << (count - 1));

out:
    spin_unlock_irqrestore(&devfs_lock, flags);

    // Release the candidates that were not linked into the tree.
    for (int i = 0; i < count; i++) {
        if (!(used & (1u << i)))
            kfree(slots[i]);
    }
    kfree(comps);

    if (result == 0) {
        debug_fs("devfs: registered '%s' (id %lu)\n", path,
                 (unsigned long)leaf->id);
    } else {
        log_warn("devfs: registration of '%s' failed (%d)\n", path, result);
    }
    return result;
}
```

> `leaf` 在进入 `out:` 之前声明并初始化为 `NULL`，因此失败路径不会引用未初始化值；
> `debug_fs()` 只在 `result == 0`（即 `leaf` 已被赋值）时执行。

- [ ] **Step 6: 把公开注册 API 改为委托**

把 `devfs_register_chrdev()` / `devfs_register_blkdev()` 替换为：

```c
int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops)
{
    return devfs_register_node(name, DEVFS_KIND_CHR, private_data, ops);
}

int devfs_register_path(const char *path, void *private_data,
                         const struct devfs_ops *ops)
{
    return devfs_register_node(path, DEVFS_KIND_CHR, private_data, ops);
}

int devfs_register_blkdev(const char *name, block_device_t *dev)
{
    return devfs_register_node(name, DEVFS_KIND_BLK, dev, NULL);
}

int devfs_register_blkdev_path(const char *path, block_device_t *dev)
{
    return devfs_register_node(path, DEVFS_KIND_BLK, dev, NULL);
}
```

删除原来的 `devfs_validate_name()` 与 `devfs_register_device()`（它们已被
`devfs_split_path()` / `devfs_register_node()` 取代）。

- [ ] **Step 7: 运行新 hosttest**

```bash
cd /home/aagu/OS01 && make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/mk/profiles/x86_64-clang.mk \
     PROFILE=x86_64-clang /home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf && \
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf
```

期望：输出 `>>> ALL TESTS PASSED <<<`，且末尾行为 `[TEST] END suite=hosttests total=7 passed=7 failed=0 skipped=0`（7 个用例）。

若 `test_devfs_hierarchy_conflicts` 失败，检查 `devfs_register_node()` 的叶子冲突分支
是否返回 `-EEXIST`、中间设备冲突分支是否返回 `-ENOTDIR`。

- [ ] **Step 8: 跑既有 devfs hosttest 确认没打破单段行为**

```bash
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_capacity.elf
```

期望：输出 `>>> ALL TESTS PASSED <<<`，且末尾行为 `[TEST] END suite=hosttests total=6 passed=6 failed=0 skipped=0`（6 个用例）。

> **注意**：`test_devfs_capacity.c` 的 `test_devfs_name_validation()` 仍断言
> `devfs_register_chrdev("sub/dev", …) == -EINVAL`。本 Task 之后该调用**会成功**，
> 所以这一步**预期失败**——这是 Task 5 要修的断言。为避免混淆，先跳过它：
>
> ```bash
> /home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_capacity.elf 2>&1 | tail -20
> ```
>
> 确认**只有** `test_devfs_name_validation` 一个用例失败，其余 5 个用例仍然 PASS。
> Task 5 会修正这条断言。

- [ ] **Step 9: 编译内核**

```bash
cd /home/aagu/OS01 && make PROFILE=x86_64-clang kernel.bin
```

期望：编译成功且无 warning。

- [ ] **Step 10: 提交**

```bash
cd /home/aagu/OS01 && git add kernel/fs/devfs.c kernel/include/fs/devfs.h \
    hosttests/cases/test_devfs_hierarchy.c hosttests/mk/fs.mk && \
git commit -m "feat(devfs): accept multi-component registration paths

devfs_register_path() / devfs_register_blkdev_path() take a
'/'-separated path relative to the devfs mount point and create the
intermediate directories implicitly.  The historical
devfs_register_chrdev() / devfs_register_blkdev() spellings now accept
the same paths, so existing single-component callers are unaffected.

Malformed paths are rejected with -EINVAL, an over-long absolute path
with -ENAMETOOLONG, a name collision with -EEXIST and a device used as
an intermediate component with -ENOTDIR."
```

---

## Task 3: 层级 lookup 与逐级 readdir（生产接线）

Task 1 已经把 lookup/readdir 改成树形，但那时注册只支持单段，所以目录节点路径没有被真正走到。
本 Task 用生产路径 `/dev/<a>/<b>/<c>` 端到端验证 VFS 的逐分量下降确实工作。

**Files:**
- Modify: `kernel/fs/devfs.c`（`devfs_fs_mount()` 设置 `mp->root->fs_data`；必要时修正 `devfs_iop_lookup()` 的父节点引用计数）
- Test: `hosttests/cases/test_devfs_hierarchy.c`（新增用例）

**Interfaces:**
- Consumes: Task 1 的 `devfs_dir_record()` / `devfs_create_dir_node()`；Task 2 的 `devfs_register_node()`。
- Produces: 无新公开 API；只修正生产接线。

- [ ] **Step 1: 写失败测试——追加用例到 `test_devfs_hierarchy.c`**

在 `TEST_LIST_BEGIN` 之前追加：

```c
TEST_FUNC(test_devfs_hierarchy_vfs_walk_through_mount) {
    vfs_init();
    devfs_init();

    // The /dev mount root must carry the devfs root record so that
    // the first lookup() below resolves against the tree.
    assert_eq(0, devfs_register_path("usb/xhci/1-1/vendor",
                                     (void *)(uintptr_t)0xAAAA, &s_hier_ops));

    // Walk all three levels through the VFS path resolver.
    vfs_node_t *leaf = vfs_lookup("/dev/usb/xhci/1-1/vendor");
    assert_not_null(leaf);
    if (leaf) {
        assert_eq(VFS_CHRDEV, leaf->type);
        uint64_t val = 0;
        assert_eq((int)sizeof(val), leaf->f_ops->read(leaf, 0, sizeof(val), &val));
        assert_eq(0xAAAA, val);
        vfs_node_put(leaf);
    }

    // The intermediate directory must be reachable as a directory too.
    vfs_node_t *mid = vfs_lookup("/dev/usb/xhci");
    assert_not_null(mid);
    assert_eq(VFS_DIR, mid->type);
    vfs_node_put(mid);

    // A path that stops at a device must fail with ENOTDIR.
    vfs_node_t *bad = vfs_lookup("/dev/usb/xhci/1-1/vendor/extra");
    assert_null(bad);
}

TEST_FUNC(test_devfs_hierarchy_remount) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("pci/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0xCCCC, &s_hier_ops));

    // Reinitialise VFS + devfs: this remounts devfs at /dev.
    vfs_init();
    devfs_init();

    // The global device records survive the remount (ARCH-10 §7.3), so
    // the whole hierarchy must still be reachable and enumerable.
    vfs_node_t *leaf = vfs_lookup("/dev/pci/0000:00:1f.2/vendor");
    assert_not_null(leaf);
    if (leaf) {
        uint64_t val = 0;
        assert_eq((int)sizeof(val), leaf->f_ops->read(leaf, 0, sizeof(val), &val));
        assert_eq(0xCCCC, val);
        vfs_node_put(leaf);
    }

    vfs_node_t *d1 = vfs_lookup("/dev/pci");
    assert_not_null(d1);
    if (d1) {
        assert_true(readdir_has(d1, "0000:00:1f.2", VFS_DIR));
        vfs_node_put(d1);
    }
}
```

并在 `TEST_LIST_BEGIN` 里加两行：

```c
    TEST_ENTRY(test_devfs_hierarchy_vfs_walk_through_mount),
    TEST_ENTRY(test_devfs_hierarchy_remount),
```

- [ ] **Step 2: 运行测试确认失败或观察行为**

```bash
cd /home/aagu/OS01 && make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/mk/profiles/x86_64-clang.mk \
     PROFILE=x86_64-clang /home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf && \
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf 2>&1 | tail -30
```

期望：新增用例 **FAIL**（`/dev/usb` 解析不到，或 `usb` 不是目录）——
因为 `devfs_fs_mount()` 还没给 mount root 设置 `fs_data`，而 `/dev` 的第一个分量
`usb` 需要在根目录记录里找到。

- [ ] **Step 3: 在 `devfs_fs_mount()` 里接上 root 记录**

把 `devfs_fs_mount()` 里的

```c
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0755;
    mp->root->nlink = 2;
    return 0;
```

改为

```c
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0755;
    mp->root->nlink = 2;
    // Bind the mount root to the devfs tree root so that lookups
    // starting at /dev resolve against the registered records.
    mp->root->fs_data = &devfs_root;
    return 0;
```

- [ ] **Step 4: 运行测试确认通过**

```bash
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf
```

期望：输出 `>>> ALL TESTS PASSED <<<`，且末尾行为 `[TEST] END suite=hosttests total=9 passed=9 failed=0 skipped=0`（9 个用例）。

若 `/dev/usb/xhci/1-1/vendor` 仍解析失败，检查 `__vfs_lookup_raw()` 在
`cur->i_ops->lookup(cur, comp, &child)` 之后是否把 `child->parent` 正确设为 `cur`
（devfs 自己设了 parent，VFS 的 `if (!child->parent)` 分支因此跳过——这是期望行为）。

- [ ] **Step 5: 提交**

```bash
cd /home/aagu/OS01 && git add kernel/fs/devfs.c hosttests/cases/test_devfs_hierarchy.c && \
git commit -m "fix(devfs): bind the mount root to the devfs tree root

The /dev mount root carried no fs_data, so the first lookup below it
had no directory record to resolve against.  Bind it to devfs_root so
that '/dev/<a>/<b>/<c>' resolves through the VFS component walk."
```

---

## Task 4: `devfs_lookup_node()` 多段支持 + private_data 树遍历

**Files:**
- Modify: `kernel/fs/devfs.c`（`devfs_lookup_node()` 扩展为多段；`tty_magic_open()` 已用 `devfs_find_device_by_private()`，确认可用）
- Modify: `kernel/include/fs/devfs.h`（`devfs_lookup_node()` 注释更新）
- Test: `hosttests/cases/test_devfs_hierarchy.c`（新增用例）

**Interfaces:**
- Consumes: Task 1 的 `devfs_find_child()`；Task 2 的 `devfs_split_path()`。
- Produces: `devfs_lookup_node()` 接受多段相对路径。

- [ ] **Step 1: 写失败测试——追加用例**

在 `TEST_LIST_BEGIN` 之前追加：

```c
TEST_FUNC(test_devfs_hierarchy_lookup_node_multi_segment) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("pts/0",
                                     (void *)(uintptr_t)0xBBBB, &s_hier_ops));

    // Single component still resolves.
    vfs_node_t *plain = NULL;
    assert_eq(0, devfs_lookup_node("pts", &plain));
    assert_not_null(plain);
    if (plain) {
        assert_eq(VFS_DIR, plain->type);
        vfs_node_put(plain);
    }

    // Multi component resolves to the device.
    vfs_node_t *dev = NULL;
    assert_eq(0, devfs_lookup_node("pts/0", &dev));
    assert_not_null(dev);
    if (dev) {
        assert_eq(VFS_CHRDEV, dev->type);
        uint64_t val = 0;
        assert_eq((int)sizeof(val), dev->f_ops->read(dev, 0, sizeof(val), &val));
        assert_eq(0xBBBB, val);
        vfs_node_put(dev);
    }

    // Unknown component → -ENOENT
    vfs_node_t *missing = NULL;
    assert_eq(-ENOENT, devfs_lookup_node("pts/9", &missing));
    assert_null(missing);
}
```

并在 `TEST_LIST_BEGIN` 里加一行：

```c
    TEST_ENTRY(test_devfs_hierarchy_lookup_node_multi_segment),
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cd /home/aagu/OS01 && make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/mk/profiles/x86_64-clang.mk \
     PROFILE=x86_64-clang /home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf && \
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf 2>&1 | tail -20
```

期望：`test_devfs_hierarchy_lookup_node_multi_segment` **FAIL**——
`devfs_lookup_node("pts/0")` 现在按整串 `"pts/0"` 找孩子，找不到 → `-ENOENT`。

- [ ] **Step 3: 实现多段 `devfs_lookup_node()`**

`devfs_split_path()` 的实现位于文件末尾的「Public API」段，而 `devfs_lookup_node()`
位于文件前部——因此必须先加前向声明。在 `devfs_lookup_node()` 的定义**之前**
（例如 `devfs_create_dir_node()` 之后）插入：

```c
// Forward declaration — the implementation lives in the public API
// section at the end of this file.
static int devfs_split_path(const char *path,
                             char comps[DEVFS_MAX_DEPTH][DEVFS_NAME_MAX],
                             int *out_count);
```

然后把 `devfs_lookup_node()` 替换为：

```c
int devfs_lookup_node(const char *name, struct vfs_node **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!name) return -EINVAL;

    char (*comps)[DEVFS_NAME_MAX] =
        (char (*)[DEVFS_NAME_MAX])calloc(DEVFS_MAX_DEPTH, DEVFS_NAME_MAX);
    if (!comps) return -ENOMEM;

    int count = 0;
    int rc = devfs_split_path(name, comps, &count);
    if (rc != 0) {
        kfree(comps);
        return rc;
    }

    devfs_node_t *found = NULL;
    uint64_t flags = spin_lock_irqsave(&devfs_lock);
    devfs_node_t *cur = &devfs_root;
    for (int i = 0; i < count; i++) {
        devfs_node_t *child = devfs_find_child(cur, comps[i]);
        if (!child) { found = NULL; break; }
        found = child;
        if (i + 1 < count) {
            if (child->kind != DEVFS_KIND_DIR) { found = NULL; break; }
            cur = child;
        }
    }
    spin_unlock_irqrestore(&devfs_lock, flags);

    kfree(comps);

    if (!found) return -ENOENT;

    vfs_node_t *child = (found->kind == DEVFS_KIND_DIR)
                        ? devfs_create_dir_node(found, NULL)
                        : devfs_create_node_for_record(found, NULL);
    if (!child) return -ENOMEM;
    *out = child;
    return 0;
}
```

- [ ] **Step 4: 更新 `kernel/include/fs/devfs.h` 的注释**

把

```c
// Lookup a devfs device by name and allocate an independent borrowed node.
int devfs_lookup_node(const char *name, struct vfs_node **out);
```

改为

```c
// Lookup a devfs node by path and allocate an independent borrowed
// node.  `name` is relative to the devfs mount point and may be a
// single component ("gfx0") or a '/'-separated path ("pts/0").
// The returned node has no parent and does not own the record.
int devfs_lookup_node(const char *name, struct vfs_node **out);
```

- [ ] **Step 5: 运行测试确认通过**

```bash
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_hierarchy.elf
```

期望：输出 `>>> ALL TESTS PASSED <<<`，且末尾行为 `[TEST] END suite=hosttests total=10 passed=10 failed=0 skipped=0`（10 个用例）。

- [ ] **Step 6: 确认 `tty_magic_open()` 的树遍历可用**

`kernel/fs/devfs.c` 的 `tty_magic_open()` 现在通过
`devfs_find_device_by_private(&devfs_root, target)` 查找设备。
确认该函数的调用点仍然编译通过：

```bash
cd /home/aagu/OS01 && make PROFILE=x86_64-clang kernel.bin
```

期望：编译成功且无 warning。

- [ ] **Step 7: 提交**

```bash
cd /home/aagu/OS01 && git add kernel/fs/devfs.c kernel/include/fs/devfs.h \
    hosttests/cases/test_devfs_hierarchy.c && \
git commit -m "feat(devfs): support multi-segment paths in devfs_lookup_node

devfs_lookup_node() now resolves '/'-separated paths through the
device tree, so internal callers can reach a nested node such as
\"pts/0\" the same way a user reaches /dev/pts/0."
```

---

## Task 5: 更新既有 ARCH-10 断言 + 全量回归

**Files:**
- Modify: `hosttests/cases/test_devfs_capacity.c`（`test_devfs_name_validation()`）
- 无生产代码改动。

**Interfaces:**
- Consumes: Task 2 的新语义。
- Produces: 全量 hosttest 与静态审计全绿。

- [ ] **Step 1: 更新 `test_devfs_name_validation()`**

把

```c
    // Slash in name
    assert_eq(-EINVAL, devfs_register_chrdev("sub/dev", NULL, &s_test_cap_ops));
    assert_eq(-EINVAL, devfs_register_chrdev("/leading", NULL, &s_test_cap_ops));
    assert_eq(-EINVAL, devfs_register_chrdev("trailing/", NULL, &s_test_cap_ops));
```

改为

```c
    // A '/' now separates path components: the name is registered as
    // the multi-component path "sub/dev" and "sub" becomes a directory.
    assert_eq(0, devfs_register_chrdev("sub/dev", NULL, &s_test_cap_ops));
    vfs_node_t *sub_dir = vfs_lookup("/dev/sub");
    assert_not_null(sub_dir);
    assert_eq(VFS_DIR, sub_dir->type);
    vfs_node_put(sub_dir);

    // Absolute paths, trailing separators and empty components stay
    // invalid.
    assert_eq(-EINVAL, devfs_register_chrdev("/leading", NULL, &s_test_cap_ops));
    assert_eq(-EINVAL, devfs_register_chrdev("trailing/", NULL, &s_test_cap_ops));
    assert_eq(-EINVAL, devfs_register_chrdev("a//b", NULL, &s_test_cap_ops));
```

- [ ] **Step 2: 跑既有 devfs hosttest**

```bash
cd /home/aagu/OS01 && make -C hosttests OS01_PROFILE_FILE=/home/aagu/OS01/mk/profiles/x86_64-clang.mk \
     PROFILE=x86_64-clang /home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_capacity.elf && \
/home/aagu/OS01/build/x86_64-clang/host-test/test_devfs_capacity.elf 2>&1 | tail -20
```

期望：输出 `>>> ALL TESTS PASSED <<<`，且末尾行为 `[TEST] END suite=hosttests total=6 passed=6 failed=0 skipped=0`（6 个用例）。

- [ ] **Step 3: 跑全量 hosttest**

```bash
cd /home/aagu/OS01 && make PROFILE=x86_64-clang test-host
```

期望：全部 hosttest 二进制通过，`[hosttests] all host test binaries passed`，
且尾部的 python 套件（`pmm_boot_reservation_test.py`、`test_user_build.py`、
`test_kernel_headers.py`、`test_cursor_icon.py`）全部 OK。

- [ ] **Step 4: 跑静态审计**

```bash
cd /home/aagu/OS01 && make PROFILE=x86_64-clang test-static
```

期望：全绿。

- [ ] **Step 5: 提交**

```bash
cd /home/aagu/OS01 && git add hosttests/cases/test_devfs_capacity.c && \
git commit -m "test(hosttests): update the devfs name-validation expectation

ARCH-11 makes '/' a path separator in devfs registration names, so
\"sub/dev\" now registers successfully and creates /dev/sub as a
directory.  Absolute paths, trailing separators and empty components
remain invalid."
```

---

## Task 6: QEMU 回归 + 文档闭环

**Files:**
- Modify: `docs/fs/filesystem.md`（新增「devfs 子目录与多级路径」章节）
- Modify: `docs/roadmap.md`（ARCH-11 标记闭环 + 当前状态行）
- Modify: `docs/changelog.md`（闭环条目）

**Interfaces:**
- Consumes: 前面所有 Task 的产出。
- Produces: 文档闭环与 QEMU 无回归证据。

- [ ] **Step 1: 构建并启动 QEMU 做无回归检查**

```bash
cd /home/aagu/OS01 && make PROFILE=x86_64-clang disk.img
```

然后按 `AGENTS.md` 的「Headless interactive test」流程启动：

```bash
FW=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^firmware=//p')
IMG=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^image=//p')

( sleep 20; printf 'ls /dev\n'; sleep 3; printf 'cat /dev/null\n'; sleep 2; \
  printf 'head -c 8 /dev/urandom\n'; sleep 3; printf 'echo DEVFS_OK_123\n'; sleep 3; ) \
  | timeout 60 qemu-system-x86_64 -M q35 \
      -drive if=pflash,format=raw,readonly=on,file="$FW" \
      -drive file="$IMG",format=raw,if=none,id=disk -device ahci,id=ahci \
      -device ide-hd,drive=disk,bus=ahci.0 -m 512 -smp 2 -serial stdio \
      -display none -no-reboot -no-shutdown > /tmp/arch11-devfs.log 2>&1
```

期望：
- `ls /dev` 输出包含 `null`、`zero`、`random`、`urandom`、`serial`、`tty`、`tty0`、
  `ptmx`、`pts0` 等既有设备，且**没有新增**层级目录（生产代码没有注册任何多段设备）；
- `cat /dev/null` 无输出且不挂起；
- `/dev/urandom` 可读；
- 日志里出现 `DEVFS_OK_123`。

若 QEMU 未就绪（缺 OVMF / 镜像），记录该情况并在 `docs/changelog.md` 里注明
「QEMU 回归未在本机执行」，**不要伪造通过结论**。

- [ ] **Step 2: 在 `docs/fs/filesystem.md` 新增章节**

在 `## devfs (...)` 章节的 `### Registration` 小节之后、`---` 之前插入：

````markdown
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
中间分量已是目录则复用该目录。

内部结构：devfs 用一棵树保存所有节点，目录与设备同构（`devfs_node_t`）。
目录与设备共用同一个单调递增的 id 计数器，`ino` / `rdev` / `readdir` 的 `ino`
保持一致且非零。记录一经发布不释放（unmount 只释放 VFS 节点）。

VFS 侧的路径解析本身已支持多级分量（`__vfs_lookup_raw()` 逐分量调用
`i_ops->lookup()`），devfs 只需对中间分量返回 `VFS_DIR` 节点即可，**无需修改 VFS**。
````

- [ ] **Step 3: 更新 `docs/roadmap.md`**

把「工程与架构治理」那一行里的 ARCH-11 从「推进中」移出。原文：

```
剩余 ARCH-6、ARCH-8、ARCH-11、ARCH-12 推进中
```

改为：

```
剩余 ARCH-6、ARCH-8、ARCH-12 推进中
```

并把 ARCH-11 那一行表格的「目标 / 完成条件」末尾补上闭环标记，或者在表格上方加一行
完成说明。建议做法：在 ARCH-11 行的开头加上 `**ARCH-11（devfs 子目录化，2026-11-02 闭环）**`，
并在 roadmap 的「当前状态」段落里把 ARCH-11 补进已闭环列表：

```
- **工程与架构治理**：AAGU 全套就绪；ARCH-1（syscall 脱离 arch）、ARCH-2（Linux ABI 兼容层独立）、ARCH-3（消除 muldefs 全局符号）、ARCH-4（拆分 sched/task.c）、**ARCH-5（VFS 抽象补强：fs 注册表/ops 分离/元数据/活跃节点回收/挂载树/dcache，2026-10-24 闭环）**、ARCH-7（移除 `#define mmap`）、ARCH-9（驱动模型/PCI/总线解耦）、**ARCH-10（静态容量与工程卫生，2026-11-02 闭环）**、**ARCH-11（devfs 子目录化：多级注册路径 + 目录树，2026-11-02 闭环）** 均已闭环合入 master；剩余 ARCH-6、ARCH-8、ARCH-12 推进中
```

- [ ] **Step 4: 在 `docs/changelog.md` 追加条目**

先看 `docs/changelog.md` 顶部的格式（最近条目的日期 / 标题 / 正文结构），
然后按同样的结构追加一条：

```markdown
## 2026-11-02 — ARCH-11 devfs 子目录化

**做了什么**

- devfs 的注册名从「≤32 字节单段」扩展为「相对 `/dev` 的 `/` 分隔多级路径」：
  新增 `devfs_register_path()` / `devfs_register_blkdev_path()`；
  `devfs_register_chrdev()` / `devfs_register_blkdev()` 接受同样的多级路径，
  单分量调用行为不变。
- devfs 内部存储从拍平链表改为按组件组织的目录树（`devfs_node_t`），
  目录与设备同构、共用同一个单调 id 计数器；中间目录隐式创建并作为真实目录
  出现在 `ls /dev` 中。
- `devfs_lookup_node()` 支持多级路径；`lookup` / `readdir` 支持逐级枚举。

**关键结论**

- 复核发现 VFS 的 `__vfs_lookup_raw()` 本来就逐分量遍历路径并回调
  `i_ops->lookup()`，多级解析能力已存在——因此 **VFS 侧零改动**，
  全部工作收敛在 devfs 内部。

**验证**

- 新增 hosttest `test_devfs_hierarchy`（9 个用例）：多级注册 / 逐级 lookup /
  逐级 readdir / 设备读取 / 单段兼容 / 冲突 errno / 非法路径 / 深度与长度上限 /
  重挂载后层级仍可枚举。
- 更新 hosttest `test_devfs_capacity` 的名称校验断言（`'/'` 现在是分隔符）。
- `make PROFILE=x86_64-clang test-host` 与 `make PROFILE=x86_64-clang test-static` 全绿。
- QEMU 回归：`/dev` 既有设备全部可用，`ls /dev` 无变化。

**影响范围**

- `kernel/fs/devfs.c`、`kernel/include/fs/devfs.h`、两个 hosttest case、
  `hosttests/mk/fs.mk`、`docs/fs/filesystem.md`。
- 未改动 `kernel/fs/vfs.c`、`kernel/fs/dcache.c`、任何 `kernel/include/uapi/` 文件。
```

- [ ] **Step 5: 提交**

```bash
cd /home/aagu/OS01 && git add docs/fs/filesystem.md docs/roadmap.md docs/changelog.md && \
git commit -m "docs: close ARCH-11 devfs sub-directory support

Document the multi-component devfs registration paths, the implicit
directory model and the path validation rules; mark ARCH-11 closed in
the roadmap and add the changelog entry."
```

- [ ] **Step 6: 最终验证**

```bash
cd /home/aagu/OS01 && make PROFILE=x86_64-clang test-host && \
make PROFILE=x86_64-clang test-static && \
make PROFILE=x86_64-clang kernel.bin
```

期望：全部成功。

```bash
cd /home/aagu/OS01 && git status --short && git log --oneline -8
```

期望：工作区干净，能看到本计划的 6 个 Task 提交。
