# OS01 Symlink Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land POSIX-aligned symlink support (`symlink(2)` / `readlink(2)` / `lstat(2)` / `fstatat(2)`) plus automatic mid-path + last-component symlink following in `vfs_lookup_at`, closing three OS01 roadmap items: P1「exec 软链接跟随」、P5「symlink/readlink」、P5「exec symlink ABI ✅ 缓解」.

**Architecture:** Two new kernel concepts (symlink node type + lookup follow loop) plus two new filesystem ops (`.symlink` / `.readlink` in `vfs_ops_t`) implemented in ext2 (fast inline + long block-pointer storage). Four new syscalls (71..74) + libc POSIX wrappers + four-entry `PF_LINUX_ABI` translation table so busybox's Linux-ABI-built `ln` / `readlink` / `find -type l` all work. Symlink-following folded into a single `vfs_lookup_resolved` helper that all public lookup APIs (including legacy `vfs_lookup` / `vfs_lookup_from`) route through.

**Tech Stack:** OS01 kernel (C, x86_64 freestanding), ext2 driver, libc freestanding (C), POSIX syscalls via `int $0x80`, Linux x86-64 ABI translation via `PF_LINUX_ABI` flag + 320-entry `linux_to_os01[]` table. Build: GNU Make profile-aware. Host tests (`make PROFILE=x86_64-clang test`) cover host-safe pure logic only; kernel behavior is verified by `make PROFILE=x86_64-clang test-kernel-selftest` and `make OS01_SYSTEST=1 test-syscall`.

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
| kernel header | `kernel/include/uapi/stat.h` | `S_IFLNK`, `S_ISLNK`, `DT_LNK`, `AT_FDCWD`, `AT_SYMLINK_NOFOLLOW` for kernel callers | T1 |
| kernel syscall | `kernel/arch/x86_64/trap.c` | syscall dispatch 71..74, `syscall_names[75]`, `linux_to_os01` table, `sys_stat/open/chdir` migration | T7, T8, T9 |
| kernel syscall | `kernel/sched/task.c` | `sys_symlink`, `sys_readlink`, `sys_lstat`, `sys_fstatat`, `sys_exec` migration, `COPY_USER_STR` macro | T7, T8 |
| libc header | `libc/include/sys/syscall.h` | `SYS_symlink=71..74`, `syscall3()`, `syscall4()` | T1 |
| libc header | `libc/include/sys/stat.h` | `AT_FDCWD`, `AT_SYMLINK_NOFOLLOW`, `lstat`/`fstatat` decls | T1 |
| libc impl | `libc/unistd/symlink.c`, `readlink.c` | real impls | T10 |
| libc impl | `libc/sys/stat/lstat.c`, `fstatat.c` | real impls | T10 |
| kernel tests | `kernel/test/symlink_selftest.c` | deterministic VFS/ext2 failure-path tests with in-kernel fakes | T11 |
| OS E2E tests | `user/systest.c` | syscall/libc/ext2 cases and busybox integration | T12 |

---

## Task Sequencing

Bottom-up by dependency, each independently buildable:

- **T1** Foundation (types/ops/syscall numbers/libc helpers) — both kernel and libc
- **T2** VFS lookup infrastructure — `__vfs_lookup_raw`, `vfs_lookup_resolved`, `vfs_lookup_at`, helpers
- **T3** ext2 readdir mapping + i_size read
- **T4** ext2_vfs_readlink
- **T5** ext2_vfs_symlink + type-aware ext2_vfs_unlink
- **T6** VFS directory/stat integration + exported split-parent helper
- **T7** sys_symlink / sys_readlink / sys_lstat / sys_fstatat in trap.c + task.c
- **T8** Migrate sys_stat / sys_open / sys_chdir / sys_exec to vfs_lookup_at
- **T9** PF_LINUX_ABI table fix
- **T10** libc POSIX wrappers
- **T11** kernel symlink selftests (resolver + injected ext2 failures)
- **T12** OS01 systest cases (syscalls + busybox ln/find integration)
- **T13** End-to-end verification + roadmap update

