/* hosttests/mock/vfs_append_runtime.c
 *
 * Host-only stubs for the O_APPEND / VFS atomic-append test.
 *
 * Everything here is a boundary stub: the parts of the kernel that the
 * append path must NOT depend on (the scheduler, the pipe wake-up
 * helpers, the user-copy direction) are replaced by something that is
 * observable and deterministic.  The append logic itself stays in the
 * production objects.
 */

#include "vfs_append_runtime.h"

#include <string.h>
#include <errno.h>

int vfs_append_user_copy_fault = 0;
int vfs_append_bounce_alloc_fail = 0;

/* ── User copy ────────────────────────────────────────────────
 *
 * The weak default copies the bytes.  When the harness arms the
 * fault flag the copy fails with -EFAULT, which is exactly what a
 * real user-space fault produces: the destination is untouched and
 * the caller must not commit anything.
 */
__attribute__((weak)) ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                                                    void (*on_fault)(void *), void *arg)
{
    (void)on_fault;
    (void)arg;
    if (vfs_append_user_copy_fault) return -EFAULT;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

__attribute__((weak)) ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                                                  void (*on_fault)(void *), void *arg)
{
    (void)on_fault;
    (void)arg;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* ── Scheduler stubs ───────────────────────────────────────── */

void schedule(void) {}
void task_wake(task_t *task) { (void)task; }

/* ── Wait-queue wake helpers ──────────────────────────────────
 *
 * file.c's OWN pipe_wake_readers/writers call these; the production
 * definitions live in kernel/sync/wait.c, which this host binary
 * does not link.  The append path never wakes a pipe, so a no-op is
 * the honest answer for this fixture.
 */
void wait_queue_wake_one(wait_queue_t *wq) { (void)wq; }
void wait_queue_wake_all(wait_queue_t *wq) { (void)wq; }

/* ── Kernel allocator (fault-injectable) ──────────────────────
 *
 * This binary does NOT link mock_kernel.o: kmalloc here is the one
 * and only definition, and it consults vfs_append_bounce_alloc_fail
 * so the harness can fail the fd_write() bounce allocation (the
 * FIRST allocation of the append path) without disturbing the
 * filesystem's own allocations in other cases.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

void *kmalloc(size_t size)
{
    if (vfs_append_bounce_alloc_fail > 0) {
        vfs_append_bounce_alloc_fail--;
        return NULL;
    }
    return malloc(size);
}

size_t kfree(void *addr)
{
    free(addr);
    return 1;
}

/* ── Kernel output (silent) ─────────────────────────────────── */

int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor;
    (void)BKcolor;
    (void)fmt;
    return 0;
}

int serial_printk(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

void kpanic(const char *msg, ...)
{
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr, "\nPANIC\n");
    exit(1);
}

/* ── lwIP netconn/netbuf stubs ────────────────────────────────
 *
 * The socket branches of fd_read/fd_write reference these, so the
 * link needs definitions — but the append test never opens a
 * socket, and each stub fails loudly instead of pretending.
 */
err_t netconn_recv(struct netconn *conn, struct netbuf **nb)
{
    (void)conn; (void)nb;
    return ERR_CLSD;
}

err_t netconn_write_partly(struct netconn *conn, const void *buf, size_t len,
                           u8_t apiflags, size_t *bytes_written)
{
    (void)conn; (void)buf; (void)len; (void)apiflags;
    if (bytes_written) *bytes_written = 0;
    return ERR_CLSD;
}

void netconn_delete(struct netconn *conn) { (void)conn; }

void netbuf_data(struct netbuf *nb, void **data, u16_t *len)
{
    if (data) *data = nb ? nb->payload : NULL;
    if (len) *len = nb ? nb->len : 0;
}

int netbuf_next(struct netbuf *nb) { (void)nb; return -1; }

void netbuf_delete(struct netbuf *nb) { (void)nb; }
