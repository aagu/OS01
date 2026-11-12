#ifndef _SELFTEST_AARCH64_USER_TASK_H
#define _SELFTEST_AARCH64_USER_TASK_H

/* Observation points for the M4 user-task acceptance case.
 *
 * The production paths (the task switch, the lower-EL IRQ adapter, the
 * image constructor and the user-task SVC handler) publish what they
 * did here, so the selftest can prove that the run it observed was a
 * real one and not a simulated one.
 *
 * Everything here is compiled into the AARCH64_EL0_TEST variant only.
 */

#include <stdint.h>
#include <stdbool.h>

/* One-shot allocation failure injection.
 *
 * The image constructor fails with -ENOMEM after it has mapped exactly
 * one page, but only for the armed PID.  The arming survives a parse
 * failure (the invalid catalog entry never reaches the mapping loop),
 * so the test can arm it before the task starts and still hit the
 * designated exec. */
void aarch64_user_image_test_arm_fail(int pid);

/* Arm the real CNTP so the next EL0 entry is followed by a genuine
 * timer tick: the write happens in the IRQ-masked kernel state, right
 * before EL0 is restored. */
void aarch64_user_task_test_set_arm_tick(bool on);

/* Counters published by the production paths. */
struct aarch64_user_task_stats {
    uint64_t switches;          /* switches that involved a user task */
    uint64_t switches_to_user;  /* switches INTO a user task */
    uint64_t svcs;              /* SVCs handled for a user task */
    uint64_t el0_irqs;          /* lower-EL IRQs dispatched */
    uint64_t image_loads;       /* image constructor calls */
    uint64_t image_failures;    /* image constructor failures */
    uint64_t execs;             /* exec attempts */
    uint64_t exits;             /* task exits */
    int      last_switch_pid;   /* the PID switched to last */
    int      last_switch_from;  /* the PID switched from last */
    uint64_t last_switch_root;  /* the root installed for that switch */
    uint64_t last_svc_imm;      /* the last SVC immediate */
    uint64_t last_exec_index;   /* the last exec catalog index */
    int      last_exec_rc;      /* the last exec result */
    int      last_exit_pid;     /* the PID that exited last */
    uint64_t last_exit_status;  /* the status that PID exited with */
    unsigned exits_recorded;    /* the number of recorded exits */
    int      exit_pid[4];       /* the PIDs that exited, in order */
    uint64_t exit_status[4];    /* their statuses */
    uint64_t el0_ticks;         /* the EL0 CNTP tick counter */
    uint64_t el0_irq_pid_violations;
    uint64_t el0_irq_switch_violations;
};

void aarch64_user_task_test_stats(struct aarch64_user_task_stats *out);
void aarch64_user_task_test_reset_stats(void);

/* Production-side notes. */
void aarch64_user_task_test_note_switch(int prev_pid, int next_pid,
                                         uint64_t root, bool to_user);
void aarch64_user_task_test_note_el0_irq(bool pid_stable, bool switches_stable);
void aarch64_user_task_test_note_image(bool ok);

/* The number of real switches observed so far (used by the IRQ
 * adapter to prove that an IRQ never schedules). */
uint64_t aarch64_user_task_test_switch_count(void);

/* A snapshot marker and the current PID, taken around the IRQ
 * dispatch to prove that an IRQ never schedules. */
uint64_t aarch64_user_task_test_mark(void);
int aarch64_user_task_test_pid(void);

#endif /* _SELFTEST_AARCH64_USER_TASK_TASK_H */
