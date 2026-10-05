#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#include <sched/task.h>
#include <memory/slab.h>
#include <memory/uaccess.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <fs/file.h>
#include <fs/poll.h>
#include <fs/select.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <log/log.h>

// ── COPY_USER_STR macro (Task 7, symlink support) ───────────
// Returns the string length (>=0) on success.
// Returns -EFAULT on bad user pointer (strnlen_user < 0).
// Returns -ENAMETOOLONG when strnlen_user returns `max` (no NUL within max).
// strnlen_user semantics (kernel/memory/uaccess.c:91-105): returns [0, max]
// where _l==max means the string had no NUL within `max` bytes (over-long),
// or -EFAULT on fault.
static inline int64_t copy_user_path(char *kbuf, const char *uptr, size_t max)
{
    long l = strnlen_user(uptr, max);
    if (l < 0) return -EFAULT;
    if (l >= (long)max) return -ENAMETOOLONG;
    if (copy_from_user_ft(kbuf, uptr, (size_t)l + 1) < 0)
        return -EFAULT;
    return l;
}

// ── SYS_symlink(71): symlink(target, linkpath) ──────────────
// Create a symbolic link at linkpath whose target is the string `target`.
// Spec §5.3, task-7 brief. Key invariants:
//   - Empty target -> -ENOENT (v3 fix)
//   - linkpath == "/" -> -EEXIST (v5 fix, before vfs_split_parent):
//     refuses to overwrite the filesystem root with a symlink.
//   - Reuse vfs_split_parent for relative linkpaths (v3 fix) — handles
//     "/dir/file" → parent="/dir" name="file", "/file" → parent="/"
//     name="file", and "file" (no slash) → parent=cwd name="file".
//   - parent->type != VFS_DIR -> -ENOTDIR
//   - !parent->ops || !parent->ops->symlink -> -EOPNOTSUPP (v2 fix)
static __attribute__((noinline)) int64_t sys_symlink(const char *target, const char *linkpath)
{
    char *paths = kmalloc(3 * VFS_NAME_MAX);
    if (!paths) return -ENOMEM;
    char *target_copy = paths;
    char *linkpath_copy = paths + VFS_NAME_MAX;
    char *parent_path = paths + 2 * VFS_NAME_MAX;

    int64_t ret = 0;
    long tlen = copy_user_path(target_copy, target, VFS_NAME_MAX);
    if (tlen < 0) { ret = tlen; goto out; }
    if (tlen == 0) { ret = -ENOENT; goto out; }

    long llen = copy_user_path(linkpath_copy, linkpath, VFS_NAME_MAX);
    if (llen < 0) { ret = llen; goto out; }

    // v5 fix: linkpath is exactly "/" → would mean replacing the mount root
    // with a symlink.  EEXIST is more informative than EINVAL here.
    if (linkpath_copy[0] == '/' && linkpath_copy[1] == '\0') {
        ret = -EEXIST;
        goto out;
    }

    const char *cwd = current->files ? current->files->cwd : "/";
    const char *name = vfs_split_parent(linkpath_copy, cwd, parent_path);
    if (!name || *name == '\0') {
        ret = -EINVAL;
        goto out;
    }

    vfs_node_t *parent = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, parent_path, LOOKUP_FOLLOW, &parent);
    if (rc < 0) { ret = rc; goto out; }
    if (parent->type != VFS_DIR) {
        vfs_node_put(parent);
        ret = -ENOTDIR;
        goto out;
    }
    // v2 fix: refuse NULL ops deref on FS without .symlink (devfs, tmpfs)
    if (!parent->ops || !parent->ops->symlink) {
        vfs_node_put(parent);
        ret = -EOPNOTSUPP;
        goto out;
    }

    ret = parent->ops->symlink(parent, name, target_copy);
    vfs_node_put(parent);

