# OS01 软链接（symlink）支持 — 设计方案

> **日期**: 2026-09-05
> **状态**: v1，待用户 review（尚未进入实现）
> **目标**: 在 VFS + ext2 + syscall + libc 四层落地 POSIX 对称的软链接支持：`symlink(2)` 创建、`readlink(2)` 读取、`lstat(2)` 不跟随、`fstatat(2)` 带 `AT_SYMLINK_NOFOLLOW`。自动让 `exec`/`open`/`stat` 跟随末段软链接（POSIX-correct），关闭三项 roadmap 项：
> - **P1**「exec 软链接跟随」（同时移除 `b32e1e0` busybox 副本化构建期规避，可选切回符号链接）
> - **P5**「symlink/readlink」
> - **P5**「exec symlink ABI ✅ 缓解」—— 升级为完全解决
>
> **修订**:
> - v1：初版（基于 brainstorming：方案 B lookup flags、MAXSYMLINKS=8、跨 mount 跟随、全套范围）

---

## 1. 背景与动机

### 1.1 现状

| 项 | 现状 | 影响 |
|---|---|---|
| `__vfs_lookup`（vfs.c:145） | 逐组件 readdir，不区分 symlink 类型（无 `VFS_SYMLINK` 节点） | `lookup` 永远返回字面目录项 |
| `sys_exec`（task.c:1286） | `vfs_lookup_from` → `node->type != VFS_FILE` 即 -EACCES；若末段是 symlink，ELF 头字节被当 ELF 解析 → -ENOEXEC | busybox rootfs 内 `/bin/wget` 等符号链接失败 |
| `libc/unistd/symlink.c` | stub `return -1` | 6 个 busybox 调用点（libarchive / copy_file / devfsd / mdev 等）拿不到符号链接 |
| `libc/unistd/readlink.c` | 推测 stub（需确认） | busybox `find -type l`、`ls -l` 等命令无法读取 target |
| ext2 `dirent_add`（ext2.c:399） | `file_type` 字段已支持，但 `file_type=EXT2_FT_SYMLINK(7)` 路径未实现 | 无法在 ext2 创建符号链接 |
| ext2 inode storage | `ext2_inode_t.i_block[15]` 共 60B 即可存放 fast symlink target | fast path 零 block 分配 |

### 1.2 动机

**根本**：POSIX 文件系统语义要求「`symlink(2)` 创建 + `readlink(2)` 读取 + 路径解析跟随末段软链接」是一个完整的最小集合；任何缺一项都让 busybox、musl、自写工具无法正确实现符号链接语义。

**架构收益**：
1. 一次性落地四项 POSIX 接口 + ext2 存储 + libc 包装 → 关闭 P1 + P5 三项阻塞 roadmap 项（exec 软链接跟随、symlink/readlink、exec symlink ABI）
2. exec 跟随让 busybox rootfs 内的符号链接正常工作，可选移除 `b32e1e0` busybox 副本化构建期变通（节省 rootfs 空间）
3. 为后续 `openat(2)` / `readlinkat(2)` / `symlinkat(2)` / `linkat(2)` 留好 `dirfd` 入口

### 1.3 已确认设计决策

| 项 | 决定 | 理由 |
|---|---|---|
| 跟随深度 | `MAXSYMLINKS = 8` | Linux 同值，足以覆盖实际误用且不误判 |
| 跨 mount 跟随 | 跟随（Linux 行为） | OS01 单 mount，跨 mount 问题不实际出现；统一 Linux 语义便于参考 |
| 范围 | 全套：`symlink` + `readlink` + `lstat` + `fstatat(AT_SYMLINK_NOFOLLOW)` | 用户确认 |
| lookup API | 方案 B：flags 参数 (`LOOKUP_FOLLOW` / `LOOKUP_NOFOLLOW`) | Linux 风格；为后续 `dirfd` 维度预留 |
| `stat` 行为 | 改为跟随（POSIX-correct） | 当前无 symlink，外部不可见 breaking |
| 内核栈 | 单 256B 栈 buf + 必要时 kmalloc target + 拆双层（`vfs_lookup_at` ↔ `__vfs_lookup_raw`） | 用户提出的栈压力顾虑 |
| syscall 编号 | 71..74（71=`symlink`、72=`readlink`、73=`lstat`、74=`fstatat`） | 接续现有 0..70 |

