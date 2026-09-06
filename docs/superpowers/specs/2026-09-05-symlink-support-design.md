# OS01 软链接（symlink）支持 — 设计方案

> **日期**: 2026-09-05
> **状态**: v5，评审通过（主代理完整审计 + 子代理复审 approve）
> **目标**: 在 VFS + ext2 + syscall + libc + Linux ABI 翻译五处落地 POSIX 对称的软链接支持：`symlink(2)` 创建、`readlink(2)` 读取、`lstat(2)` 不跟随、`fstatat(2)` 带 `AT_SYMLINK_NOFOLLOW`。自动让 `exec`/`open`/`stat` 跟随末段与中间段软链接（POSIX-correct），关闭三项 roadmap 项：
> - **P1**「exec 软链接跟随」（同时移除 `b32e1e0` busybox 副本化构建期规避，可选切回符号链接）
> - **P5**「symlink/readlink」
> - **P5**「exec symlink ABI ✅ 缓解」—— 升级为完全解决
>
> **修订**:
> - v1：初版
> - v2：5 项修订——中段 symlink、用户指针安全、libc syscall3/4 + ABI 表、errno 传播、DT_LNK + EOPNOTSUPP
> - v3：6 项修订——NOFOLLOW 仅末段、相对 linkpath 复用 vfs_split_parent、strnlen_user + copy_from_user_ft、ext2 unlink 类型感知、fstatat flag 校验、测试数量对齐
> - v4：4 项修订（3 P1 + 1 文档）——
>   - **🟢 [P1] COPY_USER_STR 长度边界**：原 `_l >= max → EFAULT` 错把 strnlen_user 的「无 NUL 超长」信号当 fault；`_l == max-1` 是合法长度却被拒。改为 `_l < 0 → EFAULT`，`_l >= max → ENAMETOOLONG`，其余 `_l+1` 字节复制。与 uaccess.c:91-105 注释一致
>   - **🟢 [P1] splice_symlink_path 返回值被忽略**：超长展开路径继续用未初始化 remaining 解析；改为检查返回值，失败时 kfree(target) + vfs_node_put(node) + 上抛 -ENAMETOOLONG。新测试 29
>   - **🟢 [P1] dirent_add 失败泄漏 inode + data block**：参考 ext2_vfs_create(ext2.c:774) 模式，失败时先 `free_block(i_block[0])`（long symlink）再 `free_inode(ino)`。新测试 30
>   - **🟢 [doc] §6 header 数量**：§6 header 与 §2.2 写 30 个，但 v3 测试清单已是 28 unit + 2 systest；统一为 30，再加 v4 新增 2 → 共 32
> - v5：完整实现前审计修订——
>   - **🟢 [P1] tmpfs E2E 矛盾**：tmpfs 明确不支持 symlink，BusyBox E2E 从 `/tmp/x` 改到 ext2 根上的 `/symlink-systest/x`
>   - **🟢 [P1] ext2 symlink size**：readdir 对 `VFS_SYMLINK` 也读取 inode `i_size`，保证 `lstat.st_size` 正确
>   - **🟢 [P1] lookup 错误语义**：walk 下一组件前显式检查当前节点为目录（否则 `-ENOTDIR`）；底层 readdir 非零统一为 `-EIO`
>   - **🟢 [P1] suffix 拼接契约**：raw lookup 保证 suffix 为空或以 `/` 开头，splice 才可安全执行 `target + suffix`
>   - **🟢 [P1] ext2 创建 I/O 原子性**：检查 read-inode、write-target-block 与 write-inode；任一步失败均在发布 dirent 前回收已分配 block/inode
>   - **🟢 [P1] 公共 lookup 迁移闭环**：将 fold 解析抽为绝对路径内部 helper；`vfs_lookup`、`vfs_lookup_from` 与 `vfs_lookup_at` 全部经此 helper，旧 pointer API 仅丢失 errno、绝不丢失 symlink 语义
>   - **🟢 [P1] 自动跟随 op 守卫**：VFS 内部的 `readlink` 调用也检查 `.ops/.readlink`，缺失则 `-EOPNOTSUPP`
>   - **🟢 [P1] ext2 查重错误**：仅 `-ENOENT` 可继续创建，其他 `ext2_find_dirent` 错误原样返回
>   - **🟡 [P2] at 绝对路径**：绝对 path 先解析并忽略 dirfd；只对相对 path 的非 `AT_FDCWD` 返回 `-EBADF`
>   - **🟡 [P2] 根路径保护**：`symlink(..., "/")` 不得被拆成 cwd 下名称为 `/` 的目录项，明确返回 `-EEXIST`
>   - **🟡 [P2] VFS type 值稳定性**：保留既有 `VFS_FILE..VFS_BLKDEV` 的数值，仅追加 `VFS_SYMLINK=5`

---

## 1. 背景与动机

### 1.1 现状

