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
#include <arch/aarch64/mmu.h>
#include <arch/aarch64/regs.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/intr_dispatch.h>
#include <arch/aarch64/intr_decode.h>
#include <arch/aarch64/el0.h>
#include <arch/aarch64/syscall.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/fp.h>
#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_mm.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/exec.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/cpu.h>
#include <arch/syscall.h>
#include <arch/irq.h>
#include <arch/spinlock.h>
#include <errno.h>
#include <fs/file.h>               /* files_dup / files_unpin (fd inheritance) */
#include <fs/vfs.h>                /* vfs_lookup / vfs_node_put (product exec) */
#include <intr/dispatch.h>
#include <kernel.h>               /* container_of */
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <memory/uaccess.h>
#include <percpu/percpu.h>
#include <sched/internal.h>
#include <sched/task.h>
#include <string.h>
#include <uapi/syscall.h>
#include <arch/aarch64/user_vm_policy.h>
#ifdef AARCH64_USER_RUNTIME_TEST
#include <selftest/aarch64_user_runtime.h>
#endif

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

/* Shared immutable zero-entry runtime ASID-0 TTBR0 root initialized by
 * BSP aarch64_user_runtime_init() and published before APs enable. */
static uint64_t g_kernel_root_pa;
/* Per-CPU TTBR0_EL1 value (PA | (ASID << 48)) and split fields
 * currently live in TTBR0_EL1. */
static uint64_t g_live_ttbr0[NR_CPUS];
static uint64_t g_live_root_pa[NR_CPUS];
static uint8_t  g_live_asid[NR_CPUS];
/* Whether g_live_root_pa[cpu] is a user root: only a user root is
 * published in the M3 registry and only a user root is revocable. */
static bool g_live_root_is_user[NR_CPUS];

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

const struct task_struct *aarch64_user_current_task(void)
{
    return arch_current_task();
}

struct aarch64_user_task *aarch64_user_task_owner(const struct task_struct *tsk)
{
    if (!aarch64_user_task_is_user(tsk))
        return NULL;
    return tsk->thread->user_task;
}

uint32_t aarch64_user_vm_cpu_id(void)
{
    percpu_t *cpu = this_cpu();
    if (!cpu || cpu->cpu_id >= NR_CPUS)
        return NR_CPUS;
    return cpu->cpu_id;
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
    /* Current CPU must have its percpu pointer installed and be fully
     * admitted (online, ipi_ready, scheduler_ok). */
    percpu_t *cpu = this_cpu();
    if (!cpu || cpu->cpu_id >= NR_CPUS)
        return false;
    if (!__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&cpu->ipi_ready, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&cpu->scheduler_ok, __ATOMIC_ACQUIRE))
        return false;
    /* The current task must be a real user task ... */
    task_t *t = arch_current_task();
    if (!aarch64_user_task_is_user(t))
        return false;
    /* ... a real process with single-bit affinity on this CPU ... */
    if (!(t->flags & PF_PROCESS))
        return false;
    if (t->cpu != cpu->cpu_id || t->allowed_cpus != (1ULL << cpu->cpu_id))
        return false;
    /* ... with a private mm that is the image's own mm ... */
    const struct aarch64_user_task *owner = t->thread->user_task;
    if (!owner || !owner->image)
        return false;
    struct mm_struct *mm = t->mm;
    if (!mm || mm != aarch64_user_image_mm(owner->image))
        return false;
    /* ... and the TTBR0_EL1 value (PA + ASID) actually installed must
     * match that task's mm. */
    uint64_t expected_ttbr0 = aarch64_user_ttbr0_value(mm);
    if (!expected_ttbr0)
        return false;
    uint64_t installed;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(installed));
    if (installed != expected_ttbr0)
        return false;
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

