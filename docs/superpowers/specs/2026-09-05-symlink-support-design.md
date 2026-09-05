# OS01 软链接（symlink）支持 — 设计方案

> **日期**: 2026-09-05
> **状态**: v2，待用户 review（v1 评审 5 项修订已落地）
> **目标**: 在 VFS + ext2 + syscall + libc + Linux ABI 翻译五处落地 POSIX 对称的软链接支持：`symlink(2)` 创建、`readlink(2)` 读取、`lstat(2)` 不跟随、`fstatat(2)` 带 `AT_SYMLINK_NOFOLLOW`。自动让 `exec`/`open`/`stat` 跟随末段与中间段软链接（POSIX-correct），关闭三项 roadmap 项：
> - **P1**「exec 软链接跟随」（同时移除 `b32e1e0` busybox 副本化构建期规避，可选切回符号链接）
> - **P5**「symlink/readlink」
> - **P5**「exec symlink ABI ✅ 缓解」—— 升级为完全解决
>
> **修订**:
> - v1：初版（基于 brainstorming：方案 B lookup flags、MAXSYMLINKS=8、跨 mount 跟随、全套范围）
> - v2：评审修订——
>   - **🟢 [P1] 中间路径组件 symlink**：vfs_lookup 现在单函数内 fold 跟随循环，遇到中段 symlink 立即 splice `target+suffix` 后 restart；不再只跟随末段
>   - **🟢 [P1] 用户指针安全复制**：sys_lstat/sys_fstatat 复用 sys_stat 的 `struct stat kstat; copy_to_user_ft(buf, &kstat, sizeof(kstat))` 模式；用户 buf 坏地址 → -EFAULT 永不 panic
>   - **🟢 [P1] libc syscall3/4 + PF_LINUX_ABI 表**：新增 `syscall3()`（即现行 3-arg `syscall()`）、`syscall4()`（r10 ABI）；Linux→OS01 表补 `[6]=73 lstat`、`[88]=71 symlink`、`[89]=72 readlink`、`[262]=74 newfstatat`（同时修正 v1 `[89]=26` 错映射 SYS_rename 的 bug）
>   - **🟢 [P1] errno 传播**：vfs_lookup_at 改签名为 `int vfs_lookup_at(dirfd, path, flags, vfs_node_t **out)`，返回 0 / -errno（ENOENT/ENOMEM/EIO/ELOOP/ENOTDIR/EOPNOTSUPP/ENAMETOOLONG 原样上抛）；所有 6+ 调用点迁移
>   - **🟡 [P2] DT_LNK + EOPNOTSUPP**：`vfs_getdents` 加 `case VFS_SYMLINK: d_type = DT_LNK`；非 ext2 FS（devfs/procfs/tmpfs/fat）调用 symlink/readlink 时 `sys_*` 检查 `parent->ops->symlink == NULL` → -EOPNOTSUPP

---

## 1. 背景与动机

### 1.1 现状

| 项 | 现状 | 影响 |
|---|---|---|
| `__vfs_lookup`（vfs.c:145） | 逐组件 readdir，不区分 symlink 类型（无 `VFS_SYMLINK` 节点）；不处理中段 symlink；返回 NULL 时 errno 丢失 | busybox 符号链接 + `find -type l` 全部失败 |
| `sys_exec`（task.c:1286） | `vfs_lookup_from` → `node->type != VFS_FILE` 即 -EACCES；若末段是 symlink，ELF 头字节被当 ELF 解析 → -ENOEXEC | busybox rootfs 内 `/bin/wget` 等符号链接失败 |
| `sys_stat`（trap.c:1759） | 用 `struct stat kstat; copy_to_user_ft(buf, &kstat, ...)` ✅ 安全 | 现成模板，需复用 |
| `libc/unistd/symlink.c` | stub `return -1` | 6 个 busybox 调用点（libarchive / copy_file / devfsd / mdev 等）拿不到符号链接 |
| `libc/unistd/readlink.c` | 推测 stub（需确认） | busybox `find -type l`、`ls -l` 等命令无法读取 target |
| `libc/include/sys/syscall.h` | 只有 `syscall()`（3-arg rdi/rsi/rdx）+ `syscall6()`（含 r10） | **缺 `syscall3()`/`syscall4()`**（r10 ABI），影响 fstatat 4 参 |
| `PF_LINUX_ABI` 表（trap.c:1104） | `[6]=-1 lstat` 未支持；`[89]=26 readlink` 错映射为 SYS_rename；缺 `[88] symlink`、`[262] newfstatat` | busybox 等 Linux ABI 程序调用这些 syscall 全部失败 |
| ext2 `dirent_add`（ext2.c:399） | `file_type` 字段已支持，但 `file_type=EXT2_FT_SYMLINK(7)` 路径未实现 | 无法在 ext2 创建符号链接 |
| ext2 inode storage | `ext2_inode_t.i_block[15]` 共 60B 即可存放 fast symlink target | fast path 零 block 分配 |
| `vfs_getdents`（vfs.c:536） | `default: DT_UNKNOWN` | 无 `DT_LNK` 映射 → `find -type l` 退化为 unknown |
| devfs/procfs/tmpfs/fat | `vfs_ops_t` 无 `.symlink` / `.readlink` 槽位 | 后续要支持则按需补；本次显式 -EOPNOTSUPP |

