/* hosttests/mock/arch9/socket_runtime.h — mock environment for socket readiness tests
 *
 * Task 10 (phase C) — readiness guard + default-iface queries.
 * Mirrors lwIP/netbuf/netconn API shapes that kernel/net/socket.c uses
 * while remaining stand-alone (no lwIP source required).
 */
#ifndef ARCH9_SOCKET_RUNTIME_H
#define ARCH9_SOCKET_RUNTIME_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "test_platform.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#ifndef _KERNEL_DEBUG_H
#define _KERNEL_DEBUG_H
#endif
#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H
#endif

#ifndef debug_block
#define debug_block(...) do {} while (0)
#endif
#ifndef debug_net
#define debug_net(...) do {} while (0)
#endif
#ifndef log_info
#define log_info(...) do {} while (0)
#endif
#ifndef log_warn
#define log_warn(...) do {} while (0)
#endif
#ifndef log_err
#define log_err(...) do {} while (0)
#endif
#ifndef log_debug
#define log_debug(...) do {} while (0)
#endif

#include "net_runtime.h"

/* Prevent arch/x86_64/spinlock.h from redefining spinlock_T, etc. */
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H 1
#endif
#ifndef _ARCH_X86_64_SPINLOCK_H
#define _ARCH_X86_64_SPINLOCK_H 1
#endif
#ifndef __KERNEL_ARCH_CPU_H__
#define __KERNEL_ARCH_CPU_H__ 1
#endif
#ifndef _KERNEL_ARCH_X86_64_CPU_H
#define _KERNEL_ARCH_X86_64_CPU_H 1
#endif

/* lwIP integer types */
typedef uint8_t  u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;

#ifndef ERR_OK
#define ERR_OK   0
#endif
#ifndef ERR_MEM
#define ERR_MEM  -1
#endif
#ifndef ERR_TIMEOUT
#define ERR_TIMEOUT -3
#endif
#ifndef ERR_CLSD
#define ERR_CLSD -7
#endif
#ifndef ERR_RTE
#define ERR_RTE  -8
#endif
#ifndef ERR_IF
#define ERR_IF   -11
#endif
#ifndef ERR_ARG
#define ERR_ARG  -16
#endif
#ifndef ERR_ABRT
#define ERR_ABRT -10
#endif

/* ip_addr_t is lwIP's union for IPv4/IPv6 — only IPv4 here. */
typedef ip4_addr_t ip_addr_t;

#define NETCONN_COPY 0x01

/* ── netconn API mocks (track which socket subsystem was reached) ──── */
typedef enum netconn_evt {
    NETCONN_EVT_RCVPLUS,
    NETCONN_EVT_RCVMINUS,
    NETCONN_EVT_SENDPLUS,
    NETCONN_EVT_SENDMINUS,
    NETCONN_EVT_ERROR,
} netconn_evt_t;

extern int fake_netconn_new_calls;
extern int fake_netconn_new_fail_next;
extern int fake_netconn_delete_calls;
extern int fake_netconn_bind_calls;
extern int fake_netconn_connect_calls;
extern int fake_netconn_listen_calls;
extern int fake_netconn_accept_calls;
extern int fake_netconn_recv_calls;
extern int fake_netconn_send_calls;
extern int fake_netconn_sendto_calls;
extern int fake_netconn_shutdown_calls;
extern int fake_netconn_getaddr_calls;

extern uint32_t fake_netconn_bound_ip;
extern uint16_t fake_netconn_bound_port;
extern uint32_t fake_netconn_peer_ip;
extern uint16_t fake_netconn_peer_port;

/* Fake netconn — opaque to socket.c.  We track call counts and the
 * "address" returned to netconn_getaddr() so do_getsockname's preference
 * for non-zero bound IP can be exercised. */
