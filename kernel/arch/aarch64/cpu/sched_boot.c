/* kernel/arch/aarch64/cpu/sched_boot.c — aarch64 scheduler boot hooks.
 *
 * Implements arch_sched_boot_prepare() and arch_sched_boot_idle() for
 * the boot-5 userspace orchestration (kernel/init/userspace.c).  The
 * legacy boot path (selftest variants, OS01_HAS_USERSPACE=0) keeps the
 * monolithic arch_aarch64_bsp_handoff sequence in cpu/task_arch.c and
 * never calls these hooks.
 */

#include <arch/boot.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/ap_sched.h>
#include <arch/aarch64/sched_tick.h>
#include <sched/boot.h>
#include <sched/task.h>
#include <log/log.h>

void arch_sched_boot_prepare(void)
{
    /* AP co-scheduler gate metadata (boot-stack safe). */
    aarch64_ap_sched_prepare();
    /* Init-task thread geometry + the per-CPU tick accounting init:
     * data-only, never reads current. */
    arch_aarch64_prepare_bsp_handoff();
    /* Remember the kernel root and disable the FP trap BEFORE any user
     * task exists: spawn_user_task runs later in the boot-5 sequence
     * and its install-root path needs the kernel root recorded.  A
     * tick-start failure halts the boot — it must never publish
     * scheduler_ok. */
    aarch64_user_runtime_init();
    /* The BSP local tick start verifies the CPU0 idle task is
     * published (the tick classifies every sample against it), so the
     * idle metadata must be in place first — publish it here;
     * kernel_start_userspace's own publish_idle call is idempotent
     * (the legacy handoff sequence runs publish_idle -> tick_start in
     * the same order).  A failure halts the boot — it must never
     * publish scheduler_ok. */
    sched_boot_publish_idle();
    if (!aarch64_sched_tick_start()) {
        log_err("[sched-tick] FATAL: BSP local tick start failed\n");
        for (;;)
            arch_cpu_halt();
    }
}

void arch_sched_boot_idle(void)
{
    /* Open the AP release gate and wait for every accepted AP's
     * scheduler_ok — the BSP scheduler_ok was published by
     * sched_boot_start() earlier in the boot-5 sequence. */
    aarch64_ap_sched_release();

    /* One-way handoff onto the init task stack: the assembly switches
     * SP and lands in arch_aarch64_bsp_handoff, whose userspace branch
     * runs only the invariant checks before idle_resume(). */
    arch_local_irq_disable();
    aarch64_bsp_stack_enter((uint64_t)&init_task_union + STACK_SIZE - 16);
    for (;;)
        arch_cpu_halt();
}
