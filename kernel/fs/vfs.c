#include <fs/vfs.h>
#include <core/debug.h>
#include <core/printk.h>
#include <memory/slab.h>
#include <sync/rwlock.h>
#include <sched/task.h>      // current->files->cwd for resolve_at
#include <string.h>
#include <stdlib.h>
#include <errno.h>

// ── Mount table ───────────────────────────────────────────
static vfs_mount_t *mount_list = NULL;
static int mount_count = 0;
static int vfs_initialized = 0;
static rwlock_t mount_lock;

// ── Helpers ────────────────────────────────────────────────
// Case-insensitive string compare — FAT32 stores 8.3 names in
// uppercase, so we loosen the lookup to accept any case.
static int vfs_name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// ── Initialization ────────────────────────────────────────
void vfs_init(void)
{
    mount_list = NULL;
    mount_count = 0;
    rwlock_init(&mount_lock);
    vfs_initialized = 1;
    debug_vfs("VFS: initialized\n");
}

// ── Mount ─────────────────────────────────────────────────
int vfs_mount(const char *path, block_device_t *dev,
              vfs_ops_t *ops, void *fs_data)
{
    if (!vfs_initialized) vfs_init();

    vfs_mount_t *mp = (vfs_mount_t *)calloc(1, sizeof(vfs_mount_t));
    if (!mp) {
        debug_vfs("VFS: mount: out of memory\n");
        return -1;
    }

    mp->dev = dev;
    mp->path = path;
    mp->ops = ops;
    mp->fs_data = fs_data;
    mp->root = NULL;

    // Allocate a root node — the filesystem fills it via ops
    mp->root = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!mp->root) {
        debug_vfs("VFS: mount: root node alloc failed\n");
        kfree(mp);
        return -1;
    }

    mp->root->mount = mp;
    mp->root->type = VFS_DIR;
    mp->root->ops = ops;
    // Root node uses fs_data = NULL to tell the filesystem to use its
    // internal root cluster (e.g., BPB_RootClus for FAT32).
    // Subdirectory nodes will hold the cluster number here.
    mp->root->fs_data = NULL;
    mp->root->refcount = 1;
    mp->root->name = strdup("/");
    if (!mp->root->name) {
        kfree(mp->root);
        kfree(mp);
        return -1;
    }

    // Publish the fully initialized mount atomically to concurrent lookups.
    rwlock_write_lock(&mount_lock);
    mp->next = mount_list;
    mount_list = mp;
    mount_count++;
    rwlock_write_unlock(&mount_lock);
    debug_vfs("VFS: mounted '%s'\n", path);
    return 0;
}

// ── Path tokenizer ────────────────────────────────────────
// Returns successive path components, modifying `path` in place.
// After the last component, returns NULL.
static char *next_component(char **path_ptr)
{
    char *p = *path_ptr;
    if (!p || *p == '\0') return NULL;

    // skip leading '/'
    while (*p == '/') p++;

    if (*p == '\0') { *path_ptr = p; return NULL; }

    char *start = p;
    while (*p && *p != '/') p++;

    if (*p == '/') {
        *p = '\0';
        *path_ptr = p + 1;
    } else {
        *path_ptr = p;  // end of string
    }

    return start;
}

// ── Path normalizer ──────────────────────────────────────
// Collapse repeated '/' and remove "/./" segments in-place.  Without
// this, paths like "/./dev" (which busybox's ls produces via
// concat_path_file(".", "dev") when scanning the cwd) fail to match
// the /dev mount in find_mount() because find_mount uses strict
// prefix matching — "/./dev" ≠ "/dev", so the lookup falls through
// to the root ext2 inode, which doesn't contain /dev.  ".." is
// intentionally NOT handled here (it requires the cwd, which
// resolve_at has already collapsed into the absolute path before us).
//
// Caller must ensure `out` has room for at least VFS_NAME_MAX bytes.
// Returns 0 on success; the normalized path is always ≤ strlen(in).
static int normalize_vfs_path(const char *in, char *out)
{
    const char *r = in;
    char *w = out;

    // Collapse leading "/" runs to a single "/".
    if (*r == '/') {
        *w++ = '/';
        r++;
        while (*r == '/') r++;
    }

    while (*r) {
        // Read next component up to the next '/' (or end).
        const char *start = r;
        while (*r && *r != '/') r++;
        size_t len = (size_t)(r - start);

        if (len == 1 && start[0] == '.') {
            // Skip "." segment.
        } else if (len == 0) {
            // Empty (consecutive '/') — already collapsed above.
        } else {
            // Add a '/' separator if this isn't the first emitted
            // component.
            if (w > out && w[-1] != '/') *w++ = '/';
            memcpy(w, start, len);
            w += len;
        }

        // Skip the '/' and any additional ones (handles "//" and
        // "/foo//bar" — also the "/" that follows a stripped "./").
        while (*r == '/') r++;
    }

    // "/." → "/" trailing trim: if the last emitted char is '.'
    // preceded by '/', drop the '.'.
    if (w > out + 1 && w[-1] == '.' && w[-2] == '/') w--;

    *w = '\0';
    return 0;
}