---

## 2. 总体架构

### 2.1 分层

```
┌────────────────────────────────────────────────────────┐
│ libc (POSIX layer)                                     │
│  symlink() / readlink() / lstat() / fstatat()          │
│  → SYS_symlink / SYS_readlink / SYS_lstat / SYS_fstatat│
├────────────────────────────────────────────────────────┤
│ syscall layer (kernel/sched/task.c)                    │
│  sys_symlink / sys_readlink / sys_lstat / sys_fstatat  │
│  + 现有 sys_stat/sys_open/sys_exec 改走 vfs_lookup_at  │
├────────────────────────────────────────────────────────┤
│ VFS (kernel/fs/vfs.c)                                  │
│  vfs_lookup_at(dirfd, path, flags)                     │
│  → __vfs_lookup_raw(path, consumed_out)                │
│  → symlink 跟随循环（≤ MAXSYMLINKS）                    │
│  → ext2_ops.readlink / .symlink / .lookup / .mkdir     │
├────────────────────────────────────────────────────────┤
│ ext2 driver (kernel/fs/ext2.c)                         │
│  ext2_vfs_symlink / ext2_vfs_readlink                  │
│  fast symlink: target ≤60B 存 i_block[], i_blocks=0    │
│  long  symlink: target   → alloc_block → i_block[0]    │
│  + ext2 readdir 识别 file_type=EXT2_FT_SYMLINK → VFS_SYMLINK │
└────────────────────────────────────────────────────────┘
```

### 2.2 涉及文件

| 文件 | 改动 |
|---|---|
| `kernel/fs/vfs.c` + `kernel/include/fs/vfs.h` | `__vfs_lookup` 改名为 `__vfs_lookup_raw(path, consumed_out, size)`；新增 `vfs_lookup_at(dirfd, path, flags)`；`vfs_stat` 加 `S_IFLNK` case；新增 `VFS_SYMLINK` 类型；新增 `vfs_ops.symlink`/`vfs_ops.readlink` ops |
| `kernel/fs/ext2.c` | 新增 `ext2_vfs_symlink` + `ext2_vfs_readlink`；`ext2_vfs_readdir` 把 `EXT2_FT_SYMLINK` 映射到 `VFS_SYMLINK`；`vfs_ops` 注册新 ops；`ext2_vfs_unlink` 补 long symlink 的 `free_block(i_block[0])` |
| `kernel/sched/task.c` | 新增 `sys_symlink` / `sys_readlink` / `sys_lstat` / `sys_fstatat`；现有 `sys_stat`/`sys_open`/`sys_exec` 改走 `vfs_lookup_at` |
| `kernel/arch/x86_64/trap.c` | syscall 表注册 71..74 |
| `libc/include/sys/syscall.h` | `SYS_symlink=71` 等宏 + `AT_FDCWD=-100`、`AT_SYMLINK_NOFOLLOW=0x100` |
| `libc/include/sys/stat.h` | `lstat` / `fstatat` 声明 + `AT_*` 常量 |
| `libc/unistd/symlink.c`、`readlink.c` | 替换 stub 为真实现 |
| `libc/sys/stat/lstat.c`、`fstatat.c` | 新建（libc sys/stat 目录如不存在则建） |
| `test/cases/test_vfs_symlink.c` | 新建——20 个 case（§6） |

---

## 3. VFS 层改动

### 3.1 节点类型与 ops 扩展

```c
// kernel/include/fs/vfs.h

// 新增节点类型
typedef enum {
    VFS_FILE = 0,
    VFS_DIR  = 1,
    VFS_CHRDEV = 2,
    VFS_BLKDEV = 3,
    VFS_SYMLINK = 4,   // 新增
} vfs_node_type_t;

// lookup flags
typedef enum {
    LOOKUP_FOLLOW    = 0,
    LOOKUP_NOFOLLOW  = 1,
} lookup_flags_t;

#define MAXSYMLINKS 8

// ops 扩展
typedef struct vfs_ops {
    /* ... existing ops ... */
    int (*symlink)(struct vfs_node *parent, const char *name,
                   const char *target);
    int (*readlink)(struct vfs_node *node, char *buf, size_t size);
    /* ... */
} vfs_ops_t;
```

