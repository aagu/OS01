/* kernel/selftest/test_aarch64_user_task.c
 *
 * M4 user-task acceptance case.
 *
 * Two real EL0 tasks (A and B) run the SAME VA layout with different
 * data, so their private roots are directly comparable.  The case
 * proves:
 *
 *   - both tasks really enter and leave EL0 through the shared restore
 *     path, and their data, stack values and registers survive the
 *     round trip;
 *   - a real EL0 CNTP interrupt completes a GIC dispatch and returns
 *     without scheduling;
 *   - an invalid catalog entry and a one-shot allocation failure are
 *     both rejected without disturbing the running image;
 *   - a successful replacement keeps the task identity and hands the
 *     task to the new image;
 *   - a guard-page access raises a Data Abort and terminates the task;
 *   - every exit releases its pages, so the 4 KiB page count returns to
 *     its baseline.
 */

#if AARCH64_EL0_TEST

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <arch/aarch64/user_task.h>
#include <arch/aarch64/vmm_gate.h>
#include <memory/pmm.h>
#include <memory/slab.h>
#include <log/log.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <time/timer.h>
#include <selftest/aarch64_user_task.h>

/* The embedded fixture images.  A and B share the VA layout; the
 * replacement image C is the one A installs at the end. */
extern const char _binary_aarch64_user_task_a_elf_start[];
extern const char _binary_aarch64_user_task_a_elf_end[];
extern const char _binary_aarch64_user_task_b_elf_start[];
extern const char _binary_aarch64_user_task_b_elf_end[];
extern const char _binary_aarch64_user_task_c_elf_start[];
extern const char _binary_aarch64_user_task_c_elf_end[];

/* The catalog the tasks share.  The indices are the ones the A fixture
 * uses for its replacement attempts. */
enum {
    UT_CAT_A = 0,        /* the A image — the task A starts from */
    UT_CAT_B = 1,        /* the B image — the task B starts from */
    UT_CAT_BAD = 2,      /* an invalid header — must be rejected */
    UT_CAT_FAIL = 3,     /* a valid image — the injected failure hits it */
    UT_CAT_NEW = 4,      /* the replacement image */
    UT_CAT_COUNT = 5,
};

/* The exit statuses the fixtures report. */
#define UT_STATUS_REPLACED 0x33   /* the replacement image leaves with it */
#define UT_STATUS_FAILED   0xf0   /* a fixture self-check failed */
#define UT_STATUS_NORMAL   0x11

struct ut_run {
    int rc;
    int pid_a;
    int pid_b;
    uint64_t pmm_before;
    uint64_t pmm_after;
    unsigned bad_buffer_size;
    void *bad_buffer;
};

static int ut_fail(struct ut_run *run, const char *what)
{
    log_err("[user-task] FAIL: %s\n", what);
    run->rc = 1;
    return 1;
}

static unsigned char *ut_make_invalid(const void *bytes, size_t size)
{
    unsigned char *copy = kmalloc(size);
    if (!copy)
        return NULL;
    __builtin_memcpy(copy, bytes, size);
    /* An invalid ELF magic: the constructor must reject the image
     * before it maps anything. */
    copy[0] = 0x00;
    return copy;
}

/* The kernel witness thread: it proves that the kernel still runs
 * after both user tasks are gone. */
static volatile int ut_witness_ran;
static uint64_t ut_witness(uint64_t arg)
{
    (void)arg;
    ut_witness_ran = 1;
    return 0;
}

