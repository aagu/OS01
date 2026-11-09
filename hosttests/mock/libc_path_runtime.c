/*
 * hosttests/mock/libc_path_runtime.c — the syscall boundary of the OS01
 * path/exec host group (plan T14).
 *
 * See libc_path_runtime.h for what is simulated and how the real kernel
 * behaves.  This translation unit is compiled WITH the OS01 include path
 * (so that `struct stat` is OS01's layout) and defines the weak
 * `host_test_syscall()` hook that OS01's <sys/syscall.h> routes every
 * syscall through when OS01_HOST_TEST is defined.
 *
 * The convention is the kernel's own: a syscall returns the NEGATIVE errno
 * on failure, and the libc wrapper turns it into errno + -1.  The mock
 * therefore never touches errno itself.
 */
#define OS01_HOST_TEST 1

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <sys/stat.h>

/* The OS01 syscall numbers this mock answers.  They are spelled out
 * here instead of taken from <sys/syscall.h>, because that header
 * declares host_test_syscall WEAK — a definition in this file would
 * then be weak too and the stream runtime's strong definition would
 * win.  The mock is reached through the stream runtime's weak
 * extension point host_test_syscall_ext() instead. */
#define OS01_SYS_EXEC     5
#define OS01_SYS_STAT     16
#define OS01_SYS_READLINK 72
#define OS01_SYS_LSTAT    73
#define OS01_SYS_GETCWD   15

#include "libc_path_runtime.h"

/* NOTE: the group does NOT define `errno` itself — the stream runtime
 * (hosttests/mock/libc_stream_runtime.c, linked into this binary for the
 * production stdio) already provides the non-TLS definition the production
 * libc needs.  The mock never sets errno either: it returns the NEGATIVE
 * errno the way the kernel does, and the libc wrapper turns it into
 * errno + -1. */

#define PATH_RT_MAX_ENTRIES 128
#define PATH_RT_MAX_PATH    256
#define PATH_RT_MAX_EXEC    16
#define PATH_RT_MAX_ARGV    16
#define PATH_RT_MAX_ENV     16
/* The command line the REAL shell mode builds. */
#define PATH_RT_MAX_CMD     1024
/* The KERNEL's own limit on the number of symbolic links it will follow in
 * ONE path resolution: MAXSYMLINKS is 8 in kernel/include/fs/vfs.h.  The
 * mock enforces exactly that, so the difference between the kernel's
 * one-shot resolution and the libc's component-wise walk is observable. */
#define PATH_RT_MAX_LINKS   8

/* The host's system(3).  OS01's <stdlib.h> does not declare it (the libc
 * has no system()), so the declaration is spelled out here; the host libc
 * provides the symbol. */
extern int system(const char *command);

struct path_rt_entry {
    char     path[PATH_RT_MAX_PATH];
    unsigned mode;
    char     target[PATH_RT_MAX_PATH];
};

static struct path_rt_entry g_entries[PATH_RT_MAX_ENTRIES];
static int g_nentries;
static char g_cwd[PATH_RT_MAX_PATH] = "/cwd";

static struct {
    char path[PATH_RT_MAX_PATH];
    char argv0[PATH_RT_MAX_PATH];
    int  argc;
    char argv[PATH_RT_MAX_ARGV][PATH_RT_MAX_PATH];
    int  envc;
    char env[PATH_RT_MAX_ENV][PATH_RT_MAX_PATH];
} g_exec[PATH_RT_MAX_EXEC];
static int g_nexec;

static int g_exec_errno = ENOENT;
static char g_exec_errno_path[PATH_RT_MAX_PATH];
static int g_exec_errno_specific;
static int g_real_shell;

void path_rt_reset(void)
{
    g_nentries = 0;
    g_nexec = 0;
    g_exec_errno = ENOENT;
    g_exec_errno_path[0] = '\0';
    g_exec_errno_specific = ENOENT;
    g_real_shell = 0;
    strcpy(g_cwd, "/cwd");
    /* the root always exists and is a directory */
    path_rt_add("/", S_IFDIR, NULL);
}

void path_rt_set_real_shell(int on)
{
    g_real_shell = on;
}

/* Build the command line the REAL shell mode runs: "/bin/sh" followed by
 * the argv[1..] the libc handed to the shell, each single-quoted.  The
 * quoting is what makes the difference observable: a bare script name is
 * resolved by the shell relative to the CURRENT DIRECTORY, while a path
 * is resolved as a path. */
