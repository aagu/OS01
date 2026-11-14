/* kernel/arch/aarch64/memory/uaccess.c
 *
 * AArch64 user-memory access.
 *
 * Two producers share the unprivileged LDTRB/STTRB copy loops in
 * user_copy.S:
 *
 *   - the FORMAL path (every aarch64 image): a copy on behalf of the
 *     running formal EL0 user task, constrained by that task's mm.  It
 *     registers a window, runs with local IRQs masked, and a same-EL
 *     data abort on one of the two user instructions is recovered to
 *     -EFAULT by aarch64_uaccess_fixup().
 *
 *   - the legacy M4 probe (AARCH64_EL0_TEST only): its private TTBR0
 *     root and its own validation.  Nothing in the formal window is
 *     involved while the probe owns the frame.
 *
 * The formal window is never nested and never crosses tasks: a second
 * copy attempted while a window is open is refused and the outer
 * window is left untouched.
 */

#include <arch/aarch64/uaccess.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/aarch64/page_table.h>
#include <arch/mmu.h>
#include <arch/irq.h>
#include <memory/memory.h>
#include <sched/task.h>
#include <errno.h>
#include <limits.h>
#include <arch/aarch64/mmu.h>
#if AARCH64_EL0_TEST
#include <arch/aarch64/el0.h>
#include <selftest/aarch64_user_svc_uaccess.h>
#endif

extern const char aarch64_uaccess_load[], aarch64_uaccess_store[];
extern const char aarch64_uaccess_fault[];

/* The formal copy window that is currently open, or a zeroed
 * `present == false` record.  Single-core BSP only: the window is
 * opened and cleared with local IRQs masked and never across a
 * scheduling point, so one static record is the whole state. */
static struct aarch64_uaccess_window g_window;

/* ── Formal range precheck ───────────────────────────────────────
 *
 * User range in [AARCH64_USER_MIN, AARCH64_USER_LIMIT), end bound
 * checked by subtraction.  The root PA comes ONLY from the running
 * formal user task's mm->pgdir; it must be a published root and the
 * hardware TTBR0_EL1 must still hold it.  The whole range is then
 * checked page by page for the requested direction.  No window is
 * opened here and no lock is held on return: the publish registry and
 * the page-table walker finish before the caller touches user memory.
 */
/* Three outcomes: the range/task/root snapshot is usable, it is not
 * usable at all, or it is usable but the page-permission walk failed.
 * Only the last case may try the anonymous demand-fill path — a hole
 * or PROT_NONE page still ends in -EFAULT after that attempt. */
enum aarch64_range_result {
    AARCH64_RANGE_OK,
    AARCH64_RANGE_DENIED,
    AARCH64_RANGE_PERMISSION,
};

static enum aarch64_range_result
aarch64_user_range_precheck(uint64_t addr, size_t len,
                            bool writable, bool skip_permission,
                            struct mm_struct **out_mm,
                            uint64_t *out_root_pa)
{
    if (addr < AARCH64_USER_MIN || addr >= AARCH64_USER_LIMIT)
        return AARCH64_RANGE_DENIED;
    if (len > AARCH64_USER_LIMIT - addr)
        return AARCH64_RANGE_DENIED;

    struct mm_struct *mm = aarch64_user_current_mm();
    if (!mm || !mm->pgdir)
        return AARCH64_RANGE_DENIED;

    uint64_t root_pa = (uintptr_t)mm->pgdir;
    if (!aarch64_pt_root_is_published(&root_pa))
        return AARCH64_RANGE_DENIED;

    uint64_t ttbr0;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    if ((ttbr0 & AARCH64_TTBR_BASE_MASK) != root_pa)
        return AARCH64_RANGE_DENIED;

    /* The M4 acceptance bypass skips ONLY this page-permission walk; the
     * range, task, mm and root checks above still gate the window. */
    if (!skip_permission &&
        !arch_user_range_accessible((void *)Phy_To_Virt(root_pa),
                                    addr, (uint64_t)len, writable)) {
        /* The caller needs the mm/root of a permission-mismatch
         * snapshot: that is exactly the range whose demand/COW
         * preparation has to run next. */
        *out_mm = mm;
        *out_root_pa = root_pa;
        return AARCH64_RANGE_PERMISSION;
    }

    *out_mm = mm;
    *out_root_pa = root_pa;
    return AARCH64_RANGE_OK;
}

