/* kernel/selftest/boot_services_fixture.c — BOOT-5 overlap observation fixture.
 *
 * Implements real FS mutex observation during boot creation window:
 *   - Armed by BSP before spawn_user_task().
 *   - In fs lock wrappers (vfs_ns_lock, fat_fs_lock, ext2_fs_lock), after
 *     successfully acquiring mutex, if armed, PID==1 and CPU!=0, captures:
 *       pid=1, cpu, bootstrap=false, locked=true, bsp_scheduler_ok=0, window=open
 *     and atomically publishes the observation with release semantics.
 *     Never prints or waits while holding the FS mutex!
 *   - BSP in boot5_test_after_init() waits bounded time (jiffies + 200, 2s)
 *     for the observation to be published, verifies the snapshot, and prints
 *     the required observation marker.
 */

#include <selftest/boot_services_fixture.h>
#include <arch/cpu.h>
#include <core/boot_failure.h>
#include <fs/vfs.h>
#include <log/log.h>
#include <percpu/percpu.h>
#include <sched/task.h>
#include <sched/boot.h>
#include <time/timer.h>

static uint32_t s_armed = 0;
static uint32_t s_observed = 0;

static uint64_t s_snap_pid = 0;
static uint32_t s_snap_cpu = 0;
static uint32_t s_snap_bootstrap = 0;
static uint32_t s_snap_locked = 0;
static uint32_t s_snap_bsp_sched_ok = 0;
static uint32_t s_snap_window_open = 0;

void boot5_test_arm(void)
{
    log_info("[boot-5-test] variant=boot-services-overlap\n");
    __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
}

void boot5_test_fs_locked(void)
{
    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (__atomic_load_n(&s_observed, __ATOMIC_ACQUIRE)) {
        return;
    }

    task_t *curr = get_current_task();
    if (!curr || curr->pid != 1) {
        return;
    }

    uint32_t cpu = cpu_id();
    if (cpu == 0) {
        return;
    }

    /* Record snapshot while holding the FS mutex */
    s_snap_pid = curr->pid;
    s_snap_cpu = cpu;
    s_snap_bootstrap = vfs_bootstrap_active() ? 1 : 0;
    s_snap_locked = 1;
    s_snap_bsp_sched_ok = __atomic_load_n(&percpu_data[0].scheduler_ok, __ATOMIC_ACQUIRE);
    s_snap_window_open = sched_boot_creation_open() ? 1 : 0;

    __atomic_store_n(&s_observed, 1, __ATOMIC_RELEASE);
}

void boot5_test_after_init(void)
{
    uint64_t deadline = jiffies + 200; /* 2.0 seconds at 100 Hz */
    while (!__atomic_load_n(&s_observed, __ATOMIC_ACQUIRE)) {
        if (jiffies >= deadline) {
            boot_failure_stop("boot5_fixture", "overlap_timeout", -110 /* -ETIMEDOUT */, -1, -1);
        }
        arch_cpu_pause();
    }

    /* Acquire snapshot data */
    if (s_snap_pid != 1 || s_snap_cpu == 0 || s_snap_bootstrap != 0 ||
        s_snap_locked != 1 || s_snap_bsp_sched_ok != 0 || s_snap_window_open != 1) {
        boot_failure_stop("boot5_fixture", "invalid_snapshot", -22 /* -EINVAL */, -1, -1);
    }

    log_info("[boot-5-test] overlap pid=1 cpu=%u bootstrap=0 locked=1 bsp_scheduler_ok=0 window=open\n",
             s_snap_cpu);
}