void aarch64_user_runtime_init_cpu(void)
{
    uint64_t empty_pa = __atomic_load_n(&g_kernel_root_pa, __ATOMIC_ACQUIRE);
    if (!empty_pa) {
        log_err("[user-task] FATAL: shared empty TTBR0 root not published\n");
        for (;;)
            arch_cpu_halt();
    }

    uint32_t cpu = aarch64_user_vm_cpu_id();
    if (cpu >= NR_CPUS) {
        log_err("[user-task] FATAL: invalid cpu id in runtime init\n");
        for (;;)
            arch_cpu_halt();
    }

    g_live_ttbr0[cpu] = empty_pa;
    g_live_root_pa[cpu] = empty_pa;
    g_live_asid[cpu] = 0;
    g_live_root_is_user[cpu] = false;

    /* Install the shared immutable empty ASID-0 TTBR0 root with a local
     * full TLB invalidation on first install; all subsequent kernel and
     * device accesses must route via TTBR1. */
    __asm__ volatile(
        "dsb ishst\n\t"
        "msr ttbr0_el1, %0\n\t"
        "isb\n\t"
        "tlbi vmalle1\n\t"
        "dsb ish\n\t"
        "isb"
        :: "r"(empty_pa) : "memory");

    /* Enable FP/SIMD for EL0 and EL1 (CPACR_EL1.FPEN = 0b11).  The
     * kernel is compiled with -mgeneral-regs-only so it never touches
     * FP/SIMD registers; user task context switches save/restore them
     * explicitly via task->fpu_save. */
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (UINT64_C(3) << 20);
    __asm__ volatile("msr cpacr_el1, %0\n\tisb" :: "r"(cpacr) : "memory");
}

void aarch64_user_runtime_init(void)
{
    uint64_t empty_pa = alloc_4k_page();
    if (!empty_pa) {
        log_err("[user-task] FATAL: failed to allocate empty TTBR0 root\n");
        for (;;)
            arch_cpu_halt();
    }
    __builtin_memset((void *)Phy_To_Virt(empty_pa), 0, PAGE_4K_SIZE);

    __atomic_store_n(&g_kernel_root_pa, empty_pa, __ATOMIC_RELEASE);
    aarch64_user_runtime_init_cpu();
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

void aarch64_user_install_mm(const struct mm_struct *mm)
{
    if (!mm || !mm->pgdir)
        return;

    uint32_t cpu = aarch64_user_vm_cpu_id();
    if (cpu >= NR_CPUS)
        vmm_gate_violation("aarch64_user_install_mm: invalid cpu");

    uint64_t target_ttbr0 = aarch64_user_ttbr0_value(mm);
    if (!target_ttbr0)
        vmm_gate_violation("aarch64_user_install_mm: unregistered mm");

    uint64_t target_pa = target_ttbr0 & AARCH64_TTBR_BASE_MASK;
    uint8_t target_asid = (uint8_t)(target_ttbr0 >> 48);

    if (g_live_root_is_user[cpu] && g_live_ttbr0[cpu] == target_ttbr0)
        return;

    /* Publish BEFORE the install: a root that is not published may not
     * be installed. */
    if (!aarch64_pt_root_publish(target_pa))
        vmm_gate_violation("aarch64_user_install_mm: root publish failed");

    bool need_full_flush =
        (target_asid == 0) || (g_live_root_is_user[cpu] && g_live_asid[cpu] == 0);

    if (need_full_flush) {
        __asm__ volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb\n\t"
                         "tlbi vmalle1\n\tdsb ish\n\tisb"
                         :: "r"(target_ttbr0) : "memory");
    } else {
        __asm__ volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb"
                         :: "r"(target_ttbr0) : "memory");
    }

#ifdef AARCH64_USER_RUNTIME_TEST
    extern void aarch64_user_runtime_test_note_ttbr_switch(
        uint64_t old_ttbr0, uint64_t new_ttbr0, bool full_flush);
    aarch64_user_runtime_test_note_ttbr_switch(g_live_ttbr0[cpu], target_ttbr0,
                                               need_full_flush);
#endif

    /* The old user root is revoked only AFTER the new one is
     * installed: each CPU keeps at most two user roots published at a
     * time across a switch. */
    if (g_live_root_is_user[cpu] && g_live_root_pa[cpu] != target_pa)
        aarch64_pt_root_unpublish_local(g_live_root_pa[cpu]);

    g_live_ttbr0[cpu] = target_ttbr0;
    g_live_root_pa[cpu] = target_pa;
    g_live_asid[cpu] = target_asid;
    g_live_root_is_user[cpu] = true;
}

