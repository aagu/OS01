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
#include <config/limits.h>
#include <errno.h>
#include <intr/dispatch.h>
#include <sched/task.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <syscall/dispatch.h>
#include <uapi/syscall.h>
#ifdef AARCH64_USER_RUNTIME_TEST
#include <selftest/aarch64_user_runtime.h>
#endif

/* ══════════════════════════════════════════════════════════════
 * Transaction layer (M4 Task 3, per-task & per-CPU state)
 * ══════════════════════════════════════════════════════════════ */

/* Per-CPU IRQ nesting depth.  A transaction requires depth zero. */
static unsigned g_irq_depth[NR_CPUS];

void aarch64_vm_irq_enter(void)
{
    uint32_t cpu = aarch64_user_vm_cpu_id();
    if (cpu < NR_CPUS)
        g_irq_depth[cpu]++;
}

void aarch64_vm_irq_leave(void)
{
    uint32_t cpu = aarch64_user_vm_cpu_id();
    if (cpu < NR_CPUS && g_irq_depth[cpu])
        g_irq_depth[cpu]--;
}

bool aarch64_vm_task_editing(const struct task_struct *task)
{
    const struct aarch64_user_task *owner = aarch64_user_task_owner(task);
    return owner ? owner->vm.edit_active : false;
}

void aarch64_vm_native_enter(pt_regs_t *regs, uint64_t nr)
{
    const struct task_struct *tsk = aarch64_user_current_task();
    struct aarch64_user_task *owner = aarch64_user_task_owner(tsk);
    if (!owner)
        return;

    owner->vm.native_auth = false;
    owner->vm.auth_task = NULL;
    owner->vm.auth_frame = NULL;
    owner->vm.auth_mm = NULL;

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

    owner->vm.native_auth = true;
    owner->vm.auth_task = tsk;
    owner->vm.auth_frame = regs;
    owner->vm.auth_mm = mm;
}

void aarch64_vm_native_leave(void)
{
    struct aarch64_user_task *owner =
        aarch64_user_task_owner(aarch64_user_current_task());
    if (!owner)
        return;

    owner->vm.native_auth = false;
    owner->vm.auth_task = NULL;
    owner->vm.auth_frame = NULL;
    owner->vm.auth_mm = NULL;
}

int aarch64_vm_edit_begin(mm_t *mm)
{
    const struct task_struct *tsk = aarch64_user_current_task();
    struct aarch64_user_task *owner = aarch64_user_task_owner(tsk);
    if (!owner)
        return -EPERM;

    if (!owner->vm.native_auth && !owner->vm.fault_auth)
        return -EPERM;
    if (owner->vm.edit_active)
        return -EBUSY;                    /* non-nestable */

    uint32_t cpu = aarch64_user_vm_cpu_id();
    if (cpu >= NR_CPUS || g_irq_depth[cpu] != 0)
        return -EBUSY;                    /* never from an IRQ path */

    if (!tsk || owner->vm.auth_task != tsk)
        return -EPERM;
    if (!mm || mm != owner->vm.auth_mm || mm != aarch64_user_current_mm())
        return -EPERM;
    if (!owner->vm.auth_frame || !aarch64_user_frame_is_valid(owner->vm.auth_frame))
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

    owner->vm.edit_root_pa = live;
    owner->vm.edit_irq = (uint64_t)arch_local_irq_save(); /* save + mask */
    aarch64_user_vm_save_tls(&owner->vm.edit_tpidr_el0,
                             &owner->vm.edit_tpidrro_el0);
    aarch64_user_restore_kernel_root();
    aarch64_user_clear_tls();
    owner->vm.edit_active = true;         /* mark before IRQs on */
    arch_local_irq_enable();              /* policy runs with IRQs */

    return 0;
}

void aarch64_vm_edit_end(void)
{
    struct aarch64_user_task *owner =
        aarch64_user_task_owner(aarch64_user_current_task());
    if (!owner || !owner->vm.edit_active)
        return;

    arch_local_irq_disable();
    aarch64_user_install_root(owner->vm.edit_root_pa); /* publish + install */
    aarch64_user_vm_restore_tls(owner->vm.edit_tpidr_el0,
                                owner->vm.edit_tpidrro_el0);
    owner->vm.edit_active = false;
    arch_local_irq_restore((arch_irq_state_t)owner->vm.edit_irq);
}

int64_t arch_syscall_user_vm(uint64_t nr, const uint64_t args[6])
{
    if (!args)
        return -EINVAL;

    struct aarch64_user_task *owner =
        aarch64_user_task_owner(aarch64_user_current_task());
    mm_t *mm = owner ? owner->vm.auth_mm : NULL;
    int rc = aarch64_vm_edit_begin(mm);
    if (rc)
        return rc;
#ifdef AARCH64_USER_RUNTIME_TEST
    aarch64_user_runtime_test_vm_edit_pause(mm);
#endif

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
#ifdef AARCH64_USER_RUNTIME_TEST
    aarch64_user_runtime_test_vm_edit_after(mm);
#endif
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

    const struct task_struct *tsk = aarch64_user_current_task();
    struct aarch64_user_task *owner = aarch64_user_task_owner(tsk);
    mm_t *mm = aarch64_user_current_mm();
    if (!owner || !mm || !aarch64_user_frame_is_valid(regs))
        return -EFAULT;

    /* Save a native authorization that a retry will rebuild; a
     * lower-EL fault cannot occur inside the SVC dispatch, but the
     * state machine must stay correct if it ever does. */
    bool saved_native = owner->vm.native_auth;
    const struct task_struct *saved_task = owner->vm.auth_task;
    const pt_regs_t *saved_frame = owner->vm.auth_frame;
    mm_t *saved_mm = owner->vm.auth_mm;

    owner->vm.fault_auth = true;
    owner->vm.auth_task = tsk;
    owner->vm.auth_frame = regs;
    owner->vm.auth_mm = mm;

    int rc = aarch64_vm_edit_begin(mm);
    if (rc == 0) {
        rc = cow ? user_vm_cow_fault(mm, event->fault_addr)
                 : user_vm_fault(mm, event->fault_addr, write);
        aarch64_vm_edit_end();
    }

    owner->vm.fault_auth = false;
    owner->vm.native_auth = saved_native;
    owner->vm.auth_task = saved_task;
    owner->vm.auth_frame = saved_frame;
    owner->vm.auth_mm = saved_mm;
    return rc;
}
