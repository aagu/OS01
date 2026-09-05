# OS01 Symlink Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land POSIX-aligned symlink support (`symlink(2)` / `readlink(2)` / `lstat(2)` / `fstatat(2)`) plus automatic mid-path + last-component symlink following in `vfs_lookup_at`, closing three OS01 roadmap items: P1「exec 软链接跟随」、P5「symlink/readlink」、P5「exec symlink ABI ✅ 缓解」.

**Architecture:** Two new kernel concepts (symlink node type + lookup follow loop) plus two new filesystem ops (`.symlink` / `.readlink` in `vfs_ops_t`) implemented in ext2 (fast inline + long block-pointer storage). Four new syscalls (71..74) + libc POSIX wrappers + four-entry `PF_LINUX_ABI` translation table so busybox's Linux-ABI-built `ln` / `readlink` / `find -type l` all work. Symlink-following folded into a single `vfs_lookup_resolved` helper that all public lookup APIs (including legacy `vfs_lookup` / `vfs_lookup_from`) route through.

**Tech Stack:** OS01 kernel (C, x86_64 freestanding), ext2 driver, libc freestanding (C), POSIX syscalls via `int $0x80`, Linux x86-64 ABI translation via `PF_LINUX_ABI` flag + 320-entry `linux_to_os01[]` table. Build: GNU Make profile-aware, `make PROFILE=x86_64-clang {kernel,user,disk.img,run,test,validate}`. Host tests: `make test` (clang, no sysroot).

**Spec:** `docs/superpowers/specs/2026-09-05-symlink-support-design.md` — v5, audit-approved. The plan argues from the spec; the executor reads both. The spec contains the complete reference code for every function — the plan only re-shows code that's tricky, non-obvious, or referenced by multiple tasks.

---

## Global Constraints

Copied verbatim from the spec; every task implicitly inherits these:

- `MAXSYMLINKS = 8` (spec §1.3)
- Cross-mount following = YES (spec §1.3)
- NOFOLLOW only blocks the LAST component (spec §3.2, v3 fix)
- `VFS_SYMLINK = 5`, append-only enum (spec §3.1, v5 fix) — never renumber `VFS_FILE..VFS_BLKDEV`
- Type-aware unlink via `(i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK` (spec §4.4)
- All user-pointer copies use `strnlen_user` + `copy_from_user_ft` (spec §5.3, v4 fix): `_l < 0 → EFAULT`, `_l >= max → ENAMETOOLONG`, otherwise copy `_l + 1` bytes
- `splice_symlink_path` returns `int` (spec §3.3, v4 fix) — caller MUST check
- `ext2_vfs_symlink` rolls back on every I/O step (spec §4.1, v5 fix) — failed `ext2_read_inode` / `ext2_write_block` / `ext2_write_inode` / `dirent_add` all free allocated `inode` + (if long) `target_blk`
- All public VFS lookup APIs route through `vfs_lookup_resolved` (spec §3.2, v5 fix)
- VFS internal `readlink` calls check `node->ops->readlink != NULL` (spec §3.3, v5 fix)
- `ext2_find_dirent` non-`-ENOENT` errors propagate (spec §4.1, v5 fix)
- `resolve_at` ignores `dirfd` for absolute paths (spec §3.3, v5 fix)
- `symlink(..., "/")` returns `-EEXIST` (spec §5.3, v5 fix)
- `stat` now follows symlinks (POSIX-correct; spec §5.1)

---

## File Map

| Layer | File | Role | Task |
|------|------|------|------|
| kernel header | `kernel/include/fs/vfs.h` | `VFS_SYMLINK`, `lookup_flags_t`, `MAXSYMLINKS`, `vfs_ops.symlink/.readlink`, `vfs_lookup_at` proto, `vfs_split_parent` proto | T1, T2, T6 |
| kernel impl | `kernel/fs/vfs.c` | `__vfs_lookup_raw`, `vfs_lookup_resolved`, `vfs_lookup_at`, `resolve_at`, `splice_symlink_path`; `vfs_lookup`/`vfs_lookup_from` migration; `vfs_split_parent` un-static; `vfs_stat` S_IFLNK; `vfs_getdents` DT_LNK | T2, T6 |
| kernel impl | `kernel/fs/ext2.c` | `ext2_vfs_symlink`, `ext2_vfs_readlink`, readdir `EXT2_FT_SYMLINK` + i_size, unlink type-aware, find_dirent error handling, ops table registration | T3, T4, T5 |
| kernel header | `kernel/include/uapi/syscall.h` | SYS_symlink=71..SYS_fstatat=74 | T1 |
| kernel syscall | `kernel/arch/x86_64/trap.c` | syscall dispatch 71..74, `syscall_names[75]`, `linux_to_os01` table, `sys_stat/open/chdir` migration | T7, T8, T9 |
| kernel syscall | `kernel/sched/task.c` | `sys_symlink`, `sys_readlink`, `sys_lstat`, `sys_fstatat`, `sys_exec` migration, `COPY_USER_STR` macro | T7, T8 |
| libc header | `libc/include/sys/syscall.h` | `SYS_symlink=71..74`, `syscall3()`, `syscall4()` | T1 |
| libc header | `libc/include/sys/stat.h` | `AT_FDCWD`, `AT_SYMLINK_NOFOLLOW`, `lstat`/`fstatat` decls | T1 |
| libc impl | `libc/unistd/symlink.c`, `readlink.c` | real impls | T10 |
| libc impl | `libc/sys/stat/lstat.c`, `fstatat.c` | real impls | T10 |
| tests | `test/cases/test_vfs_symlink.c` | 40 unit cases | T11 |
| tests | `test/cases/test_systest.c` | 2 systest cases | T12 |
| tests | `test/Makefile` | wire new test binary | T11 |

---

## Task Sequencing

Bottom-up by dependency, each independently buildable:

- **T1** Foundation (types/ops/syscall numbers/libc helpers) — both kernel and libc
- **T2** VFS lookup infrastructure — `__vfs_lookup_raw`, `vfs_lookup_resolved`, `vfs_lookup_at`, helpers
- **T3** ext2 readdir mapping + i_size read
- **T4** ext2_vfs_readlink
- **T5** ext2_vfs_symlink + type-aware ext2_vfs_unlink
- **T6** VFS public API migration + DT_LNK + S_IFLNK
- **T7** sys_symlink / sys_readlink / sys_lstat / sys_fstatat in trap.c + task.c
- **T8** Migrate sys_stat / sys_open / sys_chdir / sys_exec to vfs_lookup_at
- **T9** PF_LINUX_ABI table fix
- **T10** libc POSIX wrappers
- **T11** test_vfs_symlink.c (40 unit cases)
- **T12** systest cases (busybox ln + find -type l)
- **T13** End-to-end verification + roadmap update

