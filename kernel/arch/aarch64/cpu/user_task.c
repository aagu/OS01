/* kernel/arch/aarch64/cpu/user_task.c
 *
 * AArch64 M4 real user tasks: creation, first entry and the address
 * space switch.
 *
 * A user task is an ordinary PF_PROCESS scheduler task with three
 * things a kernel thread does not have:
 *
 *   - a private mm (the image's user root) that is installed into
 *     TTBR0_EL1 while the task runs,
 *   - a live EL0 exception frame at the top of its kernel stack,
 *   - an owner record that ties the image and the catalog reference to
 *     the task.
 *
 * The kernel half of the address space never moves: TTBR1_EL1 keeps
 * the M1 root and the direct map, so installing a user root in TTBR0
 * does not disturb the kernel code that runs on the same stack.
 */

#include <arch/context.h>          /* STACK_SIZE + the aarch64 thread_t */
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/regs.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/intr_dispatch.h>
#include <arch/aarch64/intr_decode.h>
#include <arch/aarch64/el0.h>
#include <arch/aarch64/syscall.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/cpu.h>
#include <arch/syscall.h>
#include <arch/irq.h>
#include <arch/spinlock.h>
#include <errno.h>
#include <intr/dispatch.h>
#include <kernel.h>               /* container_of */
#include <log/log.h>
#include <memory/slab.h>
#include <percpu/percpu.h>
#include <sched/internal.h>
#include <sched/task.h>
#include <string.h>
#include <arch/aarch64/user_vm_policy.h>

#if AARCH64_EL0_TEST
#include <arch/aarch64/reg.h>
#include <arch/aarch64/timer.h>
#include <selftest/aarch64_user_task.h>
#include <selftest/aarch64_user_vm.h>
#include <selftest/aarch64_user_fork_cow.h>

/* ── Test observation state ─────────────────────────────────────
 * The counters below are written by the production paths and read by
 * the acceptance case.  They live here so every producer and the
 * consumer agree on one definition. */
static struct aarch64_user_task_stats g_stats;
static bool g_arm_tick;

void aarch64_user_task_test_set_arm_tick(bool on) { g_arm_tick = on; }

void aarch64_user_task_test_stats(struct aarch64_user_task_stats *out)
{
    if (!out)
        return;
    *out = g_stats;
    out->el0_ticks = aarch64_el0_timer_ticks();
}

void aarch64_user_task_test_reset_stats(void)
{
    g_stats = (struct aarch64_user_task_stats){0};
}

void aarch64_user_task_test_note_switch(int prev_pid, int next_pid,
                                         uint64_t root, bool to_user)
{
    g_stats.switches++;
    if (to_user)
        g_stats.switches_to_user++;
    g_stats.last_switch_pid = next_pid;
    g_stats.last_switch_from = prev_pid;
    g_stats.last_switch_root = root;
}

uint64_t aarch64_user_task_test_switch_count(void)
{
    return g_stats.switches;
}

uint64_t aarch64_user_task_test_mark(void)
{
    return g_stats.switches;
}

int aarch64_user_task_test_pid(void)
{
    task_t *t = arch_current_task();
    return t ? (int)t->pid : -1;
}

void aarch64_user_task_test_note_el0_irq(bool pid_stable, bool switches_stable)
{
    g_stats.el0_irqs++;
    if (!pid_stable)
        g_stats.el0_irq_pid_violations++;
    if (!switches_stable)
        g_stats.el0_irq_switch_violations++;
}

void aarch64_user_task_test_note_image(bool ok)
{
    g_stats.image_loads++;
    if (!ok)
        g_stats.image_failures++;
}
#endif /* AARCH64_EL0_TEST */

/* The root the BSP booted with: the kernel root a kernel task or the
 * idle task gets back when a user task gives the CPU up. */
static uint64_t g_kernel_root_pa;
/* The root currently installed in TTBR0_EL1. */
static uint64_t g_live_root_pa;
/* Whether g_live_root_pa is a user root: only a user root is published
 * in the M3 registry and only a user root is revocable. */
static bool g_live_root_is_user;