/* The one fault exit: a failed copy reports -EFAULT and invokes the
 * caller's cleanup callback at most once. */
static ssize_t user_copy_fault_ret(void (*on_fault)(void *), void *arg,
                                   int err)
{
    if (on_fault)
        on_fault(arg);
    return err;
}

static ssize_t user_copy_fault(void (*on_fault)(void *), void *arg)
{
    return user_copy_fault_ret(on_fault, arg, -EFAULT);
}

/* ── The formal copy ────────────────────────────────────────────
 *
 * n == 0 succeeds without touching user memory (a NULL pointer is
 * allowed).  A non-zero length must fit in ssize_t.  The copy runs
 * with local IRQs masked; the window is cleared BEFORE the caller's
 * IRQ state is restored, and only then does a failure callback run.
 */
static ssize_t copy_user_common(uint64_t user_addr, const void *kern, size_t n,
                                bool write, bool skip_permission,
                                void (*on_fault)(void *), void *arg)
{
    if (n == 0)
        return 0;
    if (n > (size_t)SSIZE_MAX)
        return user_copy_fault(on_fault, arg);

    arch_irq_state_t state = arch_local_irq_save();

    /* A nested copy is refused before any other check; the outer
     * window is preserved. */
    if (g_window.present)
        goto fault;

    struct mm_struct *mm = NULL;
    uint64_t root_pa = 0;

    enum aarch64_range_result r =
        aarch64_user_range_precheck(user_addr, n, write, skip_permission,
                                    &mm, &root_pa);
    if (r == AARCH64_RANGE_DENIED)
        goto fault;

    if (r == AARCH64_RANGE_PERMISSION) {
        /* Only an authorized formal copy may instantiate anonymous
         * pages.  The adapter refuses (without switching roots) when
         * this is not a returning native dispatch or a demand fault;
         * -ENOMEM is preserved for the caller. */
        int prc = aarch64_user_vm_prepare_copy(mm, user_addr, n, write);
        if (prc == -ENOMEM) {
            arch_local_irq_restore(state);
            return user_copy_fault_ret(on_fault, arg, -ENOMEM);
        }
        if (prc != 0)
            goto fault;

        /* The transaction reinstalled the root: re-check the live
         * published root and the complete page permissions before the
         * exact window is opened. */
        mm = NULL;
        root_pa = 0;
        if (aarch64_user_range_precheck(user_addr, n, write, false,
                                        &mm, &root_pa) != AARCH64_RANGE_OK)
            goto fault;
    }

    g_window = (struct aarch64_uaccess_window){
        .present = true,
        .write   = write,
        .lo      = user_addr,
        .hi      = user_addr + n,
        .task    = (const void *)current,
        .mm      = (const void *)mm,
        .root_pa = root_pa,
    };

    int rc = write
        ? aarch64_copy_to_user_raw(user_addr, kern, n)
        : aarch64_copy_from_user_raw((void *)kern, user_addr, n);

    g_window = (struct aarch64_uaccess_window){0};
    arch_local_irq_restore(state);

    if (rc < 0)
        return user_copy_fault(on_fault, arg);
    return (ssize_t)n;

fault:
    arch_local_irq_restore(state);
    return user_copy_fault(on_fault, arg);
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    return copy_user_common((uint64_t)(uintptr_t)dst, src, n, true,
                            false, on_fault, arg);
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    return copy_user_common((uint64_t)(uintptr_t)src, dst, n, false,
                            false, on_fault, arg);
}

#if AARCH64_EL0_TEST
/* The M4 acceptance bypass: the SAME implementation with only the
 * page-permission precheck skipped, so a real hardware abort reaches
 * the registered window and the EL1 fixup.  There is no production
 * entry to this shape. */
ssize_t aarch64_uaccess_test_copy(bool write, uint64_t user_addr, void *kern,
                                  size_t n, void (*on_fault)(void *), void *arg)
{
    return copy_user_common(user_addr, kern, n, write, true, on_fault, arg);
}
#endif /* AARCH64_EL0_TEST */

/* ── Syscall entry-point range check ────────────────────────────
 *
 * Snapshot only: true iff the whole range is mapped for the current
 * formal user task with the requested effective permission.  A zero
 * length is a no-op success.  Callers still use the fault-tolerant
 * copies for the actual access (this check is not a pin).
 */
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    if (len == 0)
        return true;

    struct mm_struct *mm = NULL;
    uint64_t root_pa = 0;
    return aarch64_user_range_precheck(addr, (size_t)len, writable, false,
                                       &mm, &root_pa) == AARCH64_RANGE_OK;
}