// ── Find mount point by prefix match ──────────────────────
//
// Walks the mount list and returns the deepest mount whose path is a
// prefix of `path`.  Used by vfs_lookup_at to choose the root node
// before walking a path component-by-component.
//
// Defensive guard: a known fork/exec-loop corruption (see
// docs/superpowers/handoff/find-mount-pf-fork-exec-handoff.md) can
// leave an entry in the mount list pointing into the user address
// range.  Without the check, the next iteration dereferences
// mp->path at mp+8 and faults at the next unmapped page.  We
// refuse to dereference such an entry, dump the chain, and break
// out — the caller treats the result as "no matching mount" and
// returns -ENOENT instead of crashing the kernel.
static vfs_mount_t *find_mount(const char *path)
{
    // Find the deepest matching mount point
    vfs_mount_t *best = NULL;
    size_t best_len = 0;

    rwlock_read_lock(&mount_lock);
    int _iter = 0;
    for (vfs_mount_t *mp = mount_list; mp; mp = mp->next) {
        // Diagnostic: detect mount-list corruption (find_mount PF in
        // fork/exec loop — kernel-heap / PMM bug).  A legitimate mount
        // entry pointer is a kernel-high-half address (above PAGE_OFFSET);
        // anything below is user space or zero-page garbage.
        if ((unsigned long)mp < 0xffff800000000000UL) {
            serial_printk("VFS: find_mount: CORRUPT mp=%p path=%p "
                          "iter=%d\n",
                          (void *)mp, (const void *)path, _iter);
            // Dump the rest of the chain so we can see how we got here.
            for (vfs_mount_t *q = mount_list; q && q != mp; q = q->next) {
                serial_printk("  [chain] mp=%p path=%p next=%p\n",
                              (void *)q, (void *)q->path, (void *)q->next);
            }
            break;
        }
        _iter++;
        size_t len = strlen(mp->path);
        if (strncmp(path, mp->path, len) == 0) {
            // Root mount ("/") matches any path.
            // Non-root mounts must end at a component boundary.
            if (len == 1 && mp->path[0] == '/') {
                if (len > best_len) { best_len = len; best = mp; }
            } else if (path[len] == '\0' || path[len] == '/') {
                if (len > best_len) { best_len = len; best = mp; }
            }
        }
    }
    rwlock_read_unlock(&mount_lock);
    // VFS has no unmount operation: a published mount remains valid after
    // this short lookup critical section ends.
    return best;
}

