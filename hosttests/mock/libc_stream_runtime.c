/*
 * hosttests/mock/libc_stream_runtime.c — the descriptor boundary of the
 * OS01 libc stream-state group (T5).
 *
 * This translation unit is deliberately compiled WITHOUT the OS01 libc
 * include directory, so it sees the HOST's <unistd.h> and can use the
 * host's `syscall()` entry to reach descriptors the script does not
 * cover.  The production objects under test keep their own headers; the
 * ABI of read()/write()/close() is identical on both sides.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "libc_stream_runtime.h"

/* The production libc declares `extern int errno;` and its own
 * definition (libc/errno/errno.c) is not linked into a host test, so
 * exactly one plain errno object must exist in the binary. */
int errno = 0;

/* glibc keeps errno in thread-local storage; the host's raw syscall()
 * entry updates THAT copy, while the production libc reads the plain
 * object above.  Copy it across after a failed raw call so the
 * production code observes the real error. */
extern int *__errno_location(void);

#define MAX_TRACKED_FD 128
#define CAPTURE_CAP    2048

struct desc_state {
    /* A virtual descriptor has no host counterpart: the mock answers
     * every access itself, so a test can use a descriptor number that
     * the host does not know about. */
    int         virtual_fd;
    int         scripted_read;
    const char *rdata;
    size_t      rlen;
    size_t      rpos;
    int         rerr;
    int         rerr_once;

    int  capture_write;
    int  werr;
    int  werr_once;
    size_t budget;            /* 0 = no budget; else cumulative cap */
    size_t budget_used;
    char wbuf[CAPTURE_CAP];
    size_t wlen;

    int cerr;                 /* close fails with this when non-zero */
    int closes;
};

static struct desc_state g_desc[MAX_TRACKED_FD];

static struct desc_state *desc_for(int fd)
{
    if (fd < 0 || fd >= MAX_TRACKED_FD) return NULL;
    return &g_desc[fd];
}

/* ── The raw host syscall ────────────────────────────────────────────
 *
 * The mock REPLACES read()/write()/close(), so the host's own versions
 * cannot be called by name any more.  The raw syscall entry reaches the
 * same kernel without the recursion.
 */
static long raw_syscall(long nr, long a1, long a2, long a3)
{
    long r = syscall(nr, a1, a2, a3);
    if (r < 0)
        errno = *__errno_location();
    return r;
}

/* ── The replaced boundary ─────────────────────────────────────────── */
ssize_t read(int fd, void *buf, size_t count)
{
    struct desc_state *d = desc_for(fd);
    if (d && d->rerr_once) {
        int e = d->rerr_once;
        d->rerr_once = 0;
        errno = e;
        return -1;
    }
    if (d && d->rerr) {
        errno = d->rerr;
        return -1;
    }
    if (d && (d->scripted_read || d->virtual_fd)) {
        size_t avail = d->rlen - d->rpos;
        size_t n = (count < avail) ? count : avail;
        if (n > 0)
            memcpy(buf, d->rdata + d->rpos, n);
        d->rpos += n;
        return (ssize_t)n;
    }
    return (ssize_t)raw_syscall(SYS_read, fd, (long)buf, (long)count);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    struct desc_state *d = desc_for(fd);
    if (d && d->werr_once) {
        int e = d->werr_once;
        d->werr_once = 0;
        errno = e;
        return -1;
    }
    if (d && d->werr) {
        errno = d->werr;
        return -1;
    }
    if (d && d->budget) {
        if (d->budget_used >= d->budget) {
            errno = EIO;
            return -1;
        }
        size_t room = d->budget - d->budget_used;
        size_t n = (count < room) ? count : room;
        if (d->capture_write && n > 0) {
            size_t cap_room = CAPTURE_CAP - d->wlen;
            size_t cn = (n < cap_room) ? n : cap_room;
            memcpy(d->wbuf + d->wlen, buf, cn);
            d->wlen += cn;
        }
        d->budget_used += n;
        return (ssize_t)n;
    }
    if (d && d->capture_write) {
        size_t room = CAPTURE_CAP - d->wlen;
        size_t n = (count < room) ? count : room;
        if (n > 0)
            memcpy(d->wbuf + d->wlen, buf, n);
        d->wlen += n;
        return (ssize_t)count;
    }
    if (d && d->virtual_fd) {
        /* A virtual descriptor accepts the bytes and discards them. */
        return (ssize_t)count;
    }
    /* Not scripted: pass the write on to the real descriptor.  The
     * framework's own output (fd 1 / fd 2) must reach the terminal, so
     * the write is forwarded rather than swallowed. */
    return (ssize_t)raw_syscall(SYS_write, fd, (long)buf, (long)count);
}