| 项 | 现状 | 影响 |
|---|---|---|
| `__vfs_lookup`（vfs.c:145） | 逐组件 readdir，不区分 symlink 类型（无 `VFS_SYMLINK` 节点）；不处理中段 symlink；返回 NULL 时 errno 丢失 | busybox 符号链接 + `find -type l` 全部失败 |
| `vfs_split_parent`（vfs.c:590） | 已实现并被 `vfs_unlink/mkdir/rmdir/rename` 使用，正确处理相对路径（无 slash → 用 cwd） | `sys_symlink` 应复用此 helper，不要重新实现 |
| `ext2_vfs_unlink`（ext2.c:801） | 已无条件遍历 `i_block[0..11]` 释放 | **对 fast symlink（i_block[] 装 target 字符串）会把字符串字节当块号释放 → 位图损坏；对 long symlink 会 free `i_block[0]`** —— 必须 type-aware 重构 |
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
| 跟随深度 | `MAXSYMLINKS = 8` | OS01 有意采用的小上限（Linux 常用上限为 40）；足以覆盖本系统目标，超过即 ELOOP |
| 跨 mount 跟随 | 跟随（Linux 行为） | OS01 单 mount，跨 mount 问题不实际出现；统一 Linux 语义便于参考 |
| 范围 | 全套：`symlink` + `readlink` + `lstat` + `fstatat(AT_SYMLINK_NOFOLLOW)` | 用户确认 |
| **中段 symlink** | **resolve**：vfs_lookup 单函数内 fold 跟随循环，遇到中段 symlink splice `target+suffix` 后 restart | POSIX 必需（如 `/link_to_dir/file`） |
| **NOFOLLOW 语义** | **仅禁止末段 symlink**；中段 symlink 始终跟随（POSIX AT_SYMLINK_NOFOLLOW 语义） | `lstat("/link/file")` 必须跟随 `/link` 返回 `/real/file` 的 stat |
| **ext2 unlink 类型感知** | 必须用 `(i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK` 区分 fast/long symlink，跳过通用块释放循环 | fast symlink 的 `i_block[]` 是 target 字符串，误当块号会损坏位图 |
| lookup API | `int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags, vfs_node_t **out)` —— 0 成功/-errno 失败 | errno 传播必需（v1 返回 NULL 丢 errno） |
| `stat` 行为 | 改为跟随（POSIX-correct） | 当前无 symlink，外部不可见 breaking |
| 内核栈 | 单 256B 栈 buf（fold 进 `vfs_lookup_at`，无外层/内层分离）+ 必要时 kmalloc target | 用户提出的栈压力顾虑 |
| syscall 编号 | 71..74（71=`symlink`、72=`readlink`、73=`lstat`、74=`fstatat`） | 接续现有 0..70 |
| 用户指针安全 | 所有 syscall 走 `copy_from_user_ft` / `copy_to_user_ft` | 复用 sys_stat 模式，零特殊 |
| libc helper | 新增 `syscall3()`（= 现有 3-arg `syscall()`）+ `syscall4()`（r10 ABI） | fstatat 4 参必需 |
| 非 ext2 FS 行为 | `sys_symlink`/`sys_readlink` 及 lookup 自动跟随均检查 `ops->symlink/readlink == NULL` → -EOPNOTSUPP | 显式失败而非 NULL deref |
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
| `kernel/fs/vfs.c` + `kernel/include/fs/vfs.h` | `__vfs_lookup` 改名为 raw；新增绝对路径 `vfs_lookup_resolved()` fold helper，`vfs_lookup`/`vfs_lookup_from`/`vfs_lookup_at` 统一委托，确保所有既有调用点跟随 symlink；walk 显式 `ENOTDIR`/`EIO`；其余同上 |
| `kernel/fs/ext2.c` | 新增 symlink/readlink；readdir 映射并读取 symlink `i_size`；find_dirent 仅 ENOENT 可继续；**read/write inode、target block 写入与 dirent 失败均回滚**；注册 ops；unlink 类型感知重构 |
| `kernel/sched/task.c` | `sys_exec` 改走 `vfs_lookup_at` |
| `kernel/arch/x86_64/trap.c` | 新 syscall case/用户指针处理；现有 `SYS_stat`/`SYS_open`/`SYS_chdir` 改走 `vfs_lookup_at`；Linux ABI 表补映射；扩 `syscall_names` 至 75 |
| `kernel/include/uapi/syscall.h` | 追加 71..74 的内核权威 syscall 常量 |
| `libc/include/sys/syscall.h` | `SYS_symlink=71` 等 4 个宏；新增 `syscall3()` (= 现有 3-arg `syscall()` 别名) 与 `syscall4()`（r10 ABI） |
| `libc/include/sys/stat.h` | `lstat` / `fstatat` 声明 + `AT_FDCWD=-100` + `AT_SYMLINK_NOFOLLOW=0x100` |
| `libc/unistd/symlink.c`、`readlink.c` | 替换 stub 为真实现 |
| `libc/sys/stat/lstat.c`、`fstatat.c` | 新建 |
| `test/cases/test_vfs_symlink.c` | 新建——42 个 case（§6） |

