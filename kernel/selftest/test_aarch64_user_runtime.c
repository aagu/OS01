/* kernel/selftest/test_aarch64_user_runtime.c
 *
 * AArch64 formal EL0 user-runtime acceptance suite (case=irq).
 * Compiled ONLY under AARCH64_USER_RUNTIME_TEST.
 */

#ifdef AARCH64_USER_RUNTIME_TEST

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arch/aarch64/page_table.h>
#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/vmm_gate.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <kernel.h>
#include <log/log.h>
#include <memory/pmm.h>
#include <sched/internal.h>
#include <sched/task.h>
#include <selftest/aarch64_sched_observer.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_user_runtime.h>
#include <selftest/result.h>
#include <time/timer.h>

extern const uint8_t _binary_aarch64_user_runtime_elf_start[];
extern const uint8_t _binary_aarch64_user_runtime_elf_end[];

#define URT_OBS_GEN   0x55525401U
#define URT_SIGUSR1   10
#define URT_MAX_REAPS 8

static volatile bool g_urt_active;
static int g_pid_a = -1;
static int g_pid_b = -1;

static uint64_t g_eoi_samples[2];
static uint64_t g_irq_switch_resume_samples[2];
static uint64_t g_switch_seq;

static volatile int g_a_edit_paused;
static volatile int g_b_sig_injected;
static volatile int g_b_sigreturn_done;
static bool g_vm_pause_done;
static bool g_a_paused_root_ok;
static bool g_a_post_edit_checked;
static bool g_a_post_edit_ok;
static task_t *g_a_paused_task;
static uint64_t g_a_saved_root_pa;
static uint64_t g_a_saved_tpidr;

static int g_reap_count;
static int g_reap_pid[URT_MAX_REAPS];
static uint64_t g_reap_status[URT_MAX_REAPS];

static int urt_role_index(const task_t *tsk)
{
    if (!tsk || !aarch64_user_task_is_user(tsk))
        return -1;
    if ((int)tsk->pid == g_pid_a)
        return 0;
    if (g_pid_b < 0)
        g_pid_b = (int)tsk->pid;
    if ((int)tsk->pid == g_pid_b)
        return 1;
    return -1;
}

static bool urt_a_resume_cond(struct task_struct *t)
{
    (void)t;
    return __atomic_load_n(&g_b_sigreturn_done, __ATOMIC_ACQUIRE) != 0;
}

void aarch64_user_runtime_test_note_switch(const struct task_struct *prev,
                                           const struct task_struct *next)
{
    if (!g_urt_active || prev == next)
        return;
    __atomic_fetch_add(&g_switch_seq, 1, __ATOMIC_RELEASE);
}

void aarch64_user_runtime_test_timer_eoi(uint32_t intid, struct pt_regs *regs)
{
    if (!g_urt_active || intid != 30u)
        return;
    if (!aarch64_user_frame_is_valid(regs))
        return;

    task_t *cur = arch_current_task();
    int idx = urt_role_index(cur);
    if (idx < 0)
        return;

    __atomic_fetch_add(&g_eoi_samples[idx], 1, __ATOMIC_RELAXED);

    if (idx == 1 &&
        __atomic_load_n(&g_a_edit_paused, __ATOMIC_ACQUIRE) != 0 &&
        __atomic_load_n(&g_b_sig_injected, __ATOMIC_ACQUIRE) == 0) {
        __atomic_store_n(&g_b_sig_injected, 1, __ATOMIC_RELEASE);
        __atomic_fetch_or(&cur->signal, (1ULL << URT_SIGUSR1), __ATOMIC_RELEASE);
    }
}

void aarch64_user_runtime_test_irq_sched_before(struct task_struct *tsk,
                                                const struct pt_regs *regs,
                                                uint64_t *switches_out,
                                                uint64_t *elr_out)
{
    if (switches_out)
        *switches_out = __atomic_load_n(&g_switch_seq, __ATOMIC_ACQUIRE);
    if (elr_out)
        *elr_out = regs ? regs->elr_el1 : 0;
    (void)tsk;
}

void aarch64_user_runtime_test_irq_sched_after(struct task_struct *tsk,
                                               const struct pt_regs *regs,
                                               uint64_t switches_before,
                                               uint64_t elr_before)
{
    if (!g_urt_active || !tsk || !regs)
        return;
    if (arch_current_task() != tsk || regs->elr_el1 != elr_before)
        return;
    if (!aarch64_user_frame_is_valid(regs))
        return;

    uint64_t switches_after = __atomic_load_n(&g_switch_seq, __ATOMIC_ACQUIRE);
    if (switches_after <= switches_before)
        return;

    int idx = urt_role_index(tsk);
    if (idx >= 0)
        __atomic_fetch_add(&g_irq_switch_resume_samples[idx], 1,
                           __ATOMIC_RELAXED);
}