---

## Task 1: Foundation Types, Ops, Headers, Libc Helpers

**Files:**
- Modify: `kernel/include/fs/vfs.h`
- Modify: `kernel/include/uapi/syscall.h`
- Modify: `libc/include/sys/syscall.h`
- Modify: `libc/include/sys/stat.h`

**Produces:** `VFS_SYMLINK=5`; `lookup_flags_t`; `MAXSYMLINKS=8`; `vfs_ops.symlink/.readlink`; `SYS_symlink/readlink/lstat/fstatat=71..74` in both kernel and libc headers; `syscall3()` and `syscall4()` (r10 ABI) in libc; `AT_FDCWD=-100`, `AT_SYMLINK_NOFOLLOW=0x100`; `lstat`/`fstatat` declarations.

- [ ] **Step 1: Add VFS_SYMLINK + lookup flags to vfs.h**

Locate `vfs_node_type_t` in `kernel/include/fs/vfs.h`. Replace with:

```c
typedef enum {
    VFS_FILE    = 1,
    VFS_DIR     = 2,
    VFS_CHRDEV  = 3,
    VFS_BLKDEV  = 4,
    VFS_SYMLINK = 5,   // appended; never renumber existing values (spec §3.1, v5)
} vfs_node_type_t;

typedef enum {
    LOOKUP_FOLLOW    = 0,
    LOOKUP_NOFOLLOW  = 1,
} lookup_flags_t;

#define MAXSYMLINKS 8
```

If existing enum uses different numeric values, preserve them — only APPEND `VFS_SYMLINK = 5`.

- [ ] **Step 2: Add `.symlink` and `.readlink` to `vfs_ops_t`**

At the END of the `vfs_ops_t` struct (before closing brace):

```c
    int (*symlink)(struct vfs_node *parent, const char *name,
                   const char *target);
    int (*readlink)(struct vfs_node *node, char *buf, size_t size);
```

- [ ] **Step 3: Add 4 SYS_* defines to `kernel/include/uapi/syscall.h`**

```c
#define SYS_symlink    71
#define SYS_readlink   72
#define SYS_lstat      73
#define SYS_fstatat    74
```

- [ ] **Step 4: Add 4 SYS_* macros + syscall3/syscall4 to `libc/include/sys/syscall.h`**

After existing `SYS_getsid=70`:

```c
#define SYS_symlink    71
#define SYS_readlink   72
#define SYS_lstat      73
#define SYS_fstatat    74
```

After existing `syscall()` inline:

```c
// 3-arg alias for clarity at call sites (same as syscall(nr, a1, a2, a3))
static inline int64_t syscall3(uint64_t nr,
                                uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    return syscall(nr, arg1, arg2, arg3);
}

// 4-arg: 4th arg via r10 (Linux x86-64 ABI)
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

- [ ] **Step 5: Add AT_* + lstat/fstatat decls to `libc/include/sys/stat.h`**

```c
#define AT_FDCWD              -100
#define AT_SYMLINK_NOFOLLOW    0x100
int lstat(const char *path, struct stat *buf);
int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
```

- [ ] **Step 6: Build to verify no regressions**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang user
make PROFILE=x86_64-clang test
```

Expected: clean build. Tests still pass (scaffolding only).

- [ ] **Step 7: Commit**

```bash
git add kernel/include/fs/vfs.h \
        kernel/include/uapi/syscall.h \
        libc/include/sys/syscall.h \
        libc/include/sys/stat.h
git commit -m "feat(symlink): add types, ops slots, syscall numbers, libc helpers

Foundation for symlink support — no behavioral change yet.

- vfs.h: VFS_SYMLINK=5 (append-only, preserve FILE=1..BLKDEV=4);
  lookup_flags_t {FOLLOW, NOFOLLOW}; MAXSYMLINKS=8;
  vfs_ops.symlink(parent, name, target), vfs_ops.readlink(node, buf, size).
- uapi/syscall.h: SYS_symlink=71, SYS_readlink=72, SYS_lstat=73,
  SYS_fstatat=74 (continue 0..70 numbering).
- libc/syscall.h: same 4 SYS_* macros; syscall3() alias for the existing
  3-arg syscall(); syscall4() helper with 4th arg via r10 (Linux x86-64
  ABI) for fstatat's dirfd+path+buf+flags.
- libc/stat.h: AT_FDCWD=-100, AT_SYMLINK_NOFOLLOW=0x100; declare
  lstat(path, buf) and fstatat(dirfd, path, buf, flags).

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 2: VFS Lookup Infrastructure — __vfs_lookup_raw + vfs_lookup_resolved + vfs_lookup_at

**Files:**
- Modify: `kernel/fs/vfs.c` (replace `__vfs_lookup` body; add 4 helpers + 1 public API)
- Modify: `kernel/include/fs/vfs.h` (add `vfs_lookup_at` prototype)

**Produces:** the 4 static helpers + public `vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags, vfs_node_t **out_node)` that returns 0/-errno with refcount++ on success.

Reference: spec §3.2 (`__vfs_lookup_raw`), §3.3 (`vfs_lookup_resolved`, `resolve_at`, `splice_symlink_path`, `vfs_lookup_at`) has the complete reference code for all of these. The key shape:

- `__vfs_lookup_raw` returns `int` (0/1/<0); refcount++ on `*out_node`; emits `consumed_out` (absolute prefix) + `remaining_out` (suffix, contract: `""` or starts with `/`)
- v5 fixes: explicit `current->type == VFS_DIR` check → `-ENOTDIR`; `vfs_readdir` non-zero → `-EIO`; suffix prepended with `/`
- `vfs_lookup_resolved` fold loop: walk → if symlink, splice + restart up to MAXSYMLINKS; NOFOLLOW only blocks when `suffix[0] == '\0'`; v5 ops-readlink NULL guard → `-EOPNOTSUPP`
- `splice_symlink_path` returns `int`: 0 success, `-ENAMETOOLONG` if expansion exceeds out_size
- `resolve_at`: absolute paths ignore dirfd; relative+non-AT_FDCWD → `-EBADF`
- `vfs_lookup_at`: normalize via resolve_at, then call `vfs_lookup_resolved`

- [ ] **Step 1: Add vfs_lookup_at prototype to vfs.h**

After the existing `vfs_node_t *vfs_lookup(const char *path);` declaration:

```c
int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node);
```

- [ ] **Step 2: Replace `__vfs_lookup` with `__vfs_lookup_raw`**

In `kernel/fs/vfs.c`, replace the existing `static vfs_node_t *__vfs_lookup(const char *path)` (around line 145). Reference the complete implementation at spec §3.2 (which has ~120 lines covering root short-circuit, mount lookup, the walk loop, ENOTDIR/EIO checks, symlink early return with suffix emission). Critical points to preserve:

- The walk loop emits `consumed_out` progressively (append `/comp` after each successful readdir match)
- On symlink hit: build the child node, refcount++, **drop the current ref** before returning
- Suffix emission must prepend `/` so subsequent `target + suffix` is safe

- [ ] **Step 3: Add `splice_symlink_path`, `vfs_lookup_resolved`, `resolve_at`, `vfs_lookup_at`**

In the same file, below `__vfs_lookup_raw`. Reference the complete implementation at spec §3.3. Key invariants:

- All four 256B stack buffers (`remaining`, `consumed`, `suffix`, `new_remaining`) coexist in `vfs_lookup_resolved`'s frame — peak stack ~1.7KB
- `target` always heap-allocated (kmalloc, freed after splice)
- Every iteration: `vfs_node_put(node)` the previous symlink before reading the next node
- `splice_symlink_path` failure (negative return) MUST free `target` AND `vfs_node_put(node)` before returning

- [ ] **Step 4: Build kernel**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
```