### 3.2 `__vfs_lookup_raw` 签名变更

```c
// 原：static vfs_node_t *__vfs_lookup(const char *path);
// 新：加 consumed 输出参数（NULL/0 表示不关心，保持现有 6 个调用点最小改动）

static vfs_node_t *__vfs_lookup_raw(const char *path,
                                    char *consumed_out, size_t consumed_size);
```

**consumed_out 语义**：写入「到此节点为止的绝对路径前缀」，含尾斜杠前的最后一段（即 `consumed_out` = `current` 之前的 `path` 内容 + 当前节点名）。例如 `path="/a/b/c"`，走到 `c` 时 consumed = `"/a/b/c"`，走到 `b` 时 consumed = `"/a/b"`。

**实现要点**：
- 在 walk 循环里，每个组件 `comp` 之前，把当前 `ptr` 起点到当前节点名的边界写入 consumed_out
- 简单做法：进入循环前记录 start；找到组件名后把 `start..ptr'+(component_end-start)` 拷贝到 consumed_out
- 大小检查 `consumed_size > plen+1`，否则截断到不写

**现有调用点迁移**：6 个 `__vfs_lookup` 调用点全部改为 `__vfs_lookup_raw(path, NULL, 0)`，零行为变化。

### 3.3 `vfs_lookup_at` 实现（外层 + symlink 跟随）

```c
vfs_node_t *vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags) {
    char remaining[VFS_NAME_MAX];   // 唯一栈 buf
    if (!resolve_at(dirfd, path, remaining, sizeof(remaining))) return NULL;

    char consumed[VFS_NAME_MAX];
    vfs_node_t *node = __vfs_lookup_raw(remaining, consumed, sizeof(consumed));
    if (!node) return NULL;

    int depth = 0;
    while (depth < MAXSYMLINKS &&
           node->type == VFS_SYMLINK &&
           !(flags & LOOKUP_NOFOLLOW)) {

        char *target = kmalloc(VFS_NAME_MAX);
        if (!target) { vfs_node_put(node); return NULL; }
        int tlen = node->ops->readlink(node, target, VFS_NAME_MAX - 1);
        if (tlen < 0) { kfree(target); vfs_node_put(node); return NULL; }
        target[tlen] = '\0';

        char new_remaining[VFS_NAME_MAX];
        splice_symlink_path(consumed, target, remaining, new_remaining,
                            sizeof(new_remaining));
        kfree(target);

        vfs_node_put(node);
        node = __vfs_lookup_raw(new_remaining, consumed, sizeof(consumed));
        if (!node) return NULL;
        depth++;
    }

    if (depth >= MAXSYMLINKS && node->type == VFS_SYMLINK) {
        vfs_node_put(node);
        return NULL;   // → ELOOP
    }
    return node;
}

// resolve_at: 本次只实现 AT_FDCWD 语义
//   - dirfd == AT_FDCWD 且 path 绝对 → remaining = path
//   - dirfd == AT_FDCWD 且 path 相对 → 用 current->files->cwd 拼接
//   - 其他 dirfd 值 → -EBADF；本次明确不支持真实 fd，留待 openat 任务
static int resolve_at(int dirfd, const char *path,
                      char *out, size_t out_size);

// splice_symlink_path:
//   - target[0] == '/' → new_remaining = target
//   - 否则 → new_remaining = dirname(consumed) + "/" + target
//                （若 target 含 ".." 或 ".", 用 path-normalize 简化）
static void splice_symlink_path(const char *consumed, const char *target,
                               const char *original_remaining,
                               char *out, size_t out_size);
```

**关键不变量**：
- `__vfs_lookup_raw` 每次调用前栈帧已 unwind（拆双层）→ peak stack = 外层（remaining+consumed+new_remaining = 768B）+ 内层（path_copy+_entry_name+locals ≈ 612B）≈ **1380B**（详见 §7）
- `consumed` 256B 在外层栈上，每次迭代由 `__vfs_lookup_raw` 重写
- `target` 总在 heap（避免栈累计）

### 3.4 `vfs_stat` S_IFLNK case