---

## Task 1: Foundation Types, Ops, Headers, Libc Helpers

**Files:**
- Modify: `kernel/include/fs/vfs.h`
- Modify: `kernel/include/uapi/syscall.h`
- Modify: `kernel/include/uapi/stat.h`
- Modify: `libc/include/sys/syscall.h`
- Modify: `libc/include/sys/stat.h`

**Produces:** `VFS_SYMLINK=5`; `lookup_flags_t`; `MAXSYMLINKS=8`; `vfs_ops.symlink/.readlink`; `SYS_symlink/readlink/lstat/fstatat=71..74` in both kernel and libc headers; `syscall3()` and `syscall4()` (r10 ABI) in libc; kernel-and-libc `AT_FDCWD=-100`, `AT_SYMLINK_NOFOLLOW=0x100`, `S_IFLNK`, `S_ISLNK`, and `DT_LNK`; `lstat`/`fstatat` declarations.

- [ ] **Step 1: Add VFS_SYMLINK + lookup flags to vfs.h**

`vfs.h` currently defines `VFS_FILE` through `VFS_BLKDEV` as macros, not as a `vfs_node_type_t` enum. Replace those four macro definitions with:

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

Move the existing `#include <stddef.h>` from below `vfs_node_t` to immediately after `#include <stdint.h>`, because the following new callback uses `size_t` before the current include location. Then add, at the END of the `vfs_ops_t` struct (before closing brace):

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

- [ ] **Step 5: Add kernel UAPI file-type and at-style lookup constants**

In `kernel/include/uapi/stat.h`, append the missing ABI definitions next to the existing file-type and dirent constants:

```c
#define S_IFLNK  0120000
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define DT_LNK   10
#define AT_FDCWD              -100
#define AT_SYMLINK_NOFOLLOW    0x100
```

The kernel uses `AT_*` in `task.c` and `trap.c`; do not define them only in libc. `vfs_stat()` and `vfs_getdents()` use `S_IFLNK` and `DT_LNK`, respectively, so this step must precede T6.

- [ ] **Step 6: Add AT_* + lstat/fstatat decls to `libc/include/sys/stat.h`**

```c
#define AT_FDCWD              -100
#define AT_SYMLINK_NOFOLLOW    0x100
int lstat(const char *path, struct stat *buf);
int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
```

- [ ] **Step 7: Build to verify no regressions**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang user
make PROFILE=x86_64-clang test
```

Expected: clean build. Tests still pass (scaffolding only).

- [ ] **Step 8: Commit**

```bash
git add kernel/include/fs/vfs.h \
        kernel/include/uapi/syscall.h \
        kernel/include/uapi/stat.h \
        libc/include/sys/syscall.h \
        libc/include/sys/stat.h
git commit -m "feat(symlink): add types, ops slots, syscall numbers, libc helpers

Foundation for symlink support — no behavioral change yet.

- vfs.h: VFS_SYMLINK=5 (append-only, preserve FILE=1..BLKDEV=4);
  lookup_flags_t {FOLLOW, NOFOLLOW}; MAXSYMLINKS=8;
  vfs_ops.symlink(parent, name, target), vfs_ops.readlink(node, buf, size).
- uapi/syscall.h: SYS_symlink=71, SYS_readlink=72, SYS_lstat=73,
  SYS_fstatat=74 (continue 0..70 numbering).
- uapi/stat.h: S_IFLNK/S_ISLNK/DT_LNK plus AT_FDCWD and
  AT_SYMLINK_NOFOLLOW for kernel callers.
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

**Produces:** the 4 static helpers + public `vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags, vfs_node_t **out_node)` that returns 0/-errno with refcount++ on success; migrated pointer-compatible `vfs_lookup` and `vfs_lookup_from` wrappers that both delegate to `vfs_lookup_resolved`.

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

