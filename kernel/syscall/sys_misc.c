#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#include <errno.h>
#include <arch/syscall.h>
#include <sched/task.h>
#include <memory/uaccess.h>
#include <memory/vmm.h>
#include <sys/random.h>
#include <random/random.h>
#include <uapi/time.h>
#include <string.h>
#include <log/log.h>

int64_t sys_misc_dispatch(syscall_ctx_t *ctx)
{
    int64_t syscall_result = -EINVAL;
    switch (ctx->nr) {
    case SYS_putchar:
        syscall_result = arch_syscall_putchar(ctx->args[0]);
        break;
    case SYS_getrandom: {
        // getrandom(void *buf, size_t len, unsigned int flags)
        uint64_t addr  = ctx->args[0];
        uint64_t len   = ctx->args[1];
        uint64_t flags = ctx->args[2];

        if (len == 0) { syscall_result = 0; break; }              // buf may be NULL
        if (flags & ~(GRND_NONBLOCK | GRND_RANDOM)) {        // pool never blocks
            syscall_result = -EINVAL; break;
        }
        if (len > RANDOM_MAX_LEN) len = RANDOM_MAX_LEN;      // truncate, not error

        int rc = user_write_range_begin(addr, len);          // mm->lock + per-page PTE
        if (rc < 0) { syscall_result = rc; break; }               // -EFAULT (lock released)
        get_random_bytes((void *)addr, len);                 // chunked pool fill; mm->lock held
        user_write_range_end();
        syscall_result = len;                                     // actual bytes filled
        break;
    }
    case SYS_sync: {
        // sync() — flush filesystem caches to disk
        // For OS01 (FAT32 without write-back cache), this is a no-op.
        // Future: flush AHCI/FAT buffers here.
        syscall_result = 0;
        break;
    }
    case SYS_reboot:
        arch_syscall_reboot((int)(int64_t)ctx->args[0]);
    case SYS_uname: {
        // uname(struct utsname *buf) → 0 / -EFAULT.
        // Build kernel struct first, then _ft write it.  This avoids
        // bare field writes into user space (sa_handler/sa_mask path).
        struct utsname *buf = (struct utsname *)ctx->args[0];
        if (!buf) {
            syscall_result = -EFAULT;
            break;
        }
        struct utsname kuts;
        memset(&kuts, 0, sizeof(kuts));
        strcpy(kuts.sysname, "OS01");
        strcpy(kuts.nodename, "os01");
        strcpy(kuts.release, "0.1.0");
        strcpy(kuts.version, "0.1.0");
        strcpy(kuts.machine, "x86_64");
        ssize_t r = copy_to_user_ft(buf, &kuts, sizeof(kuts));
        if (r < 0) { syscall_result = r; break; }
        syscall_result = 0;
        break;
    }
    default:
        break;
    }
    return syscall_result;
}