out:
    kfree(paths);
    return ret;
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
static __attribute__((noinline)) int64_t sys_readlink(const char *path, char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0)
        return -EINVAL;

    char *paths = kmalloc(2 * VFS_NAME_MAX);
    if (!paths) return -ENOMEM;
    char *path_copy = paths;
    char *kbuf = paths + VFS_NAME_MAX;

    int64_t ret = 0;
    long plen = copy_user_path(path_copy, path, VFS_NAME_MAX);
    if (plen < 0) { ret = plen; goto out; }

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW, &node);
    if (rc < 0) { ret = rc; goto out; }
    if (node->type != VFS_SYMLINK) {
        vfs_node_put(node);
        ret = -EINVAL;
        goto out;
    }
    if (!node->ops || !node->ops->readlink) {
        vfs_node_put(node);
        ret = -EOPNOTSUPP;
        goto out;
    }

    int tlen = node->ops->readlink(node, kbuf, VFS_NAME_MAX - 1);
    vfs_node_put(node);
    if (tlen < 0) { ret = tlen; goto out; }

    if ((size_t)tlen > bufsize)
        tlen = (int)bufsize;
    {
        ssize_t user_copy_rc = copy_to_user_ft(buf, kbuf, (size_t)tlen);
        if (user_copy_rc < 0) { ret = user_copy_rc; goto out; }
    }
    ret = tlen;

out:
    kfree(paths);
    return ret;
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
static __attribute__((noinline)) int64_t sys_lstat(const char *path, struct stat *buf)
{
    if (!buf)
        return -EFAULT;

    char *path_copy = kmalloc(VFS_NAME_MAX);
    if (!path_copy)
        return -ENOMEM;

    int64_t ret = 0;
    long plen = copy_user_path(path_copy, path, VFS_NAME_MAX);
    if (plen < 0) { ret = plen; goto out; }

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_NOFOLLOW, &node);
    if (rc < 0) { ret = rc; goto out; }

    struct stat kstat;
    rc = vfs_stat(node, &kstat);
    vfs_node_put(node);
    if (rc < 0) { ret = rc; goto out; }

    {
        ssize_t user_copy_rc = copy_to_user_ft(buf, &kstat, sizeof(kstat));
        if (user_copy_rc < 0) { ret = user_copy_rc; goto out; }
    }
    ret = 0;

out:
    kfree(path_copy);
    return ret;
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