// ── Lookup (core, raw) ────────────────────────────────────
// Resolves an absolute path to a VFS node, with mid-path symlink early-return.
//
// Return value (2-state):
//   0    = walked to a non-symlink final node; *out_node holds the node
//          with refcount++.  consumed_out / remaining_out are optional and
//          contain the walked path / "" respectively.
//   1    = hit a symlink (mid-path or last component); *out_node holds the
//          symlink with refcount++.  *consumed_out is the absolute prefix
//          walked up to (but not including) the symlink; *remaining_out is
//          the unconsumed suffix after the symlink (contract: "" or starts
//          with '/').
//   < 0  = -errno (ENOENT / ENOTDIR / ENAMETOOLONG / EIO / EINVAL / ENOMEM);
//          *out_node = NULL.
//
// NOFOLLOW semantics are NOT applied here — the caller (vfs_lookup_at /
// vfs_lookup_resolved) decides whether to splice + restart based on flags
// and the suffix content.
static int __vfs_lookup_raw(const char *path,
                            vfs_node_t **out_node,
                            char *consumed_out, size_t consumed_size,
                            char *remaining_out, size_t remaining_size)
{
    *out_node = NULL;
    if (consumed_out && consumed_size > 0) consumed_out[0] = '\0';
    if (remaining_out && remaining_size > 0) remaining_out[0] = '\0';

    if (!vfs_initialized || !path) return -EINVAL;

    // Normalize the input path so find_mount() sees the canonical form
    // (collapse "//", strip "/./").  See normalize_vfs_path() for the
    // why — the short version is that busybox's ls calls
    // concat_path_file(".", entry) which produces paths like "./dev",
    // and resolve_at prepends the cwd "/" to make "/./dev".  Without
    // this step, find_mount()'s strict prefix match fails to recognize
    // the /dev mount and the lookup falls through to the root ext2
    // inode, which doesn't contain /dev, so lstat returns ENOENT.
    char path_buf[VFS_NAME_MAX];
    normalize_vfs_path(path, path_buf);
    const char *norm_path = path_buf;

    size_t plen = strlen(norm_path);
    if (plen >= VFS_NAME_MAX) return -ENAMETOOLONG;

    // Handle root
    if (strcmp(norm_path, "/") == 0) {
        vfs_mount_t *mp = find_mount("/");
        if (!mp || !mp->root) return -ENOENT;
        __sync_add_and_fetch(&mp->root->refcount, 1);
        *out_node = mp->root;
        if (consumed_out && consumed_size >= 2) {
            consumed_out[0] = '/';
            consumed_out[1] = '\0';
        }
        return 0;
    }

    // Find the mount point
    vfs_mount_t *mp = find_mount(norm_path);
    if (!mp || !mp->root || !mp->root->ops) return -ENOENT;

    // Tokenize path — skip mount point prefix for sub-mounts
    char path_copy[VFS_NAME_MAX];
    memcpy(path_copy, norm_path, plen + 1);

    char *ptr;
    size_t mp_len = strlen(mp->path);
    if (mp_len == 1 && mp->path[0] == '/') {
        ptr = path_copy;  // root mount, no prefix to skip
    } else {
        ptr = path_copy + mp_len;
        while (*ptr == '/') ptr++;  // skip leading slash
    }

    char *comp;

    // Use `cur` (not `current` — `current` is a kernel-wide macro).
    vfs_node_t *cur = mp->root;
    __sync_add_and_fetch(&cur->refcount, 1);

    // Seed consumed with the mount prefix so that mid-path symlink splicing
    // resolves relative targets against the correct directory.  For the root
    // mount (mp->path == "/") we leave the buffer empty so the first append
    // produces "/comp" rather than the spurious "//comp"; for sub-mounts we
    // copy mp->path verbatim.  Without this, a relative symlink target on a
    // sub-mount would be spliced against the root mount instead of the
    // mount's own directory.
    size_t consumed_len = 0;
    if (consumed_out && consumed_size > 0) consumed_out[0] = '\0';
    if (mp->path && mp->path[0] == '/' && mp->path[1] != '\0') {
        size_t mp_path_len = strlen(mp->path);
        if (consumed_out && mp_path_len + 1 > consumed_size)
            return -ENAMETOOLONG;
        if (consumed_out) {
            memcpy(consumed_out, mp->path, mp_path_len);
            consumed_out[mp_path_len] = '\0';
        }
        consumed_len = mp_path_len;
    }

    while ((comp = next_component(&ptr)) != NULL) {
        if (strlen(comp) == 0) continue;

        // "." — stay in current directory
        if (strcmp(comp, ".") == 0) continue;

        // Explicit VFS_DIR check before readdir — walking through a non-dir
        // would otherwise surface as a confusing readdir failure (v5 fix).
        if (cur->type != VFS_DIR) {
            __sync_sub_and_fetch(&cur->refcount, 1);
            return -ENOTDIR;
        }

        // ".." — go to parent, or stay at mount root
        if (strcmp(comp, "..") == 0) {
            if (cur->parent) {
                vfs_node_t *parent = cur->parent;
                __sync_add_and_fetch(&parent->refcount, 1);
                __sync_sub_and_fetch(&cur->refcount, 1);
                cur = parent;
            }
            // Trim consumed to last "/": "/foo/bar" → "/foo", "/foo" → "/".
            if (consumed_out) {
                size_t clen = strlen(consumed_out);
                if (clen > 1) {
                    char *last = consumed_out + clen - 1;
                    while (last > consumed_out && *last != '/') last--;
                    if (last == consumed_out) {
                        // Stay at root: e.g., "/foo" → "/".
                        last[1] = '\0';
                        consumed_len = 1;
                    } else {
                        *last = '\0';
                        consumed_len = (size_t)(last - consumed_out);
                    }
                }
            }
            continue;
        }

        vfs_dirent_t entry;
        char _entry_name[VFS_NAME_MAX];
        entry.name = _entry_name;
        int found = 0;
        uint64_t idx = 0;
        int max_iter = 256;  // safety bound for corrupted ops

        while (max_iter-- > 0) {
            int ret = vfs_readdir(cur, idx, &entry);
            if (ret != 0) {
                // readdir error on a directory we just verified is a DIR.
                // Treat as I/O failure (v5 fix).
                __sync_sub_and_fetch(&cur->refcount, 1);
                return -EIO;
            }
            if (entry.name[0] == '\0') break;
            int match;
            if (cur->ops &&
                (uint64_t)cur->ops >= 0xffff800000000000ULL &&
                (cur->ops->flags & VFS_OPS_CASE_INSENSITIVE))
                match = (vfs_name_cmp(entry.name, comp) == 0);
            else
                match = (strcmp(entry.name, comp) == 0);

            if (match) {
                found = 1;
                break;
            }
            idx++;
        }

        if (!found) {
            __sync_sub_and_fetch(&cur->refcount, 1);
            return -ENOENT;
        }

        vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
        if (!child) {
            __sync_sub_and_fetch(&cur->refcount, 1);
            return -ENOMEM;
        }

        child->mount = mp;
        child->parent = cur;
        __sync_add_and_fetch(&cur->refcount, 1);  // child holds a ref through parent pointer
        child->type = entry.type;
        child->size = entry.size;
        child->ops = cur->ops;
        child->fs_data = (void *)(uintptr_t)entry.ino;
        child->refcount = 1;
        child->name = strndup(entry.name, VFS_NAME_MAX - 1);

        // Append "/" + comp to consumed (bounded by VFS_NAME_MAX, same as path).
        if (consumed_out && consumed_size > 0) {
            size_t cl = strlen(comp);
            if (consumed_len + 1 + cl + 1 > consumed_size) {
                free(child->name);
                free(child);
                __sync_sub_and_fetch(&cur->refcount, 1);
                return -ENAMETOOLONG;
            }
            consumed_out[consumed_len] = '/';
            memcpy(consumed_out + consumed_len + 1, comp, cl);
            consumed_len += 1 + cl;
            consumed_out[consumed_len] = '\0';
        }

        // Symlink early-return: caller decides whether to follow.
        if (child->type == VFS_SYMLINK) {
            if (remaining_out && remaining_size > 0) {
                size_t rem_len = strlen(ptr);
                if (rem_len == 0) {
                    // Last component → empty suffix.
                    remaining_out[0] = '\0';
                } else {
                    // Mid-path: contract requires suffix starts with '/'.
                    if (1 + rem_len + 1 > remaining_size) {
                        free(child->name);
                        free(child);
                        __sync_sub_and_fetch(&cur->refcount, 1);
                        return -ENAMETOOLONG;
                    }
                    remaining_out[0] = '/';
                    memcpy(remaining_out + 1, ptr, rem_len);
                    remaining_out[1 + rem_len] = '\0';
                }
            }
            // Drop our walk ref on cur; child keeps its parent ref.
            __sync_sub_and_fetch(&cur->refcount, 1);
            *out_node = child;
            return 1;  // symlink hit
        }

        __sync_sub_and_fetch(&cur->refcount, 1);
        cur = child;
    }

    // Walk completed with no symlink — *out_node is the final node.
    *out_node = cur;
    return 0;
}

