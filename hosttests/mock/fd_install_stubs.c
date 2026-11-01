/*
 * hosttests/mock/fd_install_stubs.c
 *
 * ARCH-10 Task 6 — stub implementations for the descriptor-installation
 * host test.
 *
 * The host test links the PRODUCTION descriptor-installation code
 * (kernel/fs/file.c, kernel/net/socket.c, kernel/tty/pty.c,
 * kernel/sync/wait.c).  Those TUs reach outside the descriptor table
 * for the kernel services a host process cannot provide.  Every stub
 * here is observation-only: it records what the production code did
 * and returns a benign value.  None of them implements the behaviour
 * under test (the limit decision, the ownership hand-off or the
 * rollback) — that all lives in the production TUs.
 */

#include "fd_install_test_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <sched/task.h>

/* ── Kernel allocator ───────────────────────────────────────── */
int fake_kmalloc_calls = 0;
int fake_kfree_calls = 0;

void *kmalloc(size_t size)
{
    fake_kmalloc_calls++;
    return malloc(size);
}

size_t kfree(void *address)
{
    fake_kfree_calls++;
    free(address);
    return 0;
}

/* ── Kernel log sink ────────────────────────────────────────── */
int g_log_level = 0;

void _log_write(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

void _log_writev(int level, const char *fmt, va_list args)
{
    (void)level;
    (void)fmt;
    (void)args;
}

void _log_err_impl(const char *fmt, ...)  { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_info_impl(const char *fmt, ...) { (void)fmt; }

void log_set_level(int level) { g_log_level = level; }
int  log_get_level(void) { return g_log_level; }

/* ── Scheduler / signal stubs (never reached by the cases) ──── */
void schedule(void) { }
void task_wake(struct task_struct *t) { (void)t; }
int  signal_pgrp(pid_t target, int sig) { (void)target; (void)sig; return 0; }
int  arch_signal_pending_fatal(void) { return 0; }

/* ── VFS / devfs stubs ──────────────────────────────────────── */
void vfs_node_put(struct vfs_node *node) { (void)node; }

struct vfs_node *vfs_node_get(struct vfs_node *node)
{
    return node;
}

int vfs_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

int vfs_write(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    return 0;
}

void devfs_release_file(struct file *f) { (void)f; }

int devfs_ioctl_file(struct file *f, int cmd, void *arg)
{
    (void)f; (void)cmd; (void)arg;
    return -ENOTTY;
}

/* The PTMX open path registers its device through this entry.  The
 * case captures the production ptmx ops table here so it can drive
 * the real ptmx_open() without standing up the whole devfs registry. */
const struct devfs_ops *fake_fd_install_ptmx_ops = NULL;

int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops)
{
    (void)private_data;
    if (name && strcmp(name, "ptmx") == 0)
        fake_fd_install_ptmx_ops = ops;
    return 0;
}

/* ── Network readiness (socket.c asks before creating a socket) ─ */
bool net_service_ready(void) { return true; }

uint32_t net_default_ipv4(void) { return 0; }

/* ── lwIP netconn / netbuf mocks ────────────────────────────── */
int fake_netconn_new_calls = 0;
int fake_netconn_delete_calls = 0;
int fake_netconn_accept_calls = 0;
int fake_netconn_set_callback_calls = 0;
int fake_netconn_listen_calls = 0;

void fake_fd_install_runtime_reset(void)
{
    fake_netconn_new_calls = 0;
    fake_netconn_delete_calls = 0;
    fake_netconn_accept_calls = 0;
    fake_netconn_set_callback_calls = 0;
    fake_netconn_listen_calls = 0;
    fake_kmalloc_calls = 0;
    fake_kfree_calls = 0;
    fake_fd_install_ptmx_ops = NULL;
}

struct netconn *netconn_new_with_proto_and_callback(int type, uint8_t protocol,
                                                    netconn_callback cb)
{
    (void)cb;
    fake_netconn_new_calls++;
    struct netconn *c = (struct netconn *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->type = type;
    c->protocol = protocol;
    return c;
}

void netconn_delete(struct netconn *conn)
{
    if (!conn) return;
    fake_netconn_delete_calls++;
    free(conn);
}

void netconn_set_callback_arg(struct netconn *conn, void *arg)
{
    if (!conn) return;
    fake_netconn_set_callback_calls++;
    conn->callback_arg = arg;
}

void *netconn_get_callback_arg(struct netconn *conn)
{
    return conn ? conn->callback_arg : NULL;
}

err_t netconn_bind(struct netconn *conn, ip_addr_t *addr, uint16_t port)
{
    (void)addr;
    if (!conn) return ERR_ARG;
    conn->bound_ip = 0x0a000001;
    conn->bound_port = port;
    return ERR_OK;
}

err_t netconn_connect(struct netconn *conn, ip_addr_t *addr, uint16_t port)
{
    (void)conn; (void)addr; (void)port;
    return ERR_OK;
}

err_t netconn_listen_with_backlog(struct netconn *conn, uint8_t backlog)
{
    (void)backlog;
    if (!conn) return ERR_ARG;
    fake_netconn_listen_calls++;
    conn->listening = true;
    return ERR_OK;
}

err_t netconn_accept(struct netconn *conn, struct netconn **new_conn)
{
    if (!conn || !new_conn) return ERR_ARG;
    fake_netconn_accept_calls++;
    struct netconn *c = (struct netconn *)calloc(1, sizeof(*c));
    if (!c) return ERR_MEM;
    c->type = conn->type;
    c->protocol = conn->protocol;
    *new_conn = c;
    return ERR_OK;
}

err_t netconn_recv(struct netconn *conn, struct netbuf **nb)
{
    (void)conn; (void)nb;
    return ERR_CLSD;
}

err_t netconn_write_partly(struct netconn *conn, const void *buf, size_t len,
                           uint8_t apiflags, size_t *bytes_written)
{
    (void)conn; (void)buf; (void)apiflags;
    if (bytes_written) *bytes_written = len;
    return ERR_OK;
}

err_t netconn_sendto(struct netconn *conn, struct netbuf *nb, ip4_addr_t *ip,
                     uint16_t port)
{
    (void)conn; (void)nb; (void)ip; (void)port;
    return ERR_OK;
}

err_t netconn_shutdown(struct netconn *conn, uint8_t shut_rx, uint8_t shut_tx)
{
    (void)conn; (void)shut_rx; (void)shut_tx;
    return ERR_OK;
}

err_t netconn_getaddr(struct netconn *conn, ip_addr_t *addr, uint16_t *port,
                      int local)
{
    (void)local;
    if (!conn) return ERR_ARG;
    if (addr) ip4_addr_set_u32(addr, conn->bound_ip);
    if (port) *port = conn->bound_port;
    return ERR_OK;
}

struct netbuf *netbuf_new(void)
{
    return (struct netbuf *)calloc(1, sizeof(struct netbuf));
}

void *netbuf_alloc(struct netbuf *nb, uint16_t len)
{
    if (!nb) return NULL;
    nb->payload = calloc(1, len ? len : 1);
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
    if (!nb) return;
    if (data) *data = (char *)nb->payload + nb->offset;
    if (len)  *len = (uint16_t)(nb->len - nb->offset);
}

int netbuf_next(struct netbuf *nb)
{
    (void)nb;
    return -1;
}

ip4_addr_t *netbuf_fromaddr(struct netbuf *nb)
{
    (void)nb;
    return NULL;
}

uint16_t netbuf_fromport(struct netbuf *nb)
{
    (void)nb;
    return 0;
}
