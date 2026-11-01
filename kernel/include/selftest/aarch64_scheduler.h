#ifndef KERNEL_SELFTEST_AARCH64_SCHEDULER_H
#define KERNEL_SELFTEST_AARCH64_SCHEDULER_H

/* First-entry observation record geometry — shared by the C consumers
 * and thread_entry.S, so the values below must stay literal (they are
 * pinned by the _Static_asserts in the C section). */
#define AARCH64_SCHED_ENTRY_RECORD_SIZE    24
#define AARCH64_SCHED_ENTRY_RECORD_SP_OFF   0
#define AARCH64_SCHED_ENTRY_RECORD_BASE_OFF 8
#define AARCH64_SCHED_ENTRY_RECORD_OK_OFF  16

#ifdef OS01_SELFTEST
#ifndef __ASSEMBLER__
#include <sched/task.h>
#include <stddef.h>
#include <stdint.h>

/* Per-CPU first-entry record: thread_entry.S locates its CPU's slot
 * through TPIDR_EL1 + PERCPU_CPU_ID_OFFSET (never through `current`)
 * and writes sp/base/ok at offsets 0/8/16; the C consumers read the
 * same array by cpu index. */
struct aarch64_sched_entry_record {
    uint64_t sp;
    uint64_t base;
    uint64_t ok;
};
_Static_assert(sizeof(struct aarch64_sched_entry_record) ==
               AARCH64_SCHED_ENTRY_RECORD_SIZE,
               "entry record stride must stay 24 bytes for the asm site");
_Static_assert(offsetof(struct aarch64_sched_entry_record, sp) ==
               AARCH64_SCHED_ENTRY_RECORD_SP_OFF, "sp offset drift");
_Static_assert(offsetof(struct aarch64_sched_entry_record, base) ==
               AARCH64_SCHED_ENTRY_RECORD_BASE_OFF, "base offset drift");
_Static_assert(offsetof(struct aarch64_sched_entry_record, ok) ==
               AARCH64_SCHED_ENTRY_RECORD_OK_OFF, "ok offset drift");

extern volatile struct aarch64_sched_entry_record
    aarch64_sched_entry_records[];

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
#endif /* __ASSEMBLER__ */
#endif /* OS01_SELFTEST */

#endif
