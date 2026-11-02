/*
 * hosttests/mock/fd_install_test_runtime.h
 *
 * ARCH-10 Task 6 — mock runtime for the descriptor-installation host
 * test (hosttests/cases/test_nofile_limits.c).
 *
 * The case links the PRODUCTION installation code into a host binary:
 *   kernel/fs/file.c      fd_alloc_limit / fd_dup / fd_dup2 / do_pipe
 *   kernel/net/socket.c   socket() and accept() install paths
 *   kernel/tty/pty.c      PTMX install path (pty master teardown)
 *   kernel/sync/wait.c    the pipe wait queues
 *
 * Those TUs use the REAL kernel headers (<sched/task.h> and the real
 * <arch/spinlock.h>), so this mock must NOT replace the spinlock or
 * the task layout — the host task keeps the production task_t so the
 * production task_nofile_soft() snapshot is the one under test.
 *
 * What the host build cannot provide is supplied here:
 *   - the lwIP surface kernel/net/socket.c uses (its real lwIP headers
 *     are compiled out under OS01_HOST_TEST),
 *   - the counters the case asserts on.
 *
 * The stub IMPLEMENTATIONS live in hosttests/mock/fd_install_stubs.c.
 */
#ifndef OS01_FD_INSTALL_TEST_RUNTIME_H
#define OS01_FD_INSTALL_TEST_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The kernel build reaches <log/log.h> through the real lwIP headers
 * (net/arch/cc.h).  The host build compiles those out, so the log
 * surface is pulled in here — the stub sink in
 * hosttests/mock/fd_install_stubs.c implements it. */
#include <log/log.h>

/* ── lwIP integer / error types ─────────────────────────────── */
typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;

typedef int err_t;

#ifndef ERR_OK
#define ERR_OK      0
#endif
#ifndef ERR_MEM
#define ERR_MEM     -1
#endif
#ifndef ERR_BUF
#define ERR_BUF     -2
#endif
#ifndef ERR_WOULDBLOCK
#define ERR_WOULDBLOCK -3
#endif
#ifndef ERR_TIMEOUT
#define ERR_TIMEOUT -4
#endif
#ifndef ERR_CLSD
#define ERR_CLSD    -7
#endif
#ifndef ERR_RTE
#define ERR_RTE     -8
#endif
#ifndef ERR_ARG
#define ERR_ARG     -16
#endif

/* ── lwIP IPv4 address (the kernel only uses the u32 form) ──── */
typedef struct {
    uint32_t addr;
} ip4_addr_t;

typedef ip4_addr_t ip_addr_t;

static inline void ip4_addr_set_u32(ip4_addr_t *a, uint32_t v) { a->addr = v; }
static inline uint32_t ip4_addr_get_u32(const ip4_addr_t *a) { return a->addr; }

/* ── lwIP netconn surface ───────────────────────────────────── */
#define NETCONN_TCP 1
#define NETCONN_UDP 2
#define NETCONN_COPY 0x01

enum netconn_type {
    NETCONN_TYPE_TCP = NETCONN_TCP,
    NETCONN_TYPE_UDP = NETCONN_UDP,
};

enum netconn_evt {
    NETCONN_EVT_RCVPLUS,
    NETCONN_EVT_RCVMINUS,
    NETCONN_EVT_SENDPLUS,
    NETCONN_EVT_SENDMINUS,
    NETCONN_EVT_ERROR,
};

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

typedef void (*netconn_callback)(struct netconn *, enum netconn_evt, u16_t);

struct netconn *netconn_new_with_proto_and_callback(int type, uint8_t protocol,
                                                    netconn_callback cb);
void netconn_delete(struct netconn *conn);
void netconn_set_callback_arg(struct netconn *conn, void *arg);
void *netconn_get_callback_arg(struct netconn *conn);
err_t netconn_bind(struct netconn *conn, ip_addr_t *addr, uint16_t port);
err_t netconn_connect(struct netconn *conn, ip_addr_t *addr, uint16_t port);
err_t netconn_listen_with_backlog(struct netconn *conn, uint8_t backlog);
err_t netconn_accept(struct netconn *conn, struct netconn **new_conn);
err_t netconn_recv(struct netconn *conn, struct netbuf **nb);
err_t netconn_write_partly(struct netconn *conn, const void *buf, size_t len,
                           uint8_t apiflags, size_t *bytes_written);
err_t netconn_sendto(struct netconn *conn, struct netbuf *nb, ip4_addr_t *ip,
                     uint16_t port);
err_t netconn_shutdown(struct netconn *conn, uint8_t shut_rx, uint8_t shut_tx);
err_t netconn_getaddr(struct netconn *conn, ip_addr_t *addr, uint16_t *port,
                      int local);

/* ── lwIP netbuf surface ────────────────────────────────────── */
struct netbuf *netbuf_new(void);
void  *netbuf_alloc(struct netbuf *nb, uint16_t len);
void   netbuf_delete(struct netbuf *nb);
void   netbuf_data(struct netbuf *nb, void **data, uint16_t *len);
int    netbuf_next(struct netbuf *nb);
ip4_addr_t *netbuf_fromaddr(struct netbuf *nb);
uint16_t    netbuf_fromport(struct netbuf *nb);

/* ── Observation counters (the case asserts on these) ───────── */
extern int fake_netconn_new_calls;
extern int fake_netconn_delete_calls;
extern int fake_netconn_accept_calls;
extern int fake_netconn_set_callback_calls;
extern int fake_netconn_listen_calls;

/* Kernel allocator call counters — the pipe case uses them to prove
 * that the pipe object itself was released (not just its slots). */
extern int fake_kmalloc_calls;
extern int fake_kfree_calls;

/* The PTMX device ops table captured from the (stubbed) devfs
 * registration: the case drives the production ptmx_open() through
 * it instead of standing up the whole devfs registry. */
struct devfs_ops;
extern const struct devfs_ops *fake_fd_install_ptmx_ops;

void fake_fd_install_runtime_reset(void);

#endif /* OS01_FD_INSTALL_TEST_RUNTIME_H */