void aarch64_user_restore_kernel_root(void)
{
    uint32_t cpu = aarch64_user_vm_cpu_id();
    if (cpu >= NR_CPUS)
        return;

    if (!g_live_root_is_user[cpu] && g_live_ttbr0[cpu] == g_kernel_root_pa)
        return;

    bool need_full_flush = (g_live_root_is_user[cpu] && g_live_asid[cpu] == 0);

    if (need_full_flush) {
        __asm__ volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb\n\t"
                         "tlbi vmalle1\n\tdsb ish\n\tisb"
                         :: "r"(g_kernel_root_pa) : "memory");
    } else {
        __asm__ volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb"
                         :: "r"(g_kernel_root_pa) : "memory");
    }

#ifdef AARCH64_USER_RUNTIME_TEST
    extern void aarch64_user_runtime_test_note_ttbr_switch(
        uint64_t old_ttbr0, uint64_t new_ttbr0, bool full_flush);
    aarch64_user_runtime_test_note_ttbr_switch(g_live_ttbr0[cpu], g_kernel_root_pa,
                                               need_full_flush);
#endif

    if (g_live_root_is_user[cpu])
        aarch64_pt_root_unpublish_local(g_live_root_pa[cpu]);

    g_live_ttbr0[cpu] = g_kernel_root_pa;
    g_live_root_pa[cpu] = g_kernel_root_pa;
    g_live_asid[cpu] = 0;
    g_live_root_is_user[cpu] = false;
}

void aarch64_user_switch_mm(struct task_struct *prev,
                            const struct task_struct *next)
{
#ifdef AARCH64_USER_RUNTIME_TEST
    extern void aarch64_user_runtime_test_note_switch(
        const struct task_struct *prev, const struct task_struct *next);
    aarch64_user_runtime_test_note_switch(prev, next);
#endif
    uint32_t cpu = aarch64_user_vm_cpu_id();

    /* 1. Save prev's EL0 TLS unless prev is in a paused edit (whose
     *    entry TLS is already preserved in its edit snapshot). */
    if (aarch64_user_task_is_user(prev) && !aarch64_vm_task_editing(prev)) {
        struct aarch64_user_task *prev_owner = prev->thread->user_task;
        aarch64_user_vm_save_tls(&prev_owner->tpidr_el0,
                                 &prev_owner->tpidrro_el0);
    }

    /* 2. Switch TTBR0 root.  Paused-edit tasks run on the kernel root
     *    until their original edit_end restores their user root. */
    uint64_t next_ttbr0 = g_kernel_root_pa;
    bool next_user_active = aarch64_user_task_is_user(next) &&
                            !aarch64_vm_task_editing(next);
    if (next_user_active)
        next_ttbr0 = aarch64_user_ttbr0_value(next->mm);

    if (cpu < NR_CPUS && next_ttbr0 != g_live_ttbr0[cpu]) {
        if (next_user_active)
            aarch64_user_install_mm(next->mm);
        else
            aarch64_user_restore_kernel_root();
    }

    /* 3. Restore next's EL0 TLS (or zero for kernel / paused-edit
     *    tasks while the original edit TLS remains in the snapshot). */
    if (next_user_active) {
        const struct aarch64_user_task *owner = next->thread->user_task;
        aarch64_user_vm_restore_tls(owner->tpidr_el0, owner->tpidrro_el0);
    } else {
        aarch64_user_clear_tls();
    }
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

    aarch64_do_signal_delivery((pt_regs_t *)frame);
    aarch64_el0_restore(frame);
}