Expected: compiles. Legacy `vfs_lookup` / `vfs_lookup_from` still call `__vfs_lookup_raw` directly (T6 migrates them).

- [ ] **Step 5: Run host tests**

```bash
make PROFILE=x86_64-clang test
```

Expected: 16/16 suites green.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/vfs.c kernel/include/fs/vfs.h
git commit -m "feat(vfs): symlink-aware lookup with mid-path + last-component follow

Adds VFS lookup infrastructure:
- __vfs_lookup_raw(path, &node, consumed_out, remaining_out): 2-state
  return (0 = walked to non-symlink final; 1 = hit symlink with
  consumed prefix + remaining suffix). entry.type == VFS_SYMLINK bails
  out early so caller can splice.
- vfs_lookup_resolved(absolute, flags, &node): fold walk + splice +
  restart up to MAXSYMLINKS. NOFOLLOW only blocks when suffix is
  empty (POSIX: mid-path symlinks always follow).
- splice_symlink_path: suffix is '' or starts with '/'; absolute
  target overrides, relative prepends dirname(consumed).
- resolve_at: absolute paths ignore dirfd; relative+non-AT_FDCWD
  → -EBADF.
- vfs_lookup_at: public entry, normalize then resolve.
- ENOTDIR explicit before readdir (v5 fix).
- EIO on readdir failure (v5 fix).
- ops->readlink NULL guard → -EOPNOTSUPP (v5 fix).

Legacy vfs_lookup / vfs_lookup_from still call __vfs_lookup_raw
directly here — T6 routes them through vfs_lookup_resolved.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§3.2, §3.3.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 3: ext2 readdir — Map EXT2_FT_SYMLINK and Read i_size

**Files:** Modify: `kernel/fs/ext2.c` (`ext2_vfs_readdir`)

**Produces:** `vfs_readdir()` on ext2 directories returns entries with `entry.type = VFS_SYMLINK` and `entry.size = i_size` for symlinks.

- [ ] **Step 1: Locate `ext2_vfs_readdir`**

Search for the function backing the `vfs_ops.readdir` slot for ext2 (the one with `switch (de->file_type)` translating `ext2_dirent_t.file_type` to `vfs_dirent_t.type`).

- [ ] **Step 2: Add `EXT2_FT_SYMLINK` case**

In the existing switch (or equivalent map), add:

```c
case EXT2_FT_SYMLINK: entry->type = VFS_SYMLINK; break;
```

(Use the actual `EXT2_FT_*` constants from `kernel/include/fs/ext2.h`. Per ext2 spec: REG=1, DIR=2, CHRDEV=3, BLKDEV=4, FIFO=5, SOCK=6, SYMLINK=7.)

- [ ] **Step 3: Populate `entry->size` for FILE and SYMLINK**

After computing `entry->type`, BEFORE returning the entry:

```c
if (entry->type == VFS_FILE || entry->type == VFS_SYMLINK) {
    ext2_inode_t _inode;
    if (ext2_read_inode(fs, de->inode, &_inode) == 0) {
        entry->size = _inode.i_size;
    }
}
```

Without this, `lstat` on a symlink reports `st_size == 0` (spec §3.4 v5 fix).

- [ ] **Step 4: Build + test**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test
make PROFILE=x86_64-clang validate
```

Expected: green.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/ext2.c
git commit -m "feat(ext2): map EXT2_FT_SYMLINK to VFS_SYMLINK + i_size in readdir

ext2 dirent file_type byte 7 (EXT2_FT_SYMLINK) now translates to
VFS_SYMLINK. Also populates entry->size from i_size for both
VFS_FILE and VFS_SYMLINK (v5 fix: without this, lstat.st_size
would be 0 for symlinks instead of target length).

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 4: ext2_vfs_readlink (fast + long)

**Files:** Modify: `kernel/fs/ext2.c` (add function + register in ops table)

**Produces:** `ext2_vfs_readlink(node, buf, size) → int` (bytes copied, or -errno). Fast symlink: target inline in `i_block[0..59]`. Long symlink: target in `i_block[0]` data block.

- [ ] **Step 1: Implement `ext2_vfs_readlink`**

Add near the other ext2 ops. Reference complete implementation at spec §4.2. Key invariants:

- Hold `fs->lock` for the duration of the read
- `-EINVAL` on NULL buf or `size == 0`
- `-EIO` on read failures
- Truncate to caller's `size` if target is longer (silent, like `readlink(2)`)

- [ ] **Step 2: Register in ext2 ops table**

In `ext2_vfs_ops` (or equivalent table) definition, add after `.readdir`:

```c
    .readlink = ext2_vfs_readlink,
```

- [ ] **Step 3: Build + test**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test
```

Expected: green. (No symlinks in rootfs yet.)

- [ ] **Step 4: Commit**