### 1.2 动机

**根本**：POSIX 文件系统语义要求「`symlink(2)` 创建 + `readlink(2)` 读取 + 路径解析跟随末段与中间段软链接」是一个完整的最小集合；任何缺一项都让 busybox、musl、自写工具无法正确实现符号链接语义。

**架构收益**：
1. 一次性落地四项 POSIX 接口 + ext2 存储 + libc 包装 + Linux ABI 翻译 → 关闭 P1 + P5 三项阻塞 roadmap 项（exec 软链接跟随、symlink/readlink、exec symlink ABI）
2. exec 跟随让 busybox rootfs 内的符号链接正常工作，可选移除 `b32e1e0` busybox 副本化构建期变通（节省 rootfs 空间）
3. 为后续 `openat(2)` / `readlinkat(2)` / `symlinkat(2)` / `linkat(2)` 留好 `dirfd` 入口
4. 同时修补 `PF_LINUX_ABI[89]` 错映射为 SYS_rename 的预存 bug（readlink→rename 路径在 busybox 中几乎不会触发，但若运行 `readlink` 命令会触发 — 致命回归）

### 1.3 已确认设计决策

| 项 | 决定 | 理由 |
|---|---|---|
| 跟随深度 | `MAXSYMLINKS = 8` | Linux 同值，足以覆盖实际误用且不误判 |
| 跨 mount 跟随 | 跟随（Linux 行为） | OS01 单 mount，跨 mount 问题不实际出现；统一 Linux 语义便于参考 |
| 范围 | 全套：`symlink` + `readlink` + `lstat` + `fstatat(AT_SYMLINK_NOFOLLOW)` | 用户确认 |
| **中段 symlink** | **resolve**：vfs_lookup 单函数内 fold 跟随循环，遇到中段 symlink splice `target+suffix` 后 restart | POSIX 必需（如 `/link_to_dir/file`） |
| lookup API | `int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags, vfs_node_t **out)` —— 0 成功/-errno 失败 | errno 传播必需（v1 返回 NULL 丢 errno） |
| `stat` 行为 | 改为跟随（POSIX-correct） | 当前无 symlink，外部不可见 breaking |
| 内核栈 | 单 256B 栈 buf（fold 进 `vfs_lookup_at`，无外层/内层分离）+ 必要时 kmalloc target | 用户提出的栈压力顾虑 |
| syscall 编号 | 71..74（71=`symlink`、72=`readlink`、73=`lstat`、74=`fstatat`） | 接续现有 0..70 |
| 用户指针安全 | 所有 syscall 走 `copy_from_user_ft` / `copy_to_user_ft` | 复用 sys_stat 模式，零特殊 |
| libc helper | 新增 `syscall3()`（= 现有 3-arg `syscall()`）+ `syscall4()`（r10 ABI） | fstatat 4 参必需 |
| 非 ext2 FS 行为 | `sys_symlink`/`sys_readlink` 检查 `ops->symlink/readlink == NULL` → -EOPNOTSUPP | 显式失败而非 NULL deref |
| getdents | 加 `VFS_SYMLINK → DT_LNK` | `find -type l` 解锁 |

---

## 2. 总体架构

### 2.1 分层

```
┌────────────────────────────────────────────────────────────┐
│ libc (POSIX layer)                                         │
│  symlink() / readlink() / lstat() / fstatat()              │
│  → SYS_symlink / SYS_readlink / SYS_lstat / SYS_fstatat    │
│  helper: syscall3() / syscall4() (r10 ABI)                 │
├────────────────────────────────────────────────────────────┤
│ syscall layer (kernel/arch/x86_64/trap.c)                  │
│  Linux ABI 翻译 (PF_LINUX_ABI): Linux nr → OS01 nr         │
│    [6]=73 lstat, [88]=71 symlink, [89]=72 readlink,        │
│    [262]=74 newfstatat                                     │
│  sys_symlink / sys_readlink / sys_lstat / sys_fstatat      │
│  + 现有 sys_stat/sys_open/sys_exec 改走 vfs_lookup_at      │
│  用户 buf: struct stat kstat; copy_to_user_ft(buf, &kstat) │
├────────────────────────────────────────────────────────────┤
│ VFS (kernel/fs/vfs.c)                                      │
│  vfs_lookup_at(dirfd, path, flags, &node) → 0 / -errno     │
│  → 单函数内 fold: walk + splice + restart (中段 + 末段)     │
│  → 委托 ext2_ops.readlink / .symlink / .lookup / .mkdir     │
├────────────────────────────────────────────────────────────┤
│ ext2 driver (kernel/fs/ext2.c)                             │
│  ext2_vfs_symlink / ext2_vfs_readlink                      │
│  fast symlink: target ≤60B 存 i_block[], i_blocks=0        │
│  long  symlink: target   → alloc_block → i_block[0]        │
│  + ext2 readdir 识别 file_type=EXT2_FT_SYMLINK → VFS_SYMLINK │
└────────────────────────────────────────────────────────────┘

非 ext2 FS (devfs / procfs / tmpfs / fat):
  sys_* 检查 ops->symlink / ops->readlink == NULL → -EOPNOTSUPP
  vfs_getdents: case VFS_SYMLINK → DT_LNK（已有 VFS_SYMLINK 时）
```

