#ifndef _KERNEL_SELFTEST_AARCH64_USER_RUNTIME_H
#define _KERNEL_SELFTEST_AARCH64_USER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

struct pt_regs;
struct task_struct;
struct mm_struct;

#ifdef AARCH64_USER_RUNTIME_TEST
void test_aarch64_user_runtime_run(void);
void test_aarch64_user_migration_run(void);

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
void aarch64_user_runtime_test_note_ttbr_switch(uint64_t old_ttbr0,
                                                uint64_t new_ttbr0,
                                                bool full_flush);
void aarch64_user_runtime_test_note_asid_tlbi(uint8_t asid);
void aarch64_user_runtime_test_note_asid_release(uint8_t asid);
void aarch64_user_runtime_test_cow_barrier(uint64_t va, uint64_t old_pa);
void aarch64_user_runtime_test_cow_last_free(uint64_t old_pa);
void aarch64_user_runtime_test_before_exit(struct task_struct *tsk);
bool aarch64_user_runtime_test_migration_veto(const struct task_struct *tsk);
void aarch64_user_runtime_test_explicit_pin_el0(const struct task_struct *tsk);
#ifdef AARCH64_USER_RUNTIME_CASE_MIGRATION
void aarch64_user_mig_note_irq_save(const struct task_struct *tsk,
                                    const struct pt_regs *frame);
void aarch64_user_mig_note_irq_resume(const struct task_struct *tsk,
                                      const struct pt_regs *frame);
void aarch64_user_mig_note_hw_detach(const struct task_struct *tsk,
                                     uint64_t root_pa, uint8_t asid);
void aarch64_user_mig_note_root_unpublish(const struct task_struct *tsk,
                                          uint64_t root_pa, uint8_t asid);
void aarch64_user_mig_note_tail(const struct task_struct *tsk);
void aarch64_user_mig_note_commit(const struct task_struct *tsk,
                                  uint32_t src_cpu, uint32_t dst_cpu);
void aarch64_user_mig_note_pick(const struct task_struct *tsk, uint32_t cpu);
void aarch64_user_mig_note_hw_install(const struct task_struct *tsk,
                                      uint64_t root_pa, uint8_t asid);
void aarch64_user_mig_note_el0_svc(const struct task_struct *tsk,
                                   const struct pt_regs *frame);
void aarch64_user_mig_note_exec_commit(const struct task_struct *tsk,
                                       uint64_t old_root, uint64_t new_root,
                                       uint64_t old_mask, uint64_t new_mask);
void aarch64_user_mig_note_exec_el0(const struct task_struct *tsk);
void aarch64_user_mig_note_sync_fault(const struct task_struct *tsk,
                                      const struct pt_regs *frame,
                                      uint64_t esr, uint64_t far, int frc);
void aarch64_user_mig_idle_pre_wfi(uint32_t cpu, uint32_t rq_nr);
void aarch64_user_mig_idle_tick(uint32_t cpu, uint32_t intid);
void aarch64_user_mig_idle_after_check(uint32_t cpu, bool took_balance,
                                       bool entered_wfi);
void aarch64_user_mig_note_vm_begin(const struct task_struct *tsk);
void aarch64_user_mig_note_vm_edit(const struct task_struct *tsk,
                                   uint64_t nr, uint64_t va, bool is_fault,
                                   uint64_t old_pte, uint64_t new_pte,
                                   bool leaf_absent);
void aarch64_user_mig_note_va_tlbi(uint64_t va);
void aarch64_user_mig_note_tlbi(uint8_t asid, bool is_full_flush);
void aarch64_user_mig_note_copy(const struct task_struct *tsk, bool write,
                                uint64_t user_addr, int64_t ret);
void aarch64_user_mig_note_bsp_sched(uint32_t cpu);
void aarch64_user_mig_note_irq(uint32_t intid);
void aarch64_user_mig_note_ap_work(uint32_t cpu, uint32_t slot_state);
void aarch64_user_mig_note_sched_entry(uint32_t cpu, bool is_idle,
                                       uint32_t need_resched);
void aarch64_user_mig_note_wait_status(int req_pid, int child_pid,
                                       int exit_code, int copy_rc);
#endif
#endif

#endif /* _KERNEL_SELFTEST_AARCH64_USER_RUNTIME_H */
