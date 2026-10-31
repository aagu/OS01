/* kernel/sched/diag.c — arch-neutral scheduler hang/watchdog dump
 * (aarch64 EL1 scheduler plan Phase 2, Task 2).
 *
 * Moved verbatim from kernel/core/panic.c's hang_dump_all(): panic.c
 * pulls driver/fb_state.h and tty/console.h (x86-only), so the common
 * dump could not be reached from an aarch64 TU.  The architecture
 * fields of each task go through the arch_dump_task_context() facade
 * hook (Phase 1); common code never reads thread->rip/rsp/cr3.
 */

#include <sched/task.h>
#include <sched/internal.h>
#include <sched/diag.h>
#include <percpu/percpu.h>
#include <core/printk.h>
#include <kernel.h>
#include <time/timer.h>

// ── Dump all tasks ─────────────────────────────────────────
void hang_dump_all(void)
{
    uint64_t jif = jiffies;

    serial_printk("\n========== HANG DETECTED ==========\n");

    for (int i = 0; i < (int)num_cpus; i++) {
        percpu_t *cpu = &percpu_data[i];
        serial_printk("CPU %d: online=%d sched_ok=%d watchdog=%lu sched_count=%lu\n",
                      i, cpu->online, cpu->scheduler_ok,
                      (unsigned long)cpu->watchdog_counter,
                      (unsigned long)cpu->schedule_count);
    }

    // Walk the global task list
    extern union task_union init_task_union;
    list_t *pos = init_task_union.task.list.next;
    int task_idx = 0;
    while (pos != &init_task_union.task.list && task_idx < 64) {
        task_t *t = container_of(pos, task_t, list);
        pos = pos->next;

        const char *state_str = "?";
        switch (t->state) {
            case TASK_RUNNING:      state_str = "RUNNING";      break;
            case TASK_INTERRUPTIBLE: state_str = "INTR";         break;
            case TASK_UNINTERRUPTIBLE: state_str = "UNINTR";     break;
            case TASK_ZOMBIE:       state_str = "ZOMBIE";       break;
            case TASK_STOPPED:      state_str = "STOPPED";      break;
        }

        serial_printk("  task %d: pid=%d cpu=%d state=%s counter=%ld flags=%#lx\n",
                      task_idx++, t->pid, t->cpu, state_str,
                      (long)t->counter, (unsigned long)t->flags);
        /* Architecture-specific saved context (x86: rip/rsp/cr3) —
         * behind the arch facade; common code must not read
         * thread->rip / thread->rsp / thread->cr3 directly. */
        arch_dump_task_context(t);
    }
    serial_printk("  (%d tasks total at jiffies=%lu)\n",
                  task_idx, (unsigned long)jif);
    serial_printk("=====================================\n\n");
}