### 2.2 涉及文件

| 文件 | 改动 |
|---|---|
| `kernel/fs/vfs.c` + `kernel/include/fs/vfs.h` | `__vfs_lookup` 改名为 `__vfs_lookup_raw`，签名改为 `(path, flags, consumed_out, size, remaining_out, size)` 中段 symlink 早返；新增 `vfs_lookup_at(dirfd, path, flags, **out)` 单函数 fold 跟随循环；`vfs_stat` 加 `S_IFLNK` case；新增 `VFS_SYMLINK` 类型；新增 `vfs_ops.symlink`/`vfs_ops.readlink` ops；`vfs_getdents` 加 `case VFS_SYMLINK: DT_LNK` |
| `kernel/fs/ext2.c` | 新增 `ext2_vfs_symlink` + `ext2_vfs_readlink`；`ext2_vfs_readdir` 把 `EXT2_FT_SYMLINK` 映射到 `VFS_SYMLINK`；`vfs_ops` 注册新 ops；`ext2_vfs_unlink` 补 long symlink 的 `free_block(i_block[0])` |
| `kernel/sched/task.c` | 新增 `sys_symlink` / `sys_readlink` / `sys_lstat` / `sys_fstatat`；现有 `sys_stat`/`sys_open`/`sys_exec` 改走 `vfs_lookup_at` |
| `kernel/arch/x86_64/trap.c` | syscall 表注册 71..74；`PF_LINUX_ABI` 表补 `[6]=73`、`[88]=71`、`[89]=72`、`[262]=74`（同时修复 v1 `[89]=26` 错映射 bug） |
| `libc/include/sys/syscall.h` | `SYS_symlink=71` 等 4 个宏；`AT_FDCWD=-100`、`AT_SYMLINK_NOFOLLOW=0x100`；新增 `syscall3()` (= 现有 3-arg `syscall()` 别名) 与 `syscall4()`（r10 ABI） |
| `libc/include/sys/stat.h` | `lstat` / `fstatat` 声明 + `AT_*` 常量 |
| `libc/unistd/symlink.c`、`readlink.c` | 替换 stub 为真实现 |
| `libc/sys/stat/lstat.c`、`fstatat.c` | 新建 |
| `test/cases/test_vfs_symlink.c` | 新建——26 个 case（§6） |

---

## 3. VFS 层改动

### 3.1 节点类型与 ops 扩展

```c
// kernel/include/fs/vfs.h

typedef enum {
    VFS_FILE = 0,
    VFS_DIR  = 1,
    VFS_CHRDEV = 2,
    VFS_BLKDEV = 3,
    VFS_SYMLINK = 4,   // 新增
} vfs_node_type_t;

typedef enum {
    LOOKUP_FOLLOW    = 0,
    LOOKUP_NOFOLLOW  = 1,
} lookup_flags_t;

#define MAXSYMLINKS 8

typedef struct vfs_ops {
    /* ... existing ops ... */
    int (*symlink)(struct vfs_node *parent, const char *name,
                   const char *target);
    int (*readlink)(struct vfs_node *node, char *buf, size_t size);
    /* ... */
} vfs_ops_t;
```

### 3.2 `__vfs_lookup_raw` 签名变更（支持中段 symlink 早返）

```c
// 原：static vfs_node_t *__vfs_lookup(const char *path);
// 新：4-状态返回值 + 3 个 out 参数

// 返回值：
//   0    = 成功，*out_node 是最终节点（refcount++），无 remaining suffix
//   1    = 遇到 symlink，*out_node 是 symlink（refcount++），
//          *consumed_out 写到 symlink 为止的绝对前缀，
//          *remaining_out 是 symlink 之后未消费的 suffix
//   < 0  = -errno (ENOENT/ENOTDIR/ENAMETOOLONG/EIO/...)
//          *out_node = NULL
// 当 flags=LOOKUP_NOFOLLOW 时遇到 symlink 行为同 0（直接返回 symlink 节点本身，
// 不进入 remaining_out 路径）—— 因为 caller 不需要继续解析。
static int __vfs_lookup_raw(const char *path,
                            lookup_flags_t flags,
                            vfs_node_t **out_node,
                            char *consumed_out, size_t consumed_size,
                            char *remaining_out, size_t remaining_size);
```

**实现要点**：
- walk 循环里遇到目录项，先看 `entry.type == VFS_SYMLINK`
- 若 `flags & LOOKUP_FOLLOW` 且是中段（后面还有组件）→ 早返 status=1，consumed=`已 walk 完到 symlink 的绝对路径`，remaining=`symlink 之后的部分`
- 若 `flags & LOOKUP_FOLLOW` 且是末段 → 早返 status=1（caller 负责 splice + restart）；实际由 caller（vfs_lookup_at）splice 成新 path 后从头 walk
- 若 `flags & LOOKUP_NOFOLLOW` → 一律 status=0（把 symlink 节点本身当最终节点返回，caller 不再 follow）
- 非 symlink 节点正常 walk，最后返回 status=0
- consumed 累计：walk 完成时 `consumed = path` 全长

