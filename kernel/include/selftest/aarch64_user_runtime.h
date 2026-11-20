#ifndef _KERNEL_SELFTEST_AARCH64_USER_RUNTIME_H
#define _KERNEL_SELFTEST_AARCH64_USER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

struct pt_regs;
struct task_struct;
struct mm_struct;

#ifdef AARCH64_USER_RUNTIME_TEST
void test_aarch64_user_runtime_run(void);

/* Test-only observation hooks compiled solely under AARCH64_USER_RUNTIME_TEST. */
void aarch64_user_runtime_test_timer_eoi(uint32_t intid, struct pt_regs *regs);
void aarch64_user_runtime_test_irq_sched_before(struct task_struct *tsk,
                                                const struct pt_regs *regs,
                                                uint64_t *switches_out,
                                                uint64_t *elr_out);
void aarch64_user_runtime_test_irq_sched_after(struct task_struct *tsk,
                                               const struct pt_regs *regs,
                                               uint64_t switches_before,
                                               uint64_t elr_before);
void aarch64_user_runtime_test_vm_edit_pause(struct mm_struct *mm);
void aarch64_user_runtime_test_vm_edit_after(struct mm_struct *mm);
void aarch64_user_runtime_test_sigreturn(struct task_struct *tsk);
#endif

#endif /* _KERNEL_SELFTEST_AARCH64_USER_RUNTIME_H */