void aarch64_user_clear_tls(void)
{
    /* The first increment has no TLS: every user entry starts from a
     * clean slate. */
    __asm__ volatile("msr tpidr_el0, xzr\n\tmsr tpidrro_el0, xzr" ::: "memory");
}

bool aarch64_user_task_is_user(const struct task_struct *tsk)
{
    return tsk && tsk->thread && tsk->thread->user_task != NULL;
}

uint64_t aarch64_user_task_frame(const struct task_struct *tsk){
    if (!tsk || !tsk->thread)
        return 0;
    /* The exception path pushes exactly one pt_regs frame below
     * SP_EL1, and SP_EL1 is stack_top whenever the task runs in EL0:
     * the live frame therefore always sits at stack_top - PT_REGS_SIZE. */
    return tsk->thread->stack_top - PT_REGS_SIZE;
}

__attribute__((no_stack_protector))
bool aarch64_user_frame_is_valid(const struct pt_regs *regs)
{
    if (!regs)
        return false;
    /* The BSP owns the single-core user-task runtime.  The per-CPU
     * pointer must be installed: an unset architecture pointer is not
     * a supported BSP check. */
    if (!this_cpu() || cpu_id() != 0)
        return false;
    /* The scheduler must be ready: a user task only exists after the
     * scheduler published its readiness. */
    if (!__atomic_load_n(&percpu_data[0].scheduler_ok, __ATOMIC_ACQUIRE))
        return false;
    /* The current task must be a real user task ... */
    task_t *t = arch_current_task();
    if (!aarch64_user_task_is_user(t))
        return false;
    /* ... a real process ... */
    if (!(t->flags & PF_PROCESS))
        return false;
    /* ... with a private mm that is the image's own mm ... */
    const struct aarch64_user_task *owner = t->thread->user_task;
    if (!owner || !owner->image)
        return false;
    struct mm_struct *mm = t->mm;
    if (!mm || mm != aarch64_user_image_mm(owner->image))
        return false;
    /* ... and the root actually installed must be that task's root.
     * The mm root, the image root and the installed root must all be
     * the same non-zero PA. */
    uint64_t installed;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(installed));
    installed &= AARCH64_TTBR_BASE_MASK;
    return aarch64_user_frame_matches(regs, aarch64_user_sync_frame(),
                                       (uint64_t)(uintptr_t)mm->pgdir,
                                       aarch64_user_image_root_pa(owner->image),
                                       installed);
}

__attribute__((no_stack_protector))
uint64_t aarch64_user_sync_frame(void)
{
    task_t *t = arch_current_task();
    if (!aarch64_user_task_is_user(t))
        return 0;
    /* The lower-EL entry pushes exactly one pt_regs frame below
     * SP_EL1, and SP_EL1 is stack_top while the task runs in EL0 (the
     * restore path raises it again before ERET).  The exception frame
     * therefore sits exactly at aarch64_user_task_frame(). */
    return aarch64_user_task_frame(t);
}

/* Early-chain helper of the EL1 copy fixup: no stack canary (the guard
 * reload is not available before main.c reseeds it). */
__attribute__((no_stack_protector))
struct mm_struct *aarch64_user_current_mm(void)
{
    task_t *t = arch_current_task();
    if (!aarch64_user_task_is_user(t))
        return NULL;
    return t->mm;
}

void aarch64_user_runtime_init(void)
{
    uint64_t ttbr0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    g_kernel_root_pa = ttbr0 & AARCH64_TTBR_BASE_MASK;
    g_live_root_pa = g_kernel_root_pa;
    g_live_root_is_user = false;

    /* FP/SIMD is not available to user code in this increment: clear
     * CPACR_EL1.FPEN so a user FP instruction traps to EL1 instead of
     * silently touching a kernel-owned FP context. */
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr &= ~(UINT64_C(3) << 20);
    __asm__ volatile("msr cpacr_el1, %0\n\tisb" :: "r"(cpacr) : "memory");
}

/* ── M4 user-VM hardware boundary ───────────────────────────────
 * The transaction layer in memory/user_vm.c calls these; the host
 * state-machine suite replaces them with tracked stubs. */

uint64_t aarch64_user_vm_read_ttbr0(void)
{
    uint64_t ttbr0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    return ttbr0 & AARCH64_TTBR_BASE_MASK;
}