void aarch64_user_irq_return(pt_regs_t *regs)
{
    if (!aarch64_user_frame_is_valid(regs))
        return;

    task_t *cur = arch_current_task();
    if (aarch64_vm_task_editing(cur))
        return;

    percpu_t *cpu = this_cpu();
    if (!cpu || !__atomic_load_n(&cpu->scheduler_ok, __ATOMIC_ACQUIRE))
        return;

    if (__atomic_load_n(&cpu->need_resched, __ATOMIC_ACQUIRE)) {
#ifdef AARCH64_USER_RUNTIME_TEST
        uint64_t switches_before = 0;
        uint64_t elr_before = 0;
        aarch64_user_runtime_test_irq_sched_before(
            cur, regs, &switches_before, &elr_before);
#endif
        schedule();
#ifdef AARCH64_USER_RUNTIME_TEST
        aarch64_user_runtime_test_irq_sched_after(
            cur, regs, switches_before, elr_before);
#endif
    }

    if (arch_current_task() != cur)
        return;
    if (!aarch64_user_frame_is_valid(regs))
        return;

    aarch64_do_signal_delivery(regs);
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
#ifdef AARCH64_USER_RUNTIME_TEST
    aarch64_user_runtime_test_before_exit(arch_current_task());
#endif
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
        regs->x0 = 0;
        aarch64_do_signal_delivery(regs);
#if AARCH64_EL0_TEST
        if (g_arm_tick) {
            /* Re-arm the real CNTP for one tick in the IRQ-masked
             * state, right before EL0 is restored: the tick then
             * counts into the vruntime while the task runs in EL0. */
            cntp_tval_el0_write(1);
            cntp_isb();
        }
#endif
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
                                      owner->catalog[index].size,
                                      AARCH64_USER_STACK_PAGES_SELFTEST,
                                      &image);
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

    aarch64_user_install_mm(aarch64_user_image_mm(image));

    owner->image = image;
    owner->catalog_index = index;
    owner->tpidr_el0 = 0;
    owner->tpidrro_el0 = 0;
    memset(&owner->vm, 0, sizeof(owner->vm));
    t->mm = aarch64_user_image_mm(image);
    memset(t->sighand, 0, sizeof(t->sighand));

    /* The task continues at the new entry with a new stack and a fresh
     * register file. */
    aarch64_user_image_init_regs(image, regs);
    aarch64_user_clear_tls();
    if (t->fpu_save) {
        uintptr_t aligned = ((uintptr_t)t->fpu_save + 15) & ~(uintptr_t)15;
        memset((void *)aligned, 0, AARCH64_FP_STATE_SIZE);
    }
    aarch64_fp_zero();

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
    if (!owner)
        return -EINVAL;

    if (!path)
        return -EINVAL;
    if (path[0] == '\0')
        return -ENOENT;

    /* The image bytes source differs per build: the selftest variants
     * exec against the kernel-provided in-memory catalog; the product
     * build reads the ELF through the VFS.  Both feed the same staged
     * construction below. */
    const void *bytes = NULL;
    size_t size = 0;
    void *owned_bytes = NULL;
    unsigned stack_pages = AARCH64_USER_STACK_PAGES_SELFTEST;
    size_t index = 0;
    int rc;

#if AARCH64_EL0_TEST
    if (!owner->catalog)
        return -EINVAL;
    index = owner->catalog_count;
    for (size_t i = 0; i < owner->catalog_count; i++) {
        if (owner->catalog[i].path && strcmp(owner->catalog[i].path, path) == 0) {
            index = i;
            break;
        }
    }
    if (index >= owner->catalog_count)
        return -ENOENT;
    bytes = owner->catalog[index].bytes;
    size = owner->catalog[index].size;
#else
    stack_pages = AARCH64_PRODUCT_STACK_PAGES;
    vfs_node_t *node = vfs_lookup(path);
    if (!node)
        return -ENOENT;
    if (node->type != VFS_FILE) {
        vfs_node_put(node);
        return -ENOEXEC;
    }
    rc = aarch64_exec_read_elf(node, &owned_bytes, &size);
    vfs_node_put(node);
    if (rc)
        return rc;
    bytes = owned_bytes;