```c
case VFS_SYMLINK: buf->st_mode = S_IFLNK | 0777; break;
```

`st_size` = link target 字节数（由 `__vfs_lookup_raw` 在创建 node 时写入；vfs_node.size 已有此字段，ext2 readdir 返回 `entry.size`）。

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
        // Fast symlink: target 在 i_block[0..59]
        inode.i_blocks = 0;
        memset(inode.i_block, 0, sizeof(inode.i_block));
        memcpy(inode.i_block, target, tlen);
    } else {
        // Long symlink: 第一个 data block 装 target
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

    // 4) 添加目录项（file_type = EXT2_FT_SYMLINK = 7）
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
        // Fast symlink: target 在 i_block[0..59]
        size_t tlen = inode.i_size;
        if (tlen > size) tlen = size;
        memcpy(buf, inode.i_block, tlen);
        rc = (int)tlen;
    } else {
        // Long symlink
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
// 在现有 free_inode 之前，若 inode 是 symlink 且 i_blocks > 0，释放 i_block[0]
if (inode.i_mode & EXT2_S_IFLNK && inode.i_blocks > 0) {
    free_block(fs, inode.i_block[0]);
}
```

`ext2_vfs_unlink` 现有实现需先 `ext2_read_inode`（已有）→ 加入此分支。

---

## 5. syscall + libc 改动

### 5.1 syscall 表

```c
// kernel/arch/x86_64/trap.c
#define SYS_symlink    71
#define SYS_readlink   72
#define SYS_lstat      73
#define SYS_fstatat    74
```

`sys_stat(15)` / `sys_open(2)` / `sys_exec(59)` 改走 `vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW)`。语义变化：`stat` 现在跟随 symlink（POSIX-correct；当前无 symlink 所以外部不可见）。

### 5.2 syscall 实现（`kernel/sched/task.c`）

**用户指针走 syscall 边界审计规范**（用 `strncpy_from_user` / `copy_to_user` 等已审计原语，do_PF → -EFAULT）。

```c
// ── SYS_symlink(71): symlink(target, linkpath) ────────────
int64_t sys_symlink(const char *target, const char *linkpath, pt_regs_t *regs) {
    (void)regs;
    if (!target || !linkpath) return -EFAULT;

    char target_copy[VFS_NAME_MAX];
    if (strncpy_from_user(target_copy, target, VFS_NAME_MAX) < 0)
        return -EFAULT;
    if (target_copy[0] == '\0') return -ENOENT;   // 拒绝空 target

    char linkpath_copy[VFS_NAME_MAX];
    if (strncpy_from_user(linkpath_copy, linkpath, VFS_NAME_MAX) < 0)
        return -EFAULT;

    vfs_node_t *parent = resolve_parent(linkpath_copy);  // 内部 helper
    if (!parent) return -ENOENT;
    const char *name = basename_of(linkpath_copy);

    int rc = parent->ops->symlink(parent, name, target_copy);
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

    vfs_node_t *node = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW);
    if (!node) return -ENOENT;
    if (node->type != VFS_SYMLINK) { vfs_node_put(node); return -EINVAL; }

    char kbuf[VFS_NAME_MAX];
    int tlen = node->ops->readlink(node, kbuf, VFS_NAME_MAX - 1);
    vfs_node_put(node);
    if (tlen < 0) return tlen;

    if ((size_t)tlen > bufsize) tlen = bufsize;
    if (copy_to_user(buf, kbuf, tlen) < 0) return -EFAULT;
    return tlen;
}

