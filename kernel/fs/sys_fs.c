#include <fs/vfs.h>
#include <memory/uaccess.h>
#include <sched/task.h>
#include <errno.h>
#include <string.h>
#include <arch/regs.h>

// ── COPY_USER_STR macro (symlink support) ───────────────────
// Returns the string length (>=0) on success.
// Returns -EFAULT on bad user pointer (strnlen_user < 0).
// Returns -ENAMETOOLONG when strnlen_user returns `max` (no NUL within max).
// strnlen_user semantics (kernel/memory/uaccess.c:91-105): returns [0, max]
// where _l==max means the string had no NUL within `max` bytes (over-long),
// or -EFAULT on fault.
#define COPY_USER_STR(kbuf, uptr, max) ({                          \
    long _l = strnlen_user((uptr), (max));                         \
    if (_l < 0) return -EFAULT;                                   \
    if (_l >= (long)(max)) return -ENAMETOOLONG;                  \
    if (copy_from_user_ft((kbuf), (uptr), (size_t)_l + 1) < 0)    \
        return -EFAULT;                                           \
    _l; })

// ── SYS_symlink(71): symlink(target, linkpath) ──────────────
// Creates a symbolic link at linkpath pointing to target.
// Spec §5.3, task-7 brief. Key invariants:
//   - target or linkpath null/empty -> -ENOENT / -EINVAL
//   - parent->type != VFS_DIR -> -ENOTDIR
//   - !parent->ops || !parent->ops->symlink -> -EOPNOTSUPP (v2 fix)
int64_t sys_symlink(const char *target, const char *linkpath, pt_regs_t *regs)
{
    (void)regs;

    char target_copy[VFS_NAME_MAX];
    long tlen = COPY_USER_STR(target_copy, target, VFS_NAME_MAX);
    if (tlen == 0)
        return -ENOENT;                       // reject empty target

    char linkpath_copy[VFS_NAME_MAX];
    COPY_USER_STR(linkpath_copy, linkpath, VFS_NAME_MAX);

    // v5 fix: linkpath is exactly "/" → would mean replacing the mount root
    // with a symlink.  EEXIST is more informative than EINVAL here.
    if (linkpath_copy[0] == '/' && linkpath_copy[1] == '\0')
        return -EEXIST;

    const char *cwd = current->files ? current->files->cwd : "/";
    char parent_path[VFS_NAME_MAX];
    const char *name = vfs_split_parent(linkpath_copy, cwd, parent_path);
    if (!name || *name == '\0')
        return -EINVAL;                       // reject empty basename

    vfs_node_t *parent = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, parent_path, LOOKUP_FOLLOW, &parent);
    if (rc < 0)
        return rc;
    if (parent->type != VFS_DIR) {
        vfs_node_put(parent);
        return -ENOTDIR;
    }
    // v2 fix: refuse NULL ops deref on FS without .symlink (devfs, tmpfs)
    if (!parent->ops || !parent->ops->symlink) {
        vfs_node_put(parent);
        return -EOPNOTSUPP;
    }

    rc = parent->ops->symlink(parent, name, target_copy);
    vfs_node_put(parent);
    return rc;
}

