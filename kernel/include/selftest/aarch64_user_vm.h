#ifndef _SELFTEST_AARCH64_USER_VM_H
#define _SELFTEST_AARCH64_USER_VM_H

/* Observation points for the M4 anonymous-VM acceptance case.
 *
 * Three production paths publish what they actually did:
 *   - the native SVC return path (intr/syscall.c) checks that mmap/brk
 *     committed metadata only and records the two clock_gettime
 *     -EFAULT targets;
 *   - the formal user fault branch (cpu/user_task.c) records the ESR,
 *     FAR, original PC and the demand-fault result;
 *   - the image replacement path records the exec result and the old
 *     root PA after a failure or after the old image is released.
 *
 * Everything here is compiled into the AARCH64_EL0_TEST variant only.
 */

#include <stdint.h>
#include <stdbool.h>

#if AARCH64_EL0_TEST

struct pt_regs;

#define AARCH64_USER_VM_CLOCK_SAMPLES 4
#define AARCH64_USER_VM_EXEC_RECORDS  4

struct aarch64_user_vm_stats {
    uint64_t syscalls;
    uint64_t mmap_calls;
    uint64_t mmap_absent_violations;
    uint64_t brk_calls;
    uint64_t brk_absent_checked;
    uint64_t brk_absent_violations;

    uint64_t faults;
    uint64_t fault_retried;
    uint64_t fault_refused;
    uint64_t fault_oom;
    uint64_t last_fault_esr;
    uint64_t last_fault_far;
    uint64_t last_fault_pc;
    int64_t  last_fault_rc;

    uint64_t execs;
    unsigned exec_records;
    struct {
        int64_t  rc;
        uint64_t old_root_pa;
    } exec[AARCH64_USER_VM_EXEC_RECORDS];

    uint64_t clock_errors;
    unsigned clock_samples;
    struct {
        uint64_t va;
        int      state;      /* enum user_vm_leaf_state */
        uint64_t sentinel;   /* the leaf's last 8 bytes, 0 when absent */
    } clock[AARCH64_USER_VM_CLOCK_SAMPLES];
};

void aarch64_user_vm_test_reset(void);
void aarch64_user_vm_test_stats(struct aarch64_user_vm_stats *out);

/* Production-side observation points. */
void aarch64_user_vm_test_syscall(struct pt_regs *regs, uint64_t nr,
                                  int64_t rc);
void aarch64_user_vm_test_fault(uint64_t esr, uint64_t far, uint64_t pc,
                                int rc);
void aarch64_user_vm_test_exec(int rc, uint64_t old_root_pa);

/* Case-side controls. */
void aarch64_user_vm_test_watch_clock(bool on);
void aarch64_user_vm_test_arm_replace_fail(int pid);

#endif /* AARCH64_EL0_TEST */

#endif /* _SELFTEST_AARCH64_USER_VM_H */