bool aarch64_user_vm_root_published(uint64_t root_pa)
{
    if (!root_pa)
        return false;
    return aarch64_pt_root_is_published(&root_pa);
}

void aarch64_user_vm_save_tls(uint64_t *tpidr_el0, uint64_t *tpidrro_el0)
{
    if (!tpidr_el0 || !tpidrro_el0)
        return;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(*tpidr_el0));
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(*tpidrro_el0));
}

void aarch64_user_vm_restore_tls(uint64_t tpidr_el0, uint64_t tpidrro_el0)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tpidr_el0) : "memory");
    __asm__ volatile("msr tpidrro_el0, %0\n\tisb"
                     :: "r"(tpidrro_el0) : "memory");
}

void aarch64_user_install_root(uint64_t root_pa)
{
    if (!root_pa || root_pa == g_live_root_pa)
        return;

    /* Publish BEFORE the install: a root that is not published may not
     * be installed. */
    if (!aarch64_pt_root_publish(root_pa))
        vmm_gate_violation("aarch64_user_install_root: root publish failed");

    __asm__ volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb\n\t"
                     "tlbi vmalle1\n\tdsb ish\n\tisb"
                     :: "r"(root_pa) : "memory");

    /* The old user root is revoked only AFTER the new one is
     * installed: a normal single-core switch keeps at most two user
     * roots published at a time. */
    if (g_live_root_is_user)
        aarch64_pt_root_unpublish_local(g_live_root_pa);

    g_live_root_pa = root_pa;
    g_live_root_is_user = true;
    aarch64_user_clear_tls();
}

void aarch64_user_restore_kernel_root(void)
{
    if (g_live_root_pa == g_kernel_root_pa && !g_live_root_is_user)
        return;

    __asm__ volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb\n\t"
                     "tlbi vmalle1\n\tdsb ish\n\tisb"
                     :: "r"(g_kernel_root_pa) : "memory");

    if (g_live_root_is_user)
        aarch64_pt_root_unpublish_local(g_live_root_pa);

    g_live_root_pa = g_kernel_root_pa;
    g_live_root_is_user = false;
}

void aarch64_user_switch_mm(const struct task_struct *next)
{
    uint64_t next_root = g_kernel_root_pa;
    bool next_is_user = false;

    if (aarch64_user_task_is_user(next)) {
        const struct aarch64_user_task *owner = next->thread->user_task;
        next_root = aarch64_user_image_root_pa(owner->image);
        next_is_user = true;
    }

    /* Same root: nothing to install. */
    if (next_root == g_live_root_pa)
        return;

    if (next_is_user) {
        aarch64_user_install_root(next_root);
        return;
    }
    /* A kernel task or the idle task gets the kernel root back. */
    aarch64_user_restore_kernel_root();
}

/* ── First entry ────────────────────────────────────────────────
 * A user task starts in EL1 like every other task; the first entry
 * marks the running state and then hands the prepared frame to the
 * shared restore path, which ERETs into EL0. */
__attribute__((noreturn))
void aarch64_user_first_entry(void)
{
    arch_aarch64_entry_prepare();

    task_t *t = arch_current_task();
    uint64_t frame = aarch64_user_task_frame(t);

    aarch64_el0_restore(frame);
}

/* ── Low-EL synchronous routing ─────────────────────────────────
 *
 * Every lower-EL synchronous exception arrives here.  Two producers
 * exist for such a frame:
 *
 *   - the legacy M4 probe, which runs a private root on a kernel
 *     thread's stack and owns the frame while it is active;
 *   - a real EL0 user task, whose frame belongs to the scheduler.
 *
 * The formal branch builds the normalized event and hands it to the
 * COMMON dispatcher: the backend re-decodes it, checks the abort EC
 * against the saved mode and validates the formal frame before the
 * formal handler runs.  A frame that matches neither producer is an
 * unrecognised entry and stops the kernel.  It must never borrow the
 * probe's continuation: that continuation unwinds the probe's stack
 * frame and returns to a kernel caller that does not exist in a real
 * user task.
 */

