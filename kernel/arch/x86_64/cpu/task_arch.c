/* kernel/arch/x86_64/cpu/task_arch.c — x86_64 task/thread context.
 *
 * Besides the TSS/GS setup, this file implements the x86 half of the
 * kernel-thread constructor hooks declared in <arch/context.h>:
 *
 *   arch_kthread_prepare()   the resources an x86 kernel thread needs:
 *                             the inherited mm + CR3, the copied file
 *                             table, the FPU save area and the
 *                             pt_regs first-entry frame that
 *                             arch_kernel_thread_entry pops.
 *   arch_kthread_drop_files() detach + unpin the copied file table.
 *
 * aarch64 implements its own pair in
 * kernel/arch/aarch64/cpu/task_arch.c (Phase 3): shared init_mm, no file
 * table, no FPU save area, EL1 first-entry context.
 */

#include <sched/task.h>
#include <sched/internal.h>
#include <fs/file.h>
#include <arch/x86_64/regs.h>
#include <arch/x86_64/gate.h>
#include <arch/x86_64/task_arch.h>
#include <arch/thread.h>
#include <arch/x86_64/tss.h>
#include <arch/mmu.h>
#include <memory/slab.h>
#include <errno.h>
#include <string.h>

/* The x86 first-entry trampoline (kernel/arch/x86_64/cpu/thread_entry.S):
 * pops the pt_regs frame, calls fn(arg), then do_exit(). */
extern void arch_kernel_thread_entry(void);

struct tss_struct init_tss[NR_CPUS] = { [0 ... NR_CPUS - 1] = INIT_TSS };

void arch_task_init_early(void)
{
    // AP idle tasks and boot-time kthreads inherit this before task_init().
    // A zero saved CR3 makes switch_to leave a user's page table active
    // on an idle CPU even after that process migrates away and exits.
    init_mm.pgdir = arch_get_page_table();
    init_thread.cr3 = (uint64_t)init_mm.pgdir;

    load_TR(8);
    set_tss64(TSS64_Table,
              0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7800, 0x7400,
              0, 0, 0, 0);
}

void *arch_task_boot_state(void)
{
    return TSS64_Table;
}

void arch_task_init_platform(void)
{
    // Program BSP TSS with kernel stack pointers and IST entries.
    // BSP uses the legacy global TSS64_Table as its hardware TSS.
    set_tss64(TSS64_Table,
              init_thread.rsp0, init_tss[0].rsp1, init_tss[0].rsp2,
              init_tss[0].ist1, init_tss[0].ist2, init_tss[0].ist3,
              init_tss[0].ist4, init_tss[0].ist5, init_tss[0].ist6,
              init_tss[0].ist7);

    init_tss[0].rsp0 = init_thread.rsp0;
}

/* ── Kernel-thread constructor hook (x86) ─────────────────────
 * Acquires the resources an x86 kernel thread needs.  On failure every
 * resource acquired here is released and NULLed, so the common
 * rollback only has to free the thread struct and the raw stack. */
int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg)
{
    /* Inherited address space and CR3: a kernel thread runs in the
     * kernel address space of the task that created it. */
    t->mm = current->mm;
    t->thread->cr3 = current->thread->cr3;

    /* Copied file table.  The copy is what keeps the creator's open
     * files alive independently of the creator. */
    if (current->files) {
        t->files = files_dup(current->files);
        if (!t->files) {
            t->files = NULL;
            return -ENOMEM;
        }
    }

    /* FPU save area: the kernel thread inherits the FPU state slot the
     * switch tail restores on every switch away from it. */
    t->fpu_save = fpu_area_alloc();
    if (!t->fpu_save) {
        t->fpu_save = NULL;
        arch_kthread_drop_files(t);
        return -ENOMEM;
    }

    /* First-entry frame: the trampoline pops this pt_regs block off
     * the top of the task's stack, takes fn from rbx and arg from rdx,
     * then calls do_exit() when fn returns. */
    pt_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    regs.rbx    = (uint64_t)fn;
    regs.rdx    = arg;
    regs.ds     = KERNEL_DS;
    regs.es     = KERNEL_DS;
    regs.cs     = KERNEL_CS;
    regs.ss     = KERNEL_DS;
    regs.rflags = (1 << 9);              /* interrupt enable */
    regs.rip    = (uint64_t)arch_kernel_thread_entry;

    void *frame = (void *)((uint64_t)t + STACK_SIZE - sizeof(pt_regs_t));
    memcpy(frame, &regs, sizeof(regs));

    t->thread->rsp0 = (uint64_t)t + STACK_SIZE;
    t->thread->rsp  = (uint64_t)t + STACK_SIZE - sizeof(pt_regs_t);
    t->thread->fs   = KERNEL_DS;
    t->thread->gs   = KERNEL_DS;
    t->thread->rip  = (uint64_t)arch_kernel_thread_entry;

    return 0;
}

/* ── Kernel-thread file-table teardown (x86) ───────────────────
 * The detach happens under task_list_lock (the file table is
 * published under that lock); the unpin happens outside it, because
 * files_unpin() drops the last reference and may free the table —
 * which must not happen under task_list_lock (see <fs/file.h>). */
void arch_kthread_drop_files(task_t *t)
{
    files_t *fs = NULL;

    uint64_t fl = spin_lock_irqsave(&task_list_lock);
    fs = t->files;
    t->files = NULL;
    spin_unlock_irqrestore(&task_list_lock, fl);

    if (fs)
        files_unpin(fs);
}
