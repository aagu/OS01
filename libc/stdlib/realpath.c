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
 * ".." applies to the RESOLVED prefix, not to the text of the input: the
 * walk resolves each component's symbolic link as it goes, and the ".."
 * then pops the last component of the path resolved SO FAR.  E.g.
 * "/cwd/par/.." with "/cwd/par" a link to the directory "/other" answers
 * "/" — the parent of "/other", not of "/cwd/par".  That is the answer
 * glibc gives as well: glibc's realpath() walks the components the same
 * way and pops the resolved prefix, so there is NO divergence here.
 *
 * (An earlier revision of this file claimed that the ".." was popped
 * LEXICALLY and that this diverged from glibc.  That claim was WRONG:
 * the link is resolved BEFORE the ".." is applied.  The pinned case is
 * hosttests/cases/test_libc_path.c,
 * test_realpath_dotdot_applies_to_the_resolved_path.)
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/* The user-space implementation needs the path syscalls.  <unistd.h> and
 * <sys/stat.h> pull in <sys/syscall.h>, which carries x86_64 inline asm and
 * cannot be compiled into the aarch64 kernel-side library, so they are
 * included for the USER libc only. */
#ifndef __is_libk
#include <unistd.h>
#include <sys/stat.h>
#endif

#ifdef __is_libk

/* The kernel-side library (libk) keeps the HISTORICAL surface: the symbol
 * is exported, but it is the empty shell that has always been there — it
 * answers NULL.  T14 moved that shell out of malloc.c and replaced it with
 * the real user-space implementation; keeping the shell here
 *
 *   - preserves the frozen libk surface on BOTH profiles (the audit's
 *     "libk lost an already-supported export" check stays meaningful), and
 *   - keeps the kernel archive free of an object that would issue USER
 *     syscalls from ring 0 if the kernel ever referenced it.
 *
 * The aarch64 libk subset lists this file explicitly for exactly this
 * reason (see libc/Makefile). */
char *realpath(const char *path, char *resolved)
{
    (void)path;
    (void)resolved;
    return NULL;
}

#else

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
    /* dst is "/" or a directory path with no trailing slash.  The root
     * already ends in a '/', so the separator is not repeated there —
     * otherwise the root would grow a "//" on every component. */
    size_t pos = len;
    size_t need;

    if (len == 0 || dst[len - 1] != '/') {
        need = len + 1 + clen + 1;       /* '/', the component, the NUL */
        if (need > PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        dst[pos++] = '/';
    } else {
        need = len + clen + 1;           /* the component, the NUL */
        if (need > PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    memcpy(dst + pos, comp, clen);
    dst[pos + clen] = '\0';
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

        /* Build the candidate and look at it.  rp_append() expects its
         * destination to ALREADY hold the prefix, so the resolved prefix
         * is copied in first — cand is an uninitialised buffer. */
        memcpy(cand, work, len + 1);
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
                    /* `rest` points INTO `pending`, and the composition
                     * below overwrites `pending` — so the remainder is
                     * copied out FIRST, or the copy would read the bytes
                     * the first memcpy had just replaced. */
                    char restbuf[PATH_MAX];
                    const char *rest = NULL;
                    size_t tlen2 = strlen(target);
                    size_t rlen = 0;

                    if (slash != NULL) {
                        rlen = strlen(slash + 1);
                        if (rlen >= sizeof(restbuf))
                            rlen = sizeof(restbuf) - 1;
                        memcpy(restbuf, slash + 1, rlen);
                        restbuf[rlen] = '\0';
                        rest = restbuf;
                    }

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

#endif /* !__is_libk */