- [ ] **Step 4: Migrate the two legacy pointer wrappers in the same change**

The old wrappers cannot call the new raw function: its signature and return type have changed. Replace their bodies in this task, before compiling:

```c
vfs_node_t *vfs_lookup(const char *path)
{
    vfs_node_t *node = NULL;
    if (vfs_lookup_at(AT_FDCWD, path, LOOKUP_FOLLOW, &node) < 0)
        return NULL;
    return node;
}
```

For `vfs_lookup_from(path, cwd)`, retain its existing absolute-path and `cwd == NULL` rejection behavior, construct its bounded absolute path exactly as the current wrapper does, then call `vfs_lookup_resolved(abs_path, LOOKUP_FOLLOW, &node)` and return `NULL` on a negative result. Do not route this wrapper through `vfs_lookup_at`: its explicit `cwd` parameter is part of the legacy API contract.

- [ ] **Step 5: Build kernel**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang kernel.bin
```

Expected: compiles. No public API calls `__vfs_lookup_raw` directly; it is used only by `vfs_lookup_resolved`.

- [ ] **Step 6: Run host tests**

```bash
make PROFILE=x86_64-clang test
```

Expected: all pre-existing host suites green.

- [ ] **Step 7: Commit**

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

Both legacy pointer APIs now route through vfs_lookup_resolved in this
commit, so the raw helper has no signature-incompatible public callers.

Spec: docs/superpowers/specs/2026-09-05-symlink-support-design.md v5
§3.2, §3.3.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 3: ext2 readdir — Map EXT2_FT_SYMLINK and Read i_size

**Files:** Modify: `kernel/fs/ext2.c` (`ext2_vfs_readdir`); `kernel/include/fs/ext2.h` (missing ext2 constants).

**Produces:** `vfs_readdir()` on ext2 directories returns entries with `entry.type = VFS_SYMLINK` and `entry.size = i_size` for symlinks.

- [ ] **Step 1: Define the missing ext2 constants, then locate `ext2_vfs_readdir`**

`kernel/include/fs/ext2.h` currently defines only `EXT2_S_IFREG` and `EXT2_S_IFDIR`; it has no `EXT2_FT_*` constants. Add these exact definitions before using them:

```c
#define EXT2_S_IFMT    0xF000
#define EXT2_S_IFLNK   0xA000
#define EXT2_FT_REG_FILE  1
#define EXT2_FT_DIR       2
#define EXT2_FT_CHRDEV    3
#define EXT2_FT_BLKDEV    4
#define EXT2_FT_FIFO      5
#define EXT2_FT_SOCK      6
#define EXT2_FT_SYMLINK   7
```

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
git add kernel/fs/ext2.c kernel/include/fs/ext2.h
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

## Task 6: VFS Directory/Stat Integration

**Files:** Modify: `kernel/fs/vfs.c`, `kernel/include/fs/vfs.h`

**Produces:** `vfs_split_parent` exported; `vfs_getdents` outputs `DT_LNK` for symlinks; `vfs_stat` returns `S_IFLNK | 0777`. The public lookup wrappers were migrated atomically in T2.

- [ ] **Step 1: Export `vfs_split_parent`**

Remove `static` keyword from `vfs_split_parent` (vfs.c:590). Add prototype to `kernel/include/fs/vfs.h`:

```c
const char *vfs_split_parent(const char *path, const char *cwd,
                             char parent_path[VFS_NAME_MAX]);