#endif

    rc = aarch64_user_image_check_stack_args(argv, envp, stack_pages);
    if (rc < 0) {
        if (owned_bytes)
            kfree(owned_bytes);
        return rc;
    }

    /* The image is constructed with interrupts enabled: the M3 write
     * entries require that.  The window contains no scheduling point,
     * so the task keeps running while the new image is built. */
    arch_local_irq_enable();
    struct aarch64_user_image *image = NULL;
    rc = aarch64_user_image_load(bytes, size, stack_pages, &image);
    if (rc == 0) {
        rc = aarch64_user_image_setup_stack(image, argv, envp, stack_pages);
        if (rc < 0) {
            aarch64_user_image_destroy(image);
            image = NULL;
        }
    }
    arch_local_irq_disable();

    if (owned_bytes)
        kfree(owned_bytes);
    owned_bytes = NULL;

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

    aarch64_user_install_mm(aarch64_user_image_mm(image));

    owner->image = image;
    owner->catalog_index = index;
    owner->tpidr_el0 = 0;
    owner->tpidrro_el0 = 0;
    memset(&owner->vm, 0, sizeof(owner->vm));
    t->mm = aarch64_user_image_mm(image);
    memset(t->sighand, 0, sizeof(t->sighand));

    task_comm_apply_exec(t, path);
    aarch64_user_image_init_regs(image, regs);
    aarch64_user_clear_tls();
    if (t->fpu_save) {
        uintptr_t aligned = ((uintptr_t)t->fpu_save + 15) & ~(uintptr_t)15;
        memset((void *)aligned, 0, AARCH64_FP_STATE_SIZE);
    }
    aarch64_fp_zero();

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
        memset(&owner->vm, 0, sizeof(owner->vm));
        owner->tpidr_el0 = 0;
        owner->tpidrro_el0 = 0;
        owner->image = NULL;
        t->mm = NULL;
        t->thread->user_task = NULL;
    }
    aarch64_user_clear_tls();

    /* Detach our fd-table under task_list_lock, then drop the reference
     * outside the lock (files_unpin drop-to-zero may free/close files). */
    files_t *fs = NULL;
    {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        fs = t->files;
        t->files = NULL;
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }

    /* The files table, image, and private state are released with
     * interrupts enabled. */
    arch_local_irq_enable();
    if (fs)
        files_unpin(fs);
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

    /* Atomically publish ZOMBIE, reparent surviving children to idle,
     * and determine reap destination before switching away. */
    task_t *idle = percpu_data[0].idle;
    list_t reap_head;
    list_init(&reap_head);

    task_publish_process_exit(t, idle, status, &reap_head);

    while (!list_is_empty(&reap_head)) {
        list_t *node = reap_head.next;
        list_del(node);
        node->next = NULL;
        node->prev = NULL;
        task_t *orphan = container_of(node, task_t, list);
        sched_free_reaped_task(orphan);
    }

    schedule();
    for (;;)
        arch_cpu_halt();
}

/* ── Signal delivery and sigreturn ────────────────────────────── */

static bool aarch64_sig_default_ignore(int sig)
{
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG ||
           sig == SIGWINCH || sig == SIGTSTP || sig == SIGTTIN ||
           sig == SIGTTOU;
}

void aarch64_do_signal_delivery(pt_regs_t *regs)
{
    if (!regs || (regs->spsr_el1 & AARCH64_SPSR_MODE_MASK) != AARCH64_SPSR_MODE_EL0T)
        return;

    task_t *cur = arch_current_task();
    if (!aarch64_user_task_is_user(cur))
        return;

    for (;;) {
        uint64_t sig_bits = (uint64_t)__atomic_load_n(&cur->signal, __ATOMIC_ACQUIRE);
        uint64_t unblocked_mask = ~(((uint64_t)cur->blocked) << 1) |
                                  (1ULL << SIGKILL) | (1ULL << SIGSTOP);
        uint64_t deliverable = sig_bits & unblocked_mask & ((1ULL << NSIG) - 2ULL);
        if (!deliverable)
            return;

        int sig = (deliverable & (1ULL << SIGKILL))
                  ? SIGKILL
                  : __builtin_ctzll(deliverable);
        __atomic_fetch_and(&cur->signal, ~(1ULL << sig), __ATOMIC_SEQ_CST);

        void (*handler)(int) = cur->sighand[sig].sa_handler;
        if (handler == SIG_IGN)
            continue;

        if (handler == SIG_DFL) {
            if (aarch64_sig_default_ignore(sig))
                continue;
            if (cur->pid == 1)
                continue;
            log_info("signal: task %d killed by signal %d (default)\n",
                     (int)cur->pid, sig);
            aarch64_vm_native_leave();
            arch_syscall_exit((uint64_t)sig);
        }

        void (*restorer)(void) = cur->sighand[sig].sa_restorer;
        if (!restorer) {
            if (cur->pid == 1)
                continue;
            log_warn("signal: task %d sig %d has no restorer, killing\n",
                     (int)cur->pid, sig);
            aarch64_vm_native_leave();
            arch_syscall_exit((uint64_t)sig);
        }

        struct aarch64_sigframe frame = {
            .regs    = *regs,
            .blocked = (uint64_t)cur->blocked,
        };
        uint64_t new_sp = (regs->sp_el0 - sizeof(struct aarch64_sigframe)) & ~UINT64_C(0xf);

        aarch64_vm_native_enter(regs, SYS_sigreturn);
        ssize_t copy_rc = copy_to_user_ft((void *)(uintptr_t)new_sp, &frame, sizeof(frame));
        aarch64_vm_native_leave();
        if (copy_rc < 0) {
            __atomic_fetch_or(&cur->signal, (1ULL << sig), __ATOMIC_SEQ_CST);
            return;
        }

        uint64_t add_mask = (1ULL << (sig - 1)) | (uint64_t)cur->sighand[sig].sa_mask;
        add_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        cur->blocked |= (int64_t)add_mask;

        regs->x0       = (uint64_t)sig;
        regs->x30      = (uint64_t)(uintptr_t)restorer;
        regs->sp_el0   = new_sp;
        regs->elr_el1  = (uint64_t)(uintptr_t)handler;
        regs->spsr_el1 = AARCH64_USER_TASK_PSTATE;
        return;
    }
}

