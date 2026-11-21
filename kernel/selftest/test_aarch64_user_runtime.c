/* kernel/selftest/test_aarch64_user_runtime.c
 *
 * AArch64 formal EL0 user-runtime acceptance suite (case=irq).
 * Compiled ONLY under AARCH64_USER_RUNTIME_TEST.
 */

#ifdef AARCH64_USER_RUNTIME_TEST

#include <stdbool.h>
#ifdef AARCH64_USER_RUNTIME_CASE_MIGRATION
#include <selftest/aarch64_user_runtime.h>
void test_aarch64_user_runtime_run(void)
{
    test_aarch64_user_migration_run();
}
#else
#include <stdint.h>
#include <string.h>

#include <arch/aarch64/page_table.h>
#include <arch/aarch64/user_image.h>
#include <arch/aarch64/user_mm.h>
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
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
extern const uint8_t _binary_aarch64_user_runtime_comp_elf_start[];
extern const uint8_t _binary_aarch64_user_runtime_comp_elf_end[];
#endif

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
static bool g_a_post_edit_ok __attribute__((unused));
static task_t *g_a_paused_task;
static uint64_t g_a_saved_root_pa;
static uint64_t g_a_saved_tpidr;

static int g_reap_count;
static int g_reap_pid[URT_MAX_REAPS];
static uint64_t g_reap_status[URT_MAX_REAPS];

static uint8_t g_asid_a;
static uint8_t g_asid_b;
static uint64_t g_clean_switches;
static uint64_t g_full_flushes;
static uint64_t g_event_seq;
static uint64_t g_uninstall_seq[256];
static uint64_t g_tlbi_seq[256];
static uint64_t g_release_seq[256];

#ifdef AARCH64_USER_RUNTIME_CASE_SMP
static int g_pid_comp0 = -1;
static int g_pid_comp1 = -1;
static int g_pid_child = -1;
static int g_child_cpu = -1;
static task_t *g_parent_task;
static uint64_t g_smp_cpu_eoi[2];
static uint64_t g_smp_cpu_sw[2];
static volatile uint32_t g_cow_arrived_mask;
static uint64_t g_cow_test_old_pa;
static uint64_t g_cow_last_free_count;
static int g_waitpid_blocked_ok;
static uint32_t g_comp_first_cpu[2];
static uint64_t g_comp_first_mask[2];
static bool g_comp_first_seen[2];
#endif

void aarch64_user_runtime_test_explicit_pin_el0(const struct task_struct *tsk)
{
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    if (!g_urt_active || !tsk)
        return;
    int idx = -1;
    if ((int)tsk->pid == g_pid_comp0)
        idx = 0;
    else if ((int)tsk->pid == g_pid_comp1)
        idx = 1;
    if (idx >= 0 && !g_comp_first_seen[idx]) {
        g_comp_first_cpu[idx] = cpu_id();
        g_comp_first_mask[idx] = tsk->allowed_cpus;
        __atomic_store_n(&g_comp_first_seen[idx], true, __ATOMIC_RELEASE);
    }
#else
    (void)tsk;
#endif
}

bool aarch64_user_runtime_test_migration_veto(const struct task_struct *tsk)
{
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    return g_urt_active && tsk && g_pid_child >= 0 &&
           (int)tsk->pid == g_pid_child;
#else
    (void)tsk;
    return false;
#endif
}

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

#ifdef AARCH64_USER_RUNTIME_CASE_IRQ
static bool urt_a_resume_cond(struct task_struct *t)
{
    (void)t;
    return __atomic_load_n(&g_b_sigreturn_done, __ATOMIC_ACQUIRE) != 0;
}
#endif