---

## 3. VFS 层改动

### 3.1 节点类型与 ops 扩展

```c
// kernel/include/fs/vfs.h

typedef enum {
    VFS_FILE    = 1,  // preserve current public VFS values
    VFS_DIR     = 2,
    VFS_CHRDEV  = 3,
    VFS_BLKDEV  = 4,
    VFS_SYMLINK = 5,  // appended; never renumber existing values
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
// 新：2-状态返回值 + 3 个 out 参数（无 flags —— NOFOLLOW 语义在 caller 处理）

// 返回值：
//   0    = 成功（不含 symlink），*out_node 是最终节点（refcount++），无 suffix
//   1    = 遇到 symlink（mid-path 或末段），*out_node 是 symlink（refcount++），
//          *consumed_out 写到 symlink 为止的绝对前缀，
//          *remaining_out 是 symlink 之后未消费的 suffix（末段 symlink 时为 ""）
//   < 0  = -errno (ENOENT/ENOTDIR/ENAMETOOLONG/EIO/...)
//          *out_node = NULL
static int __vfs_lookup_raw(const char *path,
                            vfs_node_t **out_node,
                            char *consumed_out, size_t consumed_size,
                            char *remaining_out, size_t remaining_size);
```

**实现要点**：
- 处理每个普通组件前必须检查 `current->type == VFS_DIR`；否则直接 `-ENOTDIR`，不得以 readdir 未命中伪装成 `-ENOENT`
- walk 循环里遇到目录项，先看 `entry.type == VFS_SYMLINK`；`vfs_readdir()` 任意非零失败统一转为 `-EIO`
- 任何位置的 symlink 都早返 status=1，consumed=`已 walk 完到 symlink 的绝对路径`，remaining=`symlink 之后的部分（末段 symlink 时为 ""）`
- `remaining_out`（下文简称 suffix）严格为 `""` 或以 `/` 开头的未消费路径；例如解析 `/a/link/x/y` 时，遇到 `link` 的 suffix 必须是 `/x/y`，不是 `x/y`。这使 `target + suffix` 不会丢失组件边界。
- raw walk 在识别 symlink 时须从 tokenizer 的 `ptr` 生成 suffix：跳过连续 `/` 后，若仍有未消费字符，写 `suffix[0] = '/'` 再复制余串和 NUL；否则写空串。不得直接复制 tokenizer 已越过分隔符后的 `ptr`。
- **NOFOLLOW 决策在 caller（vfs_lookup_at）**：若 `flags=NOFOLLOW && suffix==""` → 把 symlink 节点当最终节点返回（status=0 语义）；否则 splice + restart
- 非 symlink 节点正常 walk，最后返回 status=0
- consumed 累计：walk 完成时 `consumed = path` 全长

**公共 API 迁移闭环**：
- 将 §3.3 的 fold 循环抽为 `static int vfs_lookup_resolved(const char *absolute_path, lookup_flags_t flags, vfs_node_t **out)`；它只接受规范化绝对路径。
- `vfs_lookup_at()` 先经 `resolve_at()` 得到绝对路径，再调用 helper；errno 完整上抛。
- 兼容指针 API 不能继续调用 raw：`vfs_lookup(absolute)` 与 `vfs_lookup_from(path, cwd)` 分别规范化输入后调用同一 helper，成功返回 node，失败返回 NULL（这两个旧 API 的既有 errno 丢失语义保持不变）。
- 因而 task 的 spawn/exec、trap 的 access/truncate、VFS 的 unlink/mkdir/rmdir/rename、main/selftest/tmpfs 等所有既有 public API 调用点同步得到中段和末段 link 跟随；不要求逐项机械替换，也不会形成新旧解析分叉。