int64_t arch_syscall_sigreturn(void *arch_frame)
{
    pt_regs_t *regs = (pt_regs_t *)arch_frame;
    if (!aarch64_user_frame_is_valid(regs))
        return -EFAULT;

    if ((regs->sp_el0 & UINT64_C(0xf)) != 0)
        return -EFAULT;

    struct aarch64_sigframe frame;
    if (!syscall_check_user_range(regs->sp_el0, sizeof(frame), false))
        return -EFAULT;
    if (copy_from_user_ft(&frame, (const void *)(uintptr_t)regs->sp_el0,
                          sizeof(frame)) < 0)
        return -EFAULT;

    /* Require exact AARCH64_USER_TASK_PSTATE (0x340: EL0t with D/A/F masked
     * and I enabled) in all non-NZCV bits; reject forged EL1 modes or DAIF edits. */
    if ((frame.regs.spsr_el1 & ~UINT64_C(0xf0000000)) != AARCH64_USER_TASK_PSTATE)
        return -EINVAL;

    *regs = frame.regs;
    task_t *cur = arch_current_task();
    cur->blocked = (int64_t)(frame.blocked &
                             ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1))));
#ifdef AARCH64_USER_RUNTIME_TEST
    aarch64_user_runtime_test_sigreturn(cur);
#endif
    return 0;
}

/* ── Creation ─────────────────────────────────────────────────── */

int user_task_prepare(task_t *tsk, void *arg)
{
    struct user_task_prepare_ctx *ctx = arg;
    struct aarch64_user_task *owner = ctx->owner;

    void *fpu = fpu_area_alloc();
    if (!fpu)
        return -ENOMEM;
    if (ctx->frame)
        aarch64_fp_save(fpu);

    /* The task owns a private mm: the image's user root.  The kernel
     * half of the address space stays in TTBR1. */
    tsk->mm = aarch64_user_image_mm(owner->image);
    tsk->addr_limit = AARCH64_USER_LIMIT;
    tsk->files = ctx->files;
    if (ctx->files && ctx->files_state)
        *ctx->files_state = 1;
    tsk->fpu_save = fpu;

    /* Spawn is parented to the resident BSP idle task (the creator may
     * exit on its own); fork is parented to the real parent so
     * getppid() and exit re-parenting observe the true tree.  pgrp and
     * session inherit from the effective parent so PID 1 keeps the
     * boot-5 pgrp=1/session=1 contract (kernel/init/userspace.c). */
    tsk->parent = ctx->parent ? ctx->parent : percpu_data[0].idle;
    tsk->pgrp = tsk->parent->pgrp;
    tsk->session = tsk->parent->session;

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

    memset(&owner->vm, 0, sizeof(owner->vm));
    if (ctx->parent && aarch64_user_task_is_user(ctx->parent)) {
        const struct aarch64_user_task *parent_owner =
            ctx->parent->thread->user_task;
        if (aarch64_vm_task_editing(ctx->parent)) {
            owner->tpidr_el0 = parent_owner->vm.edit_tpidr_el0;
            owner->tpidrro_el0 = parent_owner->vm.edit_tpidrro_el0;
        } else if (ctx->parent == arch_current_task()) {
            aarch64_user_vm_save_tls(&owner->tpidr_el0, &owner->tpidrro_el0);
        } else {
            owner->tpidr_el0 = parent_owner->tpidr_el0;
            owner->tpidrro_el0 = parent_owner->tpidrro_el0;
        }
    } else {
        owner->tpidr_el0 = 0;
        owner->tpidrro_el0 = 0;
    }

    ctx_thread->user_task = owner;
    return 0;
}