bool aarch64_el0_sync_route(pt_regs_t *regs, uint64_t esr, uint64_t far)
{
#if AARCH64_EL0_TEST
    /* The legacy probe owns the frame while it is active.  ONLY this
     * branch may use the probe's continuation. */
    if (aarch64_el0_probe_active())
        return aarch64_el0_probe_sync(regs, esr, far);
#endif
    /* The formal branch goes through the common dispatcher. */
    struct intr_event ev;

    aarch64_intr_decode_sync(&ev, regs, esr, far);
    /* Report the real entry: the lower-EL sync slot. */
    ev.native_vector = AARCH64_EL0_SYNC_SLOT;
    arch_intr_dispatch(&ev, regs);
    return true;
}

/* ── The user-task SVC and fault handler ──────────────────────── */

static void aarch64_user_task_exec(pt_regs_t *regs);
__attribute__((noreturn)) static void aarch64_user_task_exit(uint64_t status);

/* The shared native exit handler hands the already-encoded status
 * (user code in the high byte) to the architecture.  The AArch64
 * implementation is the formal task teardown below. */
__attribute__((noreturn))
void arch_syscall_exit(uint64_t status)
{
    aarch64_user_task_exit(status);
}

void aarch64_user_task_sync(pt_regs_t *regs, const struct intr_event *event)
{
    uint64_t esr = event->native_status;
    uint64_t ec = (esr & AARCH64_ESR_EC_MASK) >> AARCH64_ESR_EC_SHIFT;
    uint64_t imm = esr & 0xffff;

#if AARCH64_EL0_TEST
    g_stats.svcs++;
    g_stats.last_svc_imm = imm;
#endif

    if (ec != A64_EC_SVC_LOWER) {
        /* Not an SVC: an eligible EL0 demand fault is filled and the
         * same PC is retried; every other fault keeps the old
         * termination path.  Policy exhaustion reports -ENOMEM. */
        int frc = aarch64_user_vm_fault(regs, event);
#if AARCH64_EL0_TEST
        aarch64_user_vm_test_fault(
            esr, event->fault_addr_valid ? event->fault_addr : 0,
            regs->elr_el1, frc);
        aarch64_user_fork_cow_test_fault(
            regs, esr, event->fault_addr_valid ? event->fault_addr : 0,
            frc);
#endif
        if (frc == 0)
            return;

        uint64_t status = ((uint64_t)frc == (uint64_t)-ENOMEM)
                              ? (uint64_t)-ENOMEM
                              : (uint64_t)-EFAULT;
        /* The aarch64 logger prints %lx with its own 0x prefix. */
        if (event->fault_addr_valid)
            log_err("[user-task] sync fault esr=%lx far=%lx\n",
                    (unsigned long)esr, (unsigned long)event->fault_addr);
        else
            log_err("[user-task] sync fault esr=%lx far=invalid\n",
                    (unsigned long)esr);
        aarch64_user_task_exit(status);
        return;
    }

    switch (imm) {
    case 0:
        /* Native syscall: `svc #0` with the number in the full x8.
         * The shared table owns the ABI; this entry only marshals the
         * frame and never moves the saved return PC. */
        aarch64_user_syscall(regs);
        return;
    case AARCH64_USER_SVC_YIELD:
        /* Cooperative yield: give the CPU up first, then report
         * success in x0.  Every other register keeps its value. */
        schedule();
#if AARCH64_EL0_TEST
        if (g_arm_tick) {
            /* Re-arm the real CNTP for one tick in the IRQ-masked
             * state, right before EL0 is restored: the tick then
             * counts into the vruntime while the task runs in EL0. */
            cntp_tval_el0_write(1);
            cntp_isb();
        }
#endif
        regs->x0 = 0;
        return;
    case AARCH64_USER_SVC_EXEC:
        aarch64_user_task_exec(regs);
        return;
    case AARCH64_USER_SVC_EXIT:
        aarch64_user_task_exit(regs->x0);
        return;
#if AARCH64_EL0_TEST
    case AARCH64_USER_SVC_STAGE: {
        /* The fork/COW acceptance fixture's monotonic stage query.
         * Only a registered fixture PID gets an answer; every other
         * task keeps the ordinary unknown-immediate result. */
        int64_t stage = aarch64_user_fork_cow_test_stage_query();
        regs->x0 = (stage >= 0) ? (uint64_t)stage : (uint64_t)-ENOSYS;
        return;
    }
#endif
    default:
        regs->x0 = (uint64_t)-ENOSYS;
        return;
    }
}

