#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#include <errno.h>
#include <memory/uaccess.h>
#include <memory/slab.h>
#include <net/socket.h>
#include <uapi/sockaddr.h>
#include <string.h>

// Bound socket-option allocations to typical payload sizes (IP_PKTINFO,
// SO_LINGER, TCP options, etc.) so hostile lengths cannot request huge buffers.
#define SOCKOPT_MAX 4096

int64_t sys_net_dispatch(syscall_ctx_t *ctx)
{
    int64_t syscall_result = -EINVAL;
    switch (ctx->nr) {
    case SYS_socket: {
        syscall_result = do_socket((int)ctx->args[0], (int)ctx->args[1], (int)ctx->args[2]);
        break;
    }
    case SYS_connect: {
        // Cat B: copy user sockaddr_in to kernel via _ft, then call.
        if (ctx->args[2] < sizeof(struct sockaddr_in) ||
            !syscall_check_user_range(ctx->args[1],
                                      sizeof(struct sockaddr_in), false)) {
            syscall_result = -EFAULT; break;
        }
        struct sockaddr_in addr;
        if (copy_from_user_ft(&addr, (void *)ctx->args[1], sizeof(addr)) < 0) {
            syscall_result = -EFAULT; break;
        }
        // sin_port is network byte order; lwIP netconn_connect wants host order.
        syscall_result = do_connect((int)ctx->args[0], addr.sin_addr, os01_ntohs(addr.sin_port));
        break;
    }
    case SYS_sendto: {
        // Cat B: addr → kernel copy via _ft.  buf goes through
        // Task 8 (Cat C VFS bounce) — the kernel→user bounce
        // for write direction; here we only validate the buf range.
        uint64_t len = ctx->args[2];
        uint32_t ip = 0; uint16_t port = 0;
        uint64_t addr_ptr = ctx->args[4];
        if (len && !syscall_check_user_range(ctx->args[1], len, false)) {
            syscall_result = -EFAULT; break;
        }
        if (addr_ptr) {
            if (ctx->args[5] < sizeof(struct sockaddr_in) ||
                !syscall_check_user_range(addr_ptr,
                                          sizeof(struct sockaddr_in), false)) {
                syscall_result = -EFAULT; break;
            }
            struct sockaddr_in a;
            if (copy_from_user_ft(&a, (void *)addr_ptr, sizeof(a)) < 0) {
                syscall_result = -EFAULT; break;
            }
            ip = a.sin_addr; port = a.sin_port;
        }
        syscall_result = do_sendto((int)ctx->args[0], (void *)ctx->args[1],
                              len, (int)ctx->args[3], ip, os01_ntohs(port));
        break;
    }
    case SYS_recvfrom: {
        // Cat B: addrlen → kernel via _ft; addr/buf write-back via _ft.
        // Only the _ft write-back success commits the recv (do_recvfrom
        // already consumed the netbuf; on _ft failure we return -EFAULT
        // but the data is gone — POSIX semantics; the user must retry).
        uint64_t addr_ptr = ctx->args[4];
        uint64_t addrlen_ptr = ctx->args[5];
        if (ctx->args[2] && !syscall_check_user_range(ctx->args[1], ctx->args[2], true)) {
            syscall_result = -EFAULT; break;
        }
        uint32_t ip = 0;
        uint16_t port = 0;
        uint32_t addrlen = 0;
        if (addr_ptr) {
            if (!syscall_check_user_range(addrlen_ptr, sizeof(addrlen), false)) {
                syscall_result = -EFAULT; break;
            }
            if (copy_from_user_ft(&addrlen, (void *)addrlen_ptr, sizeof(addrlen)) < 0) {
                syscall_result = -EFAULT; break;
            }
            if (addrlen < sizeof(struct sockaddr_in) ||
                !syscall_check_user_range(addr_ptr,
                                          sizeof(struct sockaddr_in), true)) {
                syscall_result = -EINVAL; break;
            }
        }
        int64_t ret = do_recvfrom((int)ctx->args[0], (void *)ctx->args[1],
                                  ctx->args[2], (int)ctx->args[3],
                                  addr_ptr ? &ip : NULL,
                                  addr_ptr ? &port : NULL);
        if (ret >= 0 && addr_ptr) {
            struct sockaddr_in src;
            memset(&src, 0, sizeof(src));
            src.sin_family = AF_INET;
            src.sin_port = os01_htons(port);
            src.sin_addr = ip;
            {
                ssize_t user_copy_rc = copy_to_user_ft((void *)addr_ptr, &src, sizeof(src));
                if (user_copy_rc < 0) {
                    syscall_result = user_copy_rc; break;
                }
            }
            uint32_t new_addrlen = sizeof(src);
            {
                ssize_t user_copy_rc = copy_to_user_ft((void *)addrlen_ptr, &new_addrlen,
                                sizeof(new_addrlen));
                if (user_copy_rc < 0) {
                    syscall_result = user_copy_rc; break;
                }
            }
        }
        syscall_result = ret;
        break;
    }
    case SYS_bind: {
        // Cat B: copy user sockaddr_in to kernel via _ft.
        if (ctx->args[2] < sizeof(struct sockaddr_in) ||
            !syscall_check_user_range(ctx->args[1],
                                      sizeof(struct sockaddr_in), false)) {
            syscall_result = -EFAULT; break;
        }
        struct sockaddr_in a;
        if (copy_from_user_ft(&a, (void *)ctx->args[1], sizeof(a)) < 0) {
            syscall_result = -EFAULT; break;
        }
        syscall_result = do_bind((int)ctx->args[0], a.sin_addr, os01_ntohs(a.sin_port));
        break;
    }
    case SYS_listen: {
        syscall_result = do_listen((int)ctx->args[0], (int)ctx->args[1]);
        break;
    }
    case SYS_accept: {
        syscall_result = do_accept((int)ctx->args[0], NULL, NULL);
        break;
    }
    case SYS_setsockopt: {
        // Cat B: cap + reject a zero optlen BEFORE kmalloc
        // to prevent DoS via giant allocations.  Read optval via _ft.
        uint64_t optlen = ctx->args[4];
        if (optlen == 0 || optlen > SOCKOPT_MAX) {
            syscall_result = -EINVAL; break;
        }
        if (!syscall_check_user_range(ctx->args[3], optlen, false)) {
            syscall_result = -EFAULT; break;
        }
        void *optval = kmalloc(optlen);
        if (!optval) { syscall_result = -ENOMEM; break; }
        if (copy_from_user_ft(optval, (void *)ctx->args[3], optlen) < 0) {
            kfree(optval);
            syscall_result = -EFAULT; break;
        }
        syscall_result = do_setsockopt((int)ctx->args[0], (int)ctx->args[1],
                                  (int)ctx->args[2], optval, optlen);
        kfree(optval);
        break;
    }
    case SYS_getsockname: {
        // Cat B: do_getsockname into kernel sockaddr_in + klen, then
        // _ft write both back to user so hostile pointers cannot make the
        // kernel scribble into itself.
        if (!syscall_check_user_range(ctx->args[1],
                                      sizeof(struct sockaddr_in), true) ||
            !syscall_check_user_range(ctx->args[2], sizeof(uint32_t), true)) {
            syscall_result = -EFAULT; break;
        }
        struct sockaddr_in kaddr;
        uint32_t klen = sizeof(kaddr);
        int64_t ret = do_getsockname((int)ctx->args[0], &kaddr, &klen);
        if (ret < 0) { syscall_result = ret; break; }
        ssize_t wr = copy_to_user_ft((void *)ctx->args[1], &kaddr, sizeof(kaddr));
        if (wr >= 0) wr = copy_to_user_ft((void *)ctx->args[2], &klen, sizeof(klen));
        if (wr < 0) { syscall_result = wr; break; }
        syscall_result = ret;
        break;
    }
    case SYS_getifaddr: {
        syscall_result = do_getifaddr();
        break;
    }
    case SYS_getsockopt: {
        // Cat B: read user optlen (the fifth argument points to uint32_t) FIRST,
        // bounded by SOCKOPT_MAX.  Only then allocate + do_getsockopt
        // + _ft write-back.  This prevents oversized allocations
        // (which would kmalloc a 4GB buffer).
        if (!syscall_check_user_range(ctx->args[4], sizeof(uint32_t), false)) {
            syscall_result = -EFAULT; break;
        }
        uint32_t klen = 0;
        if (copy_from_user_ft(&klen, (void *)ctx->args[4], sizeof(klen)) < 0) {
            syscall_result = -EFAULT; break;
        }
        if (klen > SOCKOPT_MAX) { syscall_result = -EINVAL; break; }
        void *kopt = kmalloc(klen ? klen : 1);
        if (!kopt) { syscall_result = -ENOMEM; break; }
        int64_t ret = do_getsockopt((int)ctx->args[0], (int)ctx->args[1],
                                    (int)ctx->args[2], kopt, &klen);
        if (ret < 0) {
            kfree(kopt);
            syscall_result = ret;
            break;
        }
        ssize_t wr = copy_to_user_ft((void *)ctx->args[3], kopt, klen);
        ssize_t wlr = copy_to_user_ft((void *)ctx->args[4], &klen, sizeof(klen));
        kfree(kopt);
        if (wr < 0 || wlr < 0) { syscall_result = wr < 0 ? wr : wlr; break; }
        syscall_result = ret;
        break;
    }
    case SYS_shutdown: {
        syscall_result = do_shutdown((int)ctx->args[0], (int)ctx->args[1]);
        break;
    }
    default:
        break;
    }
    return syscall_result;
}