```

- [ ] **Step 2: Add `VFS_SYMLINK → DT_LNK` in `vfs_getdents`**

Find `switch (e->type)` in `vfs_getdents` (~line 531). Add:

```c
case VFS_SYMLINK: d->d_type = DT_LNK; break;
```

- [ ] **Step 3: Add `S_IFLNK` case in `vfs_stat`**

Find `switch (node->type)` in `vfs_stat` (~line 382). Add:

```c
case VFS_SYMLINK: buf->st_mode = S_IFLNK | 0777; break;
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
git add kernel/fs/vfs.c kernel/include/fs/vfs.h
git commit -m "feat(vfs): export split-parent + DT_LNK + S_IFLNK

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
kfree(path_copy);  /* preserve sys_stat's existing ownership boundary */
if (lookup_rc < 0) {
    regs->rax = lookup_rc;
    break;  /* path_copy was freed immediately after lookup above */
}
```

Rest of function (kstat; copy_to_user_ft; vfs_node_put) unchanged.

- [ ] **Step 2: Migrate `sys_open` in trap.c**

Preserve the existing `O_CREAT` control flow. Perform `vfs_lookup_at` first, but only return an error when it is not the createable miss:

```c
vfs_node_t *node = NULL;
int lookup_rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node);
if (lookup_rc < 0 && !(lookup_rc == -ENOENT && (flags & O_CREAT)))
{
    regs->rax = lookup_rc;
    goto out_open;  /* releases path_copy and any owned node/parent ref */
}
if (lookup_rc == -ENOENT) {
    /* enter the current O_CREAT branch, which declares and fills parent_path/name */
} else {
    /* node is the successful lookup result; continue at the current O_TRUNC/devfs/fd path */
}
```

Do not use the `sys_stat` early-return pattern here: it would make every `open(path, O_CREAT, ...)` fail with `ENOENT`.

Inside the existing `if (lookup_rc == -ENOENT)` create branch, retain the current `parent_path`/`name` parsing first. Replace only its current `parent = vfs_lookup_from(parent_path, current->files->cwd);` statement in place with:

```c
int parent_rc = vfs_lookup_at(AT_FDCWD, parent_path, LOOKUP_FOLLOW, &parent);
if (parent_rc < 0) {
    regs->rax = parent_rc;
    goto out_open;
}
```

This placement guarantees `parent_path` is initialized and preserves the existing cleanup label.

- [ ] **Step 3: Migrate `sys_chdir` in trap.c**

On a negative lookup result, assign `regs->rax = lookup_rc; goto out;`, using the existing `SYS_chdir` cleanup label so its allocated `path_copy` and `new_cwd` are released. Then retain the existing directory-type check, `current->files->cwd` update, and `vfs_node_put` ownership flow. Do not use `return lookup_rc` inside `do_system_call`.

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
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall
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

**Files:** Modify: `libc/unistd/symlink.c`, `libc/unistd/readlink.c`, `libc/include/unistd.h`, `libc/Makefile`. Create: `libc/sys/stat/lstat.c`, `libc/sys/stat/fstatat.c`.

**Produces:** Real libc implementations; 6 previously-blocked busybox callers (libarchive, copy_file, devfsd, mdev, etc.) work.

- [ ] **Step 1: Include the new stat wrapper directory in libc.a**

`libc/Makefile` does not currently search `sys/stat`, so add `$(wildcard sys/stat/*.c)` to `C_SOURCES` beside its other source-directory wildcards. Without this, `lstat.c` and `fstatat.c` compile nowhere and will be unresolved for user programs.

- [ ] **Step 2: Correct the public readlink declaration**

In `libc/include/unistd.h`, replace the existing `int readlink(const char *path, char *buf, size_t bufsize);` declaration with `ssize_t readlink(const char *path, char *buf, size_t bufsize);`. Ensure the header already exposes `ssize_t` (or include the header that defines it) before this declaration. This must precede compiling the `ssize_t` implementation.

- [ ] **Step 3: Verify libc build paths**

```bash
ls /home/aagu/OS01/libc/unistd/
ls /home/aagu/OS01/libc/sys/stat/ 2>/dev/null || mkdir -p /home/aagu/OS01/libc/sys/stat
```

Confirm the two existing files are stubs before replacing them. The `sys/stat` Makefile wildcard was added in Step 1 and is mandatory for these new sources.

- [ ] **Step 4: Rewrite `libc/unistd/symlink.c`**

Replace the file with the standard OS01 libc pattern: include `unistd.h`, `<sys/syscall.h>`, `stdint.h`, `errno.h`; `int symlink(const char *target, const char *linkpath)` calls `syscall3(SYS_symlink, (uint64_t)target, (uint64_t)linkpath, 0)`, on negative return sets `errno = (int)(-ret)` and returns `-1`, else returns `0`.

- [ ] **Step 5: Rewrite `libc/unistd/readlink.c`**

Replace with: same headers; `ssize_t readlink(const char *path, char *buf, size_t bufsize)` calls `syscall3(SYS_readlink, (uint64_t)path, (uint64_t)buf, (uint64_t)bufsize)`, on negative return sets `errno` and returns `-1`, else returns `(ssize_t)ret`.

- [ ] **Step 6: Create `libc/sys/stat/lstat.c`**

New file: include `<sys/stat.h>`, `<sys/syscall.h>`, `stdint.h`, `errno.h`; `int lstat(const char *path, struct stat *buf)` calls `syscall3(SYS_lstat, (uint64_t)path, (uint64_t)buf, 0)`, on negative return sets `errno` and returns `-1`, else returns `0`.

- [ ] **Step 7: Create `libc/sys/stat/fstatat.c`**

New file: same headers; `int fstatat(int dirfd, const char *path, struct stat *buf, int flags)` calls `syscall4(SYS_fstatat, dirfd, (uint64_t)path, (uint64_t)buf, (uint64_t)flags)`, on negative return sets `errno` and returns `-1`, else returns `0`.

- [ ] **Step 8: Build libc + rootfs**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang user
make PROFILE=x86_64-clang disk.img
```

Expected: builds clean.

- [ ] **Step 9: Run host tests**

```bash
make PROFILE=x86_64-clang test
```

Expected: all pre-existing host suites green.

- [ ] **Step 10: Commit**

```bash
git add libc/Makefile libc/include/unistd.h \
        libc/unistd/symlink.c libc/unistd/readlink.c \
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

## Task 11: Kernel Symlink Selftests — Resolver and Failure Paths

**Files:** Create: `kernel/test/symlink_selftest.c`. Modify: `kernel/test/selftest.c`, `kernel/include/fs/ext2.h`, `kernel/fs/ext2.c`.

**Produces:** Actual kernel tests for spec cases 09, 10, 30, 32, 33, and 37–40. This deliberately does not add a host binary: host-native linking would invoke the host's Linux syscalls rather than OS01's VFS/ext2 code.

- [ ] **Step 1: Add a selftest source compiled by the existing wildcard**

`kernel/Makefile` already includes `$(wildcard test/*.c)`, so create `kernel/test/symlink_selftest.c` and include `<kernel/selftest.h>`, `<fs/vfs.h>`, `<fs/ext2.h>`, and `<errno.h>`. Do **not** use `SELFTEST(name)`: the current runner does not scan `.selftest_table`. Export ordinary `int symlink_selftest_resolver(void);` and `int symlink_selftest_ext2_rollback(void);`. In `kernel/test/selftest.c`, add their forward declarations with the other test declarations and explicitly add `selftest_register("symlink_resolver", symlink_selftest_resolver);` and `selftest_register("symlink_ext2_rollback", symlink_selftest_ext2_rollback);` in `selftest_run_all`.

- [ ] **Step 2: Test the real resolver using a deterministic in-memory VFS**

Implement a private, fixed tree with `readdir` and `readlink` ops, create a directory root node, and mount it once at `/symlink-selftest` through the production `vfs_mount` API. Then call the production public interface `vfs_lookup_at` against that mount. Cover case 32 by supplying a `VFS_SYMLINK` node with `.readlink = NULL`, case 36 with a file followed by a component, and case 37 by making the fake `readdir` return a non-zero value. Assert the exact results `-EOPNOTSUPP`, `-ENOTDIR`, and `-EIO`; a passing test must never be an empty no-op.

- [ ] **Step 3: Add explicit ext2 fault injection hooks under `KERNEL_SELFTEST`**

Declare in `ext2.h` the exact enum `ext2_symlink_test_fault { EXT2_SYMLINK_TEST_NONE, EXT2_SYMLINK_TEST_FIND_DIRENT, EXT2_SYMLINK_TEST_READ_INODE, EXT2_SYMLINK_TEST_WRITE_BLOCK, EXT2_SYMLINK_TEST_WRITE_INODE, EXT2_SYMLINK_TEST_DIRENT_ADD };` and `void ext2_symlink_test_set_fault(enum ext2_symlink_test_fault fault);` unconditionally, because `kernel/test/symlink_selftest.c` is compiled in normal kernel builds too. In `ext2.c`, make the setter and selected failure branches functional only under `KERNEL_SELFTEST`; in ordinary builds the setter is a no-op and no I/O path branches. In selftest builds each selected operation returns `-EIO` before mutation. The selftest calls `root->ops->symlink(root, name, target)` and `root->ops->unlink(root, name)`, snapshots the free-inode/free-block counts and relevant bitmap bit before and after, then resets the selector to NONE. It verifies cases 09/10 and 30/33/38/39/40: a failed operation returns `-EIO`, allocates no surviving dirent, and restores every allocation; successful fast unlink changes no block count while successful long unlink releases exactly one target block.

- [ ] **Step 4: Run the real kernel selftest image**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test-kernel-selftest
```

Expected: QEMU boots with `KERNEL_SELFTEST=1` and reports every new symlink selftest as PASS. Do not substitute `make validate`: it validates artifacts and does not execute tests.

- [ ] **Step 5: Commit**

```bash
git add kernel/test/symlink_selftest.c kernel/test/selftest.c kernel/fs/ext2.c kernel/include/fs/ext2.h
git commit -m "test(kernel): cover symlink resolver and ext2 rollback paths"
```

---

## Task 12: OS01 Systest Cases (syscalls + busybox ln/find)

**Files:** Modify: `user/systest.c`

**Produces:** 2 systest cases (41, 42) that run inside a real OS01 boot via QEMU, exercising busybox's `ln -s` + exec via symlink, and `find -type l`. Use `/symlink-systest` (NOT `/tmp`) since tmpfs intentionally lacks symlink op.

- [ ] **Step 1: Use the actual systest runner**

The runner is `user/systest.c`: each `static void test_*` reports through the existing `CHECK`/`CHECK3` macros and is registered in its bottom-of-file `tests[]` table. Add new `static void` functions there; do not create `test/cases/test_systest.c`, and do not use placeholder helpers such as `run_cmd` or `run_cmd_capture`.

- [ ] **Step 1a: Add a deterministic fixture setup helper**

Add `static void symlink_fixture_reset(void)` that calls `mkdir("/symlink-systest", 0777)` and accepts `errno == EEXIST`; any other failure is reported through `CHECK3`. Before every case, call this helper and unlink the case's known names (`x`, `lnk`, and that case's numbered paths). This is required because `/symlink-systest` is absent in a freshly built image.

- [ ] **Step 2: Add systest case 41 — busybox ln + exec**

Call `symlink_fixture_reset()`. `test_41_busybox_ln_exec()` forks and execs `/bin/busybox` with the concrete array `char *ln_argv[] = { "ln", "-s", "/bin/busybox", "/symlink-systest/x", NULL };`, then waits for status 0. It then forks again; the second child uses `char *exec_argv[] = { "x", "--help", NULL };`, calls `exec("/symlink-systest/x", exec_argv, NULL)`, and calls `_exit(127)` only if `exec` returns. The parent calls `waitpid` and asserts `WIFEXITED(status) && WEXITSTATUS(status) == 0`; `_exit(127)` specifically identifies an unexpected exec return. This tests both PF_LINUX_ABI `ln -s` and successful non-returning exec correctly; cleanup remains in the parent.

- [ ] **Step 3: Add systest case 42 — find -type l**

Call `symlink_fixture_reset()`. `test_42_find_type_l()` creates `/symlink-systest/lnk` through the libc `symlink` wrapper, forks a child, redirects its stdout to a pipe, and execs `/bin/busybox` with `char *find_argv[] = { "find", "/symlink-systest", "-type", "l", NULL };`. The parent reads the pipe, waits for normal exit status 0, and asserts the output contains `/symlink-systest/lnk`. It then unlinks the link; leave the shared fixture directory in place so later cases are order-independent.

- [ ] **Step 4: Add direct syscall/libc coverage before registering integration cases**

In the same `user/systest.c`, add focused functions for the externally observable matrix: fast/long creation and readback; EEXIST, ENOENT, ENOTDIR, ENAMETOOLONG, EOPNOTSUPP (`/tmp`), EINVAL, root-linkpath EEXIST; `stat`/`lstat`/`fstatat` follow semantics and link target size; absolute and relative mid-path following including NOFOLLOW; loop/depth limits; and absolute path ignoring invalid dirfd. Each test creates a unique path below `/symlink-systest`, uses the real libc/syscall interface, checks exact `errno`, and cleans up in its parent process.

- [ ] **Step 5: Register cases**

Add to the systest runner's case list:

```c
    { "41_busybox_ln_exec", test_41_busybox_ln_exec },
    { "42_find_type_l",     test_42_find_type_l },
```

- [ ] **Step 6: Build + run systest**

```bash
cd /home/aagu/OS01
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall
```

Expected: all systest cases pass, including 41, 42.

- [ ] **Step 7: Commit**

```bash
git add user/systest.c
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

Expected: existing host suites green. No symlink kernel behavior is credited to this host-native suite.

- [ ] **Step 3: Kernel validate**

```bash
make PROFILE=x86_64-clang validate
```

Expected: kernel.bin and EFI artifacts validate.

- [ ] **Step 4: Kernel symlink selftests**

```bash
make PROFILE=x86_64-clang test-kernel-selftest
```

Expected: all symlink resolver and ext2 rollback selftests pass. Run separately from systest because this target deliberately rejects `OS01_SYSTEST=1`.

- [ ] **Step 5: Full systest**

```bash
make OS01_SYSTEST=1 PROFILE=x86_64-clang test-syscall
```

Expected: all systest cases pass including 41, 42.

- [ ] **Step 6: Aarch64 regression probe**

```bash
make PROFILE=aarch64-clang kernel.elf BOOTAA64.EFI
```

Expected: aarch64 build succeeds (untouched by this work).

- [ ] **Step 7: Update roadmap**

Open `docs/roadmap.md`. Mark three items:
- **P1**「exec 软链接跟随」→ done
- **P5**「symlink/readlink」→ done
- **P5**「exec symlink ABI done mitigation」→ upgraded to fully solved (`b32e1e0` busybox-copies build-time workaround reverted 2026-09-06; rootfs applet entries are debugfs symlinks again; busybox `ln`/`find -type l` validated end-to-end by systest cases 41/42)

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
| §6 cases 09/10/30/32/33/36–40 in kernel; all user-visible cases plus 41/42 in systest | T11, T12 |
| v5 fix: tmpfs E2E in /symlink-systest | T12 |
| v5 fix: ext2 readdir i_size | T3 |
| v5 fix: ENOTDIR/EIO in __vfs_lookup_raw | T2 |
| v5 fix: suffix contract | T2 |
| v5 fix: ext2 symlink I/O atomicity | T5 |
| v5 fix: public lookup migration closure | T2 |
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
