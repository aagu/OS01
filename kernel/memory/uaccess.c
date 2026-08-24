// kernel/memory/uaccess.c — syscall-boundary DoS hardening: fault-tolerant
// user-memory copy primitives.  See kernel/include/kernel/uaccess.h for the
// contract and docs/superpowers/specs/2026-08-23-syscall-boundary-audit-design.md
// for the rationale.
//
// The primitives rely on a small shim in do_page_fault (kernel-mode branch,
// see kernel/arch/x86_64/trap.c): when a user-range address faults while a
// task has a non-NULL fault_jmp, the #PF handler longjmps back to the
// primitive (value 1, compile-time constant per Clang) instead of panicking.
// do_page_fault uses `current` (= task_from_ist0 / rsp & ~(STACK_SIZE-1)),
// which is correct because #PF is dispatched on IST 0 = the task's kernel
// stack (verified by Task 0).
//
// All primitives are unsafe to call while holding a spinlock / IRQ critical
// section / resource that must be released on the normal path — the longjmp
// skip the rest of the calling function and unwind via the caller instead.
// _res variants release per-callback resources on the fault path; see
// copy_to_user_ft_res below.
//
// Note: the plain copy_to_user_ft / copy_from_user_ft are provided as
// static inline wrappers in kernel/include/kernel/uaccess.h (Task 1),
// forwarding to the _res variants with NULL callback.  No out-of-line
// definitions live here.

#include <kernel/uaccess.h>
#include <kernel/task.h>       // current, fault_jmp, fault_cleanup, fault_cleanup_arg, addr_limit
#include <kernel/memory.h>     // Phy_To_Virt
#include <errno.h>

// ── Fault-tolerant user copy with optional on-fault cleanup ──
// Longjmp value MUST be a compile-time constant (Clang constraint).
// do_page_fault redirects user-range #PF to the armed buffer (see trap.c).
//
// _to: kernel→user dirty side is `dst` (user).  Caller can pre-flight the
//      range with syscall_check_user_range — but the _ft is the authority;
//      the walker is a snapshot, munmap can race the gap.
// _from: kernel←user dirty side is `src` (user).  Identical body; split for
//        call-site clarity and a future direction that may specialize
//        (e.g. lazy SMAP enabling on _from).
ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    if (n == 0) return 0;
    os01_jmp_buf jb;
    void **old = current->fault_jmp;
    void (*old_cb)(void *) = current->fault_cleanup;
    void *old_arg = current->fault_cleanup_arg;
    current->fault_jmp = (void **)jb;
    current->fault_cleanup = on_fault;
    current->fault_cleanup_arg = arg;
    if (__builtin_setjmp((void **)jb) == 0) {
        __builtin_memcpy(dst, src, n);
        current->fault_jmp = old;
        current->fault_cleanup = old_cb;
        current->fault_cleanup_arg = old_arg;
        return (ssize_t)n;
    }
    if (on_fault) on_fault(arg);          // release reservation on the fault path
    current->fault_jmp = old;
    current->fault_cleanup = old_cb;
    current->fault_cleanup_arg = old_arg;
    return -EFAULT;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    if (n == 0) return 0;
    os01_jmp_buf jb;
    void **old = current->fault_jmp;
    void (*old_cb)(void *) = current->fault_cleanup;
    void *old_arg = current->fault_cleanup_arg;
    current->fault_jmp = (void **)jb;
    current->fault_cleanup = on_fault;
    current->fault_cleanup_arg = arg;
    if (__builtin_setjmp((void **)jb) == 0) {
        __builtin_memcpy(dst, src, n);
        current->fault_jmp = old;
        current->fault_cleanup = old_cb;
        current->fault_cleanup_arg = old_arg;
        return (ssize_t)n;
    }
    if (on_fault) on_fault(arg);
    current->fault_jmp = old;
    current->fault_cleanup = old_cb;
    current->fault_cleanup_arg = old_arg;
    return -EFAULT;
}

// Bounded fault-tolerant string scan: stop at NUL or max.  Fault -> -EFAULT.
int strnlen_user(const void *user_addr, size_t max)
{
    os01_jmp_buf jb;
    void **old = current->fault_jmp;
    current->fault_jmp = (void **)jb;
    if (__builtin_setjmp((void **)jb) == 0) {
        const char *p = (const char *)user_addr;
        size_t i = 0;
        while (i < max && p[i] != '\0') i++;
        current->fault_jmp = old;
        return (int)i;                        // ==max -> caller decides ENAMETOOLONG
    }
    current->fault_jmp = old;
    return -EFAULT;
}

// ── Range validation (fast reject + semantic filter; _ft is the authority) ──
// Order matters: len==0 -> true (no mm needed); arithmetic rejects; then
// fail-closed on missing mm/pml4 (boot ctx: init_mm.pml4 is unset) so the
// walker is NEVER invoked with a null table.
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    if (len == 0) return true;
    if (addr == 0 || addr < USER_MIN_ADDR) return false;
    if (addr >= current->addr_limit || len > current->addr_limit - addr)
        return false;
    if (current->mm == NULL || current->mm->pml4 == NULL)
        return false;                       // fail-closed: no user address space
    uint64_t *pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    return arch_user_range_accessible(pml4, addr, len, writable);
}
