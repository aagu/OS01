// kernel/memory/uaccess.c — syscall-boundary DoS hardening: fault-tolerant
// user-memory copy primitives.  See kernel/include/memory/uaccess.h for the
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
// static inline wrappers in kernel/include/memory/uaccess.h (Task 1),
// forwarding to the _res variants with NULL callback.  No out-of-line
// definitions live here.

#include <memory/uaccess.h>
#include <sched/task.h>       // current, fault_jmp, fault_cleanup, fault_cleanup_arg, addr_limit
#include <memory/memory.h>     // Phy_To_Virt
#include <memory/vma.h>        // vma_find, VMA_PROT_WRITE / VMA_IO / VMA_HEAP / VMA_ANON / VMA_PROT_READ
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)        // vmm_pt_walk, PAGE_VALID, PAGE_USER, PAGE_WRITE,
                               // PAGE_COW, PAGE_4K_SIZE, PAGE_4K_MASK
#include <memory/pmm.h>        // alloc_4k_page, free_4k_page, page_cow_put, tlb_shootdown
#include <memory/slab.h>       // kmalloc, kfree
#include <errno.h>
#include <string.h>            // memcpy (kernel-side stage copy)

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
//
// Task 7 (COW privatization): before installing fault_jmp + fault_cleanup,
// we must privatize every COW leaf in the destination.  prepare_user_write_range
// takes mm->lock and either:
//   - succeeds → we release the lock, install fault_jmp, do the memcpy
//   - fails with -EFAULT/-ENOMEM → on_fault fires EXACTLY ONCE, no
//     fault_jmp is installed, no stale cleanup callback lingers.
ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    if (n == 0) return 0;

    int rc = prepare_user_write_range(current->mm, (uint64_t)dst, n);
    if (rc < 0) {
        if (on_fault) on_fault(arg);
        return rc;
    }

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

// Validate effective page-table permissions, then enforce the VMA contract
// only for COW leaves. Writable ELF leaves have no VMA; huge stack leaves
// are already private. Neither needs a COW allocation.
static bool user_write_range_valid(mm_t *mm, uint64_t addr, size_t len,
                                   size_t *cow_count)
{
    *cow_count = 0;
    if (len == 0) return true;
    if (!mm || !mm->pgdir || addr < USER_MIN_ADDR ||
        addr >= current->addr_limit || len > current->addr_limit - addr)
        return false;
    uint64_t *pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);
    if (!arch_user_range_accessible(pgd, addr, len, true)) return false;

    uint64_t end = addr + len;
    for (uint64_t va = addr & PAGE_4K_MASK; va < end; ) {
        uint64_t *pte = vmm_pt_walk(pgd, va, 0, 0);
        if (!pte) {
            // The effective-permission walk already verified this huge leaf.
            va = (va & PAGE_2M_MASK) + PAGE_2M_SIZE;
            continue;
        }
        if (*pte & PAGE_COW) {
            vma_t *vma = vma_find(mm, va);
            if (!vma || !(vma->vm_flags & VMA_PROT_WRITE) ||
                (vma->vm_flags & VMA_IO)) return false;
            ++*cow_count;
        }
        va += PAGE_4K_SIZE;
    }
    return true;
}

// Pure snapshot: eligible COW leaves count as writable candidates, but no
// allocations, PTE changes or reference updates happen in the precheck.
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    if (len == 0) return true;
    if (addr < USER_MIN_ADDR || addr >= current->addr_limit ||
        len > current->addr_limit - addr ||
        !current->mm || !current->mm->pgdir) return false;
    if (writable) {
        size_t count;
        return user_write_range_valid(current->mm, addr, len, &count);
    }
    uint64_t *pgd = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pgdir);
    return arch_user_range_accessible(pgd, addr, len, false);
}

typedef struct {
    uint64_t *pte;
    uint64_t old_pte;
    uint64_t new_phys; // zero means the final COW owner can promote in place
    bool free_old;
} user_write_cow_t;

int prepare_user_write_range_locked(mm_t *mm, uint64_t addr, size_t len)
{
    size_t count;
    if (!user_write_range_valid(mm, addr, len, &count)) return -EFAULT;
    if (count == 0) return 0;
    if (count > SIZE_MAX / sizeof(user_write_cow_t)) return -ENOMEM;
    user_write_cow_t *staged = kmalloc(count * sizeof(*staged));
    if (!staged) return -ENOMEM;

    uint64_t *pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);
    size_t used = 0;
    int rc = -EFAULT;
    // Stage all fallible allocations before changing any PTE or COW ref.
    for (uint64_t va = addr & PAGE_4K_MASK; va < addr + len; ) {
        uint64_t *pte = vmm_pt_walk(pgd, va, 0, 0);
        if (!pte) {
            va = (va & PAGE_2M_MASK) + PAGE_2M_SIZE;
            continue;
        }
        va += PAGE_4K_SIZE;
        if (!(*pte & PAGE_COW)) continue;
        uint64_t phys = *pte & PAGE_4K_MASK;
        uint16_t refs = page_cow_refs(phys);
        if (refs == 0) goto rollback;
        uint64_t new_phys = 0;
        if (refs > 1) {
            new_phys = alloc_4k_page();
            if (!new_phys) { rc = -ENOMEM; goto rollback; }
            memcpy((void *)Phy_To_Virt(new_phys),
                   (void *)Phy_To_Virt(phys), PAGE_4K_SIZE);
        }
        staged[used++] = (user_write_cow_t){pte, *pte, new_phys, false};
    }

    for (size_t i = 0; i < used; ++i) {
        uint64_t old_phys = staged[i].old_pte & PAGE_4K_MASK;
        uint64_t phys = staged[i].new_phys ? staged[i].new_phys : old_phys;
        uint64_t flags = (staged[i].old_pte & ~PAGE_4K_MASK & ~PAGE_COW) |
                         PAGE_WRITE;
        *staged[i].pte = phys | flags;
        bool last = page_cow_put(old_phys);
        staged[i].free_old = last && staged[i].new_phys;
    }
    tlb_shootdown();
    // Another owner may exit during staging. Free its final old page only
    // after the changed PTEs are visible and stale translations are gone.
    for (size_t i = 0; i < used; ++i)
        if (staged[i].free_old)
            free_4k_page(staged[i].old_pte & PAGE_4K_MASK);
    kfree(staged);
    return 0;

rollback:
    for (size_t i = 0; i < used; ++i)
        if (staged[i].new_phys) free_4k_page(staged[i].new_phys);
    kfree(staged);
    return rc;
}

int prepare_user_write_range(mm_t *mm, uint64_t addr, size_t len)
{
    if (len == 0) return 0;
    if (!mm) return -EFAULT;
    spin_lock(&mm->lock);
    int rc = prepare_user_write_range_locked(mm, addr, len);
    spin_unlock(&mm->lock);
    return rc;
}
