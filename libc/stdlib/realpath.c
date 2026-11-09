/*
 * realpath — resolve a path to its canonical ABSOLUTE form.
 *
 * POSIX: realpath() resolves every symbolic link in the path and returns an
 * absolute pathname.  With `resolved` == NULL the result is malloc()ed and
 * the caller owns it; with a non-NULL `resolved` the caller must supply a
 * buffer of at least PATH_MAX bytes and owns the returned pointer (it is the
 * caller's buffer, and it is free()able only if the caller allocated it).
 *
 * The resolution is COMPONENT-WISE, the way the classic implementations do
 * it:
 *
 *   - the path is split on '/'; empty components and "." are dropped;
 *   - ".." pops the last component of the resolved prefix;
 *   - every other component is lstat()ed; if it is a symbolic link the
 *     target is read back with readlink() and spliced into the path being
 *     resolved — an absolute target restarts from "/", a relative target is
 *     resolved against the directory that holds the link;
 *   - the symbolic-link budget is 40 (glibc's limit): the 41st link
 *     answers ELOOP.
 *
 * The kernel's VFS resolves symbolic links for I/O, but it does not hand
 * back the resolved PATH as a string, so the userland walk above is what
 * produces the answer.
 *
 * KNOWN LIMITATION — ".." is popped LEXICALLY, not physically: if a parent
 * directory is itself a symbolic link, realpath() here returns the path
 * through that link, while a physical resolution would have followed it.
 * glibc resolves ".." physically (it asks the kernel for the parent of the
 * resolved inode).  The divergence is only observable with a ".." that
 * crosses a symbolic link.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

/* PATH_MAX must equal the kernel's VFS_NAME_MAX (kernel/include/fs/vfs.h).
 * The libc cannot include the kernel's header, so the tie is asserted on
 * the kernel side (kernel/include/fs/vfs.h) and re-checked by the host
 * audit in scripts/audit_libc_api.py; this is the libc-side half of the
 * same contract. */
_Static_assert(PATH_MAX == 256, "PATH_MAX must match the kernel's VFS_NAME_MAX");

#define REALPATH_MAX_LINKS 40

/* Append "/comp" to `dst` (which holds `len` bytes, NUL-terminated).
 * Returns 0 on success, -1 with errno = ENAMETOOLONG when the result would
 * not fit in PATH_MAX bytes (the terminating NUL included). */
static int rp_append(char *dst, size_t len, const char *comp, size_t clen)
{
    /* dst is "/" or a directory path with no trailing slash */
    size_t need = len + 1 + clen + 1;   /* '/', the component, the NUL */
    if (need > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    dst[len] = '/';
    memcpy(dst + len + 1, comp, clen);
    dst[len + 1 + clen] = '\0';
    return 0;
}

/* Drop the last component of `dst` (which holds `len` bytes).  "/" stays
 * "/"; a relative path that has no component stays empty. */
static void rp_pop(char *dst, size_t *len)
{
    while (*len > 1 && dst[*len - 1] != '/')
        (*len)--;
    if (*len > 1)
        (*len)--;            /* drop the '/' too */
    dst[*len] = '\0';
}

char *realpath(const char *path, char *resolved)
{
    char work[PATH_MAX];
    char pending[PATH_MAX];
    char cand[PATH_MAX];
    size_t len = 0;
    size_t plen;
    int links = 0;
    int trailing_slash;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (*path == '\0') {
        errno = ENOENT;
        return NULL;
    }

    trailing_slash = (path[strlen(path) - 1] == '/');

    /* The resolved prefix starts at the root for an absolute input, and at
     * the current working directory for a relative one — POSIX requires the
     * ANSWER to be absolute either way. */
    if (path[0] == '/') {
        work[0] = '/';
        work[1] = '\0';
        len = 1;
    } else {
        if (getcwd(work, sizeof(work)) == NULL)
            return NULL;         /* errno is set by getcwd */
        len = strlen(work);
        if (len == 0) {
            work[0] = '/';
            work[1] = '\0';
            len = 1;
        }
    }

    /* The not-yet-resolved part of the path. */
    plen = strlen(path);
    if (plen >= sizeof(pending)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(pending, path, plen + 1);

    for (;;) {
        char *comp = pending;
        char *slash = strchr(comp, '/');
        size_t clen;

        if (slash) {
            *slash = '\0';
            clen = (size_t)(slash - comp);
        } else {
            clen = strlen(comp);
        }

        if (clen == 0) {
            /* an empty component (a repeated '/') — skip it */
            if (!slash) break;
            memmove(pending, slash + 1, strlen(slash + 1) + 1);
            continue;
        }
        if (clen == 1 && comp[0] == '.') {
            if (!slash) break;
            memmove(pending, slash + 1, strlen(slash + 1) + 1);
            continue;
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            rp_pop(work, &len);
            if (!slash) break;
            memmove(pending, slash + 1, strlen(slash + 1) + 1);
            continue;
        }

        /* Build the candidate and look at it. */
        if (rp_append(cand, len, comp, clen) != 0)
            return NULL;

        {
            struct stat st;
            if (lstat(cand, &st) != 0)
                return NULL;     /* errno is set by lstat (ENOENT, ENOTDIR, …) */

            if (!S_ISLNK(st.st_mode)) {
                /* An ordinary component: it becomes part of the answer. */
                if (rp_append(work, len, comp, clen) != 0)
                    return NULL;
                len = strlen(work);
                if (!slash) break;
                memmove(pending, slash + 1, strlen(slash + 1) + 1);
                continue;
            }

            /* A symbolic link: read the target back and splice it in. */
            if (++links > REALPATH_MAX_LINKS) {
                errno = ELOOP;
                return NULL;
            }
            {
                char target[PATH_MAX];
                ssize_t tlen = readlink(cand, target, sizeof(target) - 1);
                if (tlen < 0)
                    return NULL;  /* errno is set by readlink */
                if ((size_t)tlen >= sizeof(target) - 1) {
                    errno = ENAMETOOLONG;
                    return NULL;
                }
                target[tlen] = '\0';

                /* Compose the new pending string: the target, then the
                 * components that were after the link. */
                {
                    const char *rest = slash ? slash + 1 : NULL;
                    size_t tlen2 = strlen(target);
                    size_t rlen = rest ? strlen(rest) : 0;

                    if (target[0] == '/') {
                        /* An absolute target restarts from the root. */
                        work[0] = '/';
                        work[1] = '\0';
                        len = 1;
                    }
                    /* A relative target is resolved against the directory
                     * that holds the link — which is exactly `work` right
                     * now, because the link's own component has not been
                     * appended yet. */

                    if (tlen2 + (rest ? 1 + rlen : 0) + 1 > sizeof(pending)) {
                        errno = ENAMETOOLONG;
                        return NULL;
                    }
                    memcpy(pending, target, tlen2);
                    if (rest) {
                        pending[tlen2] = '/';
                        memcpy(pending + tlen2 + 1, rest, rlen + 1);
                    } else {
                        pending[tlen2] = '\0';
                    }
                }
            }
        }
    }

    /* A trailing '/' means the final component must be a directory. */
    if (trailing_slash) {
        struct stat st;
        if (stat(work, &st) != 0)
            return NULL;
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return NULL;
        }
    }

    if (resolved != NULL) {
        strcpy(resolved, work);
        return resolved;
    }

    {
        char *out = malloc(strlen(work) + 1);
        if (out == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        strcpy(out, work);
        return out;
    }
}