void aarch64_user_runtime_test_note_switch(const struct task_struct *prev,
                                           const struct task_struct *next)
{
    if (!g_urt_active || prev == next)
        return;
    __atomic_fetch_add(&g_switch_seq, 1, __ATOMIC_RELEASE);
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    if (aarch64_user_task_is_user(next)) {
        int npid = (int)next->pid;
        if (npid == g_pid_a && !g_parent_task)
            __atomic_store_n(&g_parent_task, (task_t *)next, __ATOMIC_RELEASE);
        if (npid != g_pid_a && npid != g_pid_comp0 && npid != g_pid_comp1) {
            if (g_pid_child < 0) {
                g_pid_child = npid;
                __atomic_store_n(&g_child_cpu, (int)next->cpu, __ATOMIC_RELEASE);
            }
        }
    }
    if (aarch64_user_task_is_user(prev) && aarch64_user_task_is_user(next) &&
        prev->pid != next->pid) {
        uint32_t c = cpu_id();
        if (c < 2)
            __atomic_fetch_add(&g_smp_cpu_sw[c], 1, __ATOMIC_RELAXED);
    }
#endif
}

void aarch64_user_runtime_test_note_ttbr_switch(uint64_t old_ttbr0,
                                                uint64_t new_ttbr0,
                                                bool full_flush)
{
    if (!g_urt_active)
        return;

    uint8_t old_asid = (uint8_t)(old_ttbr0 >> 48);
    uint8_t new_asid = (uint8_t)(new_ttbr0 >> 48);

    if (old_asid != 0 && old_asid != new_asid) {
        uint64_t seq = __atomic_add_fetch(&g_event_seq, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&g_uninstall_seq[old_asid], seq, __ATOMIC_RELEASE);
    }

    if (new_asid != 0) {
        task_t *cur = arch_current_task();
        int idx = urt_role_index(cur);
        if (idx == 0 && g_asid_a == 0)
            g_asid_a = new_asid;
        else if (idx == 1 && g_asid_b == 0)
            g_asid_b = new_asid;
    }

    if (full_flush)
        __atomic_fetch_add(&g_full_flushes, 1, __ATOMIC_RELAXED);
    else if (old_asid != 0 || new_asid != 0)
        __atomic_fetch_add(&g_clean_switches, 1, __ATOMIC_RELAXED);
}

void aarch64_user_runtime_test_note_asid_tlbi(uint8_t asid)
{
    if (!g_urt_active || asid == 0)
        return;
    uint64_t seq = __atomic_add_fetch(&g_event_seq, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_tlbi_seq[asid], seq, __ATOMIC_RELEASE);
}

void aarch64_user_runtime_test_note_asid_release(uint8_t asid)
{
    if (!g_urt_active || asid == 0)
        return;
    uint64_t seq = __atomic_add_fetch(&g_event_seq, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_release_seq[asid], seq, __ATOMIC_RELEASE);
}

