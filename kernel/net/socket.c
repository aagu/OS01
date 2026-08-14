// kernel/net/socket.c — BSD socket implementation backed by lwIP netconn
#include <net/socket.h>
#include <uapi/sockaddr.h>
#include <net/net.h>
#include <kernel/file.h>
#include "lwip/netif.h"
#include <kernel/task.h>
#include <kernel/slab.h>   // kmalloc, kfree
#include <kernel/poll.h>
#include <kernel/wait.h>   // wait_queue_wake_all
#include <kernel.h>        // container_of
#include <string.h>
#include <errno.h>
#include "lwip/api.h"       // netconn, netbuf, NETCONN_TCP/UDP
#include "lwip/netbuf.h"   // netbuf_fromaddr, netbuf_fromport
#include "lwip/ip_addr.h"   // ip4_addr_set_u32

// ── Socket state constants ─────────────────────────────────────
#define SOCK_UNCONNECTED  0
#define SOCK_CONNECTED    1
#define SOCK_LISTENING    2
#define SOCK_CLOSED       3

// ── netconn event callback ─────────────────────────────────────
// lwIP invokes this when a netconn event occurs (data arrived,
// send space freed, error).  We use RCVPLUS to set rx_pending and
// wake poll waiters so poll(POLLIN) on a CONNECTED socket works.
static void socket_netconn_cb(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
    (void)conn; (void)len;
    if (evt != NETCONN_EVT_RCVPLUS && evt != NETCONN_EVT_RCVMINUS &&
        evt != NETCONN_EVT_ERROR)
        return;

    // Find the socket owning this conn via callback_arg (set in
    // socket_alloc with netconn_set_callback_arg).
    socket_t *s = (socket_t *)netconn_get_callback_arg(conn);
    if (!s) return;

    uint64_t flags = spin_lock_irqsave(&s->lock);
    if (evt == NETCONN_EVT_RCVPLUS)
        s->rx_pending++;
    else if (evt == NETCONN_EVT_RCVMINUS && s->rx_pending > 0)
        s->rx_pending--;
    else if (evt == NETCONN_EVT_ERROR)
        s->rx_pending = 1;

    // Wake all poll waiters (same pattern as pipe_wake_readers).
    while (!list_is_empty(&s->poll_list)) {
        list_t *node = s->poll_list.next;
        list_del_init(node);
        poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
        wait_queue_wake_all(e->poll_wq);
    }
    spin_unlock_irqrestore(&s->lock, flags);
}

// ── Socket allocation ──────────────────────────────────────────

socket_t *socket_alloc(int domain, int type, int protocol)
{
    enum netconn_type nc_type;
    switch (type) {
    case 1: nc_type = NETCONN_TCP; break;   // SOCK_STREAM
    case 2: nc_type = NETCONN_UDP; break;   // SOCK_DGRAM
    default: return NULL;
    }

    struct netconn *conn = netconn_new_with_proto_and_callback(nc_type,
        (u8_t)protocol, socket_netconn_cb);
    if (!conn) return NULL;

    socket_t *s = (socket_t *)kmalloc(sizeof(socket_t));
    if (!s) { netconn_delete(conn); return NULL; }

    s->conn     = conn;
    s->domain   = domain;
    s->type     = type;
    s->protocol = protocol;
    s->state    = SOCK_UNCONNECTED;
    s->bound    = 0;
    s->rx_pending = 0;
    s->rx_nb = NULL;
    s->rx_off = 0;
    netconn_set_callback_arg(conn, s);
    spin_init(&s->lock);
    list_init(&s->poll_list);

    return s;
}

void socket_free(socket_t *s)
{
    if (!s) return;
    if (s->rx_nb) netbuf_delete((struct netbuf *)s->rx_nb);
    if (s->conn) netconn_delete((struct netconn *)s->conn);
    kfree(s);
}

// ── FD → socket lookup ─────────────────────────────────────────

socket_t *socket_get(int fd)
{
    if (fd < 0 || fd >= NOFILE || !current || !current->files)
        return NULL;
    file_t *f = current->files->fd[fd];
    if (!f || f->type != FD_SOCKET || !f->sock)
        return NULL;
    return f->sock;
}

// ── SYS_socket — create a socket ───────────────────────────────