static __attribute__((noinline)) int64_t sys_fstatat(int dirfd, const char *path, struct stat *buf,
                    int flags)
{
    if (!buf)
        return -EFAULT;
    if (flags & ~FSTATAT_SUPPORTED_FLAGS)
        return -EINVAL;                       // v3: reject unknown flags

    char *path_copy = kmalloc(VFS_NAME_MAX);
    if (!path_copy)
        return -ENOMEM;

    int64_t ret = 0;
    long plen = copy_user_path(path_copy, path, VFS_NAME_MAX);
    if (plen < 0) { ret = plen; goto out; }

    lookup_flags_t lflags =
        (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : LOOKUP_FOLLOW;

    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(dirfd, path_copy, lflags, &node);
    if (rc < 0) { ret = rc; goto out; }

    struct stat kstat;
    rc = vfs_stat(node, &kstat);
    vfs_node_put(node);
    if (rc < 0) { ret = rc; goto out; }

    {
        ssize_t user_copy_rc = copy_to_user_ft(buf, &kstat, sizeof(kstat));
        if (user_copy_rc < 0) { ret = user_copy_rc; goto out; }
    }
    ret = 0;

out:
    kfree(path_copy);
    return ret;
}

int64_t sys_fs_dispatch(syscall_ctx_t *ctx)
{
    int64_t syscall_result = -EINVAL;
    switch (ctx->nr) {
    case SYS_write: {
        // write(int fd, const void *buf, size_t len) — fd-based
        int fd = (int)ctx->args[0];
        const void *buf = (const void *)ctx->args[1];
        uint64_t size = ctx->args[2];

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }
        if ((uint64_t)buf >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        file_t *f = current->files->fd[fd];
        syscall_result = fd_write(f, buf, size);
        break;
    }
    case SYS_read: {
        // read(int fd, void *buf, size_t len) — fd-based
        int fd = (int)ctx->args[0];
        void *buf = (void *)ctx->args[1];
        uint64_t size = ctx->args[2];

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }
        if ((uint64_t)buf >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        file_t *f = current->files->fd[fd];
        syscall_result = fd_read(f, buf, size);
        break;
    }
    case SYS_open: {
        // open(const char *path, int flags) → fd
        const char *path = (const char *)ctx->args[0];
        int flags = (int)ctx->args[1];
        char *path_copy = NULL;
        char *parent_path = NULL;
        vfs_node_t *parent = NULL;
        file_t *f = NULL;
        vfs_node_t *node = NULL;

        if ((uint64_t)path >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }
        if (!current->files) {
            syscall_result = -ENFILE;
            break;
        }

        // Bounded fault-tolerant copy of the user path.
        // strnlen_user faults on bad addresses (returns -EFAULT) and
        // bounds the length at VFS_NAME_MAX-1 (returns >= VFS_NAME_MAX
        // -> ENAMETOOLONG).  copy_from_user_ft then materializes the
        // kernel-side buffer with a single fault-tolerant pass.
        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            syscall_result = -EFAULT;
            break;
        }

        int lookup_rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node);
        if (lookup_rc < 0 && !(lookup_rc == -ENOENT && (flags & O_CREAT))) {
            syscall_result = lookup_rc;
            goto out_open;  /* releases path_copy and any owned node/parent ref */
        }

        // O_CREAT: create file if it doesn't exist
        if (lookup_rc == -ENOENT && (flags & O_CREAT)) {
            // Find parent directory — parse path_copy to extract parent
            parent_path = kmalloc(VFS_NAME_MAX);
            if (!parent_path) { syscall_result = -ENOMEM; goto out_open; }
            const char *name = NULL;

            // path_copy is the kernel-side copy; plen is its strlen.
            char *last_slash = NULL;
            for (char *s = path_copy; *s; s++)
                if (*s == '/') last_slash = s;

            if (last_slash && last_slash != path_copy) {
                // e.g., "/dir/file" — parent is "/dir", name is "file"
                *last_slash = '\0';
                name = last_slash + 1;
                strcpy(parent_path, path_copy);
            } else if (last_slash == path_copy && plen > 1) {
                // e.g., "/file" — parent is "/", name is "file"
                parent_path[0] = '/'; parent_path[1] = '\0';
                name = path_copy + 1;
            } else {
                // No slash — relative path, parent is cwd
                name = path_copy;
                // Use cwd as parent path
                size_t cwd_len = strlen(current->files->cwd);
                if (cwd_len >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; goto out_open; }
                memcpy(parent_path, current->files->cwd, cwd_len + 1);
            }

            if (!name || *name == '\0') { syscall_result = -EINVAL; goto out_open; }

            int parent_rc = vfs_lookup_at(AT_FDCWD, parent_path, LOOKUP_FOLLOW, &parent);
            if (parent_rc < 0) { syscall_result = parent_rc; goto out_open; }
            if (parent->type != VFS_DIR) { syscall_result = -ENOTDIR; goto out_open; }
            if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->create) {
                syscall_result = -EROFS;
                goto out_open;
            }
            if ((uint64_t)parent->ops->create < 0xffff800000000000ULL) {
                syscall_result = -1;
                goto out_open;
            }

            node = parent->ops->create(parent, name);
            if (!node) { syscall_result = -EEXIST; goto out_open; }
            // Successful create: ownership of parent ref transfers
            // implicitly when we vfs_node_put below.
            vfs_node_put(parent);
            parent = NULL;
        }

        if (!node) {
            syscall_result = -ENOENT;
            goto out_open;
        }

        // O_TRUNC: truncate regular files to size 0 via filesystem op
        if ((flags & O_TRUNC) && node->type == VFS_FILE) {
            if (node->ops && (uint64_t)node->ops >= 0xffff800000000000ULL && node->ops->truncate &&
                (uint64_t)node->ops->truncate >= 0xffff800000000000ULL)
                node->ops->truncate(node, 0);
            else {
                // Fallback: reset size only (fs_data is FS-specific)
                node->size = 0;
            }
        }

        // devfs_open_node passes path_copy (kernel-side) to device
        // .open callbacks.  None of the registered device opens
        // (pty.c ptmx_open / ptsN_open) dereference the string, so
        // this is safe; future device opens MUST treat the argument
        // as kernel memory, not user memory.
        int rc = devfs_open_node(node, path_copy, flags, &f);
        if (rc == -ENOSYS) {
            // Not a devfs device node → fall through to default FD_VFS
            f = file_alloc();
            if (!f) { vfs_node_put(node); node = NULL; syscall_result = -ENOMEM; goto out_open; }
            f->type = FD_VFS;
            f->node = node;  // takes ownership of lookup ref
            node = NULL;     // f owns the ref now; don't double-put on cleanup
            f->flags = flags;
        } else {
            vfs_node_put(node);  // devfs path owns its own ref
            node = NULL;
            if (rc < 0) { syscall_result = rc; goto out_open; }
            if (!f) { syscall_result = -ENOMEM; goto out_open; }
        }

        int newfd = fd_alloc(current->files, f);
        if (newfd < 0) {
            file_free(f);
            f = NULL;          // already freed
            syscall_result = -ENFILE;
            goto out_open;
        }
        f = NULL;  // fd_alloc took ownership
        syscall_result = newfd;
    out_open:
        // Cleanup: free path_copy and any node/parent/f refs still
        // owned by this code path.
        if (parent) vfs_node_put(parent);
        if (node) vfs_node_put(node);
        if (f) file_free(f);
        if (parent_path) kfree(parent_path);
        if (path_copy) kfree(path_copy);
        break;
    }
    case SYS_close: {
        // close(int fd) → 0 / -EBADF
        int fd = (int)ctx->args[0];

        if (fd < 0 || fd >= NOFILE || !current->files || !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }
        fd_close(current->files, fd);
        syscall_result = 0;
        break;
    }
    case SYS_dup: {
        // dup(int oldfd) → newfd / -errno
        int oldfd = (int)ctx->args[0];
        syscall_result = fd_dup(current->files, oldfd, 0);
        break;
    }
    case SYS_dup2: {
        // dup2(int oldfd, int newfd) → newfd / -errno
        int oldfd = (int)ctx->args[0];
        int newfd = (int)ctx->args[1];
        syscall_result = fd_dup2(current->files, oldfd, newfd);
        break;
    }
    case SYS_pipe: {
        // pipe(int fds[2]) → 0 / -errno
        int *fds = (int *)ctx->args[0];
        // Fast reject: ensure 8 bytes are mapped writable.  The
        // authoritative _ft write happens in do_pipe.  NULL is
        // rejected here (no graceful "ignore write" semantic).
        if (!fds ||
            !syscall_check_user_range((uint64_t)fds, sizeof(int) * 2, true)) {
            syscall_result = -EFAULT;
            break;
        }
        syscall_result = do_pipe(fds);
        break;
    }
    case SYS_chdir: {
        // chdir(const char *path) → 0 / -errno
        const char *path = (const char *)ctx->args[0];
        char *path_copy = NULL;
        char *new_cwd = NULL;
        if ((uint64_t)path >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }
        if (!current->files) {
            syscall_result = -ENOENT;
            break;
        }

        // Bounded fault-tolerant copy.  After this point, `path` (the
        // raw user pointer) MUST NOT be touched — all uses go through
        // path_copy.  kfree(path_copy) is at `out:`.
        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            syscall_result = -EFAULT;
            goto out;
        }

        vfs_node_t *node = NULL;
        int lookup_rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node);
        if (lookup_rc < 0) { syscall_result = lookup_rc; goto out; }
        if (node->type != VFS_DIR) { vfs_node_put(node); syscall_result = -ENOTDIR; goto out; }
        vfs_node_put(node);

        // Build the new absolute cwd from path_copy (NEVER the raw user
        // pointer — that window was the old chdir bug).
        new_cwd = kmalloc(2 * VFS_NAME_MAX);
        if (!new_cwd) { syscall_result = -ENOMEM; goto out; }
        if (path_copy[0] == '/') {
            // absolute
            size_t len = plen;
            if (len >= 2 * VFS_NAME_MAX - 1) { syscall_result = -EINVAL; goto out; }
            memcpy(new_cwd, path_copy, len + 1);
        } else {
            // relative: cwd + "/" + path_copy
            int cwd_len = (int)strlen(current->files->cwd);
            if (cwd_len + 1 + plen >= 2 * VFS_NAME_MAX) { syscall_result = -EINVAL; goto out; }
            memcpy(new_cwd, current->files->cwd, cwd_len);
            new_cwd[cwd_len] = '/';
            memcpy(new_cwd + cwd_len + 1, path_copy, plen + 1);
        }
        // Collapse "//" and trailing "/"
        // For now, simple store
        kfree(current->files->cwd);
        current->files->cwd = new_cwd;          // transfer ownership
        new_cwd = NULL;
        log_info("chdir: pid=%d -> '%s'\n", (int)current->pid, current->files->cwd);
        syscall_result = 0;
    out:
        if (new_cwd) kfree(new_cwd);
        if (path_copy) kfree(path_copy);
        break;
    }
    case SYS_getcwd: {
        // getcwd(char *buf, size_t size) → buf / NULL(-errno)
        char *buf = (char *)ctx->args[0];
        uint64_t size = ctx->args[1];
        if (!current->files) {
            syscall_result = -ENOENT;
            break;
        }
        uint64_t len = strlen(current->files->cwd) + 1;
        // NULL buf with size=0 means "allocate" — we allocate on
        // the user heap via brk.  For simplicity, just require a
        // buffer of at least 256 bytes when size=0.
        if (buf == NULL) {
            syscall_result = -EINVAL;
            break;
        }
        if (!syscall_check_user_range((uint64_t)buf, size, true)) {
            syscall_result = -EFAULT;
            break;
        }
        if (len > size) { syscall_result = -ERANGE; break; }
        ssize_t r = copy_to_user_ft(buf, current->files->cwd, len);
        if (r < 0) { syscall_result = r; break; }
        syscall_result = (int64_t)(uint64_t)buf;
        break;
    }
    case SYS_stat: {
        // stat(const char *path, struct stat *buf) → 0 / -errno
        // Cat B: build kernel struct stat first, then _ft write to user.
        const char *path = (const char *)ctx->args[0];
        struct stat *buf = (struct stat *)ctx->args[1];

        if (!buf) {
            syscall_result = -EFAULT;
            break;
        }

        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            syscall_result = -EFAULT;
            break;
        }

        vfs_node_t *node = NULL;
        int lookup_rc = vfs_lookup_at(AT_FDCWD, path_copy, LOOKUP_FOLLOW, &node);
        kfree(path_copy);  /* preserve sys_stat's existing ownership boundary */
        if (lookup_rc < 0) {
            syscall_result = lookup_rc;
            break;  /* path_copy was freed immediately after lookup above */
        }

        // Fill kernel struct, then _ft write to user.
        struct stat kstat;
        int ret = vfs_stat(node, &kstat);
        vfs_node_put(node);
        if (ret != 0) { syscall_result = -EIO; break; }
        ssize_t r = copy_to_user_ft(buf, &kstat, sizeof(kstat));
        if (r < 0) { syscall_result = r; break; }
        syscall_result = 0;
        break;
    }
    case SYS_fstat: {
        // fstat(int fd, struct stat *buf) → 0 / -errno
        // Cat B: build kernel struct first, then _ft write to user.
        int fd = (int)ctx->args[0];
        struct stat *buf = (struct stat *)ctx->args[1];

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }
        if (!buf) {
            syscall_result = -EFAULT;
            break;
        }

        file_t *f = current->files->fd[fd];

        // PTY and pipe fds have no vfs_node — fill a synthetic stat
        struct stat kstat;
        if (!f->node) {
            memset(&kstat, 0, sizeof(kstat));
            if (f->type == FD_PTY_MASTER || f->type == FD_PTY_SLAVE)
                kstat.st_mode = S_IFCHR | 0600;
            else if (f->type == FD_PIPE)
                kstat.st_mode = S_IFIFO | 0600;
            else { syscall_result = -ENOENT; break; }
            ssize_t r = copy_to_user_ft(buf, &kstat, sizeof(kstat));
            if (r < 0) { syscall_result = r; break; }
            syscall_result = 0;
            break;
        }

        if (vfs_stat(f->node, &kstat) != 0) {
            syscall_result = -EIO;
            break;
        }
        ssize_t r = copy_to_user_ft(buf, &kstat, sizeof(kstat));
        if (r < 0) { syscall_result = r; break; }
        syscall_result = 0;
        break;
    }
    case SYS_lseek: {
        // lseek(int fd, int64_t offset, int whence) → new offset / -errno
        int fd = (int)ctx->args[0];
        int64_t offset = (int64_t)ctx->args[1];
        int whence = (int)ctx->args[2];

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }

        file_t *f = current->files->fd[fd];
        if (!f->node) { syscall_result = -ESPIPE; break; }

        int64_t new_offset;
        switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = (int64_t)f->offset + offset;
            break;
        case SEEK_END:
            new_offset = (int64_t)f->node->size + offset;
            break;
        default:
            new_offset = -1;
            break;
        }
        if (new_offset < 0) { syscall_result = -EINVAL; break; }
        f->offset = (uint64_t)new_offset;
        syscall_result = new_offset;
        break;
    }
    case SYS_fcntl: {
        // fcntl(int fd, int cmd, uint64_t arg) → result / -errno
        int fd = (int)ctx->args[0];
        int cmd = (int)ctx->args[1];
        uint64_t arg = ctx->args[2];

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }

        file_t *f = current->files->fd[fd];

        switch (cmd) {
        case F_DUPFD: {
            // dup to >= arg
            int start = (int)arg;
            if (start < 0) start = 0;
            syscall_result = fd_dup(current->files, fd, start);
            break;
        }
        case F_DUPFD_CLOEXEC: {
            // Same as F_DUPFD for now (close-on-exec not implemented)
            int start = (int)arg;
            if (start < 0) start = 0;
            syscall_result = fd_dup(current->files, fd, start);
            break;
        }
        case F_GETFD:
            // close-on-exec flag (always 0 for now)
            syscall_result = 0;
            break;
        case F_SETFD:
            // ignore close-on-exec for now
            syscall_result = 0;
            break;
        case F_GETFL:
            syscall_result = (int64_t)f->flags;
            break;
        case F_SETFL:
            // Only O_APPEND modifiable
            f->flags = (f->flags & ~O_APPEND) | (int)(arg & O_APPEND);
            syscall_result = 0;
            break;
        default:
            syscall_result = -EINVAL;
            break;
        }
        break;
    }
