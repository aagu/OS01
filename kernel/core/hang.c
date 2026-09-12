#include <kernel/hang.h>
#include <kernel/printk.h>
#include <kernel/percpu.h>
#include <kernel/task.h>
#include <kernel.h>
#include <device/timer.h>

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
        if (t->thread) {
            serial_printk("    rip=%#lx rsp=%#lx cr3=%#lx\n",
                          (unsigned long)t->thread->rip,
                          (unsigned long)t->thread->rsp,
                          (unsigned long)t->thread->cr3);
        }
    }
    serial_printk("  (%d tasks total at jiffies=%lu)\n",
                  task_idx, (unsigned long)jif);
    serial_printk("=====================================\n\n");
}