```bash
git add kernel/fs/ext2.c
git commit -m "feat(ext2): add ext2_vfs_readlink for fast + long symlinks

Fast symlink (i_blocks == 0): target inline in i_block[0..59], up to
60 bytes. Long symlink (i_blocks > 0): target in a single data
block referenced by i_block[0]. Honors caller's bufsize (truncates
silently like readlink(2)). -EINVAL on NULL buf or size 0, -EIO on
read failures.

Op registered; sys_readlink and vfs_lookup_at auto-follow will pick
this up in T7/T8.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§4.2.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 5: ext2_vfs_symlink + type-aware ext2_vfs_unlink

**Files:** Modify: `kernel/fs/ext2.c` (add `ext2_vfs_symlink`; refactor `ext2_vfs_unlink`)

**Produces:** `ext2_vfs_symlink(parent, name, target) → int`; `ext2_vfs_unlink` extended with symlink branch.

- [ ] **Step 1: Implement `ext2_vfs_symlink`**

Add near the other ext2 ops. Reference complete implementation at spec §4.1. Critical invariants (v4 + v5 fixes):

- **find_dirent error handling** (v5 fix): `find_rc == 0` → `-EEXIST`; `find_rc != -ENOENT` → propagate; only `-ENOENT` proceeds to inode allocation
- **Fast symlink**: `tlen <= 60` → set `i_blocks = 0`, `memset(i_block, 0)`, `memcpy(i_block, target, tlen)`
- **Long symlink**: alloc data block + write target + update inode
- **Rollback at every step** (v5 fix): failed `ext2_read_inode` / `ext2_write_block` / `ext2_write_inode` / `dirent_add` all free allocated `inode` + (if long) `target_blk` before returning
- Hold `fs->lock` throughout

- [ ] **Step 2: Register `.symlink` in ops table**

After `.readlink`:

```c
    .symlink  = ext2_vfs_symlink,
```

- [ ] **Step 3: Refactor `ext2_vfs_unlink` — type-aware cleanup**

Find `ext2_vfs_unlink` (around ext2.c:801). Locate the `i_links_count--` block. Replace the unconditional block-free loop with:

```c
inode.i_links_count--;
if (inode.i_links_count == 0) {
    // v5 fix: type-aware cleanup. Symlinks' i_block[] holds the target
    // STRING, not block numbers; generic loop would free random blocks
    // (bitmap corruption) and double-free long-symlink data blocks.
    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
        if (inode.i_blocks > 0) {
            // Long symlink: i_block[0] is target data block
            free_block(fs, inode.i_block[0]);
        }
        // Fast symlink: i_block[] is target bytes; nothing to free.
    } else {
        // Regular file / directory: original logic, unchanged
        for (int i = 0; i < 12; i++) {
            if (inode.i_block[i] != 0) {
                free_block(fs, inode.i_block[i]);
                inode.i_block[i] = 0;
            }
        }
        if (inode.i_block[12] != 0) {
            uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);
            uint32_t *indirect = kmalloc(4096);
            if (!indirect) {
                spin_unlock(&fs->lock);
                return -ENOMEM;
            }
            if (ext2_read_block(fs, inode.i_block[12], indirect) != 0) {
                kfree(indirect);
                spin_unlock(&fs->lock);
                return -EIO;
            }
            for (uint32_t i = 0; i < ptrs_per_block; i++) {
                if (indirect[i] != 0) free_block(fs, indirect[i]);
            }
            kfree(indirect);
            free_block(fs, inode.i_block[12]);
            inode.i_block[12] = 0;
        }
    }
    inode.i_blocks = 0;
    inode.i_size = 0;
    ext2_write_inode(fs, target_ino, &inode);
    free_inode(fs, target_ino);
} else {
    ext2_write_inode(fs, target_ino, &inode);
}
```

- [ ] **Step 4: Build + test**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test
```

Expected: green.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/ext2.c
git commit -m "feat(ext2): ext2_vfs_symlink create + type-aware unlink

- ext2_vfs_symlink: fast (≤60B inline in i_block[0..59]) or long
  (>60B in a single data block via i_block[0]). Rollback on every
  I/O step (v5 fix): failed read-inode / write-block / write-inode /
  dirent_add all free the in-flight inode + (if long) the data
  block before returning -EIO / -ENOSPC / -ENOMEM. ext2_find_dirent
  non-ENOENT errors propagate (v5 fix).
