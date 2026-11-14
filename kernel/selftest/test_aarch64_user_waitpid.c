/* kernel/selftest/test_aarch64_user_waitpid.c
 *
 * M4.12 AArch64 SYS_waitpid + ZOMBIE / orphan reaping acceptance test.
 * The whole TU is test-variant only (#if AARCH64_EL0_TEST).
 */

#if AARCH64_EL0_TEST

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/aarch64/user_task.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/vmm_gate.h>
#include <selftest/aarch64_user_task.h>
#include <kernel.h>
#include <log/log.h>
#include <memory/pmm.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_sched_observer.h>
#include <time/timer.h>

extern const uint8_t _binary_aarch64_user_waitpid_elf_start[];
extern const uint8_t _binary_aarch64_user_waitpid_elf_end[];

#define WP_STATUS(code) (((uint64_t)(code)) << 8)
#define WP_OBS_GEN      0x4d12U
#define WP_MAX_REAPS    16

static int g_wp_reap_count;
static uint64_t g_wp_reap_status[WP_MAX_REAPS];
static int g_wp_reap_pid[WP_MAX_REAPS];

static void wp_reap_observe(struct task_struct *prev)
{
    if (!prev || prev->state != TASK_ZOMBIE)
        return;
    int idx = __atomic_fetch_add(&g_wp_reap_count, 1, __ATOMIC_RELAXED);
    if (idx < WP_MAX_REAPS) {
        g_wp_reap_pid[idx] = (int)prev->pid;
        g_wp_reap_status[idx] = (uint64_t)prev->exit_code;
    }
}

static bool wp_saw_status(uint64_t expected_status)
{
    int n = __atomic_load_n(&g_wp_reap_count, __ATOMIC_ACQUIRE);
    if (n > WP_MAX_REAPS)
        n = WP_MAX_REAPS;
    for (int i = 0; i < n; i++) {
        if (g_wp_reap_status[i] == expected_status)
            return true;
    }
    return false;
}

static volatile bool g_wp_done;
static volatile int g_wp_rc;

static uint64_t wp_worker(uint64_t arg)
{
    (void)arg;
    const struct aarch64_user_elf_source catalog[1] = {
        { _binary_aarch64_user_waitpid_elf_start,
          (size_t)(_binary_aarch64_user_waitpid_elf_end -
                   _binary_aarch64_user_waitpid_elf_start) },
    };

    aarch64_user_task_test_reset_stats();
    __atomic_store_n(&g_wp_reap_count, 0, __ATOMIC_RELEASE);
    memset(g_wp_reap_status, 0, sizeof(g_wp_reap_status));
    memset(g_wp_reap_pid, 0, sizeof(g_wp_reap_pid));

    uint64_t alloc_before = pmm_probe_4k_allocated_count();
    aarch64_sched_observer_begin(0, WP_OBS_GEN);
    uint64_t created_before = aarch64_sched_observer_created();
    uint64_t stack_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);
    uint64_t thread_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD);

    aarch64_sched_set_reap_observer(wp_reap_observe);

    int rc = 1;
    int pid = aarch64_user_task_spawn(catalog, 1, 0, "user_waitpid");
    if (pid < 0) {
        log_err("[waitpid-test] spawn failed (%d)\n", pid);
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
        if (s_rel == 7 && t_rel == 7)
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
    int obs_rc = aarch64_sched_observer_end(0, WP_OBS_GEN);

    struct aarch64_user_task_stats stats;
    aarch64_user_task_test_stats(&stats);

    uint64_t alloc_after = pmm_probe_4k_allocated_count();
    uint64_t roots = aarch64_pt_published_root_count();

    if (s_rel != 7 || t_rel != 7 || created_delta != 14 || obs_rc != 0) {
        log_err("[waitpid-test] resource mismatch: s_rel=%lu t_rel=%lu "
                "created=%lu obs_rc=%d exits=%lu reaps=%d\n",
                (unsigned long)s_rel, (unsigned long)t_rel,
                (unsigned long)created_delta, obs_rc,
                (unsigned long)stats.exits, g_wp_reap_count);
        for (int i = 0; i < g_wp_reap_count && i < WP_MAX_REAPS; i++) {
            log_err("[waitpid-test] reap[%d] pid=%d status=0x%lx\n",
                    i, g_wp_reap_pid[i],
                    (unsigned long)g_wp_reap_status[i]);
        }
        goto out;
    }
    if (stats.exits != 7) {
        log_err("[waitpid-test] expected 7 exits, got %llu\n",
                (unsigned long long)stats.exits);
        goto out;
    }
    if (!wp_saw_status(WP_STATUS(0x66)) ||
        !wp_saw_status(WP_STATUS(0x2a)) ||
        !wp_saw_status(WP_STATUS(0x11)) ||
        !wp_saw_status(WP_STATUS(0x22)) ||
        !wp_saw_status(WP_STATUS(0x55)) ||
        !wp_saw_status(WP_STATUS(0x33)) ||
        !wp_saw_status(WP_STATUS(0x44))) {
        log_err("[waitpid-test] missing expected exit status (reaps=%d)\n",
                g_wp_reap_count);
        goto out;
    }
    if (roots != 1 || alloc_after != alloc_before) {
        log_err("[waitpid-test] leak: roots=%llu alloc_before=%llu "
                "alloc_after=%llu\n",
                (unsigned long long)roots,
                (unsigned long long)alloc_before,
                (unsigned long long)alloc_after);
        goto out;
    }

    rc = 0;

out:
    aarch64_sched_set_reap_observer(NULL);
    g_wp_rc = rc;
    g_wp_done = true;
    return (uint64_t)rc;
}

int test_aarch64_user_waitpid(void)
{
    g_wp_done = false;
    g_wp_rc = 1;
    kernel_thread_on_cpu(wp_worker, 0, 0);

    uint64_t deadline = jiffies + 8000;
    while (!g_wp_done && jiffies < deadline)
        kthread_yield();

    return g_wp_done ? g_wp_rc : 1;
}

#endif /* AARCH64_EL0_TEST */