**现有调用点迁移**：
- 6 个原 `__vfs_lookup(path)` 调用点全部改用 `vfs_lookup_at(AT_FDCWD, path, LOOKUP_FOLLOW, &node)`，errno 由 caller 按需映射（stat → -ENOENT/-EIO；exec → -ENOENT/-EACCES；open → -ENOENT；chdir → -ENOENT）
- 暂不保留 `__vfs_lookup` 旧 wrapper——所有调用点一起迁移，避免新旧 API 长期共存导致分叉
- 行为变化：`vfs_lookup_at` 现在会跟随末段与中段 symlink；现有调用点的语义对 `LOOKUP_FOLLOW` 都是 POSIX-correct 行为，外部不可见

### 3.3 `vfs_lookup_at` 单函数 fold 跟随循环

```c
// Returns:
//   0  on success: *out_node refcount++, caller must vfs_node_put
//   -errno on failure: *out_node = NULL
// Possible errnos: ENOENT, ENOTDIR, ENAMETOOLONG, ENOMEM, EIO, ELOOP, EOPNOTSUPP, EFAULT (用户指针)
int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node);
```

**核心算法**（fold into single function）：

```c
int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node) {
    *out_node = NULL;
    char remaining[VFS_NAME_MAX];    // 唯一栈 buf（256B）
    int rc = resolve_at(dirfd, path, remaining, sizeof(remaining));
    if (rc < 0) return rc;

    vfs_node_t *node = NULL;
    char consumed[VFS_NAME_MAX];
    char suffix[VFS_NAME_MAX];       // 中段 symlink 之后未消费的 suffix
    int depth = 0;

    for (;;) {
        // Walk 当前 remaining，遇到 symlink 可能早返
        int st = __vfs_lookup_raw(remaining, flags, &node,
                                   consumed, sizeof(consumed),
                                   suffix, sizeof(suffix));
        if (st < 0) return st;
        if (st == 0) {
            // 终态：node 是最终节点（symlink 节点本身 if NOFOLLOW；普通节点 if FOLLOW 且末段非 symlink）
            *out_node = node;
            return 0;
        }
        // st == 1: 遇到 symlink，需要 splice + restart
        if (depth >= MAXSYMLINKS) {
            vfs_node_put(node);
            return -ELOOP;
        }

        // 读 symlink target
        char *target = kmalloc(VFS_NAME_MAX);
        if (!target) { vfs_node_put(node); return -ENOMEM; }
        int tlen = node->ops->readlink(node, target, VFS_NAME_MAX - 1);
        if (tlen < 0) { kfree(target); vfs_node_put(node); return tlen; }
        target[tlen] = '\0';

        // splice 成新 path：target[0]=='/' → 覆盖；否则 → dirname(consumed) + "/" + target + suffix
        char new_remaining[VFS_NAME_MAX];
        splice_symlink_path(consumed, target, suffix,
                            new_remaining, sizeof(new_remaining));
        kfree(target);

        vfs_node_put(node);
        memcpy(remaining, new_remaining, VFS_NAME_MAX);
        depth++;
    }
}

// resolve_at: 本次只实现 AT_FDCWD 语义
//   - dirfd == AT_FDCWD 且 path 绝对 → remaining = path
//   - dirfd == AT_FDCWD 且 path 相对 → 用 current->files->cwd 拼接
//   - 其他 dirfd 值 → -EBADF；本次明确不支持真实 fd，留待 openat 任务
static int resolve_at(int dirfd, const char *path,
                      char *out, size_t out_size);

// splice_symlink_path: 把 consumed + target + suffix 合成新绝对路径
//   - target[0] == '/' → new = target + suffix（绝对覆盖）
//   - 否则 → new = dirname(consumed) + "/" + target + suffix
static int splice_symlink_path(const char *consumed, const char *target,
                               const char *suffix,
                               char *out, size_t out_size);
```

**关键不变量**：
- **无内层递归调用**：所有 stack frame 都在 `vfs_lookup_at` 内（除 `readlink` 与 `__vfs_lookup_raw` 返回前）
- peak stack = `remaining[256]` + `consumed[256]` + `suffix[256]` + `new_remaining[256]` + 局部变量 ≈ **1200B**（详见 §7）
- `target` 总在 heap（避免栈累计）
- 跟随时释放上一轮 `node` 避免 refcount 泄漏

### 3.4 `vfs_stat` S_IFLNK case

```c
case VFS_SYMLINK: buf->st_mode = S_IFLNK | 0777; break;
```

`st_size` = link target 字节数（由 `__vfs_lookup_raw` 在创建 node 时写入 `vfs_node.size`；ext2 readdir 已返回 `entry.size`）。

### 3.5 `vfs_getdents` 加 DT_LNK

```c
switch (e->type) {
case VFS_FILE:   d->d_type = DT_REG; break;
case VFS_DIR:    d->d_type = DT_DIR; break;
case VFS_CHRDEV: d->d_type = DT_CHR; break;
case VFS_BLKDEV: d->d_type = DT_BLK; break;
case VFS_SYMLINK: d->d_type = DT_LNK; break;   // 新增
default:         d->d_type = DT_UNKNOWN; break;
}
```

