#ifndef KERNEL_SELFTEST_AARCH64_SCHEDULER_H
#define KERNEL_SELFTEST_AARCH64_SCHEDULER_H

#ifdef OS01_SELFTEST
#include <sched/task.h>
#include <stdint.h>

void test_aarch64_scheduler_run(void);
void aarch64_sched_observe_resource(const void *ptr, unsigned kind, bool alloc);
void aarch64_sched_observe_reap(task_t *prev, bool next_stack);
void aarch64_sched_before_block_switch(task_t *task);
void aarch64_sched_observe_block_enqueue(task_t *task);
void aarch64_sched_observe_scan_wake(task_t *task);
void aarch64_sched_observe_switch_tasks(task_t *prev, task_t *next);
void aarch64_sched_observe_switch(void);
struct pt_regs;
void aarch64_sched_observe_timer_irq(struct pt_regs *regs, bool entering);
void aarch64_sched_observe_nested_schedule(void);
extern volatile uint64_t aarch64_sched_timer_tick_count;
/* Assembly probes (kernel/arch/aarch64/cpu/probe.S). reg_probe yields
 * 16 times around callee-saved register/SP/stack-sentinel checks and
 * returns bit0=regs, bit1=canaries, bit2=SP failures; timer_irq_probe
 * verifies the real IRQ save/restore roundtrip across >=target ticks. */
int aarch64_sched_reg_probe(uint64_t pattern);
int aarch64_sched_timer_irq_probe(uint64_t target_tick, uint64_t deadline_cycles);
#endif

#endif
