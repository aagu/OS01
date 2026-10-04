#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#include <errno.h>
#include <sched/task.h>
#include <memory/vma.h>
#include <uapi/futex.h>
#include <sync/futex.h>

int64_t sys_mm_dispatch(syscall_ctx_t *ctx)
{
    int64_t syscall_result = -EINVAL;
    switch (ctx->nr) {
    case SYS_brk: {
        // brk(void *addr) — set program break, return new break.
        // Delegated to mm_set_brk (kernel/memory/vma.c, Task 4) so
        // the syscall stays in lockstep with the page-owner logic:
        // query returns 0/*result=current, bounds errors return
        // -EINVAL/-ENOMEM with *result unchanged, grow/shrink only
        // commit on success, and any OOM leaves old break/VMA/PTEs
        // intact.  See docs/.../user-heap-elf-isolation-design.md §5.2.
        uint64_t addr = ctx->args[0];
        mm_t *mm = current->mm;
        if (mm == NULL) {
            syscall_result = -ENOMEM;
            break;
        }
        uint64_t result = 0;
        int brk_rc = mm_set_brk(mm, addr, &result);
        if (brk_rc < 0) {
            syscall_result = (uint64_t)(int64_t)brk_rc;
            break;
        }
        syscall_result = result;
        break;
    }
    case SYS_mmap: {
        uint64_t addr   = ctx->args[0];
        uint64_t length = ctx->args[1];
        uint64_t prot   = ctx->args[2];
        uint64_t flags  = ctx->args[3];
        uint64_t fd     = ctx->args[4];
        uint64_t offset = ctx->args[5];
        syscall_result = do_mmap(addr, length, prot, flags, fd, offset);
        break;
    }
    case SYS_mprotect: {
        uint64_t addr   = ctx->args[0];
        uint64_t length = ctx->args[1];
        uint64_t prot   = ctx->args[2];
        syscall_result = do_mprotect(addr, length, prot);
        break;
    }
    case SYS_munmap: {
        uint64_t addr   = ctx->args[0];
        uint64_t length = ctx->args[1];
        syscall_result = do_munmap(addr, length);
        break;
    }
    case SYS_futex: {
        int *uaddr = (int *)ctx->args[0];
        int op = (int)ctx->args[1];
        int val = (int)ctx->args[2];

        if ((uint64_t)uaddr >= current->addr_limit) {
            syscall_result = -EFAULT;
            break;
        }

        switch (op) {
        case FUTEX_WAIT:
            syscall_result = do_futex_wait(uaddr, val);
            break;
        case FUTEX_WAKE:
            syscall_result = do_futex_wake(uaddr, val);
            break;
        default:
            syscall_result = -EINVAL;
        }
        break;
    }
    default:
        break;
    }
    return syscall_result;
}
