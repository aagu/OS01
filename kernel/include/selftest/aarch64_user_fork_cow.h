#ifndef _SELFTEST_AARCH64_USER_FORK_COW_H
#define _SELFTEST_AARCH64_USER_FORK_COW_H

/* Observation points for the M4 fork + COW runtime acceptance case.
 *
 * The production SVC return path, the formal EL0 fault branch and the
 * scheduler reap observer publish what they actually did, so the case
 * can prove that the run it observed used the real fork, COW and reap
 * paths.  The fixture asks the kernel for a monotonic stage through
 * the dedicated ``svc #0x4d43``; the page contents are never a
 * synchronisation signal.
 *
 * Everything here is compiled into the AARCH64_EL0_TEST variant only.
 */

#include <stdint.h>
#include <stdbool.h>

#if AARCH64_EL0_TEST

struct pt_regs;
struct task_struct;

void aarch64_user_fork_cow_test_reset(void);

/* Production-side observation points. */
void aarch64_user_fork_cow_test_syscall_enter(struct pt_regs *regs,
                                              uint64_t nr);
void aarch64_user_fork_cow_test_syscall_ret(struct pt_regs *regs,
                                            uint64_t nr, int64_t rc);
void aarch64_user_fork_cow_test_fault(struct pt_regs *regs, uint64_t esr,
                                      uint64_t far, int rc);
void aarch64_user_fork_cow_test_reap(struct task_struct *prev);

/* Stage query for the fixture SVC.  Returns the current stage for a
 * registered fixture PID, or -1 for every other task (the SVC handler
 * then keeps the ordinary "unknown immediate" answer). */
int64_t aarch64_user_fork_cow_test_stage_query(void);

#endif /* AARCH64_EL0_TEST */

#endif /* _SELFTEST_AARCH64_USER_FORK_COW_H */