### 3.3 `vfs_lookup_resolved` fold 跟随循环与公共包装

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
// absolute_path 已由调用方规范化；唯一负责 walk/follow，避免 API 语义分叉。
static int vfs_lookup_resolved(const char *absolute_path,
                               lookup_flags_t flags, vfs_node_t **out_node) {
    *out_node = NULL;
    char remaining[VFS_NAME_MAX];
    size_t plen = strlen(absolute_path);
    if (plen >= sizeof(remaining)) return -ENAMETOOLONG;
    memcpy(remaining, absolute_path, plen + 1);

    vfs_node_t *node = NULL;
    char consumed[VFS_NAME_MAX];
    char suffix[VFS_NAME_MAX];       // 中段 symlink 之后未消费的 suffix
    int depth = 0;

    for (;;) {
        // Walk 当前 remaining，遇到 symlink 早返 status=1
        int st = __vfs_lookup_raw(remaining, &node,
                                   consumed, sizeof(consumed),
                                   suffix, sizeof(suffix));
        if (st < 0) return st;
        if (st == 0) {
            // 终态：node 是非 symlink 最终节点
            *out_node = node;
            return 0;
        }
        // st == 1: 遇到 symlink
        if (depth >= MAXSYMLINKS) {
            vfs_node_put(node);
            return -ELOOP;
        }

        // NOFOLLOW 语义：仅当 symlink 是末段（suffix 为空）时返回 symlink 本身
        if ((flags & LOOKUP_NOFOLLOW) && suffix[0] == '\0') {
            *out_node = node;   // node 本身就是 symlink
            return 0;
        }

        // 否则 splice + restart（无论 FOLLOW 还是 NOFOLLOW+mid-path）
        char *target = kmalloc(VFS_NAME_MAX);
        if (!target) { vfs_node_put(node); return -ENOMEM; }
        if (!node->ops || !node->ops->readlink) {
            kfree(target);
            vfs_node_put(node);
            return -EOPNOTSUPP;
        }
        int tlen = node->ops->readlink(node, target, VFS_NAME_MAX - 1);
        if (tlen < 0) { kfree(target); vfs_node_put(node); return tlen; }
        target[tlen] = '\0';

        char new_remaining[VFS_NAME_MAX];
        // ★ v4 修复：splice 可能返回 -ENAMETOOLONG（target+suffix 拼成新路径超 max）
        //    v3 漏检会导致 memcpy 未定义内容进 remaining 继续解析
        int src = splice_symlink_path(consumed, target, suffix,
                                      new_remaining, sizeof(new_remaining));
        if (src < 0) {
            kfree(target);
            vfs_node_put(node);
            return src;     // 上抛 -ENAMETOOLONG
        }
        kfree(target);

        vfs_node_put(node);
        memcpy(remaining, new_remaining, VFS_NAME_MAX);
        depth++;
    }
}

int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node) {
    char absolute[VFS_NAME_MAX];
    int rc = resolve_at(dirfd, path, absolute, sizeof(absolute));
    if (rc < 0) { *out_node = NULL; return rc; }
    return vfs_lookup_resolved(absolute, flags, out_node);
}

// resolve_at: 本次只实现 AT_FDCWD 语义
//   - path 绝对 → remaining = path，忽略 dirfd（fstatat POSIX 语义）
//   - dirfd == AT_FDCWD 且 path 相对 → 用 current->files->cwd 拼接
//   - path 相对且其他 dirfd 值 → -EBADF；本次明确不支持真实 fd，留待 openat 任务
static int resolve_at(int dirfd, const char *path,
                      char *out, size_t out_size);

// splice_symlink_path: 把 consumed + target + suffix 合成新绝对路径。
// Contract: suffix is "" or begins with '/'.
//   - target[0] == '/' → new = target + suffix（绝对覆盖）
//   - 否则 → new = dirname(consumed) + "/" + target + suffix
// Returns:
//   0 on success; -ENAMETOOLONG if expanded path exceeds out_size
static int splice_symlink_path(const char *consumed, const char *target,
                               const char *suffix,
                               char *out, size_t out_size);
```

**关键不变量**：
- **无内层递归调用**：所有 stack frame 都在 `vfs_lookup_at` 内（除 `readlink` 与 `__vfs_lookup_raw` 返回前）
- 外层同时持有四个 256B 路径缓冲（不是“单一 256B buf”）；加上局部变量约 **1200B**，与内层 raw walk 同时在栈时峰值约 **1720B**（详见 §7）
- `target` 总在 heap（避免栈累计）
- 跟随时释放上一轮 `node` 避免 refcount 泄漏

### 3.4 `vfs_stat` S_IFLNK case

```c
case VFS_SYMLINK: buf->st_mode = S_IFLNK | 0777; break;
```

`st_size` = link target 字节数。ext2 的 `readdir` 必须对 `VFS_FILE` **和** `VFS_SYMLINK` 都读 inode `i_size` 填入 `entry.size`；只改 file_type 映射会令 link 的 `st_size` 错为 0。

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

    // 1) 检查 linkpath 已存在。只有 -ENOENT 才可继续创建；I/O、OOM、ENOTDIR 均上抛。
    uint32_t existing_ino; uint8_t ft; uint32_t blk, off;
    int find_rc = ext2_find_dirent(fs, parent_ino, name,
                                   &existing_ino, &ft, &blk, &off);
    if (find_rc == 0) {
        spin_unlock(&fs->lock); return -EEXIST;
    }
    if (find_rc != -ENOENT) {
        spin_unlock(&fs->lock); return find_rc;
    }

    // 2) 分配 inode（mode = S_IFLNK | 0777）
    uint32_t ino = alloc_inode(fs, EXT2_S_IFLNK | 0777);
    if (ino == 0) { spin_unlock(&fs->lock); return -ENOSPC; }

    // 3) 写 target
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) {
        free_inode(fs, ino);
        spin_unlock(&fs->lock);
        return -EIO;
    }
    inode.i_size = (uint32_t)tlen;
    uint32_t target_blk = 0;
    if (tlen <= 60) {
        inode.i_blocks = 0;
        memset(inode.i_block, 0, sizeof(inode.i_block));
        memcpy(inode.i_block, target, tlen);
    } else {
        target_blk = alloc_block(fs);
        if (target_blk == 0) {
            free_inode(fs, ino);
            spin_unlock(&fs->lock);
            return -ENOSPC;
        }
        inode.i_blocks = fs->block_size / 512;
        inode.i_block[0] = target_blk;

        uint8_t *buf = kmalloc(fs->block_size);
        if (!buf) {
            free_block(fs, target_blk);
            free_inode(fs, ino);
            spin_unlock(&fs->lock);
            return -ENOMEM;
        }
        memcpy(buf, target, tlen);
        memset(buf + tlen, 0, fs->block_size - tlen);
        int write_rc = ext2_write_block(fs, target_blk, buf);
        kfree(buf);
        if (write_rc != 0) {
            free_block(fs, target_blk);
            free_inode(fs, ino);
            spin_unlock(&fs->lock);
            return -EIO;
        }
    }
    if (ext2_write_inode(fs, ino, &inode) != 0) {
        if (target_blk != 0) free_block(fs, target_blk);
        free_inode(fs, ino);
        spin_unlock(&fs->lock);
        return -EIO;
    }

    // ★ v4 修复：dirent_add 失败时必须回滚已分配资源，避免 ENOSPC/EIO 重试耗尽空间
    //   - Long symlink (i_blocks > 0): 先 free_block(i_block[0]) 再 free_inode
    //   - Fast symlink (i_blocks == 0): 仅 free_inode
    //   - 参考 ext2_vfs_create (ext2.c:774) 的 inode 回滚模式
    int rc = dirent_add(fs, parent_ino, name, ino, EXT2_FT_SYMLINK);
    if (rc != 0) {
        if (target_blk != 0) free_block(fs, target_blk);
        free_inode(fs, ino);
        spin_unlock(&fs->lock);
        return rc;   // 上抛 -ENOSPC / -EIO 等
    }

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

### 4.3 `ext2_vfs_readdir` 映射 symlink file_type 与 size

```c
case EXT2_FT_SYMLINK: entry->type = VFS_SYMLINK; break;

