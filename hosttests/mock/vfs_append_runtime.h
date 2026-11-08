/* hosttests/mock/vfs_append_runtime.h
 *
 * Host-only runtime surface for the O_APPEND / VFS atomic-append test.
 *
 * It is the VFS test runtime (hosttests/mock/vfs_test_runtime.h) plus
 * the handful of scheduler/architecture stubs that kernel/fs/file.c
 * needs.  Both parts describe the SAME mock task_t and the SAME
 * `current` alias, so the VFS production objects (vfs.c, tmpfs.c,
 * ext2.c, fat.c) and the file.c production object can be linked into a
 * single host binary without an ABI mismatch.
 *
 * The test drives the REAL production fd_write() with a REAL open-file
 * description, so the append path is exercised end to end inside the
 * host: no libc-side shortcut, no re-implementation of the kernel.
 */
#ifndef OS01_VFS_APPEND_RUNTIME_H
#define OS01_VFS_APPEND_RUNTIME_H

#include "vfs_test_runtime.h"

/* ── Scheduler / architecture surface used by kernel/fs/file.c ── */

#define TASK_RUNNING       (1 << 0)
#define TASK_INTERRUPTIBLE (1 << 1)

void task_wake(task_t *task);
void schedule(void);
uint64_t task_nofile_soft(task_t *t);

static inline void arch_local_irq_enable(void) {}
static inline uint64_t arch_cycle_counter(void) { return 0; }
static inline int arch_signal_pending_fatal(void) { return 0; }
static inline int arch_do_signal_delivery(void *regs)
{
    (void)regs;
    return 0;
}


/* ── lwIP surface used by kernel/fs/file.c ────────────────────
 *
 * file.c includes "lwip/err.h" and "lwip/api.h" by name; the host
 * build resolves them through -I mock/append_include, where they
 * forward back here (same pattern as fd_install_include).  Only the
 * surface file.c actually references exists; the stubs in
 * vfs_append_runtime.c answer it, and the append path never reaches
 * any of it (the FD_SOCKET branch is never entered by this test).
 */
typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;

typedef int err_t;

#ifndef ERR_OK
#define ERR_OK      0
#endif
#ifndef ERR_CLSD
#define ERR_CLSD    -7
#endif
#ifndef ERR_MEM
#define ERR_MEM     -1
#endif
#ifndef ERR_WOULDBLOCK
#define ERR_WOULDBLOCK -3
#endif

#define NETCONN_TCP 1
#define NETCONN_UDP 2
#define NETCONN_COPY 0x01

struct netconn {
    int      type;
    uint8_t  protocol;
    void    *callback_arg;
    uint32_t bound_ip;
    uint16_t bound_port;
    bool     listening;
};

struct netbuf {
    void    *payload;
    uint16_t len;
    uint16_t offset;
};

err_t netconn_recv(struct netconn *conn, struct netbuf **nb);
err_t netconn_write_partly(struct netconn *conn, const void *buf, size_t len,
                           u8_t apiflags, size_t *bytes_written);
void  netconn_delete(struct netconn *conn);
void  netbuf_data(struct netbuf *nb, void **data, u16_t *len);
int   netbuf_next(struct netbuf *nb);
void  netbuf_delete(struct netbuf *nb);

/* ── User-copy fault injection ─────────────────────────────────
 *
 * fd_write() copies the user bytes into a kernel bounce buffer
 * BEFORE it calls the filesystem's append hook.  A failing user copy
 * must therefore leave the file UNCHANGED.  The host harness flips
 * this flag to make copy_from_user_ft_res() fail, which is how the
 * EFAULT case is produced without a real page fault.
 */
extern int vfs_append_user_copy_fault;

/* ── Allocation failure injection ─────────────────────────────
 *
 * fd_write() allocates the bounce buffer before it copies anything.
 * A failing allocation must leave the file UNCHANGED as well.  The
 * harness flips this counter to make the Nth kmalloc() of the bounce
 * buffer fail.
 */
extern int vfs_append_bounce_alloc_fail;

#endif /* OS01_VFS_APPEND_RUNTIME_H */