---

## 4. ext2 改动

### 4.1 `ext2_vfs_symlink`

```c
int ext2_vfs_symlink(struct vfs_node *parent, const char *name,
                     const char *target) {
    ext2_fs_t *fs = (ext2_fs_t *)parent->mount->fs_data;
    uint32_t parent_ino = ext2_node_ino(parent);
    size_t tlen = strlen(target);

    spin_lock(&fs->lock);

    // 1) 检查 linkpath 已存在
    uint32_t existing_ino; uint8_t ft; uint32_t blk, off;
    if (ext2_find_dirent(fs, parent_ino, name, &existing_ino, &ft, &blk, &off) == 0) {
        spin_unlock(&fs->lock); return -EEXIST;
    }

    // 2) 分配 inode（mode = S_IFLNK | 0777）
    uint32_t ino = alloc_inode(fs, EXT2_S_IFLNK | 0777);
    if (ino == 0) { spin_unlock(&fs->lock); return -ENOSPC; }

    // 3) 写 target
    ext2_inode_t inode;
    ext2_read_inode(fs, ino, &inode);
    inode.i_size = (uint32_t)tlen;
    if (tlen <= 60) {
        inode.i_blocks = 0;
        memset(inode.i_block, 0, sizeof(inode.i_block));
        memcpy(inode.i_block, target, tlen);
    } else {
        uint32_t blk = alloc_block(fs);
        if (blk == 0) {
            free_inode(fs, ino);
            spin_unlock(&fs->lock);
            return -ENOSPC;
        }
        inode.i_blocks = fs->block_size / 512;
        inode.i_block[0] = blk;

        uint8_t *buf = kmalloc(fs->block_size);
        if (!buf) {
            free_block(fs, blk);
            free_inode(fs, ino);
            spin_unlock(&fs->lock);
            return -ENOMEM;
        }
        memcpy(buf, target, tlen);
        memset(buf + tlen, 0, fs->block_size - tlen);
        ext2_write_block(fs, blk, buf);
        kfree(buf);
    }
    ext2_write_inode(fs, ino, &inode);

    int rc = dirent_add(fs, parent_ino, name, ino, EXT2_FT_SYMLINK);
    spin_unlock(&fs->lock);
    return rc;
}
```

### 4.2 `ext2_vfs_readlink`

```c
int ext2_vfs_readlink(struct vfs_node *node, char *buf, size_t size) {
    uint32_t ino = ext2_node_ino(node);
    ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;

    spin_lock(&fs->lock);
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) {
        spin_unlock(&fs->lock); return -EIO;
    }

    int rc;
    if (inode.i_blocks == 0) {
        size_t tlen = inode.i_size;
        if (tlen > size) tlen = size;
        memcpy(buf, inode.i_block, tlen);
        rc = (int)tlen;
    } else {
        uint32_t phys = inode.i_block[0];
        if (phys == 0) { spin_unlock(&fs->lock); return -EIO; }

        uint8_t *block_buf = kmalloc(fs->block_size);
        if (!block_buf) { spin_unlock(&fs->lock); return -ENOMEM; }
        if (ext2_read_block(fs, phys, block_buf) != 0) {
            kfree(block_buf); spin_unlock(&fs->lock); return -EIO;
        }
        size_t tlen = inode.i_size;
        if (tlen > size) tlen = size;
        memcpy(buf, block_buf, tlen);
        kfree(block_buf);
        rc = (int)tlen;
    }
    spin_unlock(&fs->lock);
    return rc;
}
```

### 4.3 `ext2_vfs_readdir` 映射 symlink file_type

```c
case EXT2_FT_SYMLINK: entry->type = VFS_SYMLINK; break;
```

### 4.4 `ext2_vfs_unlink` 补 long symlink 释放

```c
if (inode.i_mode & EXT2_S_IFLNK && inode.i_blocks > 0) {
    free_block(fs, inode.i_block[0]);
}
```

`ext2_vfs_unlink` 现有实现需先 `ext2_read_inode`（已有）→ 加入此分支。

---

## 5. syscall + libc + Linux ABI 改动

### 5.1 syscall 表

```c
// kernel/arch/x86_64/trap.c
#define SYS_symlink    71
#define SYS_readlink   72
#define SYS_lstat      73
#define SYS_fstatat    74
```

`sys_stat(16)` / `sys_open(7)` / `sys_exec(5)` 改走 `vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node)`。语义变化：`stat` 现在跟随 symlink（POSIX-correct；当前无 symlink 所以外部不可见）。

### 5.2 `PF_LINUX_ABI` 翻译表（trap.c:1104）

**当前问题**：`[6]=-1 lstat 未支持`、`[89]=26 readlink 错映射 SYS_rename`、缺 `[88] symlink` 与 `[262] newfstatat`。

**修复（直接 patch）**：

```c
// 在 linux_to_os01[320] 表中修改：
[6]  = 73, // lstat → SYS_lstat   (原 -1 unsupported)
[88] = 71, // symlink → SYS_symlink (新增)
[89] = 72, // readlink → SYS_readlink (原 26 = SYS_rename 错映射！)
[262] = 74, // newfstatat → SYS_fstatat (新增)
```