// ext2 dirent 不带 i_size；regular file 与 symlink 都需要读取 inode。
if (entry->type == VFS_FILE || entry->type == VFS_SYMLINK) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, de->inode, &inode) == 0)
        entry->size = inode.i_size;
}
```

### 4.4 `ext2_vfs_unlink` 类型感知重构（**重要**：现有逻辑损坏 symlink）

**问题分析**（v2 spec 遗留）：
- 现有 `ext2_vfs_unlink`（ext2.c:801）无条件遍历 `i_block[0..11]` 释放
- 对 **fast symlink**（`i_blocks==0`，`i_block[]` 装 target 字符串）：循环把字符串字节当块号 → 释放任意块，**位图损坏**
- 对 **long symlink**（`i_blocks>0`，`i_block[0]` 是 data block）：循环正确释放 `i_block[0]`，但 v2 spec 在此之前又调 `free_block(i_block[0])` → **double free**

**修复方案**：在通用块释放前 type-check，按 symlink 类型走专用路径：

```c
// 替换 ext2_vfs_unlink 现有 i_links_count==0 分支（约 ext2.c:825-852）
inode.i_links_count--;
if (inode.i_links_count == 0) {
    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
        // ── Symlink：必须 type-aware ──
        if (inode.i_blocks > 0) {
            // Long symlink: i_block[0] 是装 target 的 data block
            // i_blocks == block_size/512（fast path i_blocks==0）
            free_block(fs, inode.i_block[0]);
            // 注：i_block[1..11] 对 long symlink 为 0（symlink target 只用一个 block）
            // 单 indirect 不用管
        }
        // Fast symlink (i_blocks==0): i_block[] 装 target 字符串，绝不释放
        // （通用循环会损坏位图）
    } else {
        // ── 普通文件/目录：原逻辑保留 ──
        for (int i = 0; i < 12; i++) {
            if (inode.i_block[i] != 0) {
                free_block(fs, inode.i_block[i]);
                inode.i_block[i] = 0;
            }
        }
        if (inode.i_block[12] != 0) {
            // ... 单 indirect 处理不变 ...
        }
    }
    inode.i_blocks = 0;
    inode.i_size = 0;
    ext2_write_inode(fs, target_ino, &inode);
    free_inode(fs, target_ino);
}
```

**新增 type-aware 测试**：
- `test 25 (revised)`：`symlink_long_unlink_releases_block` —— 创建 long symlink，检查 `free_block` 被调一次且仅一次（位图前后对比）；`fs->sb_raw.s_free_blocks_count` 增 1
- `test 26 (new)`：`symlink_fast_unlink_no_block_free` —— 创建 fast symlink，检查 `i_block[]` 区域**未被 free_block 误调**（位图前后无变化）

---

## 5. syscall + libc + Linux ABI 改动

### 5.1 syscall 表

```c
// kernel/include/uapi/syscall.h (kernel canonical source; trap.c includes it)
#define SYS_symlink    71
#define SYS_readlink   72
#define SYS_lstat      73
#define SYS_fstatat    74
```

`SYS_stat(16)` / `SYS_open(7)` / `SYS_chdir(14)` / `sys_exec(5)` 改走 `vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node)`。`SYS_stat`、`SYS_open` 与 `SYS_chdir` 的 dispatch 本就在 `trap.c`；`sys_exec` 在 `task.c`，由 trap 的 `SYS_exec` case 调用。`trap.c` 的 `syscall_names` 数组扩至 75 并补齐 71..74 名称。语义变化：`stat` 现在跟随 symlink（POSIX-correct；当前无 symlink 所以外部不可见）。

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

**用户指针访问规范**（与 sys_stat/open/exec 一致）：
- uaccess API 只有 `strnlen_user()` + `copy_from_user_ft()` + `copy_to_user_ft()`（**没有 `strncpy_from_user`**，v2 误用）
- path/target 模式：先 `strnlen_user(ptr, VFS_NAME_MAX)`，若等于上限 → -ENAMETOOLONG；否则 `copy_from_user_ft(buf, ptr, len+1)`（含 NUL）
- 所有写用户 buf（如 lstat/fstatat 的 `struct stat`）：先 build kernel-local `kstat`，再 `copy_to_user_ft(buf, &kstat, sizeof)`

```c
// 辅助宏：从用户字符串复制到 kernel buf（strlen + fault-tolerant copy）
// strnlen_user(p, max) 语义（uaccess.c:91-105）：
//   返回 [0, max] —— 实际 strlen（≤ max-1 表示含 NUL 在内总 ≤ max 字节）
//                  或 max（无 NUL 终止，超长）
//   返回 -EFAULT —— 用户指针不可读
#define COPY_USER_STR(kbuf, uptr, max) ({                          \
    long _l = strnlen_user((uptr), (max));                         \
    if (_l < 0) return -EFAULT;                                   \
    if (_l >= (long)(max)) return -ENAMETOOLONG;                  \
    if (copy_from_user_ft((kbuf), (uptr), _l + 1) < 0)            \
        return -EFAULT;                                           \
    _l; })

