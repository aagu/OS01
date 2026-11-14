/* kernel/arch/aarch64/memory/user_vm.c
 *
 * AArch64 anonymous-VM transaction layer (M4 Task 3).
 *
 * Owns the syscall/fault/uaccess navigation and the root-edit state:
 * admission (native SVC + fault authorization), IRQ/TLS registration,
 * root detach on entry and reinstall on exit.  The three-state leaf
 * adapter lives in user_vm_leaf.c; the policy itself lives in
 * user_vm_policy.c.
 */

#include <arch/aarch64/page_table.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/user_vm_policy.h>  /* policy entry points */
#include <arch/irq.h>
#include <errno.h>
#include <intr/dispatch.h>
#include <sched/task.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>

/* ══════════════════════════════════════════════════════════════
 * Transaction layer (M4 Task 3)
 * ══════════════════════════════════════════════════════════════ */

/* One authorization at a time, single-core BSP.  A native SVC holds
 * `native`; the short-lived fault path holds `fault`. */
static bool              g_native_auth;
static bool              g_fault_auth;
static const pt_regs_t  *g_auth_frame;
static mm_t             *g_auth_mm;

/* BSP IRQ nesting depth.  A transaction requires zero. */
static unsigned g_irq_depth;

/* The detached transaction, non-nestable.  The saved values are the
 * caller's IRQ state and both EL0 thread pointers. */
static bool g_edit_active;
static struct {
    arch_irq_state_t irq;
    uint64_t         tpidr_el0;
    uint64_t         tpidrro_el0;
    uint64_t         root_pa;
} g_edit;

void aarch64_vm_irq_enter(void)
{
    g_irq_depth++;
}

void aarch64_vm_irq_leave(void)
{
    if (g_irq_depth)
        g_irq_depth--;
}

void aarch64_vm_native_enter(pt_regs_t *regs, uint64_t nr)
{
    g_native_auth = false;
    g_auth_frame = NULL;
    g_auth_mm = NULL;

    /* SYS_exit never returns to EL0 and an unknown number has no
     * dispatch: neither may hold an edit authorization. */
    if (nr == SYS_exit || !syscall_has_handler(nr))
        return;
    if (!aarch64_user_frame_is_valid(regs))
        return;

    mm_t *mm = aarch64_user_current_mm();
    if (!mm || !mm->pgdir)
        return;

    uint64_t root_pa = aarch64_user_vm_read_ttbr0();
    if (!root_pa || root_pa != (uint64_t)(uintptr_t)mm->pgdir)
        return;
    if (!aarch64_user_vm_root_published(root_pa))
        return;

    g_native_auth = true;
    g_auth_frame = regs;
    g_auth_mm = mm;
}

void aarch64_vm_native_leave(void)
{
    g_native_auth = false;
    g_auth_frame = NULL;
    g_auth_mm = NULL;
}

int aarch64_vm_edit_begin(mm_t *mm)
{
    if (!g_native_auth && !g_fault_auth)
        return -EPERM;
    if (g_edit_active)
        return -EBUSY;                    /* non-nestable */
    if (g_irq_depth != 0)
        return -EBUSY;                    /* never from an IRQ path */
    if (!mm || mm != g_auth_mm || mm != aarch64_user_current_mm())
        return -EPERM;
    if (!g_auth_frame || !aarch64_user_frame_is_valid(g_auth_frame))
        return -EPERM;
    if (!mm->pgdir)
        return -EPERM;

    /* The root that will be reinstalled must be the live, published
     * user root — not merely the mm field. */
    uint64_t live = aarch64_user_vm_read_ttbr0();
    if (!live || live != (uint64_t)(uintptr_t)mm->pgdir)
        return -EPERM;
    if (!aarch64_user_vm_root_published(live))
        return -EPERM;

    g_edit.root_pa = live;
    g_edit.irq = arch_local_irq_save();          /* save + mask */
    aarch64_user_vm_save_tls(&g_edit.tpidr_el0, &g_edit.tpidrro_el0);
    aarch64_user_restore_kernel_root();
    g_edit_active = true;                        /* mark before IRQs on */
    arch_local_irq_enable();                     /* policy runs with IRQs */

    return 0;
}

void aarch64_vm_edit_end(void)
{
    if (!g_edit_active)
        return;

    arch_local_irq_disable();
    aarch64_user_install_root(g_edit.root_pa);   /* publish + install */
    /* install_root() clears both thread pointers; the entry values are
     * restored ON TOP of the new root. */
    aarch64_user_vm_restore_tls(g_edit.tpidr_el0, g_edit.tpidrro_el0);
    g_edit_active = false;
    arch_local_irq_restore(g_edit.irq);
}

int64_t arch_syscall_user_vm(uint64_t nr, const uint64_t args[6])
{
    if (!args)
        return -EINVAL;

    mm_t *mm = g_auth_mm;
    int rc = aarch64_vm_edit_begin(mm);
    if (rc)
        return rc;

    int64_t ret;
    switch (nr) {
    case SYS_brk:
        ret = user_vm_brk(mm, args[0]);
        break;
    case SYS_mmap:
        ret = user_vm_mmap(mm, args);
        break;
    case SYS_mprotect:
        ret = user_vm_mprotect(mm, args[0], args[1], args[2]);
        break;
    case SYS_munmap:
        ret = user_vm_munmap(mm, args[0], args[1]);
        break;
    default:
        ret = -EINVAL;
        break;
    }

    aarch64_vm_edit_end();
    return ret;
}

int aarch64_user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len,
                                 bool write)
{
    int rc = aarch64_vm_edit_begin(mm);
    if (rc)
        return rc;

    int prc = user_vm_prepare_copy(mm, addr, len, write);
    aarch64_vm_edit_end();
    return prc;
}

int aarch64_user_vm_fault(pt_regs_t *regs, const struct intr_event *event)
{
    if (!regs || !event || event->origin != INTR_ORIGIN_USER)
        return -EFAULT;

    bool write = false;
    bool cow = false;
    if (!aarch64_user_vm_fault_classify(event->native_status,
                                        event->fault_addr_valid,
                                        &write, &cow))
        return -EFAULT;

    mm_t *mm = aarch64_user_current_mm();
    if (!mm || !aarch64_user_frame_is_valid(regs))
        return -EFAULT;

    /* Save a native authorization that a retry will rebuild; a
     * lower-EL fault cannot occur inside the SVC dispatch, but the
     * state machine must stay correct if it ever does. */
    bool saved_native = g_native_auth;
    const pt_regs_t *saved_frame = g_auth_frame;
    mm_t *saved_mm = g_auth_mm;

    g_fault_auth = true;
    g_auth_frame = regs;
    g_auth_mm = mm;

    int rc = aarch64_vm_edit_begin(mm);
    if (rc == 0) {
        rc = cow ? user_vm_cow_fault(mm, event->fault_addr)
                 : user_vm_fault(mm, event->fault_addr, write);
        aarch64_vm_edit_end();
    }

    g_fault_auth = false;
    g_native_auth = saved_native;
    g_auth_frame = saved_frame;
    g_auth_mm = saved_mm;
    return rc;
}
