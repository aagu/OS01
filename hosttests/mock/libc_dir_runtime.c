/*
 * libc_dir_runtime.c — the fake DIRECTORY boundary for the `dir` host
 * group (T15).
 *
 * WHAT IS SIMULATED: the kernel's directory interface — open(2),
 * fstat(2), getdents64(2) and close(2) — over a small, explicit set of
 * fake paths.  The PRODUCTION libc/unistd/dirent.c and
 * libc/stdlib/scandir.c are linked unchanged; only the syscall boundary
 * is replaced.
 *
 * WHAT THE REAL KERNEL DOES:
 *   - open(2) on a DIRECTORY succeeds read-only; on a REGULAR FILE it
 *     ALSO succeeds (a file is not a directory, but open() does not
 *     care) — so opendir() has to check the type with fstat() itself.
 *   - fstat(2) fills the same struct stat the libc sees.
 *   - getdents64(2) fills the buffer with struct linux_dirent64 records.
 *     The kernel writes reclen = round8(sizeof(struct linux_dirent64) +
 *     namelen + 1) and puts the name at offsetof(..., d_name) == 19; a
 *     Linux kernel uses round8(19 + namelen + 1) for reclen and the same
 *     offset for the name.
 *   - close(2) closes the descriptor.
 *
 * The mock injects the two failure modes the libc must survive:
 *   - a getdents64 error (the libc must set errno), and
 *   - a CORRUPTED record — a length that lies about the record, or a
 *     name that is not NUL-terminated inside the record (the libc must
 *     answer EIO instead of walking off the buffer).
 *
 * Descriptors: the fake descriptors live in a private range (50+) so
 * they cannot collide with the host's real ones; a descriptor the mock
 * does not own is passed through to the host, so the framework's own
 * stdio keeps working.
 */
#define OS01_HOST_TEST 1

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "libc_dir_runtime.h"

/* The kernel's syscall numbers (libc/include/sys/syscall.h). */
#define OS01_SYS_OPEN     7
#define OS01_SYS_CLOSE    8
#define OS01_SYS_FSTAT   17
#define OS01_SYS_GETDENTS64 21

/* The record header: the flexible array member starts right after
 * d_type, i.e. at offset 19; the struct's sizeof() is padded to 24. */
#define DIR_RT_NAME_OFFSET 19

#define DIR_RT_MAX_PATHS  8
#define DIR_RT_MAX_ENTRY  8
#define DIR_RT_MAX_FDS    8

struct dir_rt_entry {
    char        name[64];
    unsigned char type;
    uint64_t     d_off;
};

struct dir_rt_path {
    char           path[64];
    int            is_dir;
    int            nentries;
    struct dir_rt_entry entries[DIR_RT_MAX_ENTRY];
    int            getdents_err;   /* negative errno, 0 = no error */
    int            bad_mode;       /* 0 = fine, see below */
};

struct dir_rt_fd {
    int      used;
    int      path_index;
    int      host_fd;      /* the real host descriptor handed out */
    int      closed;
    unsigned pos;
};

static struct dir_rt_path g_paths[DIR_RT_MAX_PATHS];
static int g_npaths;
static struct dir_rt_fd g_fds[DIR_RT_MAX_FDS];

/* ── the fake tree ───────────────────────────────────────────────── */

void dir_rt_reset(void)
{
    memset(g_paths, 0, sizeof(g_paths));
    memset(g_fds, 0, sizeof(g_fds));
    g_npaths = 0;
}

void dir_rt_add_path(const char *path, int is_dir)
{
    struct dir_rt_path *p;

    if (g_npaths >= DIR_RT_MAX_PATHS)
        return;
    p = &g_paths[g_npaths++];
    memset(p, 0, sizeof(*p));
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->path[sizeof(p->path) - 1] = '\0';
    p->is_dir = is_dir;
}