Linux nr 范围：`man 2 syscall` 即可查证（lstat=6, symlink=88, readlink=89, newfstatat=262）。

### 5.3 syscall 实现（`kernel/sched/task.c`）

所有用户指针走 `strncpy_from_user` / `copy_to_user_ft` 等已审计原语。**所有写用户 buf 都走 fault-tolerant 模式**（先 build kernel-local copy，再 `_ft` 复制）。

```c
// ── SYS_symlink(71): symlink(target, linkpath) ────────────
int64_t sys_symlink(const char *target, const char *linkpath, pt_regs_t *regs) {
    (void)regs;
    if (!target || !linkpath) return -EFAULT;

    char target_copy[VFS_NAME_MAX];
    int tlen = strncpy_from_user(target_copy, target, VFS_NAME_MAX);
    if (tlen < 0) return -EFAULT;
    if (tlen == 0) return -ENOENT;     // 拒绝空 target

    char linkpath_copy[VFS_NAME_MAX];
    if (strncpy_from_user(linkpath_copy, linkpath, VFS_NAME_MAX) < 0)
        return -EFAULT;

    // 解析 linkpath 的父目录（split: parent dir + basename）
    char parent_path[VFS_NAME_MAX];
    strncpy(parent_path, linkpath_copy, sizeof(parent_path));
    char *base = strrchr(parent_path, '/');
    if (!base) return -ENOENT;
    if (base == parent_path) {
        // linkpath = "/name" → parent = "/"
        base[1] = '\0';
        base = parent_path + 1;
    } else {
        *base = '\0';
        base++;
    }

    vfs_node_t *parent = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, parent_path, LOOKUP_FOLLOW, &parent);
    if (rc < 0) return rc;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }

    // EOPNOTSUPP 守卫：非 ext2 FS 无 symlink op
    if (!parent->ops || !parent->ops->symlink) {
        vfs_node_put(parent); return -EOPNOTSUPP;
    }

    rc = parent->ops->symlink(parent, base, target_copy);
    vfs_node_put(parent);
    return rc;
}

// ── SYS_readlink(72): readlink(path, buf, bufsize) ────────
int64_t sys_readlink(const char *path, char *buf, size_t bufsize,
                     pt_regs_t *regs) {
    (void)regs;
    if (!path || !buf) return -EFAULT;
    if (bufsize == 0) return -EINVAL;

    char path_copy[VFS_NAME_MAX];
    if (strncpy_from_user(path_copy, path, VFS_NAME_MAX) < 0)
        return -EFAULT;

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW, &node);
    if (rc < 0) return rc;
    if (node->type != VFS_SYMLINK) { vfs_node_put(node); return -EINVAL; }

    // EOPNOTSUPP 守卫
    if (!node->ops || !node->ops->readlink) {
        vfs_node_put(node); return -EOPNOTSUPP;
    }

    char kbuf[VFS_NAME_MAX];
    int tlen = node->ops->readlink(node, kbuf, VFS_NAME_MAX - 1);
    vfs_node_put(node);
    if (tlen < 0) return tlen;

    if ((size_t)tlen > bufsize) tlen = bufsize;
    if (copy_to_user_ft(buf, kbuf, tlen) < 0) return -EFAULT;
    return tlen;
}

// ── SYS_lstat(73): lstat(path, buf) — NOFOLLOW ───────────
int64_t sys_lstat(const char *path, struct stat *buf, pt_regs_t *regs) {
    (void)regs;
    if (!path || !buf) return -EFAULT;
    char path_copy[VFS_NAME_MAX];
    if (strncpy_from_user(path_copy, path, VFS_NAME_MAX) < 0)
        return -EFAULT;

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW, &node);
    if (rc < 0) return rc;

    // ★ v1 错位：vfs_stat 直接 memset 用户 buf；现先 build kernel-local 再 _ft copy
    struct stat kstat;
    rc = vfs_stat(node, &kstat);
    vfs_node_put(node);
    if (rc < 0) return rc;

    if (copy_to_user_ft(buf, &kstat, sizeof(kstat)) < 0) return -EFAULT;
    return 0;
}

// ── SYS_fstatat(74): fstatat(dirfd, path, buf, flags) ────
int64_t sys_fstatat(int dirfd, const char *path, struct stat *buf,
                    int flags, pt_regs_t *regs) {
    (void)regs;
    if (!path || !buf) return -EFAULT;
    char path_copy[VFS_NAME_MAX];
    if (strncpy_from_user(path_copy, path, VFS_NAME_MAX) < 0)
        return -EFAULT;

    lookup_flags_t lflags =
        (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(dirfd, path_copy, lflags, &node);
    if (rc < 0) return rc;

    struct stat kstat;
    rc = vfs_stat(node, &kstat);
    vfs_node_put(node);
    if (rc < 0) return rc;

    if (copy_to_user_ft(buf, &kstat, sizeof(kstat)) < 0) return -EFAULT;
    return 0;
}
```

### 5.4 libc 实现

**新 helper**（libc/include/sys/syscall.h）：