// ── Splice symlink target + remaining suffix ─────────────
// Builds the next iteration's absolute path from the consumed prefix,
// the symlink target (read via ops->readlink), and the unconsumed suffix.
//
//   target[0] == '/'  →  new = target + suffix       (absolute override)
//   else              →  new = dirname(consumed) + "/" + target + suffix
//
// Returns 0 on success or -ENAMETOOLONG if the expanded path exceeds
// out_size.  On error, the caller is responsible for freeing `target`
// and dropping the symlink node (handled in vfs_lookup_resolved).
static int splice_symlink_path(const char *consumed, const char *target,
                               const char *suffix,
                               char *out, size_t out_size)
{
    if (!consumed || !target || !out || out_size == 0) return -EINVAL;

    size_t pos = 0;

    if (target[0] == '/') {
        // Absolute override — consumed is irrelevant.
        size_t tlen = strlen(target);
        if (tlen + 1 > out_size) return -ENAMETOOLONG;
        memcpy(out, target, tlen);
        pos = tlen;
    } else {
        // Relative: prepend dirname(consumed).
        //   consumed "/foo/bar" → prefix "/foo"
        //   consumed "/foo"     → prefix "/"    (root)
        //   consumed "/"        → prefix "/"    (no change)
        const char *last_slash = NULL;
        for (const char *s = consumed; *s; s++)
            if (*s == '/') last_slash = s;

        const char *prefix;
        size_t prefix_len;
        if (!last_slash) {
            // No "/" in consumed (shouldn't happen for absolute paths).
            prefix = "";
            prefix_len = 0;
        } else if (last_slash == consumed) {
            // consumed is "/" or "/foo" — prefix is just "/".
            prefix = "/";
            prefix_len = 1;
        } else {
            // consumed is "/foo/bar" or deeper.
            prefix = consumed;
            prefix_len = (size_t)(last_slash - consumed);
        }

        if (prefix_len + 1 + 1 > out_size) return -ENAMETOOLONG;
        if (prefix_len > 0) memcpy(out, prefix, prefix_len);
        out[prefix_len] = '/';
        pos = prefix_len + 1;

        size_t tlen = strlen(target);
        if (pos + tlen + 1 > out_size) return -ENAMETOOLONG;
        memcpy(out + pos, target, tlen);
        pos += tlen;
    }

    // Append suffix (already starts with '/' or is empty per contract).
    size_t slen = strlen(suffix);
    if (pos + slen + 1 > out_size) return -ENAMETOOLONG;
    if (slen > 0) memcpy(out + pos, suffix, slen);
    pos += slen;
    out[pos] = '\0';

    return 0;
}

// ── Resolve dirfd + path into an absolute path ───────────
//
//   dirfd == AT_FDCWD && path absolute → out = path
//   dirfd == AT_FDCWD && path relative → out = current->files->cwd + "/" + path
//   dirfd != AT_FDCWD                 → -EBADF (real fd support deferred)
//
// Absolute paths always ignore dirfd (v5 fix).
static int resolve_at(int dirfd, const char *path,
                      char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) return -EINVAL;

    size_t plen = strlen(path);
    if (plen == 0) return -EINVAL;

    // Absolute path — ignore dirfd entirely.
    if (path[0] == '/') {
        if (plen + 1 > out_size) return -ENAMETOOLONG;
        memcpy(out, path, plen + 1);
        return 0;
    }

    // Relative path — require AT_FDCWD for now.
    if (dirfd != AT_FDCWD) return -EBADF;

    const char *cwd = "/";
    if (current && current->files && current->files->cwd) {
        cwd = current->files->cwd;
    }

    size_t cwd_len = strlen(cwd);
    int add_sep = (cwd_len > 0 && cwd[cwd_len - 1] != '/') ? 1 : 0;
    size_t total = cwd_len + add_sep + plen;
    if (total + 1 > out_size) return -ENAMETOOLONG;

    size_t off = 0;
    if (cwd_len > 0) {
        memcpy(out, cwd, cwd_len);
        off = cwd_len;
    }
    if (add_sep) out[off++] = '/';
    memcpy(out + off, path, plen + 1);

    return 0;
}

// ── Symlink-aware lookup (fold walk + splice + restart) ──
//
// Iterates: walk → if symlink, splice + restart, up to MAXSYMLINKS hops.
// NOFOLLOW only blocks when suffix is empty (last-component symlink).
//
// Returns 0 on success (*out_node refcount++) or -errno on failure.
// Caller must vfs_node_put(*out_node) on success.
static int vfs_lookup_resolved(const char *absolute,
                               lookup_flags_t flags,
                               vfs_node_t **out_node)
{
    *out_node = NULL;

    size_t alen = strlen(absolute);
    if (alen >= VFS_NAME_MAX) return -ENAMETOOLONG;

    // Co-located stack buffers per spec §7.  Peak ~1.7 KB.
    char remaining[VFS_NAME_MAX];
    char consumed[VFS_NAME_MAX];
    char suffix[VFS_NAME_MAX];
    memcpy(remaining, absolute, alen + 1);

    vfs_node_t *node = NULL;
    int depth = 0;

    for (;;) {
        int st = __vfs_lookup_raw(remaining, &node,
                                  consumed, sizeof(consumed),
                                  suffix, sizeof(suffix));
        if (st < 0) return st;
        if (st == 0) {
            *out_node = node;
            return 0;
        }
        // st == 1: symlink hit.
        if (depth >= MAXSYMLINKS) {
            vfs_node_put(node);
            return -ELOOP;
        }

        // NOFOLLOW semantics: only block the last-component symlink.
        if ((flags & LOOKUP_NOFOLLOW) && suffix[0] == '\0') {
            *out_node = node;
            return 0;
        }

        if (!node->ops || !node->ops->readlink) {
            vfs_node_put(node);
            return -EOPNOTSUPP;
        }

        char *target = kmalloc(VFS_NAME_MAX);
        if (!target) {
            vfs_node_put(node);
            return -ENOMEM;
        }
        int tlen = node->ops->readlink(node, target, VFS_NAME_MAX - 1);
        if (tlen < 0) {
            kfree(target);
            vfs_node_put(node);
            return tlen;
        }
        if (tlen >= VFS_NAME_MAX - 1) {
            // readlink returned >= bufsize; we couldn't NUL-terminate.
            kfree(target);
            vfs_node_put(node);
            return -ENAMETOOLONG;
        }
        target[tlen] = '\0';

        char new_remaining[VFS_NAME_MAX];
        int src = splice_symlink_path(consumed, target, suffix,
                                      new_remaining, sizeof(new_remaining));
        if (src < 0) {
            // Per hard rule: free target AND vfs_node_put(node) on failure.
            kfree(target);
            vfs_node_put(node);
            return src;
        }
        kfree(target);

        vfs_node_put(node);
        memcpy(remaining, new_remaining, VFS_NAME_MAX);
        depth++;
    }
}

