/* hosttests/cases/test_socket_net_readiness.c — ARCH-9 Task 10 socket readiness tests
 *
 * Verifies that do_socket / do_getifaddr / do_getsockname respect the new
 * net_service_ready() / net_default_ipv4() surface introduced in Task 6
 * and consumed in Task 10.  All fakes are observation-only — production
 * logic in kernel/net/socket.c is exercised directly.
 */
#include "test_framework.h"

#include <net/socket.h>
#include <net/lwip.h>
#include <fs/file.h>
#include <uapi/sockaddr.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* socket_runtime.h is force-included via -include in the Makefile. */

#define AF_INET  2
#define NOFILE   16

/* ── Mock netconn counters (declared in socket_runtime.h) ──────────── */
/* These counters are referenced by net_runtime.h's mock helpers
 * (dhcp_start, etc.) which are force-included into every .o that
 * pulls in net_runtime.h.  Even though the socket test does not
 * assert on DHCP, the .o files linked in DO reference these
 * symbols (e1000/virtio production code, net_lwip production code). */
int  fake_netif_add_calls = 0;
bool fake_netif_add_fail_all = false;
int  fake_dhcp_start_calls = 0;
struct netif *fake_dhcp_last_netif = NULL;
int  fake_ethernet_input_calls = 0;
struct pbuf *fake_last_rx_pbuf = NULL;
struct netif *fake_last_rx_netif = NULL;
int  fake_tcpip_input_calls = 0;
int  fake_etharp_output_calls = 0;
int  fake_pbuf_free_calls = 0;
/* Mailbox lock-held observation (Task 9).  Referenced by
 * fake_net_runtime_reset() — only the test that drives the core
 * mailbox actually uses it (test_net_lwip.c). */
int fake_mailbox_lock_held = 0;
unsigned fake_poll_budget = 0;
int fake_tcpip_init_calls = 0;
tcpip_init_done_fn fake_tcpip_done_cb = NULL;
void *fake_tcpip_done_arg = NULL;
bool fake_tcpip_auto_callback = true;

/* Mock ethernet_input for net_device.c which calls into lwIP. */
err_t ethernet_input(struct pbuf *p, struct netif *netif)
{
    fake_ethernet_input_calls++;
    if (p) {
        fake_pbuf_free_calls++;
        if (p->ref > 0) p->ref--;
        if (p->ref == 0) free(p);
    }
    (void)netif;
    return ERR_OK;
}

err_t tcpip_input(struct pbuf *p, struct netif *inp) { (void)p; (void)inp; return ERR_OK; }
err_t etharp_output(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
    (void)netif; (void)q; (void)ipaddr; return ERR_OK;
}

int  fake_netconn_new_calls = 0;
int  fake_netconn_new_fail_next = 0;
int  fake_netconn_delete_calls = 0;
int  fake_netconn_bind_calls = 0;
int  fake_netconn_connect_calls = 0;
int  fake_netconn_listen_calls = 0;
int  fake_netconn_accept_calls = 0;
int  fake_netconn_recv_calls = 0;
int  fake_netconn_send_calls = 0;
int  fake_netconn_sendto_calls = 0;
int  fake_netconn_shutdown_calls = 0;
int  fake_netconn_getaddr_calls = 0;

uint32_t fake_netconn_bound_ip = 0;
uint16_t fake_netconn_bound_port = 0;
uint32_t fake_netconn_peer_ip = 0;
uint16_t fake_netconn_peer_port = 0;

int fake_max_delay_ms = 0;

