#ifndef _SELFTEST_AARCH64_USER_SVC_UACCESS_H
#define _SELFTEST_AARCH64_USER_SVC_UACCESS_H

/* Observation and fault-injection surface of the formal SVC + uaccess
 * acceptance case (AARCH64_EL0_TEST only).
 *
 * Nothing here is compiled into a normal aarch64 image: the hook is
 * called from aarch64_user_syscall() under the test macro, the copy
 * bypass is defined in uaccess.c under the same macro, and the
 * production-ELF audit rejects any of these symbols in a normal image.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>      /* ssize_t */

struct pt_regs;

/* Arm the observation for the fixture task.  `expected_ppid` is the
 * resident BSP idle task's PID (the formal task's parent). */
void aarch64_user_svc_uaccess_test_arm(int pid, uint64_t expected_ppid);

/* Called from aarch64_user_syscall() BEFORE the shared dispatch, for
 * the armed fixture PID only.  On the first clock_gettime it records
 * the frame's x19/x20 (the fixture's PID/PPID copy) and runs the
 * kernel copy self-test once. */
void aarch64_user_svc_uaccess_test_hook(struct pt_regs *regs, uint64_t nr);

/* The formal copy with ONLY the page-permission precheck skipped.
 * The bounds, mm, published-root, hardware-root and window checks all
 * still run, so a real hardware abort reaches the fixup exactly as the
 * production copy would. */
ssize_t aarch64_uaccess_test_copy(bool write, uint64_t user_addr, void *kern,
                                  size_t n, void (*on_fault)(void *), void *arg);

/* The acceptance case, registered as the late id
 * "aarch64_user_svc_uaccess". */
int test_aarch64_user_svc_uaccess(void);

#endif /* _SELFTEST_AARCH64_USER_SVC_UACCESS_H */