// ── Public lookup: absolute path only ─────────────────────
//
// Legacy contract: returns vfs_node_t* (NULL on error, no errno).
// T6 migration: call vfs_lookup_resolved so symlinks at any depth
// are followed (LOOKUP_FOLLOW).  errno from the resolver is dropped.
vfs_node_t *vfs_lookup(const char *path)
{
    if (!path) return NULL;
    vfs_node_t *node = NULL;
    if (vfs_lookup_resolved(path, LOOKUP_FOLLOW, &node) < 0) {
        return NULL;
    }
    return node;
}

// ── Public lookup: relative path support ──────────────────
//
// If path is absolute (starts with '/'), cwd is ignored.
// Otherwise, path is resolved relative to cwd (cwd must be non-NULL).
// Legacy contract: NULL on error.  T6 migration: call
// vfs_lookup_resolved directly to apply the same symlink-following
// behavior as vfs_lookup (LOOKUP_FOLLOW).
vfs_node_t *vfs_lookup_from(const char *path, const char *cwd)
{
    if (!path) return NULL;

    // Absolute path — use directly
    if (path[0] == '/')
        return vfs_lookup(path);

    // Relative path with no cwd — can't resolve
    if (!cwd)
        return NULL;

    // Strip leading "./" prefix — this keeps mount point matching clean
    // (./dev → dev, so the absolute path becomes /dev not /./dev)
    while (path[0] == '.' && path[1] == '/')
        path += 2;

    // Build absolute path: cwd + "/" + path
    // Avoid double slashes when cwd already ends with '/'
    char abs_path[VFS_NAME_MAX];
    int cwd_len = (int)strlen(cwd);
    int path_len = (int)strlen(path);
    int add_sep = (cwd_len > 0 && cwd[cwd_len - 1] != '/') ? 1 : 0;
    int total = cwd_len + add_sep + path_len;
    if (total >= VFS_NAME_MAX)
        return NULL;

    int off = 0;
    if (cwd_len > 0) {
        memcpy(abs_path, cwd, cwd_len);
        off = cwd_len;
    }
    if (add_sep)
        abs_path[off++] = '/';
    memcpy(abs_path + off, path, path_len + 1);  // include NUL

    return vfs_lookup(abs_path);
}

// ── Public lookup: dirfd + flags + errno ─────────────────
//
// New T2 API: returns 0/-errno with refcount++ on success.
// Normalizes via resolve_at, then loops in vfs_lookup_resolved.
int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node)
{
    if (!out_node) return -EINVAL;
    *out_node = NULL;
    if (!path) return -EINVAL;

    char remaining[VFS_NAME_MAX];
    int rc = resolve_at(dirfd, path, remaining, sizeof(remaining));
    if (rc < 0) return rc;

    return vfs_lookup_resolved(remaining, flags, out_node);
}

// ── Read ──────────────────────────────────────────────────
int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->ops || (uint64_t)node->ops < 0xffff800000000000ULL)
        return -1;
    if (!node->ops->read)
        return -1;
    if ((uint64_t)node->ops->read < 0xffff800000000000ULL)
        return -1;
    return node->ops->read(node, offset, size, buffer);
}

// ── Write ─────────────────────────────────────────────────
int vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->ops || (uint64_t)node->ops < 0xffff800000000000ULL)
        return -1;
    if (!node->ops->write)
        return -1;
    if ((uint64_t)node->ops->write < 0xffff800000000000ULL)
        return -1;
    return node->ops->write(node, offset, size, buffer);
}

// ── Read directory ────────────────────────────────────────
int vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *entry)
{
    if (!dir || !dir->ops || (uint64_t)dir->ops < 0xffff800000000000ULL)
        return -1;
    if (!dir->ops->readdir)
        return -1;
    if ((uint64_t)dir->ops->readdir < 0xffff800000000000ULL)
        return -1;
    return dir->ops->readdir(dir, index, entry);
}

// ── Reference counting ────────────────────────────────────
vfs_node_t *vfs_node_get(vfs_node_t *node)
{
    if (node) __sync_add_and_fetch(&node->refcount, 1);
    return node;
}

void vfs_node_put(vfs_node_t *node)
{
    if (!node) return;
    if (__sync_sub_and_fetch(&node->refcount, 1) == 0) {
        vfs_node_t *parent = node->parent;
        // Poison to catch use-after-free: any stale reference
        // will hit a null-pointer check (unlike kernel-address
        // guards which the optimizer may elide).
        node->ops    = NULL;
        node->parent = NULL;
        node->mount  = NULL;
        node->fs_data = NULL;
        if (node->name) kfree(node->name);
        node->name = NULL;
        free(node);
        vfs_node_put(parent);  // release parent ref after child is gone
    }
}