static uint64_t ut_worker(uint64_t arg)
{
    struct ut_run *run = (struct ut_run *)(uintptr_t)arg;
    struct aarch64_user_task_stats s;

    const size_t a_size = (size_t)(_binary_aarch64_user_task_a_elf_end -
                                   _binary_aarch64_user_task_a_elf_start);
    const size_t b_size = (size_t)(_binary_aarch64_user_task_b_elf_end -
                                   _binary_aarch64_user_task_b_elf_start);
    const size_t c_size = (size_t)(_binary_aarch64_user_task_c_elf_end -
                                   _binary_aarch64_user_task_c_elf_start);

    unsigned char *bad = ut_make_invalid(_binary_aarch64_user_task_a_elf_start,
                                         a_size);
    if (!bad)
        return ut_fail(run, "no memory for the invalid catalog entry"), 0;
    run->bad_buffer = bad;
    run->bad_buffer_size = (unsigned)a_size;

    struct aarch64_user_elf_source catalog[UT_CAT_COUNT] = {
        { _binary_aarch64_user_task_a_elf_start, a_size },
        { _binary_aarch64_user_task_b_elf_start, b_size },
        { bad, a_size },
        { _binary_aarch64_user_task_a_elf_start, a_size },
        { _binary_aarch64_user_task_c_elf_start, c_size },
    };

    aarch64_user_task_test_reset_stats();

    /* The 4 KiB page baseline: the user images are the only 4 KiB
     * pages this case allocates. */
    run->pmm_before = pmm_probe_4k_allocated_count();

    /* Arm the real CNTP so every yield is followed by a genuine timer
     * tick in EL0. */
    aarch64_user_task_test_set_arm_tick(true);

    run->pid_a = aarch64_user_task_spawn(catalog, UT_CAT_COUNT, UT_CAT_A,
                                          "user_a");
    if (run->pid_a < 0) {
        aarch64_user_task_test_set_arm_tick(false);
        ut_fail(run, "spawning task A failed");
        goto out;
    }

    /* Arm the one-shot allocation failure for task A.  The arming
     * survives the invalid catalog entry (which never reaches the
     * mapping loop) and hits the designated replacement instead. */
    aarch64_user_image_test_arm_fail(run->pid_a);

    run->pid_b = aarch64_user_task_spawn(catalog, UT_CAT_COUNT, UT_CAT_B,
                                          "user_b");
    if (run->pid_b < 0) {
        aarch64_user_task_test_set_arm_tick(false);
        ut_fail(run, "spawning task B failed");
        goto out;
    }

    /* Wait for both tasks to leave.  The deadline is jiffies-bounded:
     * the scheduler only advances a task's vruntime on a tick, so a
     * spin-count bound would starve the user tasks. */
    {
        uint64_t deadline = jiffies + 500;
        for (;;) {
            aarch64_user_task_test_stats(&s);
            if (s.exits >= 2)
                break;
            if (jiffies >= deadline) {
                ut_fail(run, "the user tasks did not finish in time");
                goto out;
            }
            kthread_yield();
        }
    }

    /* ── The observations ───────────────────────────────────── */

    /* Both tasks really entered and left EL0. */
    if (s.switches_to_user < 128) {
        ut_fail(run, "too few switches into a user task");
        goto out;
    }
    if (s.svcs < 130) {
        ut_fail(run, "too few SVC round trips");
        goto out;
    }
    /* A real EL0 CNTP interrupt completed a GIC dispatch, and it did
     * not schedule. */
    if (s.el0_ticks == 0) {
        ut_fail(run, "no real EL0 CNTP tick was counted");
        goto out;
    }
    if (s.el0_irq_pid_violations || s.el0_irq_switch_violations) {
        ut_fail(run, "an EL0 IRQ changed the PID or the switch count");
        goto out;
    }
    /* The replacement attempts: the invalid entry, the injected
     * allocation failure and the real replacement. */
    if (s.execs != 3) {
        ut_fail(run, "the fixture did not perform three replacements");
        goto out;
    }
    if (s.last_exec_rc != 0) {
        ut_fail(run, "the successful replacement reported a failure");
        goto out;
    }
    if (s.image_loads != 5 || s.image_failures != 2) {
        ut_fail(run, "the image constructor did not see two failures");
        goto out;
    }
    /* Both tasks left with their own status: the replacement image
     * reports 0x33, the guard fault reports -EFAULT. */
    {
        int seen_replaced = 0, seen_fault = 0;
        for (unsigned i = 0; i < s.exits_recorded && i < 4; i++) {
            if (s.exit_pid[i] == run->pid_a &&
                s.exit_status[i] == UT_STATUS_REPLACED)
                seen_replaced = 1;
            if (s.exit_pid[i] == run->pid_b &&
                s.exit_status[i] == (uint64_t)-EFAULT)
                seen_fault = 1;
        }
        if (!seen_replaced) {
            ut_fail(run, "task A did not exit with the replacement status");
            goto out;
        }
        if (!seen_fault) {
            ut_fail(run, "task B did not exit with the guard fault status");
            goto out;
        }
    }

    aarch64_user_task_test_set_arm_tick(false);

    /* A kernel witness thread still runs after both user tasks are
     * gone. */
    ut_witness_ran = 0;
    kernel_thread_on_cpu(ut_witness, 0, 0);
    {
        uint64_t deadline = jiffies + 100;
        while (!ut_witness_ran && jiffies < deadline)
            kthread_yield();
    }
    if (!ut_witness_ran) {
        ut_fail(run, "the kernel witness thread did not run");
        goto out;
    }

    /* Every released page went back to the pool: the 4 KiB count is
     * the baseline again, and only the kernel root stays published. */
    run->pmm_after = pmm_probe_4k_allocated_count();
    if (run->pmm_after != run->pmm_before) {
        ut_fail(run, "the 4 KiB page count did not return to its baseline");
        goto out;
    }
    if (aarch64_pt_published_root_count() != 1) {
        ut_fail(run, "a user root stayed published");
        goto out;
    }

out:
    aarch64_user_task_test_set_arm_tick(false);
    if (run->bad_buffer)
        kfree(run->bad_buffer);
    run->bad_buffer = NULL;
    return 0;
}

int test_aarch64_user_task(void)
{
    struct ut_run run = {0};
    kernel_thread_on_cpu(ut_worker, (uintptr_t)&run, 0);
    kthread_yield();
    return run.rc;
}

#endif /* AARCH64_EL0_TEST */