int close(int fd)
{
    struct desc_state *d = desc_for(fd);
    if (d)
        d->closes++;
    if (d && d->cerr) {
        errno = d->cerr;
        return -1;
    }
    if (d && d->virtual_fd)
        return 0;
    return (int)raw_syscall(SYS_close, fd, 0, 0);
}

/* ── The syscall boundary hook ────────────────────────────────────── */
int64_t host_test_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    switch (nr) {
    case 19:   /* OS01 SYS_fcntl — the libc's fcntl() wrapper is under
                * test, so its syscall must reach a REAL descriptor
                * control backend.  Redirect to the HOST kernel's fcntl
                * number (this file compiles against the HOST's
                * <sys/syscall.h>, so SYS_fcntl here is the host's,
                * e.g. 72 on x86_64 — NOT the OS01 19 the switch saw).
                * Calling fcntl() by name would recurse into the linked
                * production wrapper, so the raw syscall is used, and
                * the host's -1/errno answer is translated into the
                * kernel convention the wrapper expects (negative errno
                * as the value). */
    {
        long r = syscall(SYS_fcntl, (int)arg1, (int)arg2, (int)arg3);
        if (r < 0)
            return -(int64_t)*__errno_location();
        return (int64_t)r;
    }
    case 11:   /* SYS_fork — the test forks a child for the checks that
                * close a standard stream (closing fd 1 would take the
                * test's own output down with it). */
        return (int64_t)fork();
    default:
        (void)arg1; (void)arg2; (void)arg3;
        return -38;   /* -ENOSYS */
    }
}

/* ── The scripting API ────────────────────────────────────────────── */
void stream_rt_reset(void)
{
    for (int i = 0; i < MAX_TRACKED_FD; i++) {
        struct desc_state *d = &g_desc[i];
        d->virtual_fd = 0;
        d->scripted_read = 0;
        d->rdata = NULL;
        d->rlen = 0;
        d->rpos = 0;
        d->rerr = 0;
        d->rerr_once = 0;
        d->capture_write = 0;
        d->werr = 0;
        d->werr_once = 0;
        d->budget = 0;
        d->budget_used = 0;
        d->cerr = 0;
        d->wlen = 0;
        d->closes = 0;
    }
}

void stream_rt_virtual(int fd)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->virtual_fd = 1;
}

void stream_rt_script_read(int fd, const char *data, size_t len)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->scripted_read = 1;
    d->rdata = data;
    d->rlen = len;
    d->rpos = 0;
    d->rerr = 0;
}

void stream_rt_fail_read_once(int fd, int err)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->rerr_once = err;
}

void stream_rt_fail_read(int fd, int err)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->scripted_read = 1;
    d->rdata = NULL;
    d->rlen = 0;
    d->rpos = 0;
    d->rerr = err;
}

void stream_rt_fail_write(int fd, int err)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->werr = err;
}

void stream_rt_fail_write_once(int fd, int err)
{
    struct desc_state *d = desc_for(fd);
    if (d) d->werr_once = err;
}

void stream_rt_write_budget(int fd, size_t budget)
{
    struct desc_state *d = desc_for(fd);
    if (d) {
        d->budget = budget;
        d->budget_used = 0;
    }
}

void stream_rt_fail_close(int fd, int err)
{
    struct desc_state *d = desc_for(fd);
    if (d) d->cerr = err;
}

void stream_rt_capture_write(int fd)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->capture_write = 1;
    d->wlen = 0;
}

const char *stream_rt_written(int fd, size_t *len)
{
    struct desc_state *d = desc_for(fd);
    if (len) *len = 0;
    if (!d || d->wlen == 0) return NULL;
    if (len) *len = d->wlen;
    return d->wbuf;
}

void stream_rt_clear_written(int fd)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->wlen = 0;
}

int stream_rt_close_count(int fd)
{
    struct desc_state *d = desc_for(fd);
    return d ? d->closes : 0;
}

void stream_rt_clear_close_count(int fd)
{
    struct desc_state *d = desc_for(fd);
    if (!d) return;
    d->closes = 0;
}

void stream_rt_emit(const char *text, size_t len)
{
    if (!text || len == 0) return;
    (void)raw_syscall(SYS_write, 1, (long)text, (long)len);
}