// ── SYS_symlink(71): symlink(target, linkpath) ────────────
int64_t sys_symlink(const char *target, const char *linkpath, pt_regs_t *regs) {
    (void)regs;
    char target_copy[VFS_NAME_MAX];
    long tlen = COPY_USER_STR(target_copy, target, VFS_NAME_MAX);
    if (tlen == 0) return -ENOENT;     // 拒绝空 target

    char linkpath_copy[VFS_NAME_MAX];
    COPY_USER_STR(linkpath_copy, linkpath, VFS_NAME_MAX);

    // ★ v3 修订：复用 vfs_split_parent (vfs.c:590) 而非重新实现
    // vfs_split_parent 处理三种情形："/dir/file" → parent="/dir" name="file"；
    // "/file" → parent="/" name="file"；"file" → parent=cwd name="file"
    const char *cwd = current->files ? current->files->cwd : "/";
    char parent_path[VFS_NAME_MAX];
    // Root is an existing directory, never a legal directory-entry name.
    if (strcmp(linkpath_copy, "/") == 0) return -EEXIST;
    const char *name = vfs_split_parent(linkpath_copy, cwd, parent_path);
    if (!name || *name == '\0') return -EINVAL;     // 拒绝空 basename

    vfs_node_t *parent = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, parent_path, LOOKUP_FOLLOW, &parent);
    if (rc < 0) return rc;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }

    // EOPNOTSUPP 守卫：非 ext2 FS 无 symlink op
    if (!parent->ops || !parent->ops->symlink) {
        vfs_node_put(parent); return -EOPNOTSUPP;
    }

    rc = parent->ops->symlink(parent, name, target_copy);
    vfs_node_put(parent);
    return rc;
}

// ── SYS_readlink(72): readlink(path, buf, bufsize) ────────
int64_t sys_readlink(const char *path, char *buf, size_t bufsize,
                     pt_regs_t *regs) {
    (void)regs;
    if (!buf || bufsize == 0) return -EINVAL;
    // 注：允许 path==NULL 当 EFAULT 处理

    char path_copy[VFS_NAME_MAX];
    COPY_USER_STR(path_copy, path, VFS_NAME_MAX);

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
    if (!buf) return -EFAULT;
    char path_copy[VFS_NAME_MAX];
    COPY_USER_STR(path_copy, path, VFS_NAME_MAX);

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW, &node);
    if (rc < 0) return rc;

    // ★ v2 修复：vfs_stat 直接 memset 用户 buf 会 fault；现先 build kernel-local
    struct stat kstat;
    rc = vfs_stat(node, &kstat);
    vfs_node_put(node);
    if (rc < 0) return rc;

    if (copy_to_user_ft(buf, &kstat, sizeof(kstat)) < 0) return -EFAULT;
    return 0;
}