/* ── Replace the image of the running task ────────────────────── */

static void aarch64_user_task_exec(pt_regs_t *regs)
{
    task_t *t = arch_current_task();
    struct aarch64_user_task *owner = t->thread->user_task;
    uint64_t index = regs->x0;

    /* The whole index range is checked before anything is built. */
    if (!owner || index >= owner->catalog_count) {
        regs->x0 = (uint64_t)-EINVAL;
        return;
    }

    /* The image is constructed with interrupts enabled: the M3 write
     * entries require that.  The window contains no scheduling point,
     * so the task keeps running while the new image is built. */
    arch_local_irq_enable();
    struct aarch64_user_image *image = NULL;
    int rc = aarch64_user_image_load(owner->catalog[index].bytes,
                                      owner->catalog[index].size, &image);
    arch_local_irq_disable();

    if (rc) {
        /* Only the old frame's x0 changes: the old image stays
         * installed and the task continues where it was. */
        regs->x0 = (uint64_t)rc;
#if AARCH64_EL0_TEST
        aarch64_user_vm_test_exec(rc,
                                  aarch64_user_image_root_pa(owner->image));
#endif
#if AARCH64_EL0_TEST
        g_stats.execs++;
        g_stats.last_exec_index = index;
        g_stats.last_exec_rc = rc;
#endif
        return;
    }

    /* ── Commit region ──
     * Nothing below is recoverable: the new root is published and
     * installed, the old one is revoked, and the task metadata and the
     * exception frame follow.  An IRQ can never observe a state in
     * which the hardware root and the task metadata disagree. */
    struct aarch64_user_image *old_image = owner->image;
#if AARCH64_EL0_TEST
    uint64_t old_root_pa = aarch64_user_image_root_pa(old_image);
#endif

    aarch64_user_install_root(aarch64_user_image_root_pa(image));

    owner->image = image;
    owner->catalog_index = index;
    t->mm = aarch64_user_image_mm(image);

    /* The task continues at the new entry with a new stack and a fresh
     * register file. */
    aarch64_user_image_init_regs(image, regs);
    aarch64_user_clear_tls();

#if AARCH64_EL0_TEST
    g_stats.execs++;
    g_stats.last_exec_index = index;
    g_stats.last_exec_rc = 0;
#endif

    /* The old image is released outside the commit region, with
     * interrupts enabled. */
    arch_local_irq_enable();
    aarch64_user_image_destroy(old_image);
#if AARCH64_EL0_TEST
    aarch64_user_vm_test_exec(0, old_root_pa);
#endif
    arch_local_irq_disable();
}

int64_t arch_syscall_exec(void *arch_frame, const char *path,
                          const char *const *argv, const char *const *envp)
{
    pt_regs_t *regs = (pt_regs_t *)arch_frame;
    if (!aarch64_user_frame_is_valid(regs))
        return -EFAULT;

    task_t *t = arch_current_task();
    if (!aarch64_user_task_is_user(t))
        return -EINVAL;
    struct aarch64_user_task *owner = t->thread->user_task;
    if (!owner || !owner->catalog)
        return -EINVAL;

    if (!path)
        return -EINVAL;
    if (path[0] == '\0')
        return -ENOENT;

    size_t index = owner->catalog_count;
    for (size_t i = 0; i < owner->catalog_count; i++) {
        if (owner->catalog[i].path && strcmp(owner->catalog[i].path, path) == 0) {
            index = i;
            break;
        }
    }
    if (index >= owner->catalog_count)
        return -ENOENT;

    int rc = aarch64_user_image_check_stack_args(argv, envp);
    if (rc < 0)
        return rc;

    arch_local_irq_enable();
    struct aarch64_user_image *image = NULL;
    rc = aarch64_user_image_load(owner->catalog[index].bytes,
                                 owner->catalog[index].size, &image);
    if (rc == 0) {
        rc = aarch64_user_image_setup_stack(image, argv, envp);
        if (rc < 0) {
            aarch64_user_image_destroy(image);
            image = NULL;
        }
    }
    arch_local_irq_disable();

    if (rc < 0) {
#if AARCH64_EL0_TEST
        aarch64_user_vm_test_exec(rc,
                                  aarch64_user_image_root_pa(owner->image));
        g_stats.execs++;
        g_stats.last_exec_index = index;
        g_stats.last_exec_rc = rc;
#endif
        return rc;
    }

    struct aarch64_user_image *old_image = owner->image;
#if AARCH64_EL0_TEST
    uint64_t old_root_pa = aarch64_user_image_root_pa(old_image);
#endif

    aarch64_user_install_root(aarch64_user_image_root_pa(image));

    owner->image = image;
    owner->catalog_index = index;
    t->mm = aarch64_user_image_mm(image);

    task_comm_apply_exec(t, path);
    aarch64_user_image_init_regs(image, regs);
    aarch64_user_clear_tls();

#if AARCH64_EL0_TEST
    g_stats.execs++;
    g_stats.last_exec_index = index;
    g_stats.last_exec_rc = 0;
#endif

    arch_local_irq_enable();
    aarch64_user_image_destroy(old_image);
#if AARCH64_EL0_TEST
    aarch64_user_vm_test_exec(0, old_root_pa);
#endif
    arch_local_irq_disable();
    return 0;
}