int64_t do_socket(int domain, int type, int protocol)
{
    socket_t *s = socket_alloc(domain, type, protocol);
    if (!s) return -ENOMEM;

    file_t *f = file_alloc();
    if (!f) { socket_free(s); return -ENOMEM; }
    f->type = FD_SOCKET;
    f->sock = s;
    f->flags = O_RDWR;

    int fd = fd_alloc(current->files, f);
    if (fd < 0) {
        f->sock = NULL;
        file_free(f);
        socket_free(s);
        return -ENFILE;
    }
    return fd;
}

// ── SYS_connect — connect to remote address ──────────────────

int64_t do_connect(int fd, uint32_t ip, uint16_t port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    ip_addr_t addr;
    ip4_addr_set_u32(&addr, ip);
    err_t err = netconn_connect((struct netconn *)s->conn, &addr, port);
    if (err == ERR_OK) {
        s->state = SOCK_CONNECTED;
        return 0;
    }
    if (err == ERR_TIMEOUT) return -ETIMEDOUT;
    return -ECONNREFUSED;
}

// ── SYS_sendto — send data (and optionally specify dest) ─────

int64_t do_sendto(int fd, const void *buf, uint64_t len, int flags,
                  uint32_t ip, uint16_t port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    if (s->type == 1) {
        // TCP: use netconn_write (ip/port ignored — already connected)
        (void)flags; (void)ip; (void)port;
        err_t err = netconn_write((struct netconn *)s->conn, buf,
                                  (size_t)len, NETCONN_COPY);
        return (err == ERR_OK) ? (int64_t)len : -EIO;
    } else {
        // UDP: create netbuf with destination
        (void)flags;
        if (len > UINT16_MAX) return -EMSGSIZE;
        struct netbuf *nb = netbuf_new();
        if (!nb) return -ENOMEM;
        void *payload = netbuf_alloc(nb, (u16_t)len);
        if (!payload) { netbuf_delete(nb); return -ENOMEM; }
        memcpy(payload, buf, len);

        ip_addr_t addr;
        ip4_addr_set_u32(&addr, ip);
        err_t err = netconn_sendto((struct netconn *)s->conn, nb, &addr, port);
        log_info("sock: sendto err=%d port=%u dst=%u\n", (int)err, port, ip);
        netbuf_delete(nb);
        if (err != ERR_OK) {
            // ERR_RTE: no route / netif down.  ERR_MEM: pbuf/mbox full.
            // Surface the failure instead of pretending success.
            return (err == ERR_MEM) ? -ENOMEM : -ENETUNREACH;
        }
        return (int64_t)len;
    }
}

// ── SYS_recvfrom — receive data (and optionally get source) ──

int64_t do_recvfrom(int fd, void *buf, uint64_t len, int flags,
                    uint32_t *out_ip, uint16_t *out_port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    (void)flags;

    struct netbuf *nb;
    if (signal_pending_fatal()) return -EINTR;

    err_t err = netconn_recv((struct netconn *)s->conn, &nb);
    if (err != ERR_OK) {
        if (signal_pending_fatal()) return -EINTR;
        if (err == ERR_CLSD) {
            // Peer closed: readable-but-EOF.  Keep rx_pending set so
            // poll(POLLIN) returns and read() surfaces 0 (EOF).
            return 0;
        }
        if (err == ERR_TIMEOUT) return -ETIMEDOUT;
        return -EIO;
    }

    void *data;
    u16_t data_len;
    netbuf_data(nb, &data, &data_len);
    size_t copy = (data_len < len) ? data_len : (size_t)len;
    memcpy(buf, data, copy);

    // Fill in source address if requested
    if (out_ip) {
        ip_addr_t *addr = netbuf_fromaddr(nb);
        *out_ip = ip4_addr_get_u32(addr);
    }
    if (out_port) {
        *out_port = netbuf_fromport(nb);
    }

    netbuf_delete(nb);

    return (int64_t)copy;
}

// ── SYS_bind — bind to local address ─────────────────────────

int64_t do_bind(int fd, uint32_t ip, uint16_t port)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    ip_addr_t addr;
    ip4_addr_set_u32(&addr, ip);
    err_t err = netconn_bind((struct netconn *)s->conn, &addr, port);
    if (err == ERR_OK) {
        s->bound = 1;
        return 0;
    }
    return -EADDRINUSE;
}

// ── SYS_listen — mark socket as listening ────────────────────