#if AARCH64_EL0_TEST
/* ── Legacy M4 probe wrappers ──────────────────────────────────
 * The probe installs its own private root and validates the range
 * against it.  These wrappers are test-only; the formal entry points
 * above never consult a probe root. */
int aarch64_copy_from_user(void *dst, uint64_t src, size_t len)
{
    uint64_t *root = aarch64_el0_active_root();
    if (!root || !arch_user_range_accessible(root, src, len, false))
        return -EFAULT;
    return aarch64_copy_from_user_raw(dst, src, len);
}

int aarch64_copy_to_user(uint64_t dst, const void *src, size_t len)
{
    uint64_t *root = aarch64_el0_active_root();
    if (!root || !arch_user_range_accessible(root, dst, len, true))
        return -EFAULT;
    return aarch64_copy_to_user_raw(dst, src, len);
}
#endif /* AARCH64_EL0_TEST */

/* ── EL1 copy fixup ────────────────────────────────────────────
 *
 * PURE predicate first (inputs only), then the live-state check.  On
 * a match the exception frame's ELR is redirected to the -EFAULT
 * trampoline; the trampoline returns to the C wrapper, which clears
 * the window.  A kernel-buffer ordinary load/store, a fault outside
 * the window and any task/mm/root disagreement stay fatal.
 */
__attribute__((no_stack_protector))
bool aarch64_uaccess_fixup(pt_regs_t *regs, uint64_t esr, uint64_t far)
{
#if AARCH64_EL0_TEST
    /* The legacy probe owns its frame while it is active and keeps its
     * own independent validation; the formal window is not involved. */
    if (aarch64_el0_active_root()) {
        uint64_t ec = (esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;
        uint64_t fsc = esr & AARCH64_ESR_ISS_FSC_MASK;

        if ((regs->spsr_el1 & AARCH64_SPSR_MODE_MASK) != AARCH64_SPSR_MODE_EL1H ||
            ec != AARCH64_EC_DATA_ABORT_SAME ||
            (esr & (AARCH64_ESR_FNV_BIT | AARCH64_ESR_ISS_CM |
                    AARCH64_ESR_ISS_S1PTW)) ||
            fsc < AARCH64_FSC_ACCESS_FIRST || fsc > AARCH64_FSC_ACCESS_LAST ||
            far < AARCH64_USER_MIN || far >= AARCH64_USER_LIMIT)
            return false;

        bool write = (esr & AARCH64_ESR_ISS_WNR) != 0;
        uintptr_t pc = write ? (uintptr_t)aarch64_uaccess_store
                             : (uintptr_t)aarch64_uaccess_load;
        if (regs->elr_el1 != pc || far != (write ? regs->x0 : regs->x1))
            return false;

        regs->elr_el1 = (uintptr_t)aarch64_uaccess_fault;
        return true;
    }
#endif

    const struct aarch64_uaccess_abort ab = {
        .esr       = esr,
        .spsr      = regs->spsr_el1,
        .pc        = regs->elr_el1,
        .far       = far,
        .read_ptr  = regs->x1,
        .write_ptr = regs->x0,
    };

    /* PURE shape check first: nothing below may touch the current task,
     * its mm or TTBR0 until the snapshot has been proven to describe a
     * formal copy abort.  On the early boot stack `current` is not a
     * task, so reading it before this gate could fault again and lose
     * the fatal diagnostic. */
    if (!aarch64_uaccess_abort_shape_matches(&ab, &g_window,
                                             (uintptr_t)aarch64_uaccess_load,
                                             (uintptr_t)aarch64_uaccess_store))
        return false;

    /* Shape matched: it is safe (and required) to verify that the live
     * task still owns the window's mm and root. */
    struct mm_struct *mm = aarch64_user_current_mm();
    uint64_t ttbr0;
    __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));

    const struct aarch64_uaccess_live live = {
        .task     = (const void *)current,
        .mm       = (const void *)mm,
        .root_pa  = mm ? (uint64_t)(uintptr_t)mm->pgdir : 0,
        .ttbr0_pa = ttbr0 & AARCH64_TTBR_BASE_MASK,
    };

    if (!aarch64_uaccess_window_holds(&g_window, &live))
        return false;

    regs->elr_el1 = (uintptr_t)aarch64_uaccess_fault;
    return true;
}