struct netconn {
    int      type;
    uint8_t  protocol;
    void    *callback_arg;
    uint32_t bound_ip;       /* 0 if not bound, else the bound addr */
    uint16_t bound_port;
    /* Test hook: when fake_return_ip is true, netconn_getaddr() returns
     * fake_ip/fake_port verbatim.  Used by test_getsockname_* to drive
     * the bound/zero/default-iface preference logic. */
    bool     fake_return_ip;
    uint32_t fake_ip;
    uint16_t fake_port;
};

typedef void (*netconn_callback)(struct netconn *, netconn_evt_t, uint16_t);

struct netconn *netconn_new_with_proto_and_callback(int type, uint8_t protocol,
                                                    netconn_callback cb);
void netconn_delete(struct netconn *conn);
void netconn_set_callback_arg(struct netconn *conn, void *arg);
void *netconn_get_callback_arg(struct netconn *conn);

#define NETCONN_TCP 1
#define NETCONN_UDP 2
enum netconn_type {
    NETCONN_TYPE_TCP = NETCONN_TCP,
    NETCONN_TYPE_UDP = NETCONN_UDP,
};

/* netbuf API */
struct netbuf {
    void    *payload;
    uint16_t len;
    uint16_t offset;
    ip_addr_t src_addr;
    uint16_t src_port;
};
struct netbuf *netbuf_new(void);
void   *netbuf_alloc(struct netbuf *nb, uint16_t len);
void    netbuf_delete(struct netbuf *nb);
void    netbuf_data(struct netbuf *nb, void **data, uint16_t *len);
ip_addr_t *netbuf_fromaddr(struct netbuf *nb);
uint16_t   netbuf_fromport(struct netbuf *nb);

/* netconn operations — return lwIP err_t */
err_t netconn_bind(struct netconn *conn, ip_addr_t *addr, uint16_t port);
err_t netconn_connect(struct netconn *conn, ip_addr_t *addr, uint16_t port);
err_t netconn_listen_with_backlog(struct netconn *conn, uint8_t backlog);
err_t netconn_accept(struct netconn *conn, struct netconn **new_conn);
err_t netconn_recv(struct netconn *conn, struct netbuf **nb);
err_t netconn_write_partly(struct netconn *conn, const void *buf, size_t len,
                          uint8_t apiflags, size_t *bytes_written);
err_t netconn_sendto(struct netconn *conn, struct netbuf *nb, ip_addr_t *ip, uint16_t port);
err_t netconn_shutdown(struct netconn *conn, uint8_t shut_rx, uint8_t shut_tx);
err_t netconn_getaddr(struct netconn *conn, ip_addr_t *addr, uint16_t *port,
                      int local);

/* ── Test-only knobs (declared in production headers via OS01_HOST_TEST) ──
 * net_service_force_state_for_test() / net_service_set_default_ipv4_for_test()
 * are test-only weak symbols exposed by kernel/net/lwip.c so the readiness
 * state can be exercised without standing up tcpip_thread. */

extern int fake_max_delay_ms;

/* ── Test-side reset ───────────────────────────────────────────────── */
void fake_socket_runtime_reset(void);

/* ── Production-only stubs needed by socket.c at link time ──────────
 * Production code references these via uaccess.h (copy_from_user_ft
 * returns through `*_res`) and via sched/task.h (do_accept queries
 * poll_test_current).  The host test does not exercise the real
 * implementations — the .h header is the canonical surface, and our
 * wrapper copy_from_user_ft / copy_to_user_ft above return success
 * without touching the *_res out-parameter.  Declared (not defined)
 * here so the test's TU provides the implementations matching
 * uaccess.h's prototypes. */
#include <sys/types.h>
ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg);
ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg);
/* poll_test_current is declared in hosttests/mock/poll_test_runtime.h
 * (with the correct task_t* type) and re-exported by force-include
 * chains in other tests.  We don't redeclare it here — just rely on
 * the earlier include path. */

#endif /* ARCH9_SOCKET_RUNTIME_H */