// ── Stat ───────────────────────────────────────────────────
// Fills a struct stat from a VFS node.  Used by SYS_stat and SYS_fstat.
int vfs_stat(vfs_node_t *node, struct stat *buf)
{
    if (!node || !buf) return -1;

    memset(buf, 0, sizeof(struct stat));

    // st_ino: use fs_data (cluster number) as inode number
    buf->st_ino = (uint64_t)(uintptr_t)node->fs_data;

    // st_size: file size in bytes
    buf->st_size = (int64_t)node->size;

    // st_mode: file type + default permissions
    switch (node->type) {
    case VFS_FILE:  buf->st_mode = S_IFREG | 0755; break;
    case VFS_DIR:   buf->st_mode = S_IFDIR | 0755; break;
    case VFS_CHRDEV: buf->st_mode = S_IFCHR | 0600; break;
    case VFS_BLKDEV: buf->st_mode = S_IFBLK | 0600; break;
    case VFS_SYMLINK: buf->st_mode = S_IFLNK | 0777; break;
    default:        buf->st_mode = 0; break;
    }

    // Default ownership
    buf->st_uid = 0;
    buf->st_gid = 0;
    buf->st_nlink = 1;

    // Block size and count
    buf->st_blksize = 512;
    buf->st_blocks = (node->size + 511) / 512;

    return 0;
}

// ── Sorted getdents entry (internal, for mount-point injection) ──
// We use vfs_dirent_t (from vfs.h) directly. max 64 entries keeps the
// stack footprint under 20 KB on a 32 KB kernel stack.
#define VFS_GETDENTS_SORT_MAX 64

// ── Check if mount_path is a direct child of dir_path ───────
// Returns 1 if mount_path is exactly one component deeper than dir_path.
static int vfs_is_child_mount(const char *dir_path, const char *mount_path)
{
    if (!dir_path || !mount_path)
        return 0;

    size_t dlen = strlen(dir_path);

    // Root "/" — resolve to empty; any immediate child is single-component.
    if (dlen == 1 && dir_path[0] == '/') {
        if (mount_path[0] != '/' || mount_path[1] == '\0')
            return 0;
        // e.g., "/dev" → "dev" (no '/' after position 1)
        return strchr(mount_path + 1, '/') == NULL;
    }

    // Non-root: mount_path = dir_path + "/" + one_component
    if (strncmp(dir_path, mount_path, dlen) != 0)
        return 0;
    if (mount_path[dlen] != '/')
        return 0;
    return strchr(mount_path + dlen + 1, '/') == NULL;
}

// ── getdents64 ─────────────────────────────────────────────
// Collects entries from the underlying filesystem, injects VFS mount
// points that are direct children of this directory, sorts all entries
// case-insensitively, then streams them to the user buffer via *pos.
int vfs_getdents(vfs_node_t *dir, struct linux_dirent64 *buf, unsigned int count,
                 uint64_t *pos)
{
    if (!dir || !buf || !pos || dir->type != VFS_DIR)
        return -1;

    // ── Phase 1: Collect entries from the underlying filesystem ──
    vfs_dirent_t *entries = kmalloc(sizeof(vfs_dirent_t) * VFS_GETDENTS_SORT_MAX);
    char *entry_names = kmalloc(VFS_NAME_MAX * VFS_GETDENTS_SORT_MAX);
    if (!entries || !entry_names) {
        if (entries) kfree(entries);
        if (entry_names) kfree(entry_names);
        return -ENOMEM;
    }
    for (int i = 0; i < VFS_GETDENTS_SORT_MAX; i++)
        entries[i].name = entry_names + i * VFS_NAME_MAX;

    int total = 0;
    char _de_name[VFS_NAME_MAX];
    vfs_dirent_t de = { .name = _de_name };
    uint64_t idx = 0;

    while (total < VFS_GETDENTS_SORT_MAX) {
        int ret = vfs_readdir(dir, idx++, &de);
        if (ret != 0) continue;
        if (de.name[0] == '\0') break;

        size_t nlen = strlen(de.name);
        if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
        memcpy(entries[total].name, de.name, nlen);
        entries[total].name[nlen] = '\0';
        entries[total].size = de.size;
        entries[total].type = de.type;
        entries[total].ino  = de.ino;
        total++;
    }

    // ── Phase 2: Inject sub-mount entries ──────────────────────
    // Only when listing a mount root (e.g., "/" which is FAT32's root).
    if (dir->mount && dir == dir->mount->root) {
        rwlock_read_lock(&mount_lock);
        for (vfs_mount_t *mp = mount_list; mp && total < VFS_GETDENTS_SORT_MAX; mp = mp->next) {
            if (mp == dir->mount) continue;  // skip self
            if (!vfs_is_child_mount(dir->mount->path, mp->path))
                continue;

            // Extract basename (skip leading "/")
            const char *base = mp->path;
            if (base[0] == '/') base++;
            size_t blen = strlen(base);
            if (blen >= VFS_NAME_MAX) blen = VFS_NAME_MAX - 1;
            memcpy(entries[total].name, base, blen);
            entries[total].name[blen] = '\0';
            entries[total].size = 0;
            entries[total].type = VFS_DIR;
            entries[total].ino  = (uint32_t)(0x80000000 | total);
            total++;
        }
        rwlock_read_unlock(&mount_lock);
    }

    // ── Phase 3: Sort by name (case-insensitive) ──────────────
    for (int i = 0; i < total - 1; i++) {
        for (int j = 0; j < total - 1 - i; j++) {
            int cmp;
            if (dir->ops && (uint64_t)dir->ops >= 0xffff800000000000ULL &&
                (dir->ops->flags & VFS_OPS_CASE_INSENSITIVE))
                cmp = vfs_name_cmp(entries[j].name, entries[j + 1].name);
            else
                cmp = strcmp(entries[j].name, entries[j + 1].name);
            if (cmp > 0) {
                vfs_dirent_t tmp = entries[j];
                entries[j]     = entries[j + 1];
                entries[j + 1] = tmp;
            }
        }
    }

    // ── Phase 4: Output from sorted list starting at *pos ────
    unsigned int bytes_written = 0;

    while (*pos < (uint64_t)total) {
        vfs_dirent_t *e = &entries[*pos];
        size_t name_len = strlen(e->name);
        uint16_t reclen = (uint16_t)(sizeof(struct linux_dirent64) + name_len + 1);
        reclen = (reclen + 7) & ~7;

        if (bytes_written + reclen > count)
            break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)((char *)buf + bytes_written);
        d->d_ino   = e->ino;
        d->d_off   = (int64_t)(*pos + 1);
        d->d_reclen = reclen;

        switch (e->type) {
        case VFS_FILE:   d->d_type = DT_REG; break;
        case VFS_DIR:    d->d_type = DT_DIR; break;
        case VFS_CHRDEV: d->d_type = DT_CHR; break;
        case VFS_BLKDEV: d->d_type = DT_BLK; break;
        case VFS_SYMLINK: d->d_type = DT_LNK; break;
        default:         d->d_type = DT_UNKNOWN; break;
        }

        memcpy(d->d_name, e->name, name_len + 1);
        bytes_written += reclen;
        (*pos)++;
    }

    kfree(entry_names);
    kfree(entries);
    return (int)bytes_written;
}

