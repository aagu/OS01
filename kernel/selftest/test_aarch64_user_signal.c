/* kernel/selftest/test_aarch64_user_signal.c
 *
 * M4.14 AArch64 native signal + sigreturn acceptance test.
 * The whole TU is test-variant only (#if AARCH64_EL0_TEST).
 */

#if AARCH64_EL0_TEST

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/aarch64/user_task.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_gate.h>
#include <kernel.h>
#include <log/log.h>
#include <memory/pmm.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_sched_observer.h>
#include <selftest/aarch64_user_task.h>
#include <time/timer.h>

extern const uint8_t _binary_aarch64_user_signal_elf_start[];
extern const uint8_t _binary_aarch64_user_signal_elf_end[];

#define USIG_STATUS(code) (((uint64_t)(code)) << 8)
#define USIG_SIGUSR1      10ULL
#define USIG_OBS_GEN      0x4d14U
#define USIG_MAX_REAPS    8

static int g_usig_reap_count;
static uint64_t g_usig_reap_status[USIG_MAX_REAPS];
static int g_usig_reap_pid[USIG_MAX_REAPS];

static void usig_reap_observe(struct task_struct *prev)
{
    if (!prev || prev->state != TASK_ZOMBIE)
        return;
    int idx = __atomic_fetch_add(&g_usig_reap_count, 1, __ATOMIC_RELAXED);
    if (idx < USIG_MAX_REAPS) {
        g_usig_reap_pid[idx] = (int)prev->pid;
        g_usig_reap_status[idx] = (uint64_t)prev->exit_code;
    }
}

static bool usig_saw_status(uint64_t expected_status)
{
    int n = __atomic_load_n(&g_usig_reap_count, __ATOMIC_ACQUIRE);
    if (n > USIG_MAX_REAPS)
        n = USIG_MAX_REAPS;
    for (int i = 0; i < n; i++) {
        if (g_usig_reap_status[i] == expected_status)
            return true;
    }
    return false;
}

static volatile bool g_usig_done;
static volatile int g_usig_rc;

static uint64_t usig_worker(uint64_t arg)
{
    (void)arg;
    const struct aarch64_user_elf_source catalog[1] = {
        {
            .bytes = _binary_aarch64_user_signal_elf_start,
            .size  = (size_t)(_binary_aarch64_user_signal_elf_end -
                              _binary_aarch64_user_signal_elf_start),
            .path  = "/bin/user_signal",
        },
    };

    aarch64_user_task_test_reset_stats();
    __atomic_store_n(&g_usig_reap_count, 0, __ATOMIC_RELEASE);
    memset(g_usig_reap_status, 0, sizeof(g_usig_reap_status));
    memset(g_usig_reap_pid, 0, sizeof(g_usig_reap_pid));

    uint64_t alloc_before = pmm_probe_4k_allocated_count();
    aarch64_sched_observer_begin(0, USIG_OBS_GEN);
    uint64_t created_before = aarch64_sched_observer_created();
    uint64_t stack_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);
    uint64_t thread_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD);

    aarch64_sched_set_reap_observer(usig_reap_observe);

    int rc = 1;
    int pid = aarch64_user_task_spawn(catalog, 1, 0, "user_sig");
    if (pid < 0) {
        log_err("[user-signal-test] spawn failed (%d)\n", pid);
        goto out;
    }

    uint64_t deadline = jiffies + 300;
    while (jiffies < deadline) {
        uint64_t s_rel =
            aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) -
            stack_rel_before;
        uint64_t t_rel =
            aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD) -
            thread_rel_before;
        if (s_rel == 2 && t_rel == 2)
            break;
        kthread_yield();
    }

    uint64_t s_rel =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) -
        stack_rel_before;
    uint64_t t_rel =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD) -
        thread_rel_before;
    uint64_t created_delta = aarch64_sched_observer_created() - created_before;
    int obs_rc = aarch64_sched_observer_end(0, USIG_OBS_GEN);

    struct aarch64_user_task_stats stats;
    aarch64_user_task_test_stats(&stats);

    uint64_t alloc_after = pmm_probe_4k_allocated_count();
    uint64_t roots = aarch64_pt_published_root_count();

    if (s_rel != 2 || t_rel != 2 || created_delta != 4 || obs_rc != 0) {
        log_err("[user-signal-test] resource mismatch: s_rel=%lu t_rel=%lu "
                "created=%lu obs_rc=%d exits=%lu reaps=%d\n",
                (unsigned long)s_rel, (unsigned long)t_rel,
                (unsigned long)created_delta, obs_rc,
                (unsigned long)stats.exits, g_usig_reap_count);
        for (int i = 0; i < g_usig_reap_count && i < USIG_MAX_REAPS; i++) {
            log_err("[user-signal-test] reap[%d] pid=%d status=0x%lx\n",
                    i, g_usig_reap_pid[i],
                    (unsigned long)g_usig_reap_status[i]);
        }
        goto out;
    }
    if (stats.exits != 2) {
        log_err("[user-signal-test] expected 2 exits, got %llu\n",
                (unsigned long long)stats.exits);
        goto out;
    }
    if (!usig_saw_status(USIG_SIGUSR1) || !usig_saw_status(USIG_STATUS(0x66))) {
        log_err("[user-signal-test] missing expected exit status (reaps=%d)\n",
                g_usig_reap_count);
        for (int i = 0; i < g_usig_reap_count && i < USIG_MAX_REAPS; i++) {
            log_err("[user-signal-test] reap[%d] pid=%d status=0x%lx\n",
                    i, g_usig_reap_pid[i],
                    (unsigned long)g_usig_reap_status[i]);
        }
        goto out;
    }
    if (roots != 1 || alloc_after != alloc_before) {
        log_err("[user-signal-test] leak: roots=%llu alloc_before=%llu "
                "alloc_after=%llu\n",
                (unsigned long long)roots,
                (unsigned long long)alloc_before,
                (unsigned long long)alloc_after);
        goto out;
    }

    rc = 0;

out:
    aarch64_sched_set_reap_observer(NULL);
    g_usig_rc = rc;
    g_usig_done = true;
    return (uint64_t)rc;
}

int test_aarch64_user_signal(void)
{
    g_usig_done = false;
    g_usig_rc = 1;
    kernel_thread_on_cpu(usig_worker, 0, 0);

    uint64_t deadline = jiffies + 8000;
    while (!g_usig_done && jiffies < deadline)
        kthread_yield();

    return g_usig_done ? g_usig_rc : 1;
}

#endif /* AARCH64_EL0_TEST */