// ── SYS_readlink(72): readlink(path, buf, bufsize) ──────────
// Read the contents of a symlink (the target string) into user buf.
// Spec §5.3, task-7 brief. Key invariants:
//   - !buf || bufsize == 0 -> -EINVAL
//   - LOOKUP_NOFOLLOW: readlink MUST NOT follow any symlink (neither
//     intermediate nor final).
//   - node->type != VFS_SYMLINK -> -EINVAL
//   - !node->ops || !node->ops->readlink -> -EOPNOTSUPP
//   - Truncate output to caller's bufsize; copy via copy_to_user_ft.
int64_t sys_readlink(const char *path, char *buf, size_t bufsize,
                     pt_regs_t *regs)
{
    (void)regs;
    if (!buf || bufsize == 0)
        return -EINVAL;

    char path_copy[VFS_NAME_MAX];
    COPY_USER_STR(path_copy, path, VFS_NAME_MAX);

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW, &node);
    if (rc < 0)
        return rc;
    if (node->type != VFS_SYMLINK) {
        vfs_node_put(node);
        return -EINVAL;
    }
    if (!node->ops || !node->ops->readlink) {
        vfs_node_put(node);
        return -EOPNOTSUPP;
    }

    char kbuf[VFS_NAME_MAX];
    int tlen = node->ops->readlink(node, kbuf, VFS_NAME_MAX - 1);
    vfs_node_put(node);
    if (tlen < 0)
        return tlen;

    if ((size_t)tlen > bufsize)
        tlen = (int)bufsize;
    {
        ssize_t user_copy_rc = copy_to_user_ft(buf, kbuf, (size_t)tlen);
        if (user_copy_rc < 0) return user_copy_rc;
    }
    return tlen;
}

// ── SYS_lstat(73): lstat(path, buf) — NOFOLLOW ──────────────
// Stat a path WITHOUT following a trailing symlink (i.e. return the
// symlink's own stat, S_IFLNK).  Intermediate symlinks are still followed
// (POSIX AT_SYMLINK_NOFOLLOW semantics).
// Spec §5.3, task-7 brief. Key invariants:
//   - !buf -> -EFAULT
//   - LOOKUP_NOFOLLOW
//   - v2 fix: build kernel-local kstat, then copy_to_user_ft — never
//     write user buf directly (a memset into a bad user pointer would
//     fault mid-handler).
int64_t sys_lstat(const char *path, struct stat *buf, pt_regs_t *regs)
{
    (void)regs;
    if (!buf)
        return -EFAULT;

    char path_copy[VFS_NAME_MAX];
    COPY_USER_STR(path_copy, path, VFS_NAME_MAX);

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW, &node);
    if (rc < 0)
        return rc;

    struct stat kstat;
    rc = vfs_stat(node, &kstat);
    vfs_node_put(node);
    if (rc < 0)
        return rc;

    {
        ssize_t user_copy_rc = copy_to_user_ft(buf, &kstat, sizeof(kstat));
        if (user_copy_rc < 0) return user_copy_rc;
    }
    return 0;
}

// ── SYS_fstatat(74): fstatat(dirfd, path, buf, flags) ──────
// POSIX fstatat: stat a path relative to a directory fd.  In OS01,
// only AT_FDCWD is supported (no real openat yet); other dirfd values
// produce -EBADF from vfs_lookup_at.  Flags are strictly validated:
//   - AT_SYMLINK_NOFOLLOW → LOOKUP_NOFOLLOW; else LOOKUP_FOLLOW.
//   - any unknown flag bit → -EINVAL (v3 fix; silent ignoring breaks
//     caller expectations).
// Spec §5.3, task-7 brief.
#define FSTATAT_SUPPORTED_FLAGS (AT_SYMLINK_NOFOLLOW)

int64_t sys_fstatat(int dirfd, const char *path, struct stat *buf,
                    int flags, pt_regs_t *regs)
{
    (void)regs;
    if (!buf)
        return -EFAULT;
    if (flags & ~FSTATAT_SUPPORTED_FLAGS)
        return -EINVAL;                       // v3: reject unknown flags

    char path_copy[VFS_NAME_MAX];
    COPY_USER_STR(path_copy, path, VFS_NAME_MAX);

    lookup_flags_t lflags =
        (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(dirfd, path_copy, lflags, &node);
    if (rc < 0)
        return rc;

    struct stat kstat;
    rc = vfs_stat(node, &kstat);
    vfs_node_put(node);
    if (rc < 0)
        return rc;

    {
        ssize_t user_copy_rc = copy_to_user_ft(buf, &kstat, sizeof(kstat));
        if (user_copy_rc < 0) return user_copy_rc;
    }
    return 0;
}
