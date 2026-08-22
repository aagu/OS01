#define _GNU_SOURCE
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>
#include "stdio_test_shims.h"

#define MAX_RECS 2048

/* Shared errno storage for the host test (mirrors the real OS libc errno). */
int errno = 0;

static write_rec_t g_recs[MAX_RECS];
static int         g_nrecs = 0;
static write_mode_t g_mode = WRITE_MODE_NORMAL;
static int         g_short = 0;
static int         g_armed = 0;

void shim_write_reset(void)
{
    g_nrecs = 0;
    g_mode  = WRITE_MODE_NORMAL;
    g_short = 0;
    g_armed = 0;
}

void shim_write_push(write_mode_t mode, int short_len)
{
    g_mode  = mode;
    g_short = short_len;
    g_armed = 1;
}

int shim_write_count(void) { return g_nrecs; }

const write_rec_t *shim_write_rec(int idx)
{
    return (idx >= 0 && idx < g_nrecs) ? &g_recs[idx] : NULL;
}

long shim_write_total_bytes(void)
{
    long t = 0;
    for (int i = 0; i < g_nrecs; i++) t += g_recs[i].len;
    return t;
}

/* Override the host write() so the printf path is exercised and scriptable.
 * Every call is captured (recording the number of bytes actually accepted)
 * AND forwarded to the real stdout/stderr (via syscall, avoiding recursion)
 * so framework output is preserved. */
ssize_t write(int fd, const void *buf, size_t count)
{
    ssize_t ret = (ssize_t)count;
    if (g_armed) {
        write_mode_t m = g_mode;
        int sc = g_short;
        g_armed = 0;
        switch (m) {
        case WRITE_MODE_SHORT:
            if (sc <= 0) sc = 1;
            if ((size_t)sc >= count) sc = (int)count - 1;
            if (sc < 0) sc = 0;
            ret = (ssize_t)sc;
            break;
        case WRITE_MODE_EINTR:
            errno = EINTR;
            ret = -1;
            break;
        case WRITE_MODE_ZERO:
            ret = 0;
            break;
        case WRITE_MODE_ERROR:
            errno = EIO;
            ret = -1;
            break;
        default:
            break;
        }
    }

    if (g_nrecs < MAX_RECS) {
        write_rec_t *r = &g_recs[g_nrecs++];
        r->fd = fd;
        long cap = (ret < 0) ? 0 : ret;
        r->len = (cap > (long)sizeof(r->data)) ? (int)sizeof(r->data) : (int)cap;
        if (r->len > 0) memcpy(r->data, buf, (size_t)r->len);
    }

    if (fd == 1 || fd == 2)
        syscall(SYS_write, fd, buf, count);
    return ret;
}
