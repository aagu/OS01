/* kernel/arch/aarch64/syscall/sys_misc.c — aarch64 misc syscall group.
 *
 * The aarch64 counterpart of arch/x86_64/syscall/sys_misc.c, serving
 * the entries the product userspace needs (M4-remaining, 2026-11-19):
 * getrandom (csu.c stack-canary seeding, fail-closed), sync (block
 * cache flush), reboot (PSCI SYSTEM_RESET) and uname.  Entries stay
 * hidden in the selftest variants (the dispatch table gates this
 * group behind OS01_HAS_USERSPACE) so the frozen 15-entry subset
 * contract is untouched.
 */

#include <errno.h>
#include <string.h>
#include <uapi/syscall.h>
#include <syscall/dispatch.h>
#include <memory/uaccess.h>
#include <random/random.h>
#define GRND_NONBLOCK 0x0001UL
#define GRND_RANDOM   0x0002UL
#include <block/blockdev.h>
#include <memory/shared_backing.h>
#include <arch/aarch64/psci.h>
#include <uapi/time.h>                 /* struct utsname (kernel UAPI) */
#include <log/log.h>

int64_t sys_misc_dispatch(syscall_ctx_t *ctx)
{
    int64_t syscall_result = -EINVAL;
    switch (ctx->nr) {
    case SYS_getrandom: {
        // getrandom(void *buf, size_t len, unsigned int flags)
        uint64_t addr  = ctx->args[0];
        uint64_t len   = ctx->args[1];
        uint64_t flags = ctx->args[2];

        if (len == 0) { syscall_result = 0; break; }          // buf may be NULL
        if (flags & ~(GRND_NONBLOCK | GRND_RANDOM)) {         // pool never blocks
            syscall_result = -EINVAL;
            break;
        }
        if (len > RANDOM_MAX_LEN)
            len = RANDOM_MAX_LEN;                             // truncate, not error

        int rc = uaccess_user_write_begin(addr, (size_t)len);
        if (rc < 0) {                                         // -EFAULT
            syscall_result = rc;
            break;
        }
        get_random_bytes((void *)(uintptr_t)addr, len);
        uaccess_user_write_end();
        syscall_result = (int64_t)len;
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
    case SYS_reboot: {
        int cmd = (int)(int64_t)ctx->args[0];
        (void)cmd;
        log_info("[reboot] PSCI SYSTEM_RESET (cmd=%d)\n", cmd);
        int32_t st = psci_system_reset();
        // A failed reset returns to the caller (the reboot wrapper
        // reports it); fall through to the reboot-not-supported path.
        syscall_result = -EIO;
        if (st != 0)
            log_warn("[reboot] PSCI SYSTEM_RESET failed status=%d\n",
                     (int)st);
        break;
    }
    case SYS_uname: {
        struct utsname *buf = (struct utsname *)(uintptr_t)ctx->args[0];
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
        strcpy(kuts.machine, "aarch64");
        ssize_t r = copy_to_user_ft(buf, &kuts, sizeof(kuts));
        if (r < 0) {
            syscall_result = r;
            break;
        }
        syscall_result = 0;
        break;
    }
    default:
        break;
    }
    return syscall_result;
}
