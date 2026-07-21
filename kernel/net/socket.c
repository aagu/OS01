// kernel/net/socket.c — BSD socket implementation backed by lwIP netconn
#include <net/socket.h>
#include <uapi/sockaddr.h>
#include <net/net.h>
#include <kernel/file.h>
#include "lwip/netif.h"
#include <kernel/task.h>
#include <kernel/slab.h>   // kmalloc, kfree
#include <kernel/poll.h>
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
        (u8_t)protocol, NULL);
    if (!conn) return NULL;

    socket_t *s = (socket_t *)kmalloc(sizeof(socket_t));
    if (!s) { netconn_delete(conn); return NULL; }

    s->conn     = conn;
    s->domain   = domain;
    s->type     = type;
    s->protocol = protocol;
    s->state    = SOCK_UNCONNECTED;
    s->bound    = 0;
    spin_init(&s->lock);
    list_init(&s->poll_list);

    return s;
}

void socket_free(socket_t *s)
{
    if (!s) return;
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
                                  (u16_t)len, 0x01);  // NETCONN_COPY
        return (err == ERR_OK) ? (int64_t)len : -EIO;
    } else {
        // UDP: create netbuf with destination
        (void)flags;
        struct netbuf *nb = netbuf_new();
        if (!nb) return -ENOMEM;
        void *payload = netbuf_alloc(nb, (u16_t)len);
        if (!payload) { netbuf_delete(nb); return -ENOMEM; }
        memcpy(payload, buf, len);

        ip_addr_t addr;
        ip4_addr_set_u32(&addr, ip);
        netconn_sendto((struct netconn *)s->conn, nb, &addr, port);
        netbuf_delete(nb);
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
    err_t err = netconn_recv((struct netconn *)s->conn, &nb);
    if (err != ERR_OK) {
        if (err == ERR_CLSD) return 0;
        if (err == ERR_TIMEOUT) return -ETIMEDOUT;
        return -EIO;
    }

    void *data;
    u16_t data_len;
    netbuf_data(nb, &data, &data_len);
    u16_t copy = (data_len < (u16_t)len) ? data_len : (u16_t)len;
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

    if (signal_pending_fatal())
        return -EINTR;

    return copy;
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
    spin_init(&new_sock->lock);
    list_init(&new_sock->poll_list);

    file_t *new_f = file_alloc();
    if (!new_f) { socket_free(new_sock); return -ENOMEM; }
    new_f->type  = FD_SOCKET;
    new_f->sock  = new_sock;
    new_f->flags = O_RDWR;

    int new_fd = fd_alloc(current->files, new_f);
    if (new_fd < 0) {
        new_f->sock = NULL;
        file_free(new_f);
        return -ENFILE;
    }
    return new_fd;
}


// ── SYS_getsockname ──

int64_t do_getsockname(int fd, void *addr_ptr, uint64_t *addrlen_ptr)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;

    ip_addr_t lwip_addr;
    u16_t port;
    err_t err = netconn_getaddr((struct netconn *)s->conn, &lwip_addr, &port, 1);
    if (err != ERR_OK)
        return -EADDRNOTAVAIL;

    uint64_t usr_addrlen = *addrlen_ptr;
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
    sin.sin_port = port;
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
                      void *optval, uint64_t *optlen)
{
    socket_t *s = socket_get(fd);
    if (!s) return -EBADF;
    (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

// ── SYS_getifaddr — return netif IPv4 address ────────────────
extern struct netif os01_netif;

int64_t do_getifaddr(void)
{
    return (int64_t)ip4_addr_get_u32(&os01_netif.ip_addr);
}