int64_t do_listen(int fd, int backlog)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    if (!s->bound) return -EINVAL;

    struct netconn *conn = (struct netconn *)s->conn;
    err_t err = netconn_listen_with_backlog(conn, (u8_t)backlog);
    if (err == ERR_OK) {
        s->state = SOCK_LISTENING;
        return 0;
    }
    return -EIO;
}

// ── SYS_accept — accept incoming connection ──────────────────

int64_t do_accept(int fd, uint32_t *out_ip, uint16_t *out_port)
{
    socket_t *listen_sock = socket_get(fd);
    if (!listen_sock) return -EBADF;
    if (listen_sock->state != SOCK_LISTENING) return -EINVAL;

    struct netconn *new_conn;
    err_t err = netconn_accept((struct netconn *)listen_sock->conn, &new_conn);
    if (err != ERR_OK) return -EIO;

    // source address not easily available in netconn_accept
    if (out_ip) *out_ip = 0;
    if (out_port) *out_port = 0;

    // Create new socket + fd for the accepted connection
    socket_t *new_sock = (socket_t *)kmalloc(sizeof(socket_t));
    if (!new_sock) { netconn_delete(new_conn); return -ENOMEM; }
    new_sock->conn     = new_conn;
    new_sock->domain   = listen_sock->domain;
    new_sock->type     = listen_sock->type;
    new_sock->protocol = listen_sock->protocol;
    new_sock->state    = SOCK_CONNECTED;
    new_sock->bound    = 0;
    new_sock->rx_pending = 0;
    new_sock->rx_nb = NULL;
    new_sock->rx_off = 0;
    spin_init(&new_sock->lock);
    list_init(&new_sock->poll_list);
    netconn_set_callback_arg(new_conn, new_sock);

    file_t *new_f = file_alloc();
    if (!new_f) { socket_free(new_sock); return -ENOMEM; }
    new_f->type  = FD_SOCKET;
    new_f->sock  = new_sock;
    new_f->flags = O_RDWR;

    int new_fd = fd_alloc(current->files, new_f);
    if (new_fd < 0) {
        file_free(new_f);
        return -ENFILE;
    }
    return new_fd;
}


// ── SYS_getsockname ──

int64_t do_getsockname(int fd, void *addr_ptr, uint32_t *addrlen_ptr)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    ip_addr_t lwip_addr;
    u16_t port;
    err_t err = netconn_getaddr((struct netconn *)s->conn, &lwip_addr, &port, 1);
    if (err != ERR_OK)
        return -EADDRNOTAVAIL;

    uint32_t usr_addrlen = *addrlen_ptr;
    if (usr_addrlen < sizeof(struct sockaddr_in))
        return -EINVAL;
extern struct netif os01_netif;

    // If socket not bound, fall back to netif DHCP address
    uint32_t ip = ip4_addr_get_u32(&lwip_addr);
    if (ip == 0) {
        ip = ip4_addr_get_u32(&os01_netif.ip_addr);
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = os01_htons(port);
    sin.sin_addr = ip;
    memcpy(addr_ptr, &sin, sizeof(sin));
    *addrlen_ptr = sizeof(sin);
    return 0;
}

// ── SYS_setsockopt / SYS_getsockopt — minimal stubs ────────────

int64_t do_setsockopt(int fd, int level, int optname,
                      const void *optval, uint64_t optlen)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    // SO_REUSEADDR is a no-op — sufficient for wget
    (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int64_t do_getsockopt(int fd, int level, int optname,
                      void *optval, uint32_t *optlen)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

// ── SYS_shutdown — half-close the connection ─────────────────
// how: 0 = SHUT_RD, 1 = SHUT_WR, 2 = SHUT_RDWR.  busybox wget calls
// shutdown(fd, SHUT_WR) after sending the request so the server sees
// EOF on its read side and responds.  lwIP's netconn_shutdown sends
// a FIN for shut_tx on TCP.

int64_t do_shutdown(int fd, int how)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    if (!s->conn) return -ENOTCONN;
    u8_t shut_rx = (how == 0 || how == 2) ? 1 : 0;
    u8_t shut_tx = (how == 1 || how == 2) ? 1 : 0;
    err_t err = netconn_shutdown((struct netconn *)s->conn, shut_rx, shut_tx);
    if (err == ERR_OK) return 0;
    return -EIO;
}

// ── SYS_getifaddr — return netif IPv4 address ────────────────
extern struct netif os01_netif;

int64_t do_getifaddr(void)
{
    return (int64_t)ip4_addr_get_u32(&os01_netif.ip_addr);
}