static void path_rt_build_cmd(char *buf, size_t size, char *const argv[])
{
    static const char shell[] = "/bin/sh";
    size_t used;
    int i;

    used = 0;
    for (i = 0; i < (int)sizeof(shell) - 1; i++)
        buf[used++] = shell[i];
    buf[used] = '\0';

    for (i = 1; argv != NULL && argv[i] != NULL && used + 4 < size; i++) {
        const char *p = argv[i];

        buf[used++] = ' ';
        buf[used++] = '\'';
        while (*p != '\0' && used + 4 < size) {
            if (*p == '\'') {
                /* close the quote, escape a quote, open it again */
                buf[used++] = '\'';
                buf[used++] = '\\';
                buf[used++] = '\'';
                buf[used++] = '\'';
            } else {
                buf[used++] = *p;
            }
            p++;
        }
        buf[used++] = '\'';
        buf[used] = '\0';
    }
}

void path_rt_set_cwd(const char *cwd)
{
    if (cwd == NULL)
        return;
    strncpy(g_cwd, cwd, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
}

void path_rt_add(const char *path, unsigned mode, const char *target)
{
    struct path_rt_entry *e;

    if (path == NULL || g_nentries >= PATH_RT_MAX_ENTRIES)
        return;
    e = &g_entries[g_nentries++];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    e->mode = mode;
    if (target != NULL) {
        strncpy(e->target, target, sizeof(e->target) - 1);
        e->target[sizeof(e->target) - 1] = '\0';
    } else {
        e->target[0] = '\0';
    }
}

void path_rt_set_exec_errno(int err)
{
    g_exec_errno = err;
    g_exec_errno_path[0] = '\0';
}

void path_rt_set_exec_errno_for(const char *path, int err)
{
    if (path == NULL)
        return;
    strncpy(g_exec_errno_path, path, sizeof(g_exec_errno_path) - 1);
    g_exec_errno_path[sizeof(g_exec_errno_path) - 1] = '\0';
    /* the GLOBAL errno is NOT changed: the other candidates keep it */
    g_exec_errno_specific = err;
}

int path_rt_exec_count(void) { return g_nexec; }

const char *path_rt_exec_path(int n)
{
    if (n < 0 || n >= g_nexec)
        return NULL;
    return g_exec[n].path;
}

const char *path_rt_exec_argv0(int n)
{
    if (n < 0 || n >= g_nexec)
        return NULL;
    return g_exec[n].argv0;
}

int path_rt_exec_argc(int n)
{
    if (n < 0 || n >= g_nexec)
        return 0;
    return g_exec[n].argc;
}

const char *path_rt_exec_argv(int n, int i)
{
    if (n < 0 || n >= g_nexec || i < 0 || i >= g_exec[n].argc)
        return NULL;
    return g_exec[n].argv[i];
}

int path_rt_exec_envc(int n)
{
    if (n < 0 || n >= g_nexec)
        return 0;
    return g_exec[n].envc;
}

const char *path_rt_exec_env(int n, int i)
{
    if (n < 0 || n >= g_nexec || i < 0 || i >= g_exec[n].envc)
        return NULL;
    return g_exec[n].env[i];
}

/* ── the fake filesystem ────────────────────────────────────────────── */

static const struct path_rt_entry *path_rt_find(const char *path)
{
    for (int i = 0; i < g_nentries; i++) {
        if (strcmp(g_entries[i].path, path) == 0)
            return &g_entries[i];
    }
    return NULL;
}

/* Resolve `path` to the entry it names.
 *
 * The fake filesystem is a FLAT table of whole paths, so the resolution is
 * a walk over the components: every prefix must be in the table.  When a
 * prefix is a symbolic link it is followed (that is what the kernel's
 * LOOKUP_FOLLOW does for an intermediate component); the FINAL component
 * is followed only when `follow` is 1 — the kernel's SYS_lstat is
 * NOFOLLOW and reports the link itself, while SYS_stat follows it.
 *
 * The number of links that may be followed in ONE resolution is the
 * kernel's MAXSYMLINKS (8, kernel/include/fs/vfs.h): the mock enforces
 * exactly that, so the difference between the kernel's one-shot
 * resolution and the libc's component-wise walk is observable. */
static const struct path_rt_entry *path_rt_resolve(const char *path, int follow,
                                                    int *err)
{
    char work[PATH_RT_MAX_PATH];
    int links = 0;

    *err = 0;
    strncpy(work, path, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    for (;;) {
        char prefix[PATH_RT_MAX_PATH];
        size_t plen = 0;
        const char *p = work;
        const char *last_slash = NULL;
        int is_last;

        /* absolute-ise */
        if (work[0] != '/') {
            char abs[PATH_RT_MAX_PATH];
            snprintf(abs, sizeof(abs), "/%s", work);
            strncpy(work, abs, sizeof(work) - 1);
            work[sizeof(work) - 1] = '\0';
            p = work;
        }

        /* find the LAST component boundary */
        {
            const char *q;
            for (q = work; *q; q++) {
                if (*q == '/')
                    last_slash = q;
            }
        }
        (void)last_slash;

        /* Walk the components; the prefix grows one component at a time. */
        {
            const char *comp = work;
            while (*comp == '/')
                comp++;
            while (*comp != '\0') {
                const char *slash = strchr(comp, '/');
                size_t clen = slash ? (size_t)(slash - comp) : strlen(comp);
                const struct path_rt_entry *e;

                if (plen == 0)
                    prefix[plen++] = '/';
                else
                    prefix[plen++] = '/';
                memcpy(prefix + plen, comp, clen);
                plen += clen;
                prefix[plen] = '\0';

                e = path_rt_find(prefix);
                if (e == NULL) {
                    *err = ENOENT;
                    return NULL;
                }

                is_last = (slash == NULL);
                if (S_ISLNK(e->mode) && (!is_last || follow)) {
                    char next[PATH_RT_MAX_PATH];
                    char dir[PATH_RT_MAX_PATH];

                    if (++links > PATH_RT_MAX_LINKS) {
                        *err = ELOOP;
                        return NULL;
                    }
                    /* the target is relative to the directory that holds
                     * the link — that is the prefix without its last
                     * component */
                    strncpy(dir, prefix, sizeof(dir) - 1);
                    dir[sizeof(dir) - 1] = '\0';
                    {
                        char *s2 = strrchr(dir, '/');
                        if (s2 != NULL) {
                            if (s2 == dir)
                                dir[1] = '\0';
                            else
                                *s2 = '\0';
                        }
                    }
                    if (e->target[0] == '/') {
                        strncpy(next, e->target, sizeof(next) - 1);
                        next[sizeof(next) - 1] = '\0';
                    } else {
                        strncpy(next, dir, sizeof(next) - 1);
                        next[sizeof(next) - 1] = '\0';
                        strncat(next, "/", sizeof(next) - strlen(next) - 1);
                        strncat(next, e->target, sizeof(next) - strlen(next) - 1);
                        next[strlen(next)] = '\0';
                    }
                    if (slash != NULL) {
                        strncat(next, slash, sizeof(next) - strlen(next) - 1);
                        next[strlen(next)] = '\0';
                    }
                    strncpy(work, next, sizeof(work) - 1);
                    work[sizeof(work) - 1] = '\0';
                    plen = 0;
                    prefix[0] = '\0';
                    goto restart;
                }
                if (!is_last && !S_ISDIR(e->mode)) {
                    *err = ENOTDIR;
                    return NULL;
                }
                if (is_last)
                    return e;

                comp = slash + 1;
                while (*comp == '/')
                    comp++;
            }
        }
        /* the root itself: "/" has no component to walk */
        {
            const struct path_rt_entry *e = path_rt_find("/");
            if (e == NULL) {
                *err = ENOENT;
                return NULL;
            }
            return e;
        }
    restart:;
    }
    *err = ENOENT;
    return NULL;
}

static void path_rt_fill_stat(struct stat *st, unsigned mode)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = mode;
    st->st_nlink = 1;
    st->st_size = 0;
}

/* ── the syscall hook ───────────────────────────────────────────────── */

int64_t host_test_syscall_ext(uint64_t nr, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3)
{
    switch (nr) {
    case OS01_SYS_LSTAT: {
        const char *path = (const char *)(uintptr_t)arg1;
        struct stat *st = (struct stat *)(uintptr_t)arg2;
        int err;
        const struct path_rt_entry *e = path_rt_resolve(path, 0, &err);

        if (e == NULL)
            return -err;
        path_rt_fill_stat(st, e->mode);
        return 0;
    }
    case OS01_SYS_STAT: {
        const char *path = (const char *)(uintptr_t)arg1;
        struct stat *st = (struct stat *)(uintptr_t)arg2;
        int err;
        const struct path_rt_entry *e = path_rt_resolve(path, 1, &err);

        if (e == NULL)
            return -err;
        path_rt_fill_stat(st, e->mode);
        return 0;
    }
    case OS01_SYS_READLINK: {
        const char *path = (const char *)(uintptr_t)arg1;
        char *buf = (char *)(uintptr_t)arg2;
        size_t bufsize = (size_t)arg3;
        int err;
        const struct path_rt_entry *e = path_rt_resolve(path, 0, &err);

        if (e == NULL)
            return -err;
        if (!S_ISLNK(e->mode))
            return -EINVAL;       /* the kernel's answer for a non-link */
        {
            size_t len = strlen(e->target);
            if (len > bufsize)
                len = bufsize;
            memcpy(buf, e->target, len);
            return (int64_t)len;
        }
    }
    case OS01_SYS_GETCWD: {
        char *buf = (char *)(uintptr_t)arg1;
        size_t size = (size_t)arg2;
        size_t len = strlen(g_cwd);

        if (size < len + 1)
            return -ERANGE;
        memcpy(buf, g_cwd, len + 1);
        return (int64_t)(uintptr_t)buf;
    }
    case OS01_SYS_EXEC: {
        const char *path = (const char *)(uintptr_t)arg1;
        char *const *argv = (char *const *)(uintptr_t)arg2;
        char *const *envp = (char *const *)(uintptr_t)arg3;

        if (g_nexec < PATH_RT_MAX_EXEC) {
            int n = g_nexec++;
            strncpy(g_exec[n].path, path ? path : "",
                    sizeof(g_exec[n].path) - 1);
            g_exec[n].path[sizeof(g_exec[n].path) - 1] = '\0';
            g_exec[n].argc = 0;
            g_exec[n].envc = 0;
            g_exec[n].argv0[0] = '\0';
            if (argv != NULL) {
                for (int i = 0; i < PATH_RT_MAX_ARGV && argv[i] != NULL; i++) {
                    strncpy(g_exec[n].argv[i], argv[i],
                            sizeof(g_exec[n].argv[i]) - 1);
                    g_exec[n].argv[i][sizeof(g_exec[n].argv[i]) - 1] = '\0';
                    g_exec[n].argc = i + 1;
                }
            }
            if (g_exec[n].argc > 0)
                strncpy(g_exec[n].argv0, g_exec[n].argv[0],
                        sizeof(g_exec[n].argv0) - 1);
            if (envp != NULL) {
                for (int i = 0; i < PATH_RT_MAX_ENV && envp[i] != NULL; i++) {
                    strncpy(g_exec[n].env[i], envp[i],
                            sizeof(g_exec[n].env[i]) - 1);
                    g_exec[n].env[i][sizeof(g_exec[n].env[i]) - 1] = '\0';
                    g_exec[n].envc = i + 1;
                }
            }
        }
        /* The REAL shell mode: an execve("/bin/sh", argv, envp) attempt is
         * handed to the HOST's shell, so the script path is resolved by a
         * real POSIX shell in a real directory layout.  The mock then
         * answers ENOEXEC — the libc only ever sees the shell's exit, and
         * by then the script has already run. */
        if (g_real_shell && path != NULL && strcmp(path, "/bin/sh") == 0 &&
            argv != NULL && argv[1] != NULL) {
            char cmd[PATH_RT_MAX_CMD];

            path_rt_build_cmd(cmd, sizeof(cmd), argv);
            (void)system(cmd);
            return -ENOEXEC;
        }

        /* The exec NEVER succeeds in the mock: it always answers the
         * armed errno, so that execvp() can be observed walking the PATH. */
        if (g_exec_errno_path[0] != '\0' &&
            path != NULL && strcmp(g_exec_errno_path, path) == 0)
            return -g_exec_errno_specific;
        return -g_exec_errno;
    }
    default:
        /* not handled here: let the stream boundary deal with it */
        return INT64_MIN;
    }
}