void vfs_debug_list(const char *path)
{
    vfs_node_t *dir = vfs_lookup(path);
    if (!dir) {
        debug_vfs("VFS: cannot list '%s' (not found)\n", path);
        return;
    }
    if (dir->type != VFS_DIR) {
        debug_vfs("VFS: '%s' is not a directory\n", path);
        vfs_node_put(dir);
        return;
    }

    debug_vfs("VFS: listing '%s':\n", path);
    char _entry_name[VFS_NAME_MAX];
    vfs_dirent_t entry = { .name = _entry_name };
    uint64_t idx = 0;
    int max_iter = 256;  // safety bound to prevent infinite loops

    while (max_iter-- > 0) {
        int ret = vfs_readdir(dir, idx, &entry);
        if (ret != 0) { idx++; continue; }
        if (entry.name[0] == '\0') break;
        if (entry.type == VFS_DIR)
            debug_vfs("  [DIR ]  %s\n", entry.name);
        else if (entry.type == VFS_CHRDEV)
            debug_vfs("  [CHR ]  %s\n", entry.name);
        else if (entry.type == VFS_BLKDEV)
            debug_vfs("  [BLK ]  %s\n", entry.name);
        else
            debug_vfs("  [FILE]  %s (%lu bytes)\n", entry.name, entry.size);
        idx++;
    }
    vfs_node_put(dir);
}

// ── Split a path into parent directory path and base name ──
// Given "/foo/bar/baz", sets parent to "/foo/bar" and returns "baz".
// Given "/file", sets parent to "/" and returns "file".
// Given "file" (no slash), uses cwd as parent and returns "file".
// Returns pointer into a static buffer (parent_path), or NULL on error.
// Exported (T6): sys_symlink (T7) needs relative linkpath handling.
const char *vfs_split_parent(const char *path, const char *cwd,
                             char parent_path[VFS_NAME_MAX])
{
    if (!path || !parent_path) return NULL;

    size_t plen = strlen(path);
    if (plen >= VFS_NAME_MAX) return NULL;

    // Find the last '/'
    const char *last_slash = NULL;
    for (const char *s = path; *s; s++)
        if (*s == '/') last_slash = s;

    if (last_slash && last_slash != path) {
        // e.g., "/dir/file" — parent is "/dir", name is "file"
        size_t parent_len = (size_t)(last_slash - path);
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        return last_slash + 1;
    } else if (last_slash == path && plen > 1) {
        // e.g., "/file" — parent is "/", name is "file"
        parent_path[0] = '/';
        parent_path[1] = '\0';
        return path + 1;
    } else {
        // No slash — relative path, parent is cwd
        if (!cwd) return NULL;
        size_t cwd_len = strlen(cwd);
        if (cwd_len >= VFS_NAME_MAX) return NULL;
        memcpy(parent_path, cwd, cwd_len + 1);
        return path;
    }
}

// ── Unlink a file ─────────────────────────────────────────────
int vfs_unlink(const char *path, const char *cwd)
{
    if (!path) return -EINVAL;

    char parent_path[VFS_NAME_MAX];
    const char *name = vfs_split_parent(path, cwd, parent_path);
    if (!name || *name == '\0') return -EINVAL;

    vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }
    if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->unlink) {
        vfs_node_put(parent);
        return -EROFS;
    }
    if ((uint64_t)parent->ops->unlink < 0xffff800000000000ULL) {
        vfs_node_put(parent);
        return -1;
    }

    int ret = parent->ops->unlink(parent, name);
    vfs_node_put(parent);
    return ret;
}

// ── Create a directory ───────────────────────────────────────
int vfs_mkdir(const char *path, const char *cwd)
{
    if (!path) return -EINVAL;

    char parent_path[VFS_NAME_MAX];
    const char *name = vfs_split_parent(path, cwd, parent_path);
    if (!name || *name == '\0') return -EINVAL;

    vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }
    if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->mkdir) {
        vfs_node_put(parent);
        return -EROFS;
    }
    if ((uint64_t)parent->ops->mkdir < 0xffff800000000000ULL) {
        vfs_node_put(parent);
        return -1;
    }

    vfs_node_t *newdir = parent->ops->mkdir(parent, name);
    if (!newdir) { vfs_node_put(parent); return -EEXIST; }

    // The directory was created on disk; we don't need the node ref
    vfs_node_put(newdir);
    vfs_node_put(parent);
    return 0;
}