void user_task_discard(task_t *tsk, void *arg)
{
    (void)arg;
    /* The image and the owner record belong to the creator, which
     * releases them once the publication failed.  Only the task-side
     * linkage is undone here.  The pre-duplicated fd table is a
     * single-owner reference the publication never adopted: release
     * it here (files_unpin contract: never under task_list/rq locks —
     * the discard path runs before publication, so that holds). */
    if (tsk->files) {
        files_unpin(tsk->files);
        tsk->files = NULL;
        if (arg) {
            struct user_task_prepare_ctx *ctx = arg;
            if (ctx->files_state)
                *ctx->files_state = 2;
        }
    }
    if (tsk->fpu_save) {
        kfree(tsk->fpu_save);
        tsk->fpu_save = NULL;
    }
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
    files_t         *files;   /* pre-duplicated parent fd table */
    int              files_state;  /* ownership ledger, see prepare_ctx */
    int              pid;
};

static int user_task_fork_publish(void *arg)
{
    struct user_task_fork_ctx *ctx = arg;
    struct user_task_prepare_ctx pctx = {
        .owner  = ctx->owner,
        .parent = ctx->parent,
        .frame  = ctx->frame,
        .files  = ctx->files,
        .files_state = &ctx->files_state,
    };

    int target_cpu = aarch64_user_pick_cpu();
    if (target_cpu < 0)
        return target_cpu;

    task_t *tsk = NULL;
    int pid = 0;
    int rc = sched_alloc_and_publish_task((uint32_t)target_cpu,
                                          true /* single-bit affinity */,
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
    memset(child_owner, 0, sizeof(*child_owner));
    child_owner->image         = child_image;
    child_owner->catalog       = parent_owner->catalog;
    child_owner->catalog_count = parent_owner->catalog_count;
    child_owner->catalog_index = parent_owner->catalog_index;

    /* The child inherits a duplicate of the parent's fd table so the
     * stdio fds (and anything the parent opened) survive fork.  The
     * duplicate is released by user_task_discard when publication
     * fails and adopted by the child task otherwise. */
    files_t *child_files = parent_task->files ? files_dup(parent_task->files)
                                              : NULL;
    if (parent_task->files && !child_files) {
        aarch64_user_image_destroy(child_image);
        kfree(child_owner);
        return -ENOMEM;
    }

    struct user_task_fork_ctx fctx = {
        .owner  = child_owner,
        .parent = parent_task,
        .frame  = regs,
        .files  = child_files,
        .pid    = 0,
    };

    rc = aarch64_vm_edit_begin(parent_mm);
    if (rc) {
        if (child_files)
            files_unpin(child_files);
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
        if (child_files && fctx.files_state == 0)
            files_unpin(child_files);
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

int aarch64_user_task_spawn_on_cpu(const struct aarch64_user_elf_source *catalog,
                                   size_t count, size_t index, const char *name,
                                   uint32_t cpu)
{
    /* The whole index range is checked before anything is built. */
    if (!catalog || !count || index >= count || cpu >= NR_CPUS)
        return -EINVAL;
    if (!catalog[index].bytes || !catalog[index].size)
        return -EINVAL;

    /* Runtime gate: SMP started, all DTB IPIs ready, current CPU ready. */
    int rc = aarch64_user_mm_runtime_check();
    if (rc)
        return rc;

    /* Target CPU must be online and schedulable. */
    if (!__atomic_load_n(&percpu_data[cpu].online, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&percpu_data[cpu].ipi_ready, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&percpu_data[cpu].scheduler_ok, __ATOMIC_ACQUIRE))
        return -EINVAL;

    struct aarch64_user_image *image = NULL;
    rc = aarch64_user_image_load(catalog[index].bytes, catalog[index].size,
                                  AARCH64_USER_STACK_PAGES_SELFTEST, &image);
    if (rc)
        return rc;

    struct aarch64_user_task *owner = kmalloc(sizeof(*owner));
    if (!owner) {
        aarch64_user_image_destroy(image);
        return -ENOMEM;
    }
    memset(owner, 0, sizeof(*owner));
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
    rc = sched_alloc_and_publish_task(cpu, true /* pinned to explicit CPU */,
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

int aarch64_user_task_spawn(const struct aarch64_user_elf_source *catalog,
                            size_t count, size_t index, const char *name)
{
    return aarch64_user_task_spawn_on_cpu(catalog, count, index, name, 0);
}