```c
// syscall3 = 现有 syscall() 的语义别名（3 arg via rdi/rsi/rdx）
// 提供明确命名便于阅读
static inline int64_t syscall3(uint64_t nr,
                                uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    return syscall(nr, arg1, arg2, arg3);
}

// syscall4: 4 arg，第 4 个走 r10（Linux x86-64 ABI 约定）
static inline int64_t syscall4(uint64_t nr,
                                uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                uint64_t arg4) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3), "r" (r10)
        : "memory");
    return ret;
}
```

**POSIX wrapper**：

```c
// libc/unistd/symlink.c
int symlink(const char *target, const char *linkpath) {
    int64_t ret = syscall3(SYS_symlink, (uint64_t)target, (uint64_t)linkpath, 0);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

// libc/unistd/readlink.c
ssize_t readlink(const char *path, char *buf, size_t bufsize) {
    int64_t ret = syscall3(SYS_readlink, (uint64_t)path, (uint64_t)buf, (uint64_t)bufsize);
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}

// libc/sys/stat/lstat.c
int lstat(const char *path, struct stat *buf) {
    int64_t ret = syscall3(SYS_lstat, (uint64_t)path, (uint64_t)buf, 0);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

// libc/sys/stat/fstatat.c
int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    int64_t ret = syscall4(SYS_fstatat, dirfd, (uint64_t)path, (uint64_t)buf, flags);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}
```

**常量声明**（libc/include/sys/stat.h）：

```c
#define AT_FDCWD              -100
#define AT_SYMLINK_NOFOLLOW    0x100
int lstat(const char *path, struct stat *buf);
int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
```

---

## 6. 测试矩阵

`test/cases/test_vfs_symlink.c` —— 26 个 case：

```
基础创建与读取
  01 fast_symlink_create_read()        target ≤60B
  02 long_symlink_create_read()         target 80B
  03 symlink_eexist()                  linkpath 已存在 → -EEXIST
  04 symlink_empty_target()            target="" → -ENOENT
  05 symlink_nonexist_parent()         parent 目录不存在 → -ENOENT
  06 readlink_on_regular()              -EINVAL
  07 readlink_bufsize_zero()            -EINVAL
  08 unlink_cleans_long_symlink()      long symlink unlink 后 data block 已释放

跟随语义（末段）
  09 symlink_follow_in_lookup()         FOLLOW → 跟随到 target
  10 symlink_no_follow_returns_link()   NOFOLLOW → 返回 link 节点本身
  11 stat_follows()                     stat 跟随到 target
  12 stat_on_link_no_follow()           lstat → S_IFLNK
  13 fstatat_no_follow()                AT_SYMLINK_NOFOLLOW
  14 fstatat_follow_default()           flags=0 跟随

跟随语义（中段 + v2 新增）
  15 symlink_mid_path_absolute()        /link_to_dir/file → /real_dir/file
  16 symlink_mid_path_relative()        /a/link_to_dir/file → /a/real_dir/file
  17 symlink_mid_path_no_follow()       /link_to_dir/file NOFOLLOW → 返回 link
  18 symlink_mid_path_with_suffix()     /link_to_dir/a/b → /real_dir/a/b
  19 symlink_chained_mid_path()         /a/link1/link2/file（双层）

环路与深度
  20 symlink_loop_eloop()               A→B→A → ELOOP
  21 symlink_depth_8_passes()           link1→…→link8 (8 跳 OK)
  22 symlink_depth_mid_path()           /a/link1/link2/.../link8/f (中段 8 跳)

错误码传播（v2 新增）
  23 lookup_enotdir_for_mid()           /file/link_to_dir/f → ENOTDIR
  24 lookup_enametoolong()              VFS_NAME_MAX+1 path → ENAMETOOLONG
  25 stat_eopnotsupp_on_tmpfs()         /tmp 路径下 symlink 不支持 → EOPNOTSUPP

集成
  26 systest_busybox_ln_exec()          ln -s /bin/busybox /tmp/x; /tmp/x --help → exit 0
  27 systest_find_type_l()              find / -type l → 输出含 l → DT_LNK
```

`test/cases/test_systest.c` 加 case 26, 27。

### 6.1 错误码矩阵（v2 增强）