// ── SYS_lstat(73): lstat(path, buf) — NOFOLLOW ───────────
int64_t sys_lstat(const char *path, struct stat *buf, pt_regs_t *regs) {
    (void)regs;
    if (!path || !buf) return -EFAULT;
    char path_copy[VFS_NAME_MAX];
    if (strncpy_from_user(path_copy, path, VFS_NAME_MAX) < 0)
        return -EFAULT;

    vfs_node_t *node = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW);
    if (!node) return -ENOENT;
    int rc = vfs_stat(node, buf);
    vfs_node_put(node);
    return rc;
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
    vfs_node_t *node = vfs_lookup_at(dirfd, path_copy, lflags);
    if (!node) return -ENOENT;
    int rc = vfs_stat(node, buf);
    vfs_node_put(node);
    return rc;
}
```

要点：
- `sys_symlink` 拒绝空 target → -ENOENT（POSIX 未强制；OS01 简化边角 case）
- `sys_readlink` 用 `LOOKUP_NOFOLLOW`，buf 大小由 `bufsize` 限，返回实际字节数（不含 NUL）
- `sys_readlink` 对非 symlink 节点返回 -EINVAL
- `sys_lstat` 用 `LOOKUP_NOFOLLOW`
- `sys_fstatat(dirfd, path, buf, flags)`：flags & AT_SYMLINK_NOFOLLOW → `LOOKUP_NOFOLLOW`，否则 `LOOKUP_FOLLOW`

### 5.3 libc 实现

```c
// libc/unistd/symlink.c
int symlink(const char *target, const char *linkpath) {
    int64_t ret = syscall3(SYS_symlink, (int64_t)target, (int64_t)linkpath, 0);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

// libc/unistd/readlink.c
ssize_t readlink(const char *path, char *buf, size_t bufsize) {
    int64_t ret = syscall3(SYS_readlink, (int64_t)path, (int64_t)buf, (int64_t)bufsize);
    if (ret < 0) { errno = -ret; return -1; }
    return ret;
}

// libc/sys/stat/lstat.c
int lstat(const char *path, struct stat *buf) {
    int64_t ret = syscall3(SYS_lstat, (int64_t)path, (int64_t)buf, 0);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

// libc/sys/stat/fstatat.c
int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    int64_t ret = syscall4(SYS_fstatat, dirfd, (int64_t)path, (int64_t)buf, flags);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}
```

`libc/include/sys/syscall.h` 加 `SYS_symlink` 等 4 个宏。

`libc/include/sys/stat.h` 加：
```c
#define AT_FDCWD              -100
#define AT_SYMLINK_NOFOLLOW    0x100
int lstat(const char *path, struct stat *buf);
int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
```

---

## 6. 测试矩阵

`test/cases/test_vfs_symlink.c` —— 20 个 case：

```
基础
  01 fast_symlink_create_read()        target ≤60B
  02 long_symlink_create_read()         target 80B
  03 symlink_eexist()                  linkpath 已存在 → -EEXIST
  04 symlink_empty_target()            target="" → -ENOENT
  05 symlink_nonexist_parent()         parent 目录不存在 → -ENOENT
  06 readlink_on_regular()              -EINVAL
  07 readlink_bufsize_zero()            -EINVAL
  08 unlink_cleans_long_symlink()      long symlink unlink 后 data block 已释放

跟随语义
  09 symlink_follow_in_lookup()         FOLLOW → 跟随到 target
  10 symlink_no_follow_returns_link()   NOFOLLOW → 返回 link 节点本身
  11 stat_follows()                     stat 跟随到 target
  12 stat_on_link_no_follow()           lstat → S_IFLNK
  13 fstatat_no_follow()                AT_SYMLINK_NOFOLLOW
  14 fstatat_follow_default()           flags=0 跟随

路径拼接
  15 symlink_absolute_target()          "/etc/passwd" → 直接覆盖
  16 symlink_relative_target()          "subdir/foo" → 相对 symlink 所在目录
  17 symlink_dotdot_target()            "../etc/foo" → 相对父目录

环路与深度
  18 symlink_loop_eloop()               A→B→A → ELOOP
  19 symlink_depth_8_passes()           link1→link2→…→link8 (8 跳 OK)

集成
  20 systest_busybox_ln_exec()          ln -s /bin/busybox /tmp/x; /tmp/x --help → exit 0
```

`test/cases/test_systest.c` 加 case 20。

### 6.1 错误码矩阵

| 场景 | 返回 | errno |
|---|---|---|
| `symlink("/x", "/y")` 已存在 | -EEXIST | EEXIST |
| `symlink("", "/y")` 空 target | -ENOENT | ENOENT |
| `symlink("/x", "/nonexist/z")` 父不存在 | -ENOENT | ENOENT |
| `symlink(NULL, ...)` / 用户指针非法 | -EFAULT | EFAULT |
| `readlink("/regular-file", ...)` | -EINVAL | EINVAL |
| `readlink("/loop1", buf, 0)` bufsize=0 | -EINVAL | EINVAL |
| `lstat("/bin/sh")` (symlink) → S_IFLNK \| 0777, size=target 长度 | 0 | - |
| `lstat("/bin/regular")` → S_IFREG \| 0755 | 0 | - |
| `lstat("/no/such")` | -ENOENT | ENOENT |
| `lstat(...)` 用户 buf 非法 | -EFAULT | EFAULT |
| `stat("/bin/sh")` 跟随到 /bin/busybox | 0 | - |
| `fstatat(AT_FDCWD, "/bin/sh", buf, AT_SYMLINK_NOFOLLOW)` → link 自身 | 0 | - |
| `fstatat(AT_FDCWD, "/bin/sh", buf, 0)` → busybox | 0 | - |
| 跟随超过 8 跳 | -ELOOP | ELOOP |
| 跟随中间 readlink 失败 | -EIO | EIO |
| 跟随到不存在的 target | -ENOENT | ENOENT |

---

## 7. 栈使用复核

| 函数 | 栈上 buf | 字节 |
|---|---|---|
| `vfs_lookup_at`（外层） | `remaining[256]` | 256 |
| `vfs_lookup_at` | `consumed[256]` | 256 |
| `vfs_lookup_at` | `new_remaining[256]` | 256 |
| `vfs_lookup_at` 堆 | `target` (kmalloc) | 0 |
| `__vfs_lookup_raw`（内层） | `path_copy[256]` + `_entry_name[256]` | 512 |
| 内层调用栈（frame ptr + locals） | ≈ | ~100 |
| **peak** | | **~1380** |

STACK_SIZE = 32KB（task.h:46）。**峰值 1380B ≈ 4%**。安全余量充足。

`readlink` / `lstat` / `fstatat` 各 syscall 顶层栈：单 buf 256B + 局部变量 ≈ 300B，远低于栈限。

---

## 8. 风险与不在范围内

### 8.1 风险

| 风险 | 缓解 |
|---|---|
| **exec symlink TOCTOU**：lookup 时 link → X，exec 加载时 X 被替换为 Y | OS01 单用户，影响小；文档化。Linux 同样不防此 TOCTOU（需 `O_NOFOLLOW` open 语义配合，OS01 无 `open` flag 扩展） |
| **`__vfs_lookup_raw` 签名变化**：consumed 输出参数影响 6 个现有调用点 | 全部传 NULL/0（编译期强制） |
| **新增 kmalloc 失败路径** | vfs_lookup_at 处理 kmalloc 失败返回 NULL → EIO |
| **`consumed_out` 截断**：consumed_size < path 长度时不写 | 不影响主路径（vfs_lookup_at 用 sizeof(consumed) = 256），只影响理论极端长路径 |
| **ext2 unlink 漏释放 long symlink 的 data block** | §4.4 显式补；若现状已正确则 no-op |
| **busybox rootfs 切回符号链接**（移除 `b32e1e0`） | **不在本次范围**——本次仅启用 kernel 支持，下次重构验证后切回 |

### 8.2 不在范围（明确不做）

- `AT_EMPTY_PATH` / `AT_NO_AUTOMOUNT` / `AT_RECURSIVE`
- `readlinkat(2)`（at 版 readlink）
- `symlinkat(2)`（at 版 symlink）
- `linkat(2)` / `link(2)` / `unlink(2)`（unlink 已存在；link 不在范围）
- `lchmod(2)` / `chown` 对 symlink 的处理
- 真实 `dirfd` 实现（fd → vfs_node_t 表）—— 当前只支持 `AT_FDCWD`
- 权限/owner 检查（OS01 root 单用户）
- 链接数（`i_links_count`）跟踪 + 硬 link（POSIX `link(2)`）

### 8.3 后续 roadmap 联动

完成后可关闭/升级：
- P1「exec 软链接跟随」→ ✅
- P5「symlink/readlink」→ ✅
- P5「exec symlink ABI ✅ 缓解」→ 完全解决（可移除 `b32e1e0` busybox 副本化构建期变通，下次重构验证）
- P5「更多 applet」→ 部分解锁（`find -type l`、`ln` 等可工作）