// ── Remove an empty directory ─────────────────────────────────
int vfs_rmdir(const char *path, const char *cwd)
{
    if (!path) return -EINVAL;

    char parent_path[VFS_NAME_MAX];
    const char *name = vfs_split_parent(path, cwd, parent_path);
    if (!name || *name == '\0') return -EINVAL;

    vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }
    if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->rmdir) {
        vfs_node_put(parent);
        return -EROFS;
    }
    if ((uint64_t)parent->ops->rmdir < 0xffff800000000000ULL) {
        vfs_node_put(parent);
        return -1;
    }

    int ret = parent->ops->rmdir(parent, name);
    vfs_node_put(parent);
    return ret;
}

// ── Rename a file/directory ───────────────────────────────────
int vfs_rename(const char *oldpath, const char *newpath, const char *cwd)
{
    if (!oldpath || !newpath) return -EINVAL;

    char old_parent[VFS_NAME_MAX], new_parent[VFS_NAME_MAX];
    const char *oldname = vfs_split_parent(oldpath, cwd, old_parent);
    const char *newname = vfs_split_parent(newpath, cwd, new_parent);
    if (!oldname || *oldname == '\0' || !newname || *newname == '\0')
        return -EINVAL;

    vfs_node_t *olddir = vfs_lookup_from(old_parent, cwd);
    if (!olddir) return -ENOENT;
    if (olddir->type != VFS_DIR) { vfs_node_put(olddir); return -ENOTDIR; }

    vfs_node_t *newdir = vfs_lookup_from(new_parent, cwd);
    if (!newdir) { vfs_node_put(olddir); return -ENOENT; }
    if (newdir->type != VFS_DIR) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        return -ENOTDIR;
    }

    if (!olddir->ops || (uint64_t)olddir->ops < 0xffff800000000000ULL || !olddir->ops->rename) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        return -EROFS;
    }
    if ((uint64_t)olddir->ops->rename < 0xffff800000000000ULL) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        return -1;
    }

    int ret = olddir->ops->rename(olddir, oldname, newdir, newname);
    vfs_node_put(olddir);
    vfs_node_put(newdir);
    return ret;
}

// ── Truncate a file ──────────────────────────────────────────
int vfs_truncate(vfs_node_t *node, uint64_t new_size)
{
    if (!node) return -EINVAL;
    if (node->type != VFS_FILE) return -EISDIR;
    if (!node->ops || (uint64_t)node->ops < 0xffff800000000000ULL || !node->ops->truncate) return -EROFS;
    if ((uint64_t)node->ops->truncate < 0xffff800000000000ULL)
        return -1;

    return node->ops->truncate(node, new_size);
}

// ── Resolve full path from a VFS node ─────────────────────────
//
// Walks parent chain up to mount root, collecting names in a
// stack array, then emits mount->path + names top-down + leaf.
// Mount root's own name ("/") is skipped to avoid doubling.
//
// Returns path length (excl NUL).  Returns >= pathsz if truncated.
// Returns -1 on error (node, mount, or name is NULL).
//
// CONTRACT: path is always NUL-terminated on return, even when
// truncated.  Caller should call with pathsz = real_bufsize - 4
// to reserve room for "...\0" appended after truncation.
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz)
{
    if (!node || !node->mount || !node->name)
        return -1;

    // Collect names bottom-up (max 32 depth — far more than any
    // real path in this system; returning -1 is safer than silent
    // corruption if the limit is ever exceeded)
    const char *names[32];
    int depth = 0;
    vfs_node_t *cur = node;

    while (cur) {
        if (depth >= 32)
            return -1;  // path too deep
        names[depth++] = cur->name;
        if (!cur->parent)
            break;  // reached mount root
        cur = cur->parent;
    }
    // names[0] = leaf, names[depth-1] = mount root ("/")

    // The mount root (names[depth-1]) is "/" — skip it.
    // Build: mount->path + "/" + names[depth-2] + "/" + ... + names[0]

    size_t written = 0;
    const char *mpath = node->mount->path;

    // Helper: safely write a character — NUL-terminates on overflow
    #define PUT(c) do {                                \
        if (written < pathsz) path[written] = (c);     \
        written++;                                      \
    } while (0)
    #define PUTS(s, len) do {                           \
        for (size_t _k = 0; _k < (len); _k++)          \
            PUT((s)[_k]);                               \
    } while (0)
    #define TERM() do {                                 \
        /* At exact-fit (written==pathsz), the last byte  \
           is overwritten with NUL — 1 char lost.         \
           Inherent: pathsz bytes can't store pathsz      \
           chars + NUL. */                                \
        if (pathsz > 0)                                \
            path[(written < pathsz) ? written           \
                                    : pathsz - 1] = '\0'; \
    } while (0)

    if (mpath) {
        size_t mlen = strlen(mpath);
        // Strip trailing '/' from mount path (root mount has "/")
        // so we don't produce "//bin/init.elf".
        if (mlen > 0 && mpath[mlen - 1] == '/')
            mlen--;
        PUTS(mpath, mlen);
    }

    // Emit names from mount root's child down to leaf
    // (skip index depth-1 which is mount root "/")
    for (int i = depth - 2; i >= 0; i--) {
        PUT('/');
        size_t nlen = strlen(names[i]);
        PUTS(names[i], nlen);
    }

    TERM();
    #undef PUT
    #undef PUTS
    #undef TERM

    return (int)written;
}