| 场景 | 返回 | errno |
|---|---|---|
| `symlink("/x", "/y")` 已存在 | -EEXIST | EEXIST |
| `symlink("", "/y")` 空 target | -ENOENT | ENOENT |
| `symlink("/x", "/nonexist/z")` 父不存在 | -ENOENT | ENOENT |
| `symlink("/x", "/regular-file/z")` 父是普通文件 | -ENOTDIR | ENOTDIR |
| `symlink("/x", NULL)` / 用户指针非法 | -EFAULT | EFAULT |
| `symlink("/x", "/tmp/x")` tmpfs 无 symlink op | -EOPNOTSUPP | EOPNOTSUPP |
| `readlink("/regular-file", ...)` | -EINVAL | EINVAL |
| `readlink("/loop1", buf, 0)` bufsize=0 | -EINVAL | EINVAL |
| `readlink("/dev/null", ...)` devfs 无 readlink op | -EOPNOTSUPP | EOPNOTSUPP |
| `readlink("/nonexist", ...)` | -ENOENT | ENOENT |
| `readlink("/regular-file", buf, 0xdeadbeef)` 用户 buf 非法 | -EFAULT | EFAULT |
| `lstat("/bin/sh")` (symlink) → S_IFLNK \| 0777, size=target 长度 | 0 | - |
| `lstat("/bin/regular")` → S_IFREG \| 0755 | 0 | - |
| `lstat("/no/such")` | -ENOENT | ENOENT |
| `lstat(..., 0xdeadbeef)` 用户 buf 非法 | -EFAULT | EFAULT |
| `stat("/bin/sh")` 跟随到 /bin/busybox | 0 | - |
| `fstatat(AT_FDCWD, "/bin/sh", buf, AT_SYMLINK_NOFOLLOW)` → link 自身 | 0 | - |
| `fstatat(AT_FDCWD, "/bin/sh", buf, 0)` → busybox | 0 | - |
| 跟随超过 8 跳（末段或中段） | -ELOOP | ELOOP |
| 跟随中间 readlink 失败 | -EIO | EIO |
| 跟随到不存在的 target | -ENOENT | ENOENT |
| 跟随路径含非法组件（ENOTDIR 等） | -ENOTDIR | ENOTDIR |
| 路径总长 > VFS_NAME_MAX | -ENAMETOOLONG | ENAMETOOLONG |

---

## 7. 栈使用复核

| 函数 | 栈上 buf | 字节 |
|---|---|---|
| `vfs_lookup_at` | `remaining[256]` | 256 |
| `vfs_lookup_at` | `consumed[256]` | 256 |
| `vfs_lookup_at` | `suffix[256]` | 256 |
| `vfs_lookup_at` | `new_remaining[256]` | 256 |
| `vfs_lookup_at` 堆 | `target` (kmalloc) | 0 |
| `__vfs_lookup_raw`（内层） | `path_copy[256]` + `_entry_name[256]` + locals | ~600 |
| 内层调用栈（frame ptr + locals） | ≈ | ~100 |
| **peak** | | **~1720** |

STACK_SIZE = 32KB（task.h:46）。**峰值 1720B ≈ 5%**。安全余量充足。

`readlink` / `lstat` / `fstatat` 各 syscall 顶层栈：单 buf 256B + `kstat` (sizeof(struct stat)) + 局部变量 ≈ 500B，远低于栈限。

---

## 8. 风险与不在范围内

### 8.1 风险

| 风险 | 缓解 |
|---|---|
| **exec symlink TOCTOU**：lookup 时 link → X，exec 加载时 X 被替换为 Y | OS01 单用户，影响小；文档化。Linux 同样不防此 TOCTOU（需 `O_NOFOLLOW` open 语义配合，OS01 无 `open` flag 扩展） |
| **`__vfs_lookup_raw` 签名变化**：3 个 out 参数影响 6 个现有调用点 | 提供 thin wrapper `vfs_lookup_legacy(path)`；迁移期机械替换 |
| **新增 kmalloc 失败路径** | vfs_lookup_at 处理 kmalloc 失败返回 -ENOMEM |
| **`consumed_out`/`suffix` 截断**：buffer < path 长度时不写 | 不影响主路径（vfs_lookup_at 用 sizeof = 256），只影响理论极端长路径；v1 矩阵新增 ENAMETOOLONG 显式错误 |
| **ext2 unlink 漏释放 long symlink 的 data block** | §4.4 显式补；若现状已正确则 no-op |
| **busybox rootfs 切回符号链接**（移除 `b32e1e0`） | **不在本次范围**——本次仅启用 kernel 支持，下次重构验证后切回 |
| **非 ext2 FS 未实现 symlink/readlink** | sys_* 守卫 → -EOPNOTSUPP（显式失败）而非 NULL deref |

### 8.2 不在范围（明确不做）

- `AT_EMPTY_PATH` / `AT_NO_AUTOMOUNT` / `AT_RECURSIVE`
- `readlinkat(2)`（at 版 readlink）
- `symlinkat(2)`（at 版 symlink）
- `linkat(2)` / `link(2)` / `unlink(2)`（unlink 已存在；link 不在范围）
- `lchmod(2)` / `chown` 对 symlink 的处理
- 真实 `dirfd` 实现（fd → vfs_node_t 表）—— 当前只支持 `AT_FDCWD`
- 权限/owner 检查（OS01 root 单用户）
- 链接数（`i_links_count`）跟踪 + 硬 link（POSIX `link(2)`）
- 其他 FS（devfs/procfs/tmpfs/fat）实现 symlink/readlink op

### 8.3 后续 roadmap 联动

完成后可关闭/升级：
- P1「exec 软链接跟随」→ ✅
- P5「symlink/readlink」→ ✅
- P5「exec symlink ABI ✅ 缓解」→ 完全解决（可移除 `b32e1e0` busybox 副本化构建期变通，下次重构验证）
- P5「更多 applet」→ 部分解锁（`find -type l`、`ln` 等可工作）
- **附带修复**：PF_LINUX_ABI 表 `[89]=26 readlink→rename` 错映射 bug 同期修复