void aarch64_user_runtime_test_vm_edit_pause(struct mm_struct *mm)
{
    (void)mm;
    if (!g_urt_active)
        return;

    task_t *cur = arch_current_task();
    if (!cur || (int)cur->pid != g_pid_a || g_vm_pause_done)
        return;

    struct aarch64_user_task *owner = aarch64_user_task_owner(cur);
    if (!owner || !owner->image || !owner->vm.edit_active)
        return;

    g_vm_pause_done = true;
    g_a_paused_task = cur;
    g_a_saved_root_pa = aarch64_user_image_root_pa(owner->image);
    g_a_saved_tpidr = owner->vm.edit_tpidr_el0;
    __atomic_store_n(&g_a_edit_paused, 1, __ATOMIC_RELEASE);

    if (blocker_wait(urt_a_resume_cond, BLOCKER_NANOSLEEP, false) != 0)
        return;

    uint64_t live_root = aarch64_user_vm_read_ttbr0();
    uint64_t tpidr = 0, tpidrro = 0;
    aarch64_user_vm_save_tls(&tpidr, &tpidrro);
    if (live_root != 0 && live_root != g_a_saved_root_pa &&
        owner->vm.edit_active && tpidr == 0) {
        g_a_paused_root_ok = true;
    }
}

void aarch64_user_runtime_test_vm_edit_after(struct mm_struct *mm)
{
    (void)mm;
    if (!g_urt_active || !g_a_paused_root_ok || g_a_post_edit_checked)
        return;

    task_t *cur = arch_current_task();
    if (!cur || (int)cur->pid != g_pid_a)
        return;

    g_a_post_edit_checked = true;
    struct aarch64_user_task *owner = aarch64_user_task_owner(cur);
    uint64_t live_root = aarch64_user_vm_read_ttbr0();
    uint64_t tpidr = 0, tpidrro = 0;
    aarch64_user_vm_save_tls(&tpidr, &tpidrro);
    if (owner && !owner->vm.edit_active &&
        live_root == g_a_saved_root_pa &&
        tpidr == g_a_saved_tpidr) {
        g_a_post_edit_ok = true;
    }
}

void aarch64_user_runtime_test_sigreturn(struct task_struct *tsk)
{
    if (!g_urt_active || !tsk)
        return;
    if ((int)tsk->pid == g_pid_a)
        return;
    if (__atomic_load_n(&g_a_edit_paused, __ATOMIC_ACQUIRE) == 0)
        return;
    if (__atomic_exchange_n(&g_b_sigreturn_done, 1, __ATOMIC_ACQ_REL) == 0) {
        if (g_a_paused_task)
            blocker_wake(g_a_paused_task);
    }
}

static void urt_reap_observe(struct task_struct *prev)
{
    if (!prev || prev->state != TASK_ZOMBIE)
        return;
    int idx = __atomic_fetch_add(&g_reap_count, 1, __ATOMIC_RELAXED);
    if (idx < URT_MAX_REAPS) {
        g_reap_pid[idx] = (int)prev->pid;
        g_reap_status[idx] = (uint64_t)prev->exit_code;
    }
}

static bool urt_saw_status(uint64_t expected_status)
{
    int n = __atomic_load_n(&g_reap_count, __ATOMIC_ACQUIRE);
    if (n > URT_MAX_REAPS)
        n = URT_MAX_REAPS;
    for (int i = 0; i < n; i++) {
        if (g_reap_status[i] == expected_status)
            return true;
    }
    return false;
}

static volatile bool g_urt_done;
static volatile int g_urt_rc;

