/* kernel/selftest/test_aarch64_user_exec.c
 *
 * M4.13 AArch64 SYS_exec + user stack argument marshaling acceptance test.
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

extern const uint8_t _binary_aarch64_user_exec_main_elf_start[];
extern const uint8_t _binary_aarch64_user_exec_main_elf_end[];
extern const uint8_t _binary_aarch64_user_exec_child_elf_start[];
extern const uint8_t _binary_aarch64_user_exec_child_elf_end[];

#define UE_STATUS(code) (((uint64_t)(code)) << 8)
#define UE_OBS_GEN      0x4d13U
#define UE_MAX_REAPS    8

static int g_ue_reap_count;
static uint64_t g_ue_reap_status[UE_MAX_REAPS];
static int g_ue_reap_pid[UE_MAX_REAPS];

static void ue_reap_observe(struct task_struct *prev)
{
    if (!prev || prev->state != TASK_ZOMBIE)
        return;
    int idx = __atomic_fetch_add(&g_ue_reap_count, 1, __ATOMIC_RELAXED);
    if (idx < UE_MAX_REAPS) {
        g_ue_reap_pid[idx] = (int)prev->pid;
        g_ue_reap_status[idx] = (uint64_t)prev->exit_code;
    }
}

static bool ue_saw_status(uint64_t expected_status)
{
    int n = __atomic_load_n(&g_ue_reap_count, __ATOMIC_ACQUIRE);
    if (n > UE_MAX_REAPS)
        n = UE_MAX_REAPS;
    for (int i = 0; i < n; i++) {
        if (g_ue_reap_status[i] == expected_status)
            return true;
    }
    return false;
}

static volatile bool g_ue_done;
static volatile int g_ue_rc;

static uint64_t ue_worker(uint64_t arg)
{
    (void)arg;
    const struct aarch64_user_elf_source catalog[2] = {
        {
            .bytes = _binary_aarch64_user_exec_main_elf_start,
            .size  = (size_t)(_binary_aarch64_user_exec_main_elf_end -
                              _binary_aarch64_user_exec_main_elf_start),
            .path  = "/bin/exec_main",
        },
        {
            .bytes = _binary_aarch64_user_exec_child_elf_start,
            .size  = (size_t)(_binary_aarch64_user_exec_child_elf_end -
                              _binary_aarch64_user_exec_child_elf_start),
            .path  = "/bin/exec_child",
        },
    };

    aarch64_user_task_test_reset_stats();
    __atomic_store_n(&g_ue_reap_count, 0, __ATOMIC_RELEASE);
    memset(g_ue_reap_status, 0, sizeof(g_ue_reap_status));
    memset(g_ue_reap_pid, 0, sizeof(g_ue_reap_pid));

    uint64_t alloc_before = pmm_probe_4k_allocated_count();
    aarch64_sched_observer_begin(0, UE_OBS_GEN);
    uint64_t created_before = aarch64_sched_observer_created();
    uint64_t stack_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);
    uint64_t thread_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD);

    aarch64_sched_set_reap_observer(ue_reap_observe);

    int rc = 1;
    int pid = aarch64_user_task_spawn(catalog, 2, 0, "user_exec");
    if (pid < 0) {
        log_err("[user-exec-test] spawn failed (%d)\n", pid);
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
    int obs_rc = aarch64_sched_observer_end(0, UE_OBS_GEN);

    struct aarch64_user_task_stats stats;
    aarch64_user_task_test_stats(&stats);

    uint64_t alloc_after = pmm_probe_4k_allocated_count();
    uint64_t roots = aarch64_pt_published_root_count();

    if (s_rel != 2 || t_rel != 2 || created_delta != 4 || obs_rc != 0) {
        log_err("[user-exec-test] resource mismatch: s_rel=%lu t_rel=%lu "
                "created=%lu obs_rc=%d exits=%lu reaps=%d\n",
                (unsigned long)s_rel, (unsigned long)t_rel,
                (unsigned long)created_delta, obs_rc,
                (unsigned long)stats.exits, g_ue_reap_count);
        for (int i = 0; i < g_ue_reap_count && i < UE_MAX_REAPS; i++) {
            log_err("[user-exec-test] reap[%d] pid=%d status=0x%lx\n",
                    i, g_ue_reap_pid[i],
                    (unsigned long)g_ue_reap_status[i]);
        }
        goto out;
    }
    if (stats.exits != 2) {
        log_err("[user-exec-test] expected 2 exits, got %llu\n",
                (unsigned long long)stats.exits);
        goto out;
    }
    if (!ue_saw_status(UE_STATUS(0x2a)) || !ue_saw_status(UE_STATUS(0x55))) {
        log_err("[user-exec-test] missing expected exit status (reaps=%d)\n",
                g_ue_reap_count);
        for (int i = 0; i < g_ue_reap_count && i < UE_MAX_REAPS; i++) {
            log_err("[user-exec-test] reap[%d] pid=%d status=0x%lx\n",
                    i, g_ue_reap_pid[i],
                    (unsigned long)g_ue_reap_status[i]);
        }
        goto out;
    }
    if (roots != 1 || alloc_after != alloc_before) {
        log_err("[user-exec-test] leak: roots=%llu alloc_before=%llu "
                "alloc_after=%llu\n",
                (unsigned long long)roots,
                (unsigned long long)alloc_before,
                (unsigned long long)alloc_after);
        goto out;
    }

    rc = 0;

out:
    aarch64_sched_set_reap_observer(NULL);
    g_ue_rc = rc;
    g_ue_done = true;
    return (uint64_t)rc;
}

int test_aarch64_user_exec(void)
{
    g_ue_done = false;
    g_ue_rc = 1;
    kernel_thread_on_cpu(ue_worker, 0, 0);

    uint64_t deadline = jiffies + 8000;
    while (!g_ue_done && jiffies < deadline)
        kthread_yield();

    return g_ue_done ? g_ue_rc : 1;
}

#endif /* AARCH64_EL0_TEST */