void aarch64_user_runtime_test_timer_eoi(uint32_t intid, struct pt_regs *regs)
{
    if (!g_urt_active || intid != 30u)
        return;
    if (!aarch64_user_frame_is_valid(regs))
        return;

#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    uint32_t c = cpu_id();
    if (c < 2)
        __atomic_fetch_add(&g_smp_cpu_eoi[c], 1, __ATOMIC_RELAXED);
    return;
#endif

    task_t *cur = arch_current_task();
    int idx = urt_role_index(cur);
    if (idx < 0)
        return;

    __atomic_fetch_add(&g_eoi_samples[idx], 1, __ATOMIC_RELAXED);

#ifdef AARCH64_USER_RUNTIME_CASE_IRQ
    if (idx == 1 &&
        __atomic_load_n(&g_a_edit_paused, __ATOMIC_ACQUIRE) != 0 &&
        __atomic_load_n(&g_b_sig_injected, __ATOMIC_ACQUIRE) == 0) {
        __atomic_store_n(&g_b_sig_injected, 1, __ATOMIC_RELEASE);
        __atomic_fetch_or(&cur->signal, (1ULL << URT_SIGUSR1), __ATOMIC_RELEASE);
    }
#endif
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
    if (tsk->irq_return_migratable) {
        log_err("irq_sched_after: irq_return_migratable left set on pid=%lld\n",
                (long long)tsk->pid);
        return;
    }
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
#ifdef AARCH64_USER_RUNTIME_CASE_IRQ
    if (!g_urt_active)
        return;

    task_t *cur = arch_current_task();
    if (!cur || (int)cur->pid != g_pid_a || g_vm_pause_done)
        return;

    struct aarch64_user_task *owner = aarch64_user_task_owner(cur);
    if (!owner || !owner->image || !owner->vm.edit_active)
        return;
    if (cur->irq_return_migratable) {
        log_err("vm_edit_pause: irq_return_migratable set during paused edit\n");
        return;
    }

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
#endif
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

void aarch64_user_runtime_test_cow_barrier(uint64_t va, uint64_t old_pa)
{
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    if (!g_urt_active || va != 0x40000000ULL)
        return;
    uint32_t c = cpu_id();
    if (c >= 2)
        return;
    __atomic_store_n(&g_cow_test_old_pa, old_pa, __ATOMIC_RELEASE);
    __atomic_fetch_or(&g_cow_arrived_mask, (1U << c), __ATOMIC_ACQ_REL);

    uint64_t freq = arch_cycle_freq();
    uint64_t deadline = arch_cycle_counter() + (freq ? 2 * freq : 100000000ULL);
    while ((__atomic_load_n(&g_cow_arrived_mask, __ATOMIC_ACQUIRE) & 3U) != 3U) {
        if (arch_cycle_counter() > deadline)
            break;
        arch_cpu_pause();
    }
#else
    (void)va;
    (void)old_pa;
#endif
}

void aarch64_user_runtime_test_cow_last_free(uint64_t old_pa)
{
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    if (!g_urt_active)
        return;
    if (old_pa == __atomic_load_n(&g_cow_test_old_pa, __ATOMIC_ACQUIRE))
        __atomic_fetch_add(&g_cow_last_free_count, 1, __ATOMIC_RELAXED);
#else
    (void)old_pa;
#endif
}

void aarch64_user_runtime_test_before_exit(struct task_struct *tsk)
{
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    if (!g_urt_active || !tsk)
        return;
    int child_pid = __atomic_load_n(&g_pid_child, __ATOMIC_ACQUIRE);
    task_t *parent = __atomic_load_n(&g_parent_task, __ATOMIC_ACQUIRE);
    if (child_pid <= 0 || (int)tsk->pid != child_pid || !parent)
        return;

    uint64_t freq = arch_cycle_freq();
    uint64_t deadline = arch_cycle_counter() + (freq ? 2 * freq : 100000000ULL);
    while (arch_cycle_counter() <= deadline) {
        if (__atomic_load_n(&parent->state, __ATOMIC_ACQUIRE) == TASK_INTERRUPTIBLE &&
            parent->blocker.type == BLOCKER_WAITPID &&
            !__atomic_load_n(&parent->on_cpu, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_waitpid_blocked_ok, 1, __ATOMIC_RELEASE);
            return;
        }
        arch_cpu_pause();
    }
#else
    (void)tsk;
#endif
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
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    const struct aarch64_user_elf_source catalog[2] = {
        {
            .bytes = _binary_aarch64_user_runtime_elf_start,
            .size  = (size_t)(_binary_aarch64_user_runtime_elf_end -
                              _binary_aarch64_user_runtime_elf_start),
            .path  = "/bin/user_runtime",
        },
        {
            .bytes = _binary_aarch64_user_runtime_comp_elf_start,
            .size  = (size_t)(_binary_aarch64_user_runtime_comp_elf_end -
                              _binary_aarch64_user_runtime_comp_elf_start),
            .path  = "/bin/user_runtime_comp",
        },
    };
#else
    const struct aarch64_user_elf_source catalog[1] = {
        {
            .bytes = _binary_aarch64_user_runtime_elf_start,
            .size  = (size_t)(_binary_aarch64_user_runtime_elf_end -
                              _binary_aarch64_user_runtime_elf_start),
            .path  = "/bin/user_runtime",
        },
    };
#endif

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
#ifdef AARCH64_USER_RUNTIME_CASE_IRQ
    g_vm_pause_done = false;
    g_a_paused_root_ok = false;
    g_a_post_edit_checked = false;
    g_a_post_edit_ok = false;
#endif
    g_a_paused_task = NULL;
    g_a_saved_root_pa = 0;
    g_a_saved_tpidr = 0;
    __atomic_store_n(&g_reap_count, 0, __ATOMIC_RELEASE);
    memset(g_reap_pid, 0, sizeof(g_reap_pid));
    memset(g_reap_status, 0, sizeof(g_reap_status));
    g_asid_a = 0;
    g_asid_b = 0;
    __atomic_store_n(&g_clean_switches, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_full_flushes, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_event_seq, 0, __ATOMIC_RELEASE);
    memset(g_uninstall_seq, 0, sizeof(g_uninstall_seq));
    memset(g_tlbi_seq, 0, sizeof(g_tlbi_seq));
    memset(g_release_seq, 0, sizeof(g_release_seq));

    uint64_t alloc_before = pmm_probe_4k_allocated_count();
    uint64_t roots_before = aarch64_pt_published_root_count();
    aarch64_sched_observer_begin(0, URT_OBS_GEN);
    uint64_t stack_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK);
    uint64_t thread_rel_before =
        aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD);

    aarch64_sched_set_reap_observer(urt_reap_observe);
    g_urt_active = true;