// ── SYS_fstatat(74): fstatat(dirfd, path, buf, flags) ────
// ★ v3 新增：仅接受 AT_SYMLINK_NOFOLLOW 与 0；其他 flag 一律 -EINVAL
//    （POSIX 严格要求，避免静默忽略调用方的语义请求）
#define FSTATAT_SUPPORTED_FLAGS (AT_SYMLINK_NOFOLLOW)

int64_t sys_fstatat(int dirfd, const char *path, struct stat *buf,
                    int flags, pt_regs_t *regs) {
    (void)regs;
    if (!buf) return -EFAULT;
    if (flags & ~FSTATAT_SUPPORTED_FLAGS) return -EINVAL;   // ★ v3: 拒绝未知 flag

    char path_copy[VFS_NAME_MAX];
    COPY_USER_STR(path_copy, path, VFS_NAME_MAX);

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

`test/cases/test_vfs_symlink.c` —— **42 个 case**（40 unit + 2 systest）：

```
基础创建与读取
  01 fast_symlink_create_read()           target ≤60B
  02 long_symlink_create_read()            target 80B
  03 symlink_eexist()                     linkpath 已存在 → -EEXIST
  04 symlink_empty_target()               target="" → -ENOENT
  05 symlink_nonexist_parent()            parent 目录不存在 → -ENOENT
  06 symlink_relative_linkpath()          "link" (no slash) → parent=cwd，期望成功
  07 readlink_on_regular()                 -EINVAL
  08 readlink_bufsize_zero()               -EINVAL
  09 unlink_cleans_long_symlink_bitmap()  long symlink unlink 后 sb_raw.s_free_blocks_count +1，bitmap 一位翻转
  10 unlink_fast_symlink_doesnt_free()    fast symlink unlink 后 sb_raw.s_free_blocks_count 不变（i_block[] 是字符串）

跟随语义（末段）
  11 symlink_follow_in_lookup()            FOLLOW → 跟随到 target
  12 symlink_no_follow_returns_link()      NOFOLLOW + 末段 symlink → 返回 link 节点本身
  13 stat_follows()                        stat 跟随到 target
  14 lstat_returns_S_IFLNK()               lstat → S_IFLNK | 0777, size=target len
  15 fstatat_no_follow()                   AT_SYMLINK_NOFOLLOW
  16 fstatat_follow_default()              flags=0 跟随

跟随语义（中段，v3 关键 — NOFOLLOW 不阻止中段跟随）
  17 symlink_mid_path_absolute_follow()    /link_to_dir/file FOLLOW → 跟随 /link_to_dir，返回 file
  18 symlink_mid_path_absolute_nofollow()  /link_to_dir/file NOFOLLOW → 仍跟随 /link_to_dir，返回 file
  19 symlink_mid_path_relative_follow()    /a/link_to_dir/file → /a/real_dir/file
  20 symlink_mid_path_with_suffix()        /link_to_dir/a/b → /real_dir/a/b
  21 symlink_chained_mid_path()            /a/link1/link2/file（双层中段）

环路与深度
  22 symlink_loop_eloop()                  A→B→A → ELOOP
  23 symlink_depth_8_passes()              link1→…→link8 (8 跳 OK)
  24 symlink_depth_mid_path()              /a/link1/link2/.../link8/f (中段 8 跳)

错误码传播（v2 + v3 + v4 新增）
  25 lookup_enotdir_for_mid()              /file/link_to_dir/f → ENOTDIR
  26 lookup_enametoolong()                 用户 path 长度 ≥ VFS_NAME_MAX → ENAMETOOLONG
  27 stat_eopnotsupp_on_tmpfs()            /tmp 路径下 symlink 不支持 → EOPNOTSUPP
  28 fstatat_unknown_flag_einval()          fstatat(..., flags=AT_EMPTY_PATH) → EINVAL
  29 splice_symlink_path_enametoolong()     /short → /long_symlink → target 超 VFS_NAME_MAX → ENAMETOOLONG（v4 新增）
  30 ext2_symlink_rollback_on_dirent_fail() 模拟 dirent_add 失败（block bitmap 满）→ long symlink 的 data block 与 inode 必须回滚（v4 新增）
  31 lstat_symlink_reports_target_size()    fast + long symlink 的 st_size == target 字节数
  32 lookup_readlink_op_missing()           VFS_SYMLINK 缺 readlink op → EOPNOTSUPP，绝不调用空指针
  33 ext2_symlink_find_dirent_error()       ext2_find_dirent 的 EIO/ENOMEM/ENOTDIR 原样返回，不分配 inode/block
  34 symlink_root_linkpath_eexist()         symlink("x", "/") → EEXIST，绝不创建名称为 "/" 的 dirent
  35 fstatat_absolute_ignores_dirfd()       fstatat(invalid_fd, "/file", ...) 成功；相对 path 才 EBADF
  36 lookup_non_directory_returns_enotdir() 普通文件后接组件 → ENOTDIR（不退化为 ENOENT）
  37 lookup_readdir_failure_is_eio()        底层 readdir 失败 → EIO
  38 ext2_symlink_read_inode_failure()      读新 inode 失败 → EIO，inode 已回收
  39 ext2_symlink_write_block_failure()     long target block 写失败 → EIO，block 与 inode 已回收
  40 ext2_symlink_write_inode_failure()     inode 写失败 → EIO，block（如有）与 inode 已回收

集成（systest）
  41 systest_busybox_ln_exec()             ln -s /bin/busybox /symlink-systest/x; /symlink-systest/x --help → exit 0（根 ext2）
  42 systest_find_type_l()                 find / -type l → 输出含 symlink 行 → DT_LNK 验证
```

`test/cases/test_systest.c` 加 case 41, 42。Systest 在执行 case 41 前须 `mkdir /symlink-systest`（若已存在则复用），结束后 unlink/rmdir 清理；不得使用 `/tmp`，因为它是故意不支持 symlink 的 tmpfs。

### 6.1 错误码矩阵（v3 增强）

| 场景 | 返回 | errno |
|---|---|---|
| `symlink("/x", "/y")` 已存在 | -EEXIST | EEXIST |
| `symlink("", "/y")` 空 target | -ENOENT | ENOENT |
| `symlink("/x", "/nonexist/z")` 父不存在 | -ENOENT | ENOENT |
| `symlink("/x", "/regular-file/z")` 父是普通文件 | -ENOTDIR | ENOTDIR |
| `symlink("/x", "link")` (无 slash, cwd=/tmp) | 0 | -  (v3 修订：相对 linkpath 走 vfs_split_parent → parent=cwd) |
| `symlink("/x", NULL)` / 用户指针非法 | -EFAULT | EFAULT |
| `symlink` 用户字符串 len ≥ VFS_NAME_MAX | -ENAMETOOLONG | ENAMETOOLONG (v3 新增) |
| `symlink("/x", "/tmp/x")` tmpfs 无 symlink op | -EOPNOTSUPP | EOPNOTSUPP |
| `symlink("/x", "/")` | -EEXIST | EEXIST |
| `readlink("/regular-file", ...)` | -EINVAL | EINVAL |
| `readlink("/loop1", buf, 0)` bufsize=0 | -EINVAL | EINVAL |
| `readlink("/dev/null", ...)` devfs 无 readlink op | -EOPNOTSUPP | EOPNOTSUPP |
| `readlink("/nonexist", ...)` | -ENOENT | ENOENT |
| `readlink("/regular-file", buf, 0xdeadbeef)` 用户 buf 非法 | -EFAULT | EFAULT |
| `lstat("/bin/sh")` (symlink) → S_IFLNK \| 0777, size=target 长度 | 0 | - |
| `lstat("/bin/regular")` → S_IFREG \| 0755 | 0 | - |
| `lstat("/link_to_dir/file")` (mid-path) → 跟随 /link_to_dir 返回 file | 0 | -  (v3 修订：NOFOLLOW 不阻止中段) |
| `lstat("/no/such")` | -ENOENT | ENOENT |
| `lstat(..., 0xdeadbeef)` 用户 buf 非法 | -EFAULT | EFAULT |
| `stat("/bin/sh")` 跟随到 /bin/busybox | 0 | - |
| `fstatat(AT_FDCWD, "/bin/sh", buf, AT_SYMLINK_NOFOLLOW)` → link 自身 | 0 | - |
| `fstatat(AT_FDCWD, "/bin/sh", buf, 0)` → busybox | 0 | - |
| `fstatat(AT_FDCWD, "/x", buf, AT_EMPTY_PATH)` (未支持 flag) | -EINVAL | EINVAL (v3 新增) |
| `fstatat(bad_fd, "/x", buf, 0)` | 0（绝对 path 忽略 dirfd） | - |
| 跟随超过 8 跳（末段或中段） | -ELOOP | ELOOP |
| 跟随中间 readlink 失败 | -EIO | EIO |
| 跟随到不存在的 target | -ENOENT | ENOENT |
| 跟随路径含非法组件（ENOTDIR 等） | -ENOTDIR | ENOTDIR |
| 路径总长 ≥ VFS_NAME_MAX（vfs_lookup_at 内部） | -ENAMETOOLONG | ENAMETOOLONG |

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
| **`__vfs_lookup_raw` 签名变化**：3 个 out 参数影响既有调用点 | 全部迁移至 `vfs_lookup_at`；不保留旧 wrapper，避免语义分叉 |
| **新增 kmalloc 失败路径** | vfs_lookup_at 处理 kmalloc 失败返回 -ENOMEM |
| **`consumed_out`/`suffix` 截断**：buffer < path 长度时不写 | 不影响主路径（vfs_lookup_at 用 sizeof = 256），只影响理论极端长路径；v1 矩阵新增 ENAMETOOLONG 显式错误 |
| **ext2 unlink 回收错误** | §4.4 类型感知专用路径：long link 释放一次 block，fast link 不将 target 字节解释为块号 |
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