void dir_rt_add_entry(const char *path, const char *name, unsigned char type,
                      uint64_t d_off)
{
    struct dir_rt_path *p = NULL;
    struct dir_rt_entry *e;
    int i;

    for (i = 0; i < g_npaths; i++)
        if (strcmp(g_paths[i].path, path) == 0)
            p = &g_paths[i];
    if (!p || p->nentries >= DIR_RT_MAX_ENTRY)
        return;
    e = &p->entries[p->nentries++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->type = type;
    e->d_off = d_off;
}

void dir_rt_set_getdents_error(const char *path, int err)
{
    int i;

    for (i = 0; i < g_npaths; i++)
        if (strcmp(g_paths[i].path, path) == 0)
            g_paths[i].getdents_err = err;
}

void dir_rt_set_bad_record(const char *path, int mode)
{
    int i;

    for (i = 0; i < g_npaths; i++)
        if (strcmp(g_paths[i].path, path) == 0)
            g_paths[i].bad_mode = mode;
}

static int dir_rt_fd_index(int fd);

/* Is this one of the mock's descriptors? */
int dir_rt_fd_is_ours(int fd)
{
    return dir_rt_fd_index(fd) >= 0;
}

/* ── the syscall boundary ────────────────────────────────────────── */

static int dir_rt_find_path(const char *path)
{
    int i;

    for (i = 0; i < g_npaths; i++)
        if (strcmp(g_paths[i].path, path) == 0)
            return i;
    return -1;
}

/* The fake descriptor is a REAL host descriptor (a dup of stdin): the
 * group observes the descriptor's lifetime with the host's close(), so
 * "closedir() closed it" and "the failed fdopendir() did NOT close it"
 * are both checked against the real descriptor.  dup() is used because
 * open() is the production wrapper under test here. */
static int dir_rt_alloc_fd(int path_index)
{
    int i;
    int fd;

    for (i = 0; i < DIR_RT_MAX_FDS; i++) {
        if (!g_fds[i].used) {
            fd = dup(0);
            if (fd < 0)
                return -1;
            g_fds[i].used = 1;
            g_fds[i].path_index = path_index;
            g_fds[i].host_fd = fd;
            g_fds[i].closed = 0;
            g_fds[i].pos = 0;
            return fd;
        }
    }
    return -1;
}

static int dir_rt_fd_index(int fd)
{
    int i;

    for (i = 0; i < DIR_RT_MAX_FDS; i++)
        if (g_fds[i].used && g_fds[i].host_fd == fd)
            return i;
    return -1;
}

/* Fill the buffer with the records of one directory.  The record layout
 * follows the OS01 kernel: the name at offset 19, reclen =
 * round8(sizeof(struct linux_dirent64) + namelen + 1). */
static int64_t dir_rt_fill(struct dir_rt_fd *f, char *buf, unsigned count)
{
    struct dir_rt_path *p = &g_paths[f->path_index];
    unsigned written = 0;

    while (f->pos < (unsigned)p->nentries) {
        struct dir_rt_entry *e = &p->entries[f->pos];
        size_t namelen = strlen(e->name);
        size_t reclen = (24 + namelen + 1 + 7) & ~(size_t)7;
        char *rec = buf + written;
        struct linux_dirent64 *d;

        if (written + reclen > count)
            break;

        memset(rec, 0, reclen);
        d = (struct linux_dirent64 *)(void *)rec;
        d->d_ino = 1000 + f->pos;
        d->d_off = (int64_t)e->d_off;
        d->d_type = e->type;
        d->d_reclen = (unsigned short)reclen;

        if (p->bad_mode == 2) {
            /* The name is NOT NUL-terminated inside the record: the
             * whole name area is filled with a non-NUL byte. */
            memset(rec + DIR_RT_NAME_OFFSET, 'x', reclen - DIR_RT_NAME_OFFSET);
        } else {
            memcpy(rec + DIR_RT_NAME_OFFSET, e->name, namelen + 1);
        }

        if (p->bad_mode == 1) {
            /* The record length LIES: it claims more bytes than the
             * kernel wrote.  The libc must notice and answer EIO
             * instead of reading past the end of the buffer. */
            d->d_reclen = (unsigned short)(reclen + 4096);
        } else if (p->bad_mode == 3) {
            /* The record length is smaller than the record header. */
            d->d_reclen = 8;
        }

        written += (unsigned)reclen;
        f->pos++;
    }

    return (int64_t)written;
}

int64_t host_test_syscall_ext(uint64_t nr, uint64_t arg1, uint64_t arg2,
                              uint64_t arg3)
{
    switch (nr) {
    case OS01_SYS_OPEN: {
        const char *path = (const char *)(uintptr_t)arg1;
        int idx;

        if (path == NULL)
            return INT64_MIN;
        idx = dir_rt_find_path(path);
        if (idx < 0)
            return INT64_MIN;       /* not ours: the host handles it */
        {
            int fd = dir_rt_alloc_fd(idx);
            if (fd < 0)
                return -EMFILE;
            return (int64_t)fd;
        }
    }
    case OS01_SYS_FSTAT: {
        int fd = (int)arg1;
        struct stat *st = (struct stat *)(uintptr_t)arg2;
        int idx = dir_rt_fd_index(fd);

        if (idx < 0)
            return INT64_MIN;
        memset(st, 0, sizeof(*st));
        st->st_mode = g_paths[g_fds[idx].path_index].is_dir ? S_IFDIR : S_IFREG;
        st->st_nlink = 1;
        st->st_size = 0;
        return 0;
    }
    case OS01_SYS_GETDENTS64: {
        int fd = (int)arg1;
        char *buf = (char *)(uintptr_t)arg2;
        unsigned count = (unsigned)arg3;
        int idx = dir_rt_fd_index(fd);

        if (idx < 0)
            return INT64_MIN;
        if (g_paths[g_fds[idx].path_index].getdents_err != 0)
            return (int64_t)g_paths[g_fds[idx].path_index].getdents_err;
        return dir_rt_fill(&g_fds[idx], buf, count);
    }
    default:
        return INT64_MIN;
    }
}