#ifdef AARCH64_USER_RUNTIME_CASE_SMP
#define URT_CASE_NAME "smp"
#define URT_EXPECTED_TASKS 4
#elif defined(AARCH64_USER_RUNTIME_CASE_ASID)
#define URT_CASE_NAME "asid"
#define URT_EXPECTED_TASKS 2
#else
#define URT_CASE_NAME "irq"
#define URT_EXPECTED_TASKS 2
#endif

    int rc = 1;
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    g_pid_comp0 = -1;
    g_pid_comp1 = -1;
    g_pid_child = -1;
    g_child_cpu = -1;
    g_parent_task = NULL;
    g_smp_cpu_eoi[0] = 0;
    g_smp_cpu_eoi[1] = 0;
    g_smp_cpu_sw[0] = 0;
    g_smp_cpu_sw[1] = 0;
    __atomic_store_n(&g_cow_arrived_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cow_test_old_pa, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cow_last_free_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_waitpid_blocked_ok, 0, __ATOMIC_RELEASE);

    g_pid_comp0 = aarch64_user_task_spawn_on_cpu(catalog, 2, 1, "user_rt_comp0", 0);
    g_pid_comp1 = aarch64_user_task_spawn_on_cpu(catalog, 2, 1, "user_rt_comp1", 1);
    g_pid_a = aarch64_user_task_spawn_on_cpu(catalog, 2, 0, "user_rt_parent", 0);
    if (g_pid_comp0 < 0 || g_pid_comp1 < 0 || g_pid_a < 0) {
        log_err("[user-runtime] FAIL case=smp reason=spawn_failed "
                "p=%d c0=%d c1=%d\n", g_pid_a, g_pid_comp0, g_pid_comp1);
        goto out;
    }
#else
    g_pid_a = aarch64_user_task_spawn(catalog, 1, 0, "user_rt_a");
    if (g_pid_a < 0) {
        log_err("[user-runtime] FAIL case=%s reason=spawn_failed rc=%d\n",
                URT_CASE_NAME, g_pid_a);
        goto out;
    }