static uint64_t urt_worker(uint64_t arg)
{
    (void)arg;
    const struct aarch64_user_elf_source catalog[1] = {
        {
            .bytes = _binary_aarch64_user_runtime_elf_start,
            .size  = (size_t)(_binary_aarch64_user_runtime_elf_end -
                              _binary_aarch64_user_runtime_elf_start),
            .path  = "/bin/user_runtime",
        },
    };

    g_pid_a = -1;
    g_pid_b = -1;
    g_eoi_samples[0] = 0;
    g_eoi_samples[1] = 0;
    g_irq_switch_resume_samples[0] = 0;
    g_irq_switch_resume_samples[1] = 0;
    __atomic_store_n(&g_switch_seq, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_a_edit_paused, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b_sig_injected, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_b_sigreturn_done, 0, __ATOMIC_RELEASE);
    g_vm_pause_done = false;
    g_a_paused_root_ok = false;
    g_a_post_edit_checked = false;
    g_a_post_edit_ok = false;
    g_a_paused_task = NULL;
    g_a_saved_root_pa = 0;
    g_a_saved_tpidr = 0;
    __atomic_store_n(&g_reap_count, 0, __ATOMIC_RELEASE);
    memset(g_reap_pid, 0, sizeof(g_reap_pid));
    memset(g_reap_status, 0, sizeof(g_reap_status));

    uint64_t alloc_before = pmm_probe_4k_allocated_count();
    aarch64_sched_observer_begin(0, URT_OBS_GEN);
    uint64_t stack_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);
    uint64_t thread_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD);

    aarch64_sched_set_reap_observer(urt_reap_observe);
    g_urt_active = true;

    int rc = 1;
    g_pid_a = aarch64_user_task_spawn(catalog, 1, 0, "user_rt_a");
    if (g_pid_a < 0) {
        log_err("[user-runtime] FAIL case=irq reason=spawn_failed rc=%d\n",
                g_pid_a);
        goto out;
    }

    uint64_t deadline = jiffies + 1500;
    while (jiffies < deadline) {
        uint64_t s_rel =
            aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) -
            stack_rel_before;
        uint64_t t_rel =
            aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD) -
            thread_rel_before;
        if (s_rel >= 2 && t_rel >= 2)
            break;
        kthread_yield();
    }

    g_urt_active = false;
    uint64_t s_rel =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) -
        stack_rel_before;
    uint64_t t_rel =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD) -
        thread_rel_before;
    int obs_rc = aarch64_sched_observer_end(0, URT_OBS_GEN);
    uint64_t alloc_after = pmm_probe_4k_allocated_count();
    uint64_t roots = aarch64_pt_published_root_count();

    if (s_rel != 2 || t_rel != 2 || obs_rc != 0) {
        log_err("[user-runtime] FAIL case=irq reason=resource_mismatch "
                "s_rel=%lu t_rel=%lu obs_rc=%d\n",
                (unsigned long)s_rel, (unsigned long)t_rel, obs_rc);
        goto out;
    }
    if (!urt_saw_status(0x11ULL << 8) || !urt_saw_status(0x22ULL << 8)) {
        log_err("[user-runtime] FAIL case=irq reason=exit_status_mismatch "
                "reaps=%d\n", g_reap_count);
        for (int i = 0; i < g_reap_count && i < URT_MAX_REAPS; i++) {
            log_err("[user-runtime] reap[%d] pid=%d status=0x%lx\n",
                    i, g_reap_pid[i], (unsigned long)g_reap_status[i]);
        }
        goto out;
    }
    if (g_eoi_samples[0] == 0 || g_eoi_samples[1] == 0 ||
        g_irq_switch_resume_samples[0] == 0 ||
        g_irq_switch_resume_samples[1] == 0) {
        log_err("[user-runtime] FAIL case=irq reason=irq_samples "
                "eoi_a=%lu eoi_b=%lu sw_a=%lu sw_b=%lu\n",
                (unsigned long)g_eoi_samples[0],
                (unsigned long)g_eoi_samples[1],
                (unsigned long)g_irq_switch_resume_samples[0],
                (unsigned long)g_irq_switch_resume_samples[1]);
        goto out;
    }
    if (!g_a_paused_root_ok || !g_a_post_edit_ok ||
        !__atomic_load_n(&g_b_sig_injected, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&g_b_sigreturn_done, __ATOMIC_ACQUIRE)) {
        log_err("[user-runtime] FAIL case=irq reason=vm_sig_mismatch "
                "paused=%d post=%d sig_inj=%d sigret=%d\n",
                g_a_paused_root_ok ? 1 : 0, g_a_post_edit_ok ? 1 : 0,
                g_b_sig_injected, g_b_sigreturn_done);
        goto out;
    }
    if (roots != 1 || alloc_after != alloc_before) {
        log_err("[user-runtime] FAIL case=irq reason=leak roots=%lu "
                "alloc_before=%lu alloc_after=%lu\n",
                (unsigned long)roots,
                (unsigned long)alloc_before,
                (unsigned long)alloc_after);
        goto out;
    }

    log_info("[user-runtime] irq-samples eoi_a=%lu eoi_b=%lu "
             "switch_resume_a=%lu switch_resume_b=%lu\n",
             (unsigned long)g_eoi_samples[0],
             (unsigned long)g_eoi_samples[1],
             (unsigned long)g_irq_switch_resume_samples[0],
             (unsigned long)g_irq_switch_resume_samples[1]);
    log_info("[user-runtime] PASS case=irq cpu_mask=1\n");
    rc = 0;

out:
    g_urt_active = false;
    aarch64_sched_set_reap_observer(NULL);
    g_urt_rc = rc;
    g_urt_done = true;
    return (uint64_t)rc;
}

void test_aarch64_user_runtime_run(void)
{
    g_urt_done = false;
    g_urt_rc = 1;
    kernel_thread_on_cpu(urt_worker, 0, 0);

    uint64_t deadline = jiffies + 3000;
    while (!g_urt_done && jiffies < deadline)
        kthread_yield();

    int rc = g_urt_done ? g_urt_rc : 1;
    selftest_record_late("aarch64_user_runtime_irq", rc);
}

#endif /* AARCH64_USER_RUNTIME_TEST */
