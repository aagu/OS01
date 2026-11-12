/* kernel/selftest/test_aarch64_user_svc_uaccess.c
 *
 * M4 formal SVC + uaccess acceptance case.
 *
 * One real EL0 task runs the SVC fixture and proves, in the SAME boot:
 *
 *   - the native ABI: getpid/getppid travel in x19/x20 and the kernel
 *     reads them back from the saved exception frame at the first
 *     clock_gettime; an unknown immediate, a defined-but-unserved
 *     number and an x8 with a non-zero high 32 bits all answer
 *     -ENOSYS;
 *   - clock_gettime writes a 16-byte timespec across the boundary of
 *     the two mapped RW pages;
 *   - clock_gettime into the read-only code page and across the
 *     unmapped page at the end of the RW segment both return -EFAULT,
 *     and the latter leaves its prefix sentinel intact;
 *   - the kernel copy self-test (run once, on the fixture's first
 *     clock_gettime) sees a real cross-page copy-in, zero length and
 *     overflow rejection, and reaches the EXACT user-copy fault labels
 *     for an unmapped read and a read-only write.  Each abort returns
 *     -EFAULT with exactly one callback, and a following copy succeeds
 *     (proving the window was cleared);
 *   - the native exit is reaped with PF_SELF_REAP and the encoded
 *     status (0x2a << 8);
 *   - every 4 KiB page returns to the baseline and only the kernel root
 *     stays published.
 *
 * The fixture's own guard-page regression stays in the
 * aarch64_user_task_lifecycle case, which runs in the same boot.
 */

#if AARCH64_EL0_TEST

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <arch/regs.h>
#include <arch/aarch64/uaccess.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/vmm_gate.h>
#include <memory/pmm.h>
#include <memory/uaccess.h>
#include <percpu/percpu.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_user_svc_uaccess.h>
#include <time/timer.h>
#include <uapi/syscall.h>
#include <log/log.h>

/* The embedded fixture image. */
extern const char _binary_aarch64_user_svc_uaccess_elf_start[];
extern const char _binary_aarch64_user_svc_uaccess_elf_end[];

/* The fixture's user layout (see the .S / .ld). */
#define UC_USER_BOUNDARY  UINT64_C(0x00401ff8)  /* 16 B straddle 0x402000 */
#define UC_USER_SEG_END   UINT64_C(0x00402ff8)  /* 16 B cross 0x403000    */
#define UC_USER_UNMAPPED  UINT64_C(0x00403000)
#define UC_USER_CODE      UINT64_C(0x00400000)

/* The pattern the fixture pre-fills at UC_USER_BOUNDARY. */
static const uint8_t uc_pattern[16] = {
    0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
    0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
};

/* The fixture's native exit code (0x2a) and the kernel's encoding. */
#define UC_EXIT_USER     0x2a
#define UC_EXIT_ENCODED  ((uint64_t)UC_EXIT_USER << 8)

/* ── Observations ─────────────────────────────────────────────── */

struct uc_obs {
    /* Frame x19/x20 at the first clock_gettime. */
    bool     frame_seen;
    uint64_t pid_reg;
    uint64_t ppid_reg;
    bool     pid_ok;
    bool     ppid_ok;

    /* The kernel copy self-test. */
    unsigned copy_checks;
    unsigned copy_failures;

    /* The reap observation (PID-filtered, before release). */
    bool     reap_seen;
    bool     reap_self;
    uint64_t reap_pid;
    uint64_t reap_exit;
};

static struct uc_obs g_obs;
static int g_pid = -1;
static uint64_t g_expected_ppid;
static volatile bool g_armed;
static volatile bool g_hook_ran;
static volatile bool g_reaped;

void aarch64_user_svc_uaccess_test_arm(int pid, uint64_t expected_ppid)
{
    g_obs = (struct uc_obs){0};
    g_pid = pid;
    g_expected_ppid = expected_ppid;
    g_hook_ran = false;
    g_reaped = false;
    g_armed = true;
}

/* ── The PID-filtered reap observer ───────────────────────────── */

static void uc_reap_observe(task_t *prev)
{
    if (!g_armed || !prev || (int)prev->pid != g_pid)
        return;
    g_obs.reap_seen = true;
    g_obs.reap_self = (prev->flags & PF_SELF_REAP) != 0;
    g_obs.reap_pid  = (uint64_t)prev->pid;
    g_obs.reap_exit = (uint64_t)prev->exit_code;
    g_reaped = true;
}

/* ── The kernel copy self-test ────────────────────────────────── */

static void uc_note_fault(void *arg)
{
    unsigned *count = (unsigned *)arg;
    (*count)++;
}

static void uc_check(bool ok)
{
    g_obs.copy_checks++;
    if (!ok)
        g_obs.copy_failures++;
}