/* ── Terminate the running task ───────────────────────────────── */

__attribute__((noreturn))
static void aarch64_user_task_exit(uint64_t status)
{
    task_t *t = arch_current_task();
    struct aarch64_user_task *owner = t->thread->user_task;
    struct aarch64_user_image *image = owner ? owner->image : NULL;

    /* Switch back to the kernel root and revoke the user root BEFORE
     * anything is released: a page that is still mapped must never be
     * freed. */
    arch_local_irq_disable();
    aarch64_user_restore_kernel_root();

    if (owner) {
        owner->image = NULL;
        t->mm = NULL;
        t->thread->user_task = NULL;
    }

    /* The image and the private state are released with interrupts
     * enabled. */
    arch_local_irq_enable();
    aarch64_user_image_destroy(image);
    if (owner)
        kfree(owner);
    arch_local_irq_disable();

#if AARCH64_EL0_TEST
    g_stats.exits++;
    g_stats.last_exit_pid = (int)t->pid;
    g_stats.last_exit_status = status;
    if (g_stats.exits_recorded < 4) {
        g_stats.exit_pid[g_stats.exits_recorded] = (int)t->pid;
        g_stats.exit_status[g_stats.exits_recorded] = status;
        g_stats.exits_recorded++;
    }
#endif

    /* Atomically under task_list_lock:
     *   1. Re-parent surviving children to idle and collect unwaited
     *      orphan ZOMBIEs (on_cpu == 0) onto reap_head for lock-free
     *      reclamation;
     *   2. Decide whether t has a live waitable PF_PROCESS parent and
     *      should remain on task_list as TASK_ZOMBIE or self-reap. */
    task_t *idle = percpu_data[0].idle;
    list_t reap_head;
    list_init(&reap_head);
    bool waitable_parent = false;

    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    if (idle) {
        list_t *p = init_task_union.task.list.next;
        while (p != &init_task_union.task.list) {
            task_t *child = container_of(p, task_t, list);
            p = p->next;
            if (child->parent != t)
                continue;
            child->parent = idle;
            if (child->state == TASK_ZOMBIE) {
                list_del(&child->list);
                if (__atomic_load_n(&child->on_cpu, __ATOMIC_ACQUIRE) == 0) {
                    list_add_to_before(&reap_head, &child->list);
                } else {
                    child->list.next = NULL;
                    child->list.prev = NULL;
                    child->flags |= PF_SELF_REAP;
                }
            }
        }
    }

    task_t *parent = t->parent;
    if (parent && parent != idle &&
        (parent->flags & PF_PROCESS) &&
        parent->state != TASK_ZOMBIE) {
        waitable_parent = true;
        t->exit_code = status;
        __atomic_store_n(&t->on_cpu, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&task_list_lock, tl_flags);

    while (!list_is_empty(&reap_head)) {
        list_t *node = reap_head.next;
        list_del(node);
        node->next = NULL;
        node->prev = NULL;
        task_t *orphan = container_of(node, task_t, list);
        sched_free_reaped_task(orphan);
    }

    if (!waitable_parent)
        task_exit_self_reap(status);

    task_wake_parent(t);
    task_exit_zombie();
    schedule();
    for (;;)
        arch_cpu_halt();
}

/* ── Creation ─────────────────────────────────────────────────── */

struct user_task_prepare_ctx {
    struct aarch64_user_task *owner;
    task_t          *parent;   /* NULL: resident BSP idle (spawn) */
    const pt_regs_t *frame;    /* NULL: fresh image frame (spawn) */
};

static int user_task_prepare(task_t *tsk, void *arg)
{
    struct user_task_prepare_ctx *ctx = arg;
    struct aarch64_user_task *owner = ctx->owner;

    /* The task owns a private mm: the image's user root.  The kernel
     * half of the address space stays in TTBR1. */
    tsk->mm = aarch64_user_image_mm(owner->image);
    tsk->files = NULL;
    tsk->fpu_save = NULL;

    /* Spawn is parented to the resident BSP idle task (the creator may
     * exit on its own); fork is parented to the real parent so
     * getppid() and exit re-parenting observe the true tree. */
    tsk->parent = ctx->parent ? ctx->parent : percpu_data[0].idle;

    /* Kernel-stack geometry, mirroring arch_kthread_prepare(). */
    uintptr_t base = (uintptr_t)tsk;
    uintptr_t top = base + STACK_SIZE;
    uintptr_t low = (base + sizeof(*tsk) + 256 + 15) & ~(uintptr_t)15;

    thread_t *ctx_thread = tsk->thread;
    ctx_thread->stack_base = base;
    ctx_thread->stack_top = top;
    ctx_thread->usable_low = low;
    ctx_thread->sched_flags = 0;
    ctx_thread->x19 = 0;
    ctx_thread->x20 = 0;
    ctx_thread->x21 = (uint64_t)(uintptr_t)ctx_thread;
    ctx_thread->x30 = (uint64_t)(uintptr_t)aarch64_user_first_entry;

    /* The first EL0 frame lives at the top of the kernel stack.  A
     * fork copies the parent's SVC frame so both resume at the same
     * instruction, with the child's x0 set to 0. */
    uint64_t frame = top - PT_REGS_SIZE;
    if (ctx->frame) {
        pt_regs_t *child_frame = (pt_regs_t *)frame;
        *child_frame = *ctx->frame;
        child_frame->x0 = 0;
    } else {
        aarch64_user_image_init_regs(owner->image, (pt_regs_t *)frame);
    }
    ctx_thread->sp = frame;

    ctx_thread->user_task = owner;
    return 0;
}

static void user_task_discard(task_t *tsk, void *arg)
{
    (void)arg;
    /* The image and the owner record belong to the creator, which
     * releases them once the publication failed.  Only the task-side
     * linkage is undone here. */
    tsk->mm = NULL;
    if (tsk->thread)
        tsk->thread->user_task = NULL;
}

/* ── fork: clone the address space, then publish the child ──────
 *
 * The transaction order is fixed: the clone (user_vm_clone_fork)
 * borrows every parent page, commits the checked COW references and
 * lowers the parent's writable leaves, and only then publishes the
 * child task.  Publication is the last fallible step.  Any failure
 * rolls the clone back and releases the empty child image/owner while
 * the parent root is detached (IRQs enabled); the parent root is
 * restored exactly once by aarch64_vm_edit_end(). */

struct user_task_fork_ctx {
    struct aarch64_user_task *owner;
    task_t          *parent;
    const pt_regs_t *frame;
    int              pid;
};

static int user_task_fork_publish(void *arg)
{
    struct user_task_fork_ctx *ctx = arg;
    struct user_task_prepare_ctx pctx = {
        .owner  = ctx->owner,
        .parent = ctx->parent,
        .frame  = ctx->frame,
    };

    task_t *tsk = NULL;
    int pid = 0;
    int rc = sched_alloc_and_publish_task(0, true /* pinned to BSP */,
                                          PF_PROCESS, ctx->parent->comm,
                                          user_task_prepare, user_task_discard,
                                          &pctx, &tsk, &pid);
    if (rc)
        return rc;
    ctx->pid = pid;
    return 0;
}

int64_t arch_syscall_fork(void *arch_frame)
{
    pt_regs_t *regs = (pt_regs_t *)arch_frame;
    if (!regs || !aarch64_user_frame_is_valid(regs))
        return -EFAULT;

    task_t *parent_task = arch_current_task();
    if (!aarch64_user_task_is_user(parent_task))
        return -EINVAL;
    struct aarch64_user_task *parent_owner = parent_task->thread->user_task;
    struct mm_struct *parent_mm = parent_task->mm;
    if (!parent_owner || !parent_owner->image || !parent_mm)
        return -EINVAL;

    struct aarch64_user_image *child_image = NULL;
    int rc = aarch64_user_image_fork_shell(parent_owner->image, &child_image);
    if (rc)
        return rc;

    struct aarch64_user_task *child_owner = kmalloc(sizeof(*child_owner));
    if (!child_owner) {
        aarch64_user_image_destroy(child_image);
        return -ENOMEM;
    }
    child_owner->image         = child_image;
    child_owner->catalog       = parent_owner->catalog;
    child_owner->catalog_count = parent_owner->catalog_count;
    child_owner->catalog_index = parent_owner->catalog_index;

    struct user_task_fork_ctx fctx = {
        .owner  = child_owner,
        .parent = parent_task,
        .frame  = regs,
        .pid    = 0,
    };

    rc = aarch64_vm_edit_begin(parent_mm);
    if (rc) {
        aarch64_user_image_destroy(child_image);
        kfree(child_owner);
        return rc;
    }

    rc = user_vm_clone_fork(parent_mm, aarch64_user_image_mm(child_image),
                            user_task_fork_publish, &fctx);
    if (rc) {
        /* The clone already removed its child leaves/records/VMAs;
         * this reclaims the empty child root's intermediate tables
         * while the transaction still runs with IRQs enabled. */
        aarch64_user_image_destroy(child_image);
        child_image = NULL;
    }

    /* Exactly one end for every successful begin.  It reinstalls the
     * parent root and restores the entry IRQ state. */
    aarch64_vm_edit_end();

    if (rc) {
        kfree(child_owner);
        return rc;
    }
    return fctx.pid;
}

int aarch64_user_task_spawn(const struct aarch64_user_elf_source *catalog,
                            size_t count, size_t index, const char *name)
{
    /* The whole index range is checked before anything is built. */
    if (!catalog || !count || index >= count)
        return -EINVAL;
    if (!catalog[index].bytes || !catalog[index].size)
        return -EINVAL;

    /* Single-core runtime gate: the user-mm primitives refuse to run
     * on a multi-core or non-BSP configuration. */
    int rc = aarch64_user_mm_runtime_check();
    if (rc)
        return rc;

    /* The scheduler must be ready: a user task is published into a
     * live runqueue. */
    if (!__atomic_load_n(&percpu_data[0].scheduler_ok, __ATOMIC_ACQUIRE))
        return -EINVAL;

    struct aarch64_user_image *image = NULL;
    rc = aarch64_user_image_load(catalog[index].bytes, catalog[index].size,
                                  &image);
    if (rc)
        return rc;

    struct aarch64_user_task *owner = kmalloc(sizeof(*owner));
    if (!owner) {
        aarch64_user_image_destroy(image);
        return -ENOMEM;
    }
    owner->image = image;
    owner->catalog = catalog;
    owner->catalog_count = count;
    owner->catalog_index = index;

    struct user_task_prepare_ctx pctx = {
        .owner  = owner,
        .parent = NULL,
        .frame  = NULL,
    };

    task_t *tsk = NULL;
    int pid = 0;
    rc = sched_alloc_and_publish_task(0, true /* pinned to the BSP */,
                                      PF_PROCESS, name,
                                      user_task_prepare, user_task_discard,
                                      &pctx, &tsk, &pid);
    if (rc) {
        /* The creator still owns the image and the owner record. */
        kfree(owner);
        aarch64_user_image_destroy(image);
        return rc;
    }
    return pid;
}