case SYS_ioctl: {
    // ioctl(int fd, unsigned long request, void *arg) -> 0 / -errno
    int fd = (int)ctx->args[0];
    int request = (int)ctx->args[1];
    void *arg = (void *)ctx->args[2];

    // Spec §3: pin the file via files_get_file so a concurrent
    // close on another fd table (e.g. dup'd sibling dropped
    // to zero) cannot release the file_t and its dev_private
    // out from under the ioctl dispatch.  Always file_put
    // after fd_ioctl — including on negative return — so the
    // refcount stays balanced.  fd_ioctl takes no fd-table
    // lock of its own, so pinning once around the call is
    // sufficient.
    file_t *f = (current->files)
        ? files_get_file(current->files, fd)
        : NULL;
    if (!f) {
        syscall_result = -EBADF;
        break;
    }

    // Cat B: per-request bounce happens inside the
    // ioctl handler (e.g. tty_phys_ioctl in tty.c).
    // The syscall entry does NOT pre-validate arg
    // because each request has its own size/perm
    // semantics; arg may be NULL.

    syscall_result = fd_ioctl(f, request, arg);
    file_put(f);
    break;
}
    case SYS_getdents64: {
        // getdents64(int fd, struct linux_dirent64 *buf, unsigned int count)
        // → bytes read / -errno
        // Cat B: kernel bounce (cap = UACCESS_BOUNCE_SIZE) + offset
        // commit ONLY on successful _ft write-back.  If the user
        // pointer is bogus, the offset is NOT advanced (so the next
        // call retries — no silently-lost directory entries).
        int fd = (int)ctx->args[0];
        struct linux_dirent64 *buf = (struct linux_dirent64 *)ctx->args[1];
        unsigned int count = (unsigned int)ctx->args[2];

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }
        if (!buf) {
            syscall_result = -EFAULT;
            break;
        }

        file_t *f = current->files->fd[fd];
        if (!f->node || f->node->type != VFS_DIR) {
            syscall_result = -ENOTDIR;
            break;
        }

        uint64_t cap = count;
        if (cap > UACCESS_BOUNCE_SIZE) cap = UACCESS_BOUNCE_SIZE;
        uint8_t *kbuf = kmalloc(cap);
        if (!kbuf) { syscall_result = -ENOMEM; break; }
        uint64_t next_offset = f->offset;
        int n = vfs_getdents(f->node, (struct linux_dirent64 *)kbuf,
                             (unsigned)cap, &next_offset);
        if (n < 0) {
            kfree(kbuf);
            syscall_result = -EIO;
            break;
        }
        ssize_t rc;
        if (n == 0) {
            // No entries: nothing to copy.  Treat as EOF — do NOT
            // commit next_offset (which equals f->offset) since it
            // would be a no-op anyway; just return 0.
            rc = 0;
        } else {
            rc = copy_to_user_ft(buf, kbuf, n);
        }
        kfree(kbuf);
        if (rc < 0) {
            syscall_result = -EFAULT;     // offset NOT advanced
        } else {
            f->offset = next_offset; // commit only on success
            syscall_result = n;
        }
        break;
    }
    case SYS_access: {
        // access(const char *path, int mode) → 0 / -errno
        const char *path = (const char *)ctx->args[0];
        int mode = (int)ctx->args[1];

        if ((uint64_t)path >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            syscall_result = -EFAULT;
            break;
        }

        const char *cwd = current->files ? current->files->cwd : "/";
        vfs_node_t *node = vfs_lookup_from(path_copy, cwd);
        kfree(path_copy);

        if (!node) { syscall_result = -ENOENT; break; }

        // Check access mode
        int ok = 1;
        if (mode & R_OK) {
            // For now, all files are readable
        }
        if (mode & W_OK) {
            // Check if filesystem is writable (not devfs)
            if (node->type == VFS_CHRDEV || node->type == VFS_BLKDEV) {
                // Devices: check if write op exists
                if (!node->ops || (uint64_t)node->ops < 0xffff800000000000ULL ||
                    !node->ops->write)
                    ok = 0;
            }
        }
        if (mode & X_OK) {
            // For now, no execute permission checks
        }

        vfs_node_put(node);

        if (!ok) { syscall_result = -EACCES; break; }
        syscall_result = 0;
        break;
    }
    case SYS_unlink: {
        // unlink(const char *path) → 0 / -errno
        const char *path = (const char *)ctx->args[0];

        if ((uint64_t)path >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            syscall_result = -EFAULT;
            break;
        }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_unlink(path_copy, cwd);
        kfree(path_copy);

        syscall_result = (int64_t)ret;
        break;
    }
    case SYS_mkdir: {
        // mkdir(const char *path, int mode) → 0 / -errno
        // (mode is ignored for now — always 0755)
        const char *path = (const char *)ctx->args[0];
        // int mode = (int)ctx->args[1];  // ignored for now

        if ((uint64_t)path >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            syscall_result = -EFAULT;
            break;
        }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_mkdir(path_copy, cwd);
        kfree(path_copy);

        syscall_result = (int64_t)ret;
        break;
    }
    case SYS_rmdir: {
        // rmdir(const char *path) → 0 / -errno
        const char *path = (const char *)ctx->args[0];

        if ((uint64_t)path >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            syscall_result = -EFAULT;
            break;
        }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_rmdir(path_copy, cwd);
        kfree(path_copy);

        syscall_result = (int64_t)ret;
        break;
    }
    case SYS_rename: {
        // rename(const char *oldpath, const char *newpath) → 0 / -errno
        const char *oldpath = (const char *)ctx->args[0];
        const char *newpath = (const char *)ctx->args[1];
        char *old_copy = NULL;
        char *new_copy = NULL;

        if ((uint64_t)oldpath >= current->addr_limit ||
            (uint64_t)newpath >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        // Bounded copy of oldpath.
        int olen = strnlen_user(oldpath, VFS_NAME_MAX);
        if (olen < 0) { syscall_result = -EFAULT; break; }
        if (olen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        old_copy = kmalloc(olen + 1);
        if (!old_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(old_copy, oldpath, olen + 1) < 0) {
            kfree(old_copy);
            syscall_result = -EFAULT;
            break;
        }

        // Bounded copy of newpath.
        int nlen = strnlen_user(newpath, VFS_NAME_MAX);
        if (nlen < 0) { kfree(old_copy); syscall_result = -EFAULT; break; }
        if (nlen >= VFS_NAME_MAX) { kfree(old_copy); syscall_result = -ENAMETOOLONG; break; }
        new_copy = kmalloc(nlen + 1);
        if (!new_copy) { kfree(old_copy); syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(new_copy, newpath, nlen + 1) < 0) {
            kfree(old_copy);
            kfree(new_copy);
            syscall_result = -EFAULT;
            break;
        }

        const char *cwd = current->files ? current->files->cwd : "/";
        int ret = vfs_rename(old_copy, new_copy, cwd);
        kfree(old_copy);
        kfree(new_copy);

        syscall_result = (int64_t)ret;
        break;
    }
    case SYS_truncate: {
        // truncate(const char *path, off_t length) → 0 / -errno
        const char *path = (const char *)ctx->args[0];
        int64_t length = (int64_t)ctx->args[1];

        if ((uint64_t)path >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        int plen = strnlen_user(path, VFS_NAME_MAX);
        if (plen < 0) { syscall_result = -EFAULT; break; }
        if (plen >= VFS_NAME_MAX) { syscall_result = -ENAMETOOLONG; break; }
        char *path_copy = kmalloc(plen + 1);
        if (!path_copy) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(path_copy, path, plen + 1) < 0) {
            kfree(path_copy);
            syscall_result = -EFAULT;
            break;
        }

        const char *cwd = current->files ? current->files->cwd : "/";
        vfs_node_t *node = vfs_lookup_from(path_copy, cwd);
        kfree(path_copy);

        if (!node) { syscall_result = -ENOENT; break; }

        int ret = vfs_truncate(node, (uint64_t)length);
        vfs_node_put(node);
        syscall_result = (int64_t)ret;
        break;
    }
    case SYS_ftruncate: {
        // ftruncate(int fd, off_t length) → 0 / -errno
        int fd = (int)ctx->args[0];
        int64_t length = (int64_t)ctx->args[1];

        if (fd < 0 || fd >= NOFILE || !current->files ||
            !current->files->fd[fd]) {
            syscall_result = -EBADF;
            break;
        }

        file_t *f = current->files->fd[fd];
        if (!f->node) { syscall_result = -ENOENT; break; }

        int ret = vfs_truncate(f->node, (uint64_t)length);
        syscall_result = (int64_t)ret;
        break;
    }
    case SYS_symlink: {
        // symlink(target, linkpath) — local kernel handler.
        // Both args are user pointers; the handler runs COPY_USER_STR
        // and vfs_split_parent, so this entry is a thin wrapper.
        syscall_result = sys_symlink((const char *)ctx->args[0],
                                (const char *)ctx->args[1]);
        break;
    }
    case SYS_readlink: {
        // readlink(path, buf, bufsize) — NOFOLLOW; non-symlink → -EINVAL.
        syscall_result = sys_readlink((const char *)ctx->args[0],
                                 (char *)ctx->args[1],
                                 (size_t)ctx->args[2]);
        break;
    }
    case SYS_lstat: {
        // lstat(path, buf) — NOFOLLOW (last component); kernel-local kstat
        // then copy_to_user_ft (v2 fix).
        syscall_result = sys_lstat((const char *)ctx->args[0],
                              (struct stat *)ctx->args[1]);
        break;
    }
    case SYS_fstatat: {
        // fstatat(dirfd, path, buf, flags) — flags are the fourth argument.
        // v3 fix: flags & ~AT_SYMLINK_NOFOLLOW → -EINVAL.
        syscall_result = sys_fstatat((int)ctx->args[0],
                                (const char *)ctx->args[1],
                                (struct stat *)ctx->args[2],
                                (int)ctx->args[3]);
        break;
    }
    case SYS_chmod: {
        // chmod(const char *path, mode_t mode) — stub: always success
        syscall_result = 0;
        break;
    }
    case SYS_fchmod: {
        // fchmod(int fd, mode_t mode) — stub: always success
        syscall_result = 0;
        break;
    }
    case SYS_poll: {
        int64_t nfds64 = (int64_t)ctx->args[1];
        syscall_result = do_poll((struct pollfd *)ctx->args[0],
                            (uint64_t)nfds64,
                            (int)ctx->args[2]);
        break;
    }
    case SYS_ppoll: {
        syscall_result = -ENOSYS;
        break;
    }
    case SYS_select: {
        syscall_result = do_select((int)ctx->args[0],
                              (void *)ctx->args[1], (void *)ctx->args[2],
                              (void *)ctx->args[3], (void *)ctx->args[4]);
        break;
    }
    case SYS_pselect6: {
        syscall_result = do_pselect6((int)ctx->args[0],
                                (void *)ctx->args[1], (void *)ctx->args[2],
                                (void *)ctx->args[3], (void *)ctx->args[4],
                                (const void *)ctx->args[5]);
        break;
    }
    default:
        break;
    }
    return syscall_result;
}