static void uc_copy_selftest(void)
{
    uint8_t buf[32];
    unsigned faults;
    ssize_t r;

    /* (a) A real copy-in across the two mapped RW pages. */
    memset(buf, 0, sizeof(buf));
    faults = 0;
    r = copy_from_user_ft_res(buf, (const void *)(uintptr_t)UC_USER_BOUNDARY,
                              16, uc_note_fault, &faults);
    uc_check(r == 16 && faults == 0 &&
             memcmp(buf, uc_pattern, sizeof(uc_pattern)) == 0);

    /* (b) Zero length succeeds even for an unmapped pointer and never
     *     invokes the callback. */
    faults = 0;
    r = copy_from_user_ft_res(buf, (const void *)(uintptr_t)UC_USER_UNMAPPED,
                              0, uc_note_fault, &faults);
    uc_check(r == 0 && faults == 0);

    /* (c) A length that cannot be represented as ssize_t is refused
     *     without writing. */
    faults = 0;
    r = copy_from_user_ft_res(buf, (const void *)(uintptr_t)UC_USER_BOUNDARY,
                              ~(size_t)0, uc_note_fault, &faults);
    uc_check(r == -EFAULT);

    /* (d) Read the unmapped page with only the permission precheck
     *     skipped: the real LDTRB aborts and the fixup recovers it. */
    faults = 0;
    r = aarch64_uaccess_test_copy(false, UC_USER_UNMAPPED, buf, 8,
                                  uc_note_fault, &faults);
    uc_check(r == -EFAULT && faults == 1);

    /* (e) Write the read-only code page: the real STTRB aborts. */
    faults = 0;
    r = aarch64_uaccess_test_copy(true, UC_USER_CODE, buf, 8,
                                  uc_note_fault, &faults);
    uc_check(r == -EFAULT && faults == 1);

    /* (f) A successful copy proves the window was cleared after each
     *     abort (a stale window would be refused as nested). */
    memset(buf, 0, sizeof(buf));
    r = copy_from_user_ft_res(buf, (const void *)(uintptr_t)UC_USER_BOUNDARY,
                              16, NULL, NULL);
    uc_check(r == 16 &&
             memcmp(buf, uc_pattern, sizeof(uc_pattern)) == 0);
}

/* ── The SVC hook ─────────────────────────────────────────────── */

void aarch64_user_svc_uaccess_test_hook(struct pt_regs *regs, uint64_t nr)
{
    if (!g_armed || g_hook_ran)
        return;
    /* The observation point is the fixture's first clock_gettime. */
    if (!regs || nr != SYS_clock_gettime)
        return;
    if (!current || (int)current->pid != g_pid)
        return;

    g_obs.frame_seen = true;
    g_obs.pid_reg  = regs->x19;
    g_obs.ppid_reg = regs->x20;
    g_obs.pid_ok   = regs->x19 == (uint64_t)g_pid;
    g_obs.ppid_ok  = regs->x20 == g_expected_ppid;

    uc_copy_selftest();
    g_hook_ran = true;
}

/* ── The case body ────────────────────────────────────────────── */

struct uc_run {
    int rc;
};

static int uc_fail(struct uc_run *run, const char *what)
{
    log_err("[user-svc] FAIL: %s\n", what);
    run->rc = 1;
    return 1;
}

static uint64_t uc_worker(uint64_t arg)
{
    struct uc_run *run = (struct uc_run *)(uintptr_t)arg;

    const size_t size = (size_t)(_binary_aarch64_user_svc_uaccess_elf_end -
                                 _binary_aarch64_user_svc_uaccess_elf_start);
    if (!size)
        return uc_fail(run, "the fixture image is empty"), 0;

    struct task_struct *idle = percpu_data[0].idle;
    if (!idle)
        return uc_fail(run, "no resident BSP idle task"), 0;

    struct aarch64_user_elf_source catalog[1] = {
        { _binary_aarch64_user_svc_uaccess_elf_start, size },
    };

    /* The 4 KiB page baseline: the fixture image is the only page
     * allocation this case makes. */
    uint64_t pmm_before = pmm_probe_4k_allocated_count();

    aarch64_sched_set_reap_observer(uc_reap_observe);

    int pid = aarch64_user_task_spawn(catalog, 1, 0, "user_svc");
    if (pid < 0) {
        aarch64_sched_set_reap_observer(NULL);
        return uc_fail(run, "spawning the fixture task failed"), 0;
    }

    /* The formal task's parent is the resident BSP idle task; the
     * fixture expects getppid() to report exactly that. */
    aarch64_user_svc_uaccess_test_arm(pid, (uint64_t)idle->pid);

    /* Wait for the fixture to exit and be reaped.  The deadline is
     * jiffies-bounded: the scheduler only advances a task's vruntime on
     * a tick. */
    {
        uint64_t deadline = jiffies + 500;
        while (!g_reaped && jiffies < deadline)
            kthread_yield();
    }

    aarch64_sched_set_reap_observer(NULL);
    g_armed = false;

    if (!g_reaped) {
        uc_fail(run, "the fixture task did not exit in time");
        goto out;
    }
    if (!g_obs.frame_seen || !g_obs.pid_ok || !g_obs.ppid_ok) {
        uc_fail(run, "the frame's PID/PPID copy does not match");
        goto out;
    }
    if (g_obs.copy_failures || g_obs.copy_checks < 6) {
        uc_fail(run, "the kernel copy self-test failed");
        goto out;
    }
    if (!g_obs.reap_seen || !g_obs.reap_self ||
        g_obs.reap_exit != UC_EXIT_ENCODED) {
        uc_fail(run, "the fixture exit was not reaped with the encoded status");
        goto out;
    }

    /* Every released page went back to the pool and only the kernel
     * root stays published. */
    if (pmm_probe_4k_allocated_count() != pmm_before) {
        uc_fail(run, "the 4 KiB page count did not return to its baseline");
        goto out;
    }
    if (aarch64_pt_published_root_count() != 1) {
        uc_fail(run, "a user root stayed published");
        goto out;
    }

out:
    return 0;
}

int test_aarch64_user_svc_uaccess(void)
{
    struct uc_run run = {0};
    kernel_thread_on_cpu(uc_worker, (uintptr_t)&run, 0);
    kthread_yield();
    return run.rc;
}

#endif /* AARCH64_EL0_TEST */