#endif

    uint64_t deadline = jiffies + 1500;
    while (jiffies < deadline) {
        uint64_t s_rel =
            aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_STACK) -
            stack_rel_before;
        uint64_t t_rel =
            aarch64_sched_observer_released(AARCH64_SCHED_RESOURCE_THREAD) -
            thread_rel_before;
        if (s_rel >= URT_EXPECTED_TASKS && t_rel >= URT_EXPECTED_TASKS)
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

    if (s_rel != URT_EXPECTED_TASKS || t_rel != URT_EXPECTED_TASKS || obs_rc != 0) {
        log_err("[user-runtime] FAIL case=%s reason=resource_mismatch "
                "s_rel=%lu t_rel=%lu obs_rc=%d\n",
                URT_CASE_NAME,
                (unsigned long)s_rel, (unsigned long)t_rel, obs_rc);
        goto out;
    }
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    if (!urt_saw_status(0x11ULL << 8) || !urt_saw_status(42ULL << 8) ||
        !urt_saw_status(0x33ULL << 8)) {
        log_err("[user-runtime] FAIL case=smp reason=exit_status_mismatch "
                "reaps=%d\n", g_reap_count);
        for (int i = 0; i < g_reap_count && i < URT_MAX_REAPS; i++) {
            log_err("[user-runtime] reap[%d] pid=%d status=0x%lx\n",
                    i, g_reap_pid[i], (unsigned long)g_reap_status[i]);
        }
        goto out;
    }
    if (g_child_cpu != 1) {
        log_err("[user-runtime] FAIL case=smp reason=child_cpu expected=1 "
                "got=%d\n", g_child_cpu);
        goto out;
    }
    if (g_smp_cpu_eoi[0] == 0 || g_smp_cpu_eoi[1] == 0 ||
        g_smp_cpu_sw[0] == 0 || g_smp_cpu_sw[1] == 0) {
        log_err("[user-runtime] FAIL case=smp reason=smp_samples "
                "eoi=(%lu,%lu) sw=(%lu,%lu)\n",
                (unsigned long)g_smp_cpu_eoi[0],
                (unsigned long)g_smp_cpu_eoi[1],
                (unsigned long)g_smp_cpu_sw[0],
                (unsigned long)g_smp_cpu_sw[1]);
        goto out;
    }
    if (g_cow_last_free_count != 1 || g_waitpid_blocked_ok != 1) {
        log_err("[user-runtime] FAIL case=smp reason=cow_or_waitpid "
                "cow_last=%lu waitpid_blk=%d\n",
                (unsigned long)g_cow_last_free_count, g_waitpid_blocked_ok);
        goto out;
    }
    uint64_t leaked_roots = (roots > roots_before) ? (roots - roots_before) : 0;
    uint64_t leaked_pages = (alloc_after > alloc_before) ? (alloc_after - alloc_before) : 0;
    if (leaked_roots != 0 || leaked_pages != 0) {
        log_err("[user-runtime] FAIL case=smp reason=leak roots=%lu(%lu) "
                "alloc=%lu(%lu)\n",
                (unsigned long)roots, (unsigned long)roots_before,
                (unsigned long)alloc_after, (unsigned long)alloc_before);
        goto out;
    }

    if (!g_comp_first_seen[0] || !g_comp_first_seen[1]) {
        log_err("[user-runtime] FAIL case=smp reason=missing_explicit_pin (%d,%d)\n",
                (int)g_comp_first_seen[0], (int)g_comp_first_seen[1]);
        goto out;
    }
    log_info("[user-runtime] explicit_pin pid=%d cpu=%u mask=%lu\n",
             g_pid_comp0, (unsigned)g_comp_first_cpu[0],
             (unsigned long)g_comp_first_mask[0]);
    log_info("[user-runtime] explicit_pin pid=%d cpu=%u mask=%lu\n",
             g_pid_comp1, (unsigned)g_comp_first_cpu[1],
             (unsigned long)g_comp_first_mask[1]);
    log_info("[user-runtime] smp-samples child_cpu=%d "
             "cpu0_eoi=%lu cpu1_eoi=%lu cpu0_sw=%lu cpu1_sw=%lu "
             "cow_last_free=%lu waitpid_blocked=%d waitpid_exit=42 "
             "leaked_roots=%lu leaked_pages=%lu\n",
             g_child_cpu,
             (unsigned long)g_smp_cpu_eoi[0],
             (unsigned long)g_smp_cpu_eoi[1],
             (unsigned long)g_smp_cpu_sw[0],
             (unsigned long)g_smp_cpu_sw[1],
             (unsigned long)g_cow_last_free_count,
             g_waitpid_blocked_ok,
             (unsigned long)leaked_roots,
             (unsigned long)leaked_pages);
    log_info("[user-runtime] PASS case=smp cpu_mask=3\n");
#else
    if (!urt_saw_status(0x11ULL << 8) || !urt_saw_status(0x22ULL << 8)) {
        log_err("[user-runtime] FAIL case=%s reason=exit_status_mismatch "
                "reaps=%d\n", URT_CASE_NAME, g_reap_count);
        for (int i = 0; i < g_reap_count && i < URT_MAX_REAPS; i++) {
            log_err("[user-runtime] reap[%d] pid=%d status=0x%lx\n",
                    i, g_reap_pid[i], (unsigned long)g_reap_status[i]);
        }
        goto out;
    }
