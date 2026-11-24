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
#include <block/blockdev.h>
#include <memory/shared_backing.h>
#include <log/log.h>
#include <arch/x86_64/uaccess.h>
#include <arch/x86_64/user_layout.h>
#include <arch/irq.h>

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
        // sync() — write back dirty shared mappings first, then flush
        // the ARCH-6 block cache for every disk.
        int first_err = shared_backing_sync_all();
        for (int i = 0; ; i++) {
            block_device_t *d = block_device_get(i);
            if (!d)
                break;
            if (d->kind != BLOCK_DISK)
                continue;
            int r = block_device_flush(d);
            if (r != 0 && first_err == 0)
                first_err = r;
        }
        syscall_result = first_err;
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
    case SYS_arch_prctl: {
        /* arch_prctl(op, arg): op is the full 64-bit operation code.
         * ARCH_SET_FS accepts an inclusive low-half address; a null FS
         * selector plus the IA32_FS_BASE MSR carries the value. */
        uint64_t op  = ctx->args[0];
        uint64_t arg = ctx->args[1];

        if (op == ARCH_SET_FS) {
            if (arg > X86_USER_ADDR_LIMIT) {
                syscall_result = -EPERM;
                break;
            }
            task_t *cur = arch_current_task();
            arch_irq_state_t flags = arch_local_irq_save();
            x86_install_current_user_fs(cur->thread, arg);
            arch_local_irq_restore(flags);
            syscall_result = 0;
        } else if (op == ARCH_GET_FS) {
            uint64_t base = x86_read_fs_base();
            ssize_t r = copy_to_user_ft((void *)(uintptr_t)arg, &base,
                                        sizeof(base));
            syscall_result = r < 0 ? (int64_t)r : 0;
        } else {
            syscall_result = -EINVAL;
        }
        break;
    }
    default:
        break;
    }
    return syscall_result;
}