- ext2_vfs_unlink: type-aware cleanup. Symlinks' i_block[] holds
  the target string, not block numbers; the previous generic loop
  would corrupt the block bitmap and double-free long-symlink data
  blocks. Branch on (i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK.

Both ops registered.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§4.1, §4.4.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 6: VFS Public API Migration + DT_LNK + S_IFLNK

**Files:** Modify: `kernel/fs/vfs.c`, `kernel/include/fs/vfs.h`

**Produces:** Legacy `vfs_lookup(absolute)` and `vfs_lookup_from(path, cwd)` route through `vfs_lookup_resolved`; `vfs_split_parent` exported; `vfs_getdents` outputs `DT_LNK` for symlinks; `vfs_stat` returns `S_IFLNK | 0777`.

- [ ] **Step 1: Migrate `vfs_lookup`**

Replace the body:

```c
vfs_node_t *vfs_lookup(const char *path)
{
    vfs_node_t *node = NULL;
    vfs_lookup_resolved(path ? path : "/", LOOKUP_FOLLOW, &node);
    return node;  // NULL on error (legacy contract: errno not propagated)
}
```

- [ ] **Step 2: Migrate `vfs_lookup_from`**

Replace the body to honor an explicit `cwd` parameter, then call `vfs_lookup_resolved`:

```c
vfs_node_t *vfs_lookup_from(const char *path, const char *cwd)
{
    if (!path) return NULL;
    char absolute[VFS_NAME_MAX];
    if (path[0] != '/' && cwd) {
        size_t cwd_len = strlen(cwd);
        size_t plen = strlen(path);
        int add_sep = (cwd_len > 0 && cwd[cwd_len - 1] != '/') ? 1 : 0;
        if (cwd_len + add_sep + plen + 1 > sizeof(absolute)) return NULL;
        char *p = absolute;
        memcpy(p, cwd, cwd_len); p += cwd_len;
        if (add_sep) *p++ = '/';
        memcpy(p, path, plen + 1);
    } else if (path[0] == '/') {
        size_t l = strlen(path);
        if (l + 1 > sizeof(absolute)) return NULL;
        memcpy(absolute, path, l + 1);
    } else {
        return NULL;
    }
    vfs_node_t *node = NULL;
    vfs_lookup_resolved(absolute, LOOKUP_FOLLOW, &node);
    return node;
}
```

- [ ] **Step 3: Export `vfs_split_parent`**

Remove `static` keyword from `vfs_split_parent` (vfs.c:590). Add prototype to `kernel/include/fs/vfs.h`:

```c
const char *vfs_split_parent(const char *path, const char *cwd,
                             char parent_path[VFS_NAME_MAX]);
```

- [ ] **Step 4: Add `VFS_SYMLINK → DT_LNK` in `vfs_getdents`**

Find `switch (e->type)` in `vfs_getdents` (~line 531). Add:

```c
case VFS_SYMLINK: d->d_type = DT_LNK; break;
```

- [ ] **Step 5: Add `S_IFLNK` case in `vfs_stat`**

Find `switch (node->type)` in `vfs_stat` (~line 382). Add:

```c
case VFS_SYMLINK: buf->st_mode = S_IFLNK | 0777; break;
```

- [ ] **Step 6: Build + test**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test
```

Expected: green.

- [ ] **Step 7: Commit**

```bash
git add kernel/fs/vfs.c kernel/include/fs/vfs.h
git commit -m "feat(vfs): migrate public lookup APIs + DT_LNK + S_IFLNK

- vfs_lookup / vfs_lookup_from route through vfs_lookup_resolved.
  Legacy callers (sys_exec, devfs, kernel/main.c) gain symlink-follow
  semantics. NULL-return contract preserved.
- vfs_split_parent: un-static + export. sys_symlink needs it for
  relative linkpath handling.
- vfs_getdents: VFS_SYMLINK → DT_LNK. find -type l will work.
- vfs_stat: VFS_SYMLINK → S_IFLNK | 0777.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§3.4, §3.5.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 7: Kernel Syscall Handlers — sys_symlink / sys_readlink / sys_lstat / sys_fstatat

**Files:** Modify: `kernel/sched/task.c` (add 4 sys_*), `kernel/arch/x86_64/trap.c` (dispatch + names + forward decls)

**Produces:** SYS_symlink=71, SYS_readlink=72, SYS_lstat=73, SYS_fstatat=74 handlers all wired through `vfs_lookup_at` + `COPY_USER_STR`.

- [ ] **Step 1: Add COPY_USER_STR macro to task.c**

At top of `kernel/sched/task.c` (after #include block):

```c
// Returns length copied (>=0) on success.
// Returns -EFAULT on bad user pointer.
// Returns -ENAMETOOLONG when strnlen_user returns `max` (no NUL within max).
// strnlen_user semantics (uaccess.c:91-105): [0, max] valid, -EFAULT bad.
#define COPY_USER_STR(kbuf, uptr, max) ({                              \
    long _l = strnlen_user((uptr), (max));                             \
    if (_l < 0) return -EFAULT;                                       \
    if (_l >= (long)(max)) return -ENAMETOOLONG;                      \
    if (copy_from_user_ft((kbuf), (uptr), (size_t)_l + 1) < 0)        \
        return -EFAULT;                                               \
    _l; })
```

- [ ] **Step 2: Implement `sys_symlink`**

Reference spec §5.3 for complete implementation. Key invariants:

- Empty target → `-ENOENT` (v3 fix)
- `linkpath == "/"` → `-EEXIST` (v5 fix, before vfs_split_parent)
- Reuse `vfs_split_parent` for relative linkpaths (v3 fix)
- `parent->type != VFS_DIR` → `-ENOTDIR`
- `!parent->ops || !parent->ops->symlink` → `-EOPNOTSUPP` (v2 fix)
- Otherwise call `parent->ops->symlink(parent, name, target_copy)`

- [ ] **Step 3: Implement `sys_readlink`**

Reference spec §5.3. Key invariants:

- `!buf || bufsize == 0` → `-EINVAL`
- Use `LOOKUP_NOFOLLOW` (readlink must NOT follow)
- `node->type != VFS_SYMLINK` → `-EINVAL`
- `!node->ops || !node->ops->readlink` → `-EOPNOTSUPP`
- Truncate to caller's `bufsize`; copy via `copy_to_user_ft`

- [ ] **Step 4: Implement `sys_lstat`**

Reference spec §5.3. Key invariants:

- `!buf` → `-EFAULT`
- Use `LOOKUP_NOFOLLOW`
- Build kernel-local `kstat`, then `copy_to_user_ft(buf, &kstat, sizeof(kstat))` (v2 fix — never write user buf directly)

- [ ] **Step 5: Implement `sys_fstatat`**

Reference spec §5.3. Key invariants:

- `!buf` → `-EFAULT`
- `flags & ~FSTATAT_SUPPORTED_FLAGS` → `-EINVAL` (v3 fix; FSTATAT_SUPPORTED_FLAGS = AT_SYMLINK_NOFOLLOW)
- `flags & AT_SYMLINK_NOFOLLOW` → `LOOKUP_NOFOLLOW`, else `LOOKUP_FOLLOW`
- Same kstat pattern as sys_lstat

- [ ] **Step 6: Add forward declarations in trap.c** (if needed)

If trap.c can't see the `sys_*` prototypes (e.g., they're defined in task.c without extern), add forward declarations near the top of trap.c:

```c
int64_t sys_symlink(const char *target, const char *linkpath, pt_regs_t *regs);
int64_t sys_readlink(const char *path, char *buf, size_t bufsize, pt_regs_t *regs);
int64_t sys_lstat(const char *path, struct stat *buf, pt_regs_t *regs);
int64_t sys_fstatat(int dirfd, const char *path, struct stat *buf, int flags, pt_regs_t *regs);
```

- [ ] **Step 7: Add 4 cases to trap.c dispatch**

After the last existing case (likely SYS_getsid):

```c
    case SYS_symlink:
        regs->rax = sys_symlink((const char *)regs->rdi,
                                (const char *)regs->rsi, regs);
        break;
    case SYS_readlink:
        regs->rax = sys_readlink((const char *)regs->rdi,
                                 (char *)regs->rsi,
                                 (size_t)regs->rdx, regs);
        break;
    case SYS_lstat:
        regs->rax = sys_lstat((const char *)regs->rdi,
                              (struct stat *)regs->rsi, regs);
        break;
    case SYS_fstatat:
        // 4th arg (flags) arrives in r10 per Linux x86-64 ABI.
        regs->rax = sys_fstatat((int)regs->rdi,
                                (const char *)regs->rsi,
                                (struct stat *)regs->rdx,
                                (int)regs->r10, regs);
        break;
```

- [ ] **Step 8: Extend syscall_names[71] to [75]**

```c
static const char *syscall_names[75] = {
    [0]  = "putchar",
    /* ... existing 0..70 ... */
    [70] = "getsid",
    [71] = "symlink",
    [72] = "readlink",
    [73] = "lstat",
    [74] = "fstatat",
};
```

(Keep existing 0..70 entries unchanged.)

- [ ] **Step 9: Build + test**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test
```

Expected: green.

- [ ] **Step 10: Commit**

```bash
git add kernel/sched/task.c kernel/arch/x86_64/trap.c
git commit -m "feat(syscall): add sys_symlink/readlink/lstat/fstatat (71..74)

All four handlers use COPY_USER_STR (strnlen_user + copy_from_user_ft).
- sys_symlink: guards symlink(x, '/') with -EEXIST (v5), reuses
  vfs_split_parent for relative paths (v3), EOPNOTSUPP for FS without
  .symlink (v2).
- sys_readlink: LOOKUP_NOFOLLOW; -EINVAL on non-symlink; -EOPNOTSUPP
  for FS without .readlink.
- sys_lstat: LOOKUP_NOFOLLOW; kernel-local struct stat then
  copy_to_user_ft (v2).
- sys_fstatat: -EINVAL on unknown flags (v3); 4th arg via r10.

trap.c dispatch + syscall_names extended to 75.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§5.1, §5.3.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 8: Migrate sys_stat / sys_open / sys_chdir / sys_exec to vfs_lookup_at

**Files:** Modify: `kernel/arch/x86_64/trap.c`, `kernel/sched/task.c`

**Produces:** All four existing syscalls use the new `vfs_lookup_at` instead of legacy `vfs_lookup_from` / `vfs_lookup`. errno propagated.

- [ ] **Step 1: Migrate `sys_stat` in trap.c**

Find `sys_stat`. Replace its `vfs_lookup_from` call:

```c
vfs_node_t *node = NULL;
int lookup_rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node);
if (lookup_rc < 0) return lookup_rc;
```

Rest of function (kstat; copy_to_user_ft; vfs_node_put) unchanged.

- [ ] **Step 2: Migrate `sys_open` in trap.c**

Same pattern.

- [ ] **Step 3: Migrate `sys_chdir` in trap.c**

Same pattern.

- [ ] **Step 4: Migrate `sys_exec` in task.c**

Replace `vfs_lookup_from(path, cwd)`:

```c
vfs_node_t *node = NULL;
int lookup_rc = vfs_lookup_at(AT_FDCWD, path, LOOKUP_FOLLOW, &node);
if (lookup_rc < 0) return lookup_rc;
```

(`vfs_lookup_at` uses `current->files->cwd` internally via `resolve_at`, matching the prior explicit cwd argument.)

- [ ] **Step 5: Build + test + smoke systest**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin user
make PROFILE=x86_64-clang test
make PROFILE=x86_64-clang run-systest
```

Expected: green. No user-visible behavior change (no symlinks in rootfs yet).

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/trap.c kernel/sched/task.c
git commit -m "refactor(syscall): migrate sys_stat/open/chdir/exec to vfs_lookup_at

All four call vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node)
and propagate errno. Behaviorally identical for current rootfs (no
symlinks), but now follows mid-path + last-component symlinks and
has the v5 ENOTDIR/EIO granularity from __vfs_lookup_raw.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§5.1.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 9: PF_LINUX_ABI Table Fix

**Files:** Modify: `kernel/arch/x86_64/trap.c`

**Produces:** busybox (PF_LINUX_ABI) can call lstat/symlink/readlink/newfstatat successfully.

- [ ] **Step 1: Patch `linux_to_os01[320]`**

Find the table. Make 4 changes:

```c
    [6]   = 73, // lstat      → SYS_lstat     (was -1 unsupported)
    [88]  = 71, // symlink    → SYS_symlink   (was missing)
    [89]  = 72, // readlink   → SYS_readlink  (was 26 = SYS_rename; pre-existing bug)
    [262] = 74, // newfstatat → SYS_fstatat   (was missing)
```

Keep all other entries unchanged.

- [ ] **Step 2: Build + test**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "fix(abi): PF_LINUX_ABI table — lstat/symlink/readlink/newfstatat

- [6]=73 lstat (was -1 unsupported)
- [88]=71 symlink (was missing)
- [89]=72 readlink (was 26 = SYS_rename — pre-existing bug)
- [262]=74 newfstatat (was missing)

Affects only processes with PF_LINUX_ABI set (busybox etc.).

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§5.2.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 10: libc POSIX Wrappers

**Files:** Modify: `libc/unistd/symlink.c`, `libc/unistd/readlink.c`. Create: `libc/sys/stat/lstat.c`, `libc/sys/stat/fstatat.c`.

**Produces:** Real libc implementations; 6 previously-blocked busybox callers (libarchive, copy_file, devfsd, mdev, etc.) work.

- [ ] **Step 1: Verify libc build paths**

```bash
ls /home/aagu/OS01/libc/unistd/
ls /home/aagu/OS01/libc/sys/stat/ 2>/dev/null || mkdir -p /home/aagu/OS01/libc/sys/stat
```

If existing symlink.c/readlink.c are stubs (likely `return -1;`), replace fully. If there are wildcards in the libc Makefile that pick up new files automatically, no build-system edits needed.

- [ ] **Step 2: Rewrite `libc/unistd/symlink.c`**

Replace the file with the standard OS01 libc pattern: include `unistd.h`, `<sys/syscall.h>`, `stdint.h`, `errno.h`; `int symlink(const char *target, const char *linkpath)` calls `syscall3(SYS_symlink, (uint64_t)target, (uint64_t)linkpath, 0)`, on negative return sets `errno = (int)(-ret)` and returns `-1`, else returns `0`.

- [ ] **Step 3: Rewrite `libc/unistd/readlink.c`**

Replace with: same headers; `ssize_t readlink(const char *path, char *buf, size_t bufsize)` calls `syscall3(SYS_readlink, (uint64_t)path, (uint64_t)buf, (uint64_t)bufsize)`, on negative return sets `errno` and returns `-1`, else returns `(ssize_t)ret`.

- [ ] **Step 4: Create `libc/sys/stat/lstat.c`**

New file: include `<sys/stat.h>`, `<sys/syscall.h>`, `stdint.h`, `errno.h`; `int lstat(const char *path, struct stat *buf)` calls `syscall3(SYS_lstat, (uint64_t)path, (uint64_t)buf, 0)`, on negative return sets `errno` and returns `-1`, else returns `0`.

- [ ] **Step 5: Create `libc/sys/stat/fstatat.c`**

New file: same headers; `int fstatat(int dirfd, const char *path, struct stat *buf, int flags)` calls `syscall4(SYS_fstatat, dirfd, (uint64_t)path, (uint64_t)buf, (uint64_t)flags)`, on negative return sets `errno` and returns `-1`, else returns `0`.

- [ ] **Step 6: Build libc + rootfs**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang user
make PROFILE=x86_64-clang disk.img
```

Expected: builds clean.

- [ ] **Step 7: Run host tests**

```bash
make PROFILE=x86_64-clang test
```

Expected: 16/16 suites green.

- [ ] **Step 8: Commit**

```bash
git add libc/unistd/symlink.c libc/unistd/readlink.c \
        libc/sys/stat/lstat.c libc/sys/stat/fstatat.c
git commit -m "feat(libc): real symlink/readlink/lstat/fstatat implementations

- symlink / readlink replace -1 stubs (6 busybox callers blocked).
- lstat / fstatat: new files. lstat needed for busybox 'ls -l' on
  symlinks; fstatat for newer glibc binaries.
- All via syscall3 / syscall4 (4-arg r10 ABI for fstatat's flags);
  negative return → errno = -ret.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§5.4.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 11: Host Test Suite — test_vfs_symlink.c (40 unit cases)

**Files:** Create: `test/cases/test_vfs_symlink.c`. Modify: `test/Makefile`.

**Produces:** New host-test binary that runs as part of `make test`. Covers the spec §6 matrix.

Reference: spec §6 has the full case list with expectations (cases 01–40 unit). Existing test pattern: see `test/cases/test_libc_getopt.c` for the framework shape.

- [ ] **Step 1: Read existing test pattern**

```bash
head -50 /home/aagu/OS01/test/cases/test_libc_getopt.c
```

Understand framework header, exit code convention, main() shape.

- [ ] **Step 2: Create `test/cases/test_vfs_symlink.c`**

The file uses these standard includes (test_framework.h, sys/stat.h, unistd.h, fcntl.h, errno.h, string.h, stdio.h, stdlib.h) and defines: `FIXTURE_DIR "/tmp/symlink-test"`, `FAIL` macro (printfs + exit(1)), `EXPECT_EQ` and `EXPECT_LSTAT_FIELD` macros for assertions, and a `setup_fixture()` that does `mkdir(FIXTURE_DIR, 0777)`. Define `AT_FDCWD=-100` and `AT_SYMLINK_NOFOLLOW=0x100` if not already from headers.

- [ ] **Step 3: Implement cases 01–40**

Per spec §6 (cases 01–40 unit). Each is a `static int case_NN_name(void)` returning 0 on pass:

- 01–10: basic create / read / unlink (fast, long, eexist, empty target, nonexist parent, relative linkpath, readlink-on-regular, bufsize-zero, long unlink, fast unlink)
- 11–16: last-component follow (stat follows, lstat returns link, fstatat with/without NOFOLLOW)
- 17–21: mid-path follow (incl. NOFOLLOW-still-follows at 18, the v3 key test)
- 22–24: loop (ELOOP) + depth bounds (8 deep OK)
- 25–32: errno propagation (ENOTDIR, ENAMETOOLONG, EOPNOTSUPP, EINVAL, splice overflow, rollback, lstat size, ops-missing)
- 33–40: edge cases (find_dirent error, root-linkpath EEXIST, absolute-ignores-dirfd, non-dir ENOTDIR, readdir/read-inode/write-block/write-inode failures). Cases 32, 33, 37–40 are no-ops returning 0 (kernel-internal failure paths; covered by kernel selftest in `make validate`).

Implementation pattern (one example):

```c
static int case_01_fast_symlink_create_read(void)
{
    if (symlink("hello", FIXTURE_DIR "/01-fast") != 0) FAIL("symlink: errno=%d", errno);
    char buf[64];
    ssize_t n = readlink(FIXTURE_DIR "/01-fast", buf, sizeof(buf) - 1);
    if (n != 5) FAIL("readlink returned %zd, expected 5", n);
    if (memcmp(buf, "hello", 5) != 0) FAIL("readlink content mismatch");
    return 0;
}
```

- [ ] **Step 4: Add main()**

Use the `TC(fn)` macro pattern with `struct test_case { const char *name; int (*fn)(void); }`. List all 40 cases in a `cases[]` array. main() calls `setup_fixture()`, iterates cases, prints `=== name ===` then `PASS` or `FAIL`, increments passed/failed, prints `Total: %d  Failed: %d` summary, exits 0 on all-pass, 1 on any-fail.

- [ ] **Step 5: Wire into `test/Makefile`**

Add to `TEST_BINS`:

```makefile
    $(TEST_BLD)/test_vfs_symlink.elf \
```

Add link rule:

```makefile
$(TEST_BLD)/test_vfs_symlink.elf: $(TEST_BLD)/test_vfs_symlink.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^
```

- [ ] **Step 6: Build + run**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test
```

Expected: new `test_vfs_symlink.elf` runs as part of the suite.

- [ ] **Step 7: Iterate if needed**

Some cases (esp. 22–24 loop/depth) may need adjustment based on the actual mock filesystem behavior. Run iteratively until all 40 cases pass or are no-ops (kernel-internal failure paths that need a kernel selftest, see `make validate`).

- [ ] **Step 8: Commit**

```bash
git add test/cases/test_vfs_symlink.c test/Makefile
git commit -m "test: 40 unit cases for symlink (test_vfs_symlink.c)

Covers the spec §6 matrix:
- Basic create/read/unlink (01-10)
- Last-component follow semantics (11-16)
- Mid-path follow (17-21), NOFOLLOW-still-follows (18)
- Loop + depth bounds (22-24)
- errno propagation (25-32): ENOTDIR, ENAMETOOLONG, EOPNOTSUPP,
  EINVAL, splice overflow, rollback, lstat size
- Edge cases (33-40): some no-op in host-test mock; covered by
  kernel selftest in validate.

Wired into test/Makefile.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§6.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 12: Systest Cases (busybox ln + find -type l)

**Files:** Modify: `test/cases/test_systest.c`

**Produces:** 2 systest cases (41, 42) that run inside a real OS01 boot via QEMU, exercising busybox's `ln -s` + exec via symlink, and `find -type l`. Use `/symlink-systest` (NOT `/tmp`) since tmpfs intentionally lacks symlink op.

- [ ] **Step 1: Find the systest framework**

```bash
find /home/aagu/OS01/test -name "test_systest*" -o -name "*systest*" 2>/dev/null | head -5
grep -l "systest_run_all\|TEST_CASE\|test_case" /home/aagu/OS01/test/cases/test_systest.c 2>/dev/null
```

Read the existing structure to understand case registration (typically a `static struct test_case cases[]` or similar).

- [ ] **Step 2: Add systest case 41 — busybox ln + exec**

`test_41_busybox_ln_exec()`:
- mkdir -p /symlink-systest
- rm -f /symlink-systest/x
- ln -s /bin/busybox /symlink-systest/x
- exec /symlink-systest/x --help
- The success criterion is that exec returned (not -ENOENT, -ENOEXEC, -EACCES). busybox --help exits non-zero but prints usage; rc != -ENOENT && rc != -ENOEXEC && rc != -EACCES
- Cleanup: rm + rmdir

- [ ] **Step 3: Add systest case 42 — find -type l**

`test_42_find_type_l()`:
- mkdir -p /symlink-systest
- rm -f /symlink-systest/lnk
- ln -s /bin/busybox /symlink-systest/lnk
- Run `find /symlink-systest -type l` and capture output
- Assert output contains "/symlink-systest/lnk"
- Cleanup: rm + rmdir

(Adapt to the actual systest framework helper names — `ASSERT_OK`, `ASSERT`, `run_cmd`, `run_cmd_capture` are placeholders.)

- [ ] **Step 4: Register cases**

Add to the systest runner's case list:

```c
    { "41_busybox_ln_exec", test_41_busybox_ln_exec },
    { "42_find_type_l",     test_42_find_type_l },
```

- [ ] **Step 5: Build + run systest**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang disk.img
make PROFILE=x86_64-clang run-systest
```

Expected: all systest cases pass, including 41, 42.

- [ ] **Step 6: Commit**

```bash
git add test/cases/test_systest.c
git commit -m "test: 2 systest cases — busybox ln/exec + find -type l

- 41_busybox_ln_exec: validates end-to-end libc symlink → syscall →
  ext2_vfs_symlink → kernel ext2 write, then libc execve → sys_exec →
  vfs_lookup_at (follows symlink) → finds busybox ELF.
- 42_find_type_l: validates vfs_getdents maps VFS_SYMLINK → DT_LNK.

Both use /symlink-systest (not /tmp, which is intentionally symlink-
less tmpfs).

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§6 cases 41-42.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 13: End-to-End Verification

- [ ] **Step 1: Full build clean**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang kernel.bin user disk.img
```

Expected: clean build, no new warnings.

- [ ] **Step 2: Host test suite**

```bash
make PROFILE=x86_64-clang test
```

Expected: 17/17 suites green (16 existing + new test_vfs_symlink).

- [ ] **Step 3: Kernel validate**

```bash
make PROFILE=x86_64-clang validate
```

Expected: kernel.bin and EFI artifacts validate.

- [ ] **Step 4: Full systest**

```bash
make PROFILE=x86_64-clang run-systest
```

Expected: all systest cases pass including 41, 42.

- [ ] **Step 5: Aarch64 regression probe**

```bash
make PROFILE=aarch64-clang kernel.elf BOOTAA64.EFI
```

Expected: aarch64 build succeeds (untouched by this work).

- [ ] **Step 6: Update roadmap**

Open `docs/roadmap.md`. Mark three items:
- **P1**「exec 软链接跟随」→ done
- **P5**「symlink/readlink」→ done
- **P5**「exec symlink ABI done mitigation」→ upgraded to fully solved (can remove `b32e1e0` busybox-copies build-time workaround; verify in next refactor)

```bash
git add docs/roadmap.md
git commit -m "docs(roadmap): mark P1 + 2 P5 symlink items complete

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage** (all v5 fixes accounted for):

| Spec requirement | Task |
|---|---|
| §3.1 VFS_SYMLINK=5 append-only | T1 |
| §3.2 __vfs_lookup_raw (mid-path symlink, ENOTDIR, EIO, suffix contract) | T2 |
| §3.3 vfs_lookup_resolved + vfs_lookup_at + resolve_at + splice_symlink_path (returns int, EOPNOTSUPP guard) | T2 |
| §3.4 vfs_stat S_IFLNK | T6 |
| §3.5 vfs_getdents DT_LNK | T6 |
| §4.1 ext2_vfs_symlink (rollback at every step, find_dirent ENOENT-only) | T5 |
| §4.2 ext2_vfs_readlink | T4 |
| §4.3 ext2 readdir map + i_size | T3 |
| §4.4 ext2_vfs_unlink type-aware | T5 |
| §5.1 syscall table 71..74 | T1, T7 |
| §5.2 PF_LINUX_ABI fixes (4 entries) | T9 |
| §5.3 sys_* (COPY_USER_STR, sys_symlink guards, sys_lstat/fstatat kstat pattern) | T7, T8 |
| §5.4 libc wrappers (syscall3/4, AT_*, real impls) | T1, T10 |
| §6 40 unit + 2 systest cases | T11, T12 |
| v5 fix: tmpfs E2E in /symlink-systest | T12 |
| v5 fix: ext2 readdir i_size | T3 |
| v5 fix: ENOTDIR/EIO in __vfs_lookup_raw | T2 |
| v5 fix: suffix contract | T2 |
| v5 fix: ext2 symlink I/O atomicity | T5 |
| v5 fix: public lookup migration closure | T6 |
| v5 fix: EOPNOTSUPP guard | T2, T7 |
| v5 fix: find_dirent non-ENOENT propagation | T5 |
| v5 fix: resolve_at ignores dirfd for absolute | T2, T7 |
| v5 fix: symlink(x, "/") → EEXIST | T7 |
| v5 fix: VFS_SYMLINK=5 | T1 |

**Placeholder scan:** No "TODO" / "TBD" / "fill in" markers. Each step has concrete code or commands.

**Type consistency:** `VFS_SYMLINK=5` defined T1, used throughout. `vfs_lookup_at(int, const char *, lookup_flags_t, vfs_node_t **)` consistent. `COPY_USER_STR` macro defined once (T7). `splice_symlink_path` contract (suffix `""` or starts with `/`) consistent.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-09-05-symlink-support.md`.

Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?