#ifdef AARCH64_USER_RUNTIME_CASE_ASID
    if (g_asid_a == 0 || g_asid_b == 0 || g_asid_a == g_asid_b) {
        log_err("[user-runtime] FAIL case=asid reason=bad_asids "
                "asid_a=%u asid_b=%u\n", g_asid_a, g_asid_b);
        goto out;
    }
    if (g_clean_switches < 2 || g_full_flushes != 0) {
        log_err("[user-runtime] FAIL case=asid reason=switch_flush "
                "clean=%lu full=%lu\n",
                (unsigned long)g_clean_switches,
                (unsigned long)g_full_flushes);
        goto out;
    }
    if (!(g_uninstall_seq[g_asid_a] > 0 &&
          g_uninstall_seq[g_asid_a] < g_tlbi_seq[g_asid_a] &&
          g_tlbi_seq[g_asid_a] < g_release_seq[g_asid_a]) ||
        !(g_uninstall_seq[g_asid_b] > 0 &&
          g_uninstall_seq[g_asid_b] < g_tlbi_seq[g_asid_b] &&
          g_tlbi_seq[g_asid_b] < g_release_seq[g_asid_b])) {
        log_err("[user-runtime] FAIL case=asid reason=destroy_order "
                "a=(%lu,%lu,%lu) b=(%lu,%lu,%lu)\n",
                (unsigned long)g_uninstall_seq[g_asid_a],
                (unsigned long)g_tlbi_seq[g_asid_a],
                (unsigned long)g_release_seq[g_asid_a],
                (unsigned long)g_uninstall_seq[g_asid_b],
                (unsigned long)g_tlbi_seq[g_asid_b],
                (unsigned long)g_release_seq[g_asid_b]);
        goto out;
    }

    mm_t *probe_mm = NULL;
    if (aarch64_user_mm_create(&probe_mm) != 0 || !probe_mm) {
        log_err("[user-runtime] FAIL case=asid reason=reuse_create_failed\n");
        goto out;
    }
    uint8_t reused_asid = (uint8_t)(aarch64_user_ttbr0_value(probe_mm) >> 48);
    if (aarch64_user_mm_destroy(probe_mm) != 0) {
        log_err("[user-runtime] FAIL case=asid reason=reuse_destroy_failed\n");
        goto out;
    }
    if (reused_asid == 0 ||
        (reused_asid != g_asid_a && reused_asid != g_asid_b)) {
        log_err("[user-runtime] FAIL case=asid reason=not_reused "
                "reused=%u asid_a=%u asid_b=%u\n",
                reused_asid, g_asid_a, g_asid_b);
        goto out;
    }
    alloc_after = pmm_probe_4k_allocated_count();
    if (roots != 1 || alloc_after != alloc_before) {
        log_err("[user-runtime] FAIL case=asid reason=leak roots=%lu "
                "alloc_before=%lu alloc_after=%lu\n",
                (unsigned long)roots,
                (unsigned long)alloc_before,
                (unsigned long)alloc_after);
        goto out;
    }

    log_info("[user-runtime] asid-samples asid_a=%u asid_b=%u "
             "clean_switches=%lu full_flushes=%lu reused=%u\n",
             g_asid_a, g_asid_b,
             (unsigned long)g_clean_switches,
             (unsigned long)g_full_flushes,
             reused_asid);
    log_info("[user-runtime] PASS case=asid cpu_mask=1\n");
#else
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
#endif
#endif
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
#ifdef AARCH64_USER_RUNTIME_CASE_SMP
    selftest_record_late("aarch64_user_runtime_smp", rc);
#elif defined(AARCH64_USER_RUNTIME_CASE_ASID)
    selftest_record_late("aarch64_user_runtime_asid", rc);
#else
    selftest_record_late("aarch64_user_runtime_irq", rc);
#endif
}
#endif /* !AARCH64_USER_RUNTIME_CASE_MIGRATION */

#endif /* AARCH64_USER_RUNTIME_TEST */