/* ── netconn mock implementations ──────────────────────────────────── */
struct netconn *netconn_new_with_proto_and_callback(int type, uint8_t protocol,
                                                    netconn_callback cb)
{
    fake_netconn_new_calls++;
    if (fake_netconn_new_fail_next) {
        fake_netconn_new_fail_next--;
        return NULL;
    }
    struct netconn *c = (struct netconn *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->type = type;
    c->protocol = protocol;
    (void)cb;
    return c;
}

void netconn_delete(struct netconn *conn)
{
    fake_netconn_delete_calls++;
    free(conn);
}

void netconn_set_callback_arg(struct netconn *conn, void *arg)
{
    if (conn) conn->callback_arg = arg;
}

void *netconn_get_callback_arg(struct netconn *conn)
{
    return conn ? conn->callback_arg : NULL;
}

err_t netconn_bind(struct netconn *conn, ip_addr_t *addr, uint16_t port)
{
    fake_netconn_bind_calls++;
    if (conn) {
        conn->bound_ip = addr ? ip4_addr_get_u32(addr) : 0;
        conn->bound_port = port;
    }
    fake_netconn_bound_ip = addr ? ip4_addr_get_u32(addr) : 0;
    fake_netconn_bound_port = port;
    return ERR_OK;
}

err_t netconn_connect(struct netconn *conn, ip_addr_t *addr, uint16_t port)
{
    fake_netconn_connect_calls++;
    (void)conn;
    fake_netconn_peer_ip = addr ? ip4_addr_get_u32(addr) : 0;
    fake_netconn_peer_port = port;
    return ERR_OK;
}

err_t netconn_listen_with_backlog(struct netconn *conn, uint8_t backlog)
{
    fake_netconn_listen_calls++;
    (void)conn; (void)backlog;
    return ERR_OK;
}

err_t netconn_accept(struct netconn *conn, struct netconn **new_conn)
{
    fake_netconn_accept_calls++;
    (void)conn;
    if (new_conn) *new_conn = NULL;
    return ERR_OK;
}

err_t netconn_recv(struct netconn *conn, struct netbuf **nb)
{
    fake_netconn_recv_calls++;
    (void)conn;
    if (nb) *nb = NULL;
    return ERR_OK;
}

err_t netconn_write_partly(struct netconn *conn, const void *buf, size_t len,
                          uint8_t apiflags, size_t *bytes_written)
{
    fake_netconn_send_calls++;
    (void)conn; (void)buf; (void)apiflags;
    if (bytes_written) *bytes_written = len;
    return ERR_OK;
}

err_t netconn_sendto(struct netconn *conn, struct netbuf *nb, ip_addr_t *ip, uint16_t port)
{
    fake_netconn_sendto_calls++;
    (void)conn; (void)nb;
    fake_netconn_peer_ip = ip ? ip4_addr_get_u32(ip) : 0;
    fake_netconn_peer_port = port;
    return ERR_OK;
}

err_t netconn_shutdown(struct netconn *conn, uint8_t shut_rx, uint8_t shut_tx)
{
    fake_netconn_shutdown_calls++;
    (void)conn; (void)shut_rx; (void)shut_tx;
    return ERR_OK;
}

err_t netconn_getaddr(struct netconn *conn, ip_addr_t *addr, uint16_t *port,
                      int local)
{
    fake_netconn_getaddr_calls++;
    (void)local;
    if (!conn) return ERR_ARG;
    if (addr) ip4_addr_set_u32(addr, conn->fake_return_ip
                                    ? conn->fake_ip
                                    : (conn->bound_ip ? conn->bound_ip : 0));
    if (port) *port = conn->fake_return_ip ? conn->fake_port : conn->bound_port;
    return ERR_OK;
}

/* ── netbuf mock implementations ───────────────────────────────────── */
struct netbuf *netbuf_new(void)
{
    return (struct netbuf *)calloc(1, sizeof(struct netbuf));
}

void *netbuf_alloc(struct netbuf *nb, uint16_t len)
{
    if (!nb) return NULL;
    nb->payload = calloc(1, len);
    nb->len = len;
    return nb->payload;
}

void netbuf_delete(struct netbuf *nb)
{
    if (!nb) return;
    free(nb->payload);
    free(nb);
}

void netbuf_data(struct netbuf *nb, void **data, uint16_t *len)
{
    if (!nb) { if (data) *data = NULL; if (len) *len = 0; return; }
    if (data) *data = (void *)((char *)nb->payload + nb->offset);
    if (len) *len = nb->len - nb->offset;
}

ip_addr_t *netbuf_fromaddr(struct netbuf *nb)
{
    static ip_addr_t empty;
    if (!nb) return &empty;
    return &nb->src_addr;
}

uint16_t netbuf_fromport(struct netbuf *nb)
{
    return nb ? nb->src_port : 0;
}

void fake_socket_runtime_reset(void)
{
    fake_netconn_new_calls = 0;
    fake_netconn_new_fail_next = 0;
    fake_netconn_delete_calls = 0;
    fake_netconn_bind_calls = 0;
    fake_netconn_connect_calls = 0;
    fake_netconn_listen_calls = 0;
    fake_netconn_accept_calls = 0;
    fake_netconn_recv_calls = 0;
    fake_netconn_send_calls = 0;
    fake_netconn_sendto_calls = 0;
    fake_netconn_shutdown_calls = 0;
    fake_netconn_getaddr_calls = 0;
    fake_netconn_bound_ip = 0;
    fake_netconn_bound_port = 0;
    fake_netconn_peer_ip = 0;
    fake_netconn_peer_port = 0;
    fake_max_delay_ms = 0;
}

/* ── Process / files stubs so do_socket can run on host ─────────────
 *
 * do_socket() reaches into current->files->fd[fd] via fd_alloc().
 * Production code's task_t is huge; for a host-side readiness test we
 * only need:
 *   - `current` resolves to a writable task_t with a valid files_t
 *   - files_t has the same layout as kernel/include/fs/file.h
 *     (spinlock_T lock, refcount, fd[], cwd heap pointer) so socket.c
 *     can dereference files->fd[fd] without UB.
 *
 * poll_test_runtime.h defines its own task_t (just enough for the
 * poll machinery); we cast our static task to (task_t *) and the
 * offset of `files` is fixed at compile time.  Spinlocks are
 * no-ops in the single-threaded test. */
static char s_fake_cwd[256] = "/";
static files_t s_fake_files = {
    .lock = { .lock = 1 },
    .refcount = 1,
    .fd = { NULL },
    .cwd = s_fake_cwd,
};
static task_t s_fake_task = {
    .state = 0,
    .pid = 1,
    .files = &s_fake_files,
    .io_wait_node = { NULL, NULL },
};

/* Override the kernel's current-task indirection.  The production
 * build defines `current` as `get_current_task()`; we provide a
 * minimal version here.  do_socket/do_getsockname/do_getifaddr only
 * need current->files; do_accept queries current via the global
 * `current` macro which poll_test_runtime.h redefines to
 * poll_test_current — define that here too so production code finds
 * it at link time. */
task_t *poll_test_current = NULL;

#define get_current_task() (&s_fake_task)

void *kmalloc(size_t sz) { return malloc(sz); }
size_t kfree(void *ptr)   { free(ptr); return 0; }

static void init_poll_test_current(void)
{
    if (!poll_test_current) {
        poll_test_current = &s_fake_task;
    }
}

void arch_irq_install(void) {}
void softirq_init(void) {}

static void reset_task_files(void)
{
    init_poll_test_current();
    memset(s_fake_task.files, 0, sizeof(*s_fake_task.files));
    /* Spinlock is left untouched (single-threaded); refcount = 1. */
    s_fake_task.files->refcount = 1;
    /* Reset cwd to root */
    s_fake_task.files->cwd = s_fake_cwd;
    s_fake_cwd[0] = '/';
    s_fake_cwd[1] = 0;
}

file_t *file_alloc(void)
{
    file_t *f = (file_t *)calloc(1, sizeof(*f));
    return f;
}
void file_free(file_t *f) { free(f); }

/* fd_alloc / fd_close stubs so do_socket can run on host without
 * pulling in kernel/fs/file.c (which needs the scheduler and poll
 * test runtime we don't want for a pure readiness test). */
int fd_alloc(files_t *fs, file_t *f)
{
    if (!fs || !f) return -1;
    for (int i = 0; i < NOFILE; i++) {
        if (fs->fd[i] == NULL) {
            fs->fd[i] = f;
            return i;
        }
    }
    return -1;
}
void fd_close(files_t *fs, int fd) { (void)fs; (void)fd; }

int copy_to_user_ft(void *dst, const void *src, size_t n)
{
    (void)dst; (void)src; (void)n;
    return 0;
}
int copy_from_user_ft(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
    return 0;
}

/* Stubs for kernel/sync/wait.h used by socket_netconn_cb */
void wait_queue_wake_all(wait_queue_t *wq) { (void)wq; }
void wait_queue_wake_one(wait_queue_t *wq) { (void)wq; }
void wait_queue_init(wait_queue_t *wq) { (void)wq; }

/* The kernel's uaccess.h expects these *_res wrappers to exist at
 * link time.  socket.c is compiled as production code and references
 * them via the header, so we provide simple in-memory copies. */
ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*cb)(void *), void *arg)
{
    (void)cb; (void)arg;
    memcpy(dst, src, n);
    return (ssize_t)n;
}
ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*cb)(void *), void *arg)
{
    (void)cb; (void)arg;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

static int count_used_fds(void)
{
    int n = 0;
    for (int i = 0; i < NOFILE; i++)
        if (s_fake_task.files->fd[i]) n++;
    return n;
}

static void reset_all(void)
{
    fake_socket_runtime_reset();
    fake_net_runtime_reset();
    net_lwip_reset_state();
    reset_task_files();
}

static void enter_off(void)       { net_lwip_reset_state(); }
static void enter_starting(void)  { net_lwip_reset_state();
                                    net_service_force_state_for_test(NET_STARTING); }
static void enter_failed(void)    { net_lwip_reset_state();
                                    net_service_force_state_for_test(NET_FAILED); }
static void enter_online(void)    { net_lwip_reset_state();
                                    net_service_force_state_for_test(NET_ONLINE);
                                    net_service_set_default_ipv4_for_test(0x0F02000A); /* 10.0.2.15 */ }

/* ── Test 1: socket returns -ENETDOWN before ONLINE ──────────────── */
TEST_FUNC(test_socket_off_starting_failed)
{
    /* OFF */
    reset_all();
    enter_off();
    assert_eq(-ENETDOWN, do_socket(AF_INET, 2 /*SOCK_DGRAM*/, 0));
    assert_eq(0, fake_netconn_new_calls);

    /* STARTING */
    reset_all();
    enter_starting();
    assert_eq(-ENETDOWN, do_socket(AF_INET, 1 /*SOCK_STREAM*/, 0));
    assert_eq(0, fake_netconn_new_calls);

    /* FAILED */
    reset_all();
    enter_failed();
    assert_eq(-ENETDOWN, do_socket(AF_INET, 2, 0));
    assert_eq(0, fake_netconn_new_calls);
}

/* ── Test 2: ONLINE can allocate; real alloc failure returns ENOMEM ─ */
TEST_FUNC(test_online_allocation_errors)
{
    /* ONLINE succeeds */
    reset_all();
    enter_online();
    int fd = do_socket(AF_INET, 2, 0);
    assert_true(fd >= 0);
    assert_eq(1, fake_netconn_new_calls);

    /* Force next netconn_new to fail */
    reset_all();
    enter_online();
    fake_netconn_new_fail_next = 1;
    int rc = do_socket(AF_INET, 2, 0);
    assert_eq(-ENOMEM, rc);
    /* Either -ENOMEM from socket_alloc short-circuit OR consumed by
     * netconn_new.  Either way: no fd was created. */
    assert_eq(0, count_used_fds());
}

/* ── Test 3: getifaddr before ONLINE returns 0 ───────────────────── */
TEST_FUNC(test_getifaddr_before_online)
{
    reset_all();
    enter_off();
    assert_eq(0, do_getifaddr());

    enter_starting();
    assert_eq(0, do_getifaddr());

    enter_failed();
    assert_eq(0, do_getifaddr());

    /* ONLINE: returns default IPv4 */
    enter_online();
    assert_eq((int64_t)0x0F02000A, do_getifaddr());
}

/* ── Test 4: getsockname prefers bound non-zero IP ──────────────── */
TEST_FUNC(test_getsockname_prefers_bound_address)
{
    reset_all();
    enter_online();
    int fd = do_socket(AF_INET, 2, 0);
    assert_true(fd >= 0);

    socket_t *s = socket_get(fd);
    assert_true(s != NULL);

    /* Configure the netconn to return a non-zero IP from getaddr */
    struct netconn *c = (struct netconn *)s->conn;
    c->fake_return_ip = 1;
    c->fake_ip = 0xC0A80101; /* 192.168.1.1 */
    c->fake_port = 8080;

    struct sockaddr_in sin;
    uint32_t addrlen = sizeof(sin);
    int64_t rc = do_getsockname(fd, &sin, &addrlen);
    assert_eq(0, rc);
    assert_eq((uint32_t)0xC0A80101, sin.sin_addr);
    /* sin_port is in network byte order (os01_htons), so 8080 host
     * byte order becomes 0x901F = 36895 in big-endian. */
    assert_eq((uint16_t)36895, sin.sin_port);

    /* Case B: netconn returns 0 IP — fall back to default iface */
    fake_socket_runtime_reset();
    /* Re-allocate socket since we just zeroed counters, but
     * the previous netconn's state is preserved */
    s->conn = netconn_new_with_proto_and_callback(NETCONN_UDP, 0, NULL);
    struct netconn *c2 = (struct netconn *)s->conn;
    c2->fake_return_ip = 1;
    c2->fake_ip = 0;
    c2->fake_port = 0;
    addrlen = sizeof(sin);
    rc = do_getsockname(fd, &sin, &addrlen);
    assert_eq(0, rc);
    assert_eq((uint32_t)0x0F02000A, sin.sin_addr); /* default iface */
}

/* ── Test 5: invalid fd before lwIP is EBADF, no netconn call ──── */
TEST_FUNC(test_bad_fd_before_lwip)
{
    reset_all();
    enter_off();
    /* No socket allocated: bad fd -> -EBADF, no netconn touched */
    assert_eq((int64_t)-EBADF, do_bind(-1, 0, 0));
    assert_eq((int64_t)-EBADF, do_connect(-1, 0, 0));
    assert_eq(0, fake_netconn_new_calls);
    assert_eq(0, fake_netconn_bind_calls);
    assert_eq(0, fake_netconn_connect_calls);
}

/* ── Test 6: socket completes without permanent mailbox wait ─────── */
TEST_FUNC(test_no_nic_socket_finishes)
{
    /* No NIC ever registered, OFF state — socket must return promptly
     * with -ENETDOWN.  We loop 10 socket attempts — all ENETDOWN. */
    reset_all();
    enter_off();
    for (int i = 0; i < 10; i++) {
        assert_eq((int64_t)-ENETDOWN, do_socket(AF_INET, 2, 0));
    }
    assert_eq(0, fake_netconn_new_calls);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_socket_off_starting_failed),
    TEST_ENTRY(test_online_allocation_errors),
    TEST_ENTRY(test_getifaddr_before_online),
    TEST_ENTRY(test_getsockname_prefers_bound_address),
    TEST_ENTRY(test_bad_fd_before_lwip),
    TEST_ENTRY(test_no_nic_socket_finishes),
TEST_LIST_END

int main(void)
{
    printf("=== ARCH-9 Task 10 socket readiness tests ===\n");
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}