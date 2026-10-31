/* Production-path AArch64 EL1 scheduler acceptance cases.
 *
 * Guard: the x86 kernel build collects every C file under selftest/
 * into the compile, and the production aarch64 build compiles this
 * file without OS01_SELFTEST — in both shapes the TU must be empty
 * (the observer hooks and the assembly probes only exist in selftest
 * aarch64 builds). */
#if defined(__aarch64__) && defined(OS01_SELFTEST)

#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/boot/shootdown_probe.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/result.h>
#include <time/timer.h>
#include <memory/slab.h>
#include <log/log.h>
#include <core/printk.h>
#include <errno.h>
#include <string.h>
#include <kernel.h>

enum { CASES = 9, TARGET_YIELDS = 10000, RESOURCE_STACK = 1,
       RESOURCE_THREAD = 2, RESOURCE_FPU = 3 };

static volatile uint64_t yield_counts[2];
static volatile uint64_t yield_done;
static volatile uint64_t entry_count;
static volatile uint64_t entry_arg;
volatile uint64_t aarch64_sched_entry_pre_c_sp;
volatile uint64_t aarch64_sched_entry_pre_c_base;
volatile uint64_t aarch64_sched_entry_pre_c_ok;
static volatile uint64_t irq_checks;
static volatile uint64_t irq_stop;
static volatile uint64_t irq_peer_started;
static volatile uint64_t irq_peer_done;
static volatile uint64_t irq_case_done;
static volatile uint64_t ap_worker_finished;
static volatile uint64_t ap_worker_bad_cpu;
static volatile uint64_t ap_worker_bad_affinity;
static volatile uint64_t ap_workers_on_cpu0;
static volatile uint64_t blocker_duplicate_enqueue;
static volatile uint64_t blocker_preswitch_observed;
static volatile uint64_t blocker_preswitch_matches;
static volatile uint64_t blocker_preswitch_state;
static volatile uint64_t blocker_preswitch_on_rq;
static volatile uint64_t entry_arg_ok;
static volatile uint64_t entry_current_ok;
static volatile uint64_t entry_irq_ok;
static volatile uint64_t entry_oncpu_ok;
static volatile uint64_t entry_schedule_ok;
static volatile uint64_t blocker_done;
static volatile uint64_t blocker_ready;
static volatile uint64_t blocker_switch_seen;
static volatile uint64_t blocker_preswitch_armed;
static task_t *volatile blocker_preswitch_task;
static volatile uint64_t blocker_scan_switch_seen;
static task_t *volatile blocker_scan_target;
static volatile uint64_t blocker_scan_wake_seen;
static volatile uint64_t scheduler_observation_active;
static volatile uint64_t cpu0_observation_count;
static volatile uint64_t cpu0_bad_observations;
static volatile uint64_t blocker_scan_mode;
static volatile uint64_t blocker_cond;
static task_t *volatile blocker_target;
static volatile uint64_t allocation_active;
static volatile int64_t resource_live[4];
static volatile uint64_t created_count;
static volatile uint64_t next_stack_count;
static volatile uint64_t switch_count;
static volatile uint64_t idle_observed;
static volatile uint64_t idle_tick_start;
static volatile uint64_t idle_worker_done;
static volatile uint64_t idle_case_active;
static volatile uint64_t idle_blocked_observed;
static volatile uint64_t blocker_tick_deadline;
static volatile uint64_t irq_nested_seen;
static volatile uint64_t irq_frame_seen;
static volatile uint64_t irq_frame_bad;
static volatile uint64_t timer_irq_active;
static volatile uint64_t irq_switch_count;
static volatile uint64_t frame_checksum;
static volatile uint64_t reg_done;
static volatile uint64_t reg_fail_mask;
static volatile uint64_t reg_status[2];
static volatile uint64_t timer_pressure_until;
volatile uint64_t aarch64_sched_timer_tick_count;
static volatile uint64_t timer_stop;
static volatile uint64_t timer_probe_done;
static volatile uint64_t timer_probe_status;
static volatile uint64_t timer_peer_done;
static volatile uint64_t timer_switch_baseline;
static void *reclaim_raw[1000];
static uint8_t reclaim_freed[1000];
static unsigned reclaim_raw_count;
static unsigned reclaim_freed_count;
static uint64_t reclaim_bad_stack;
static const void *resource_ptrs[4][1000];
static uint8_t resource_freed[4][1000];
static unsigned resource_count[4];

static uint64_t test_yielder(uint64_t arg)
{
    unsigned slot = (unsigned)arg;
    for (uint64_t i = 0; i < TARGET_YIELDS; ++i) {
        yield_counts[slot]++;
        kthread_yield();
    }
    yield_done++;
    return 0;
}

static uint64_t test_entry(uint64_t arg)
{
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    task_t *self = current;
    bool pre_c_ok = self && self->thread && aarch64_sched_entry_pre_c_ok &&
        aarch64_sched_entry_pre_c_base == (uintptr_t)self &&
        aarch64_sched_entry_pre_c_sp >= self->thread->usable_low &&
        aarch64_sched_entry_pre_c_sp < self->thread->stack_top &&
        !(aarch64_sched_entry_pre_c_sp & 15);
    entry_arg_ok = arg == 0x51a7;
    entry_current_ok = self && self == current && self->cpu == 0 &&
                       self->on_cpu && !self->in_schedule;
    entry_oncpu_ok = self && self->on_cpu;
    entry_schedule_ok = self && !self->in_schedule;
    entry_irq_ok = !(daif & (1u << 7));
    if (entry_arg_ok && pre_c_ok && entry_current_ok && entry_irq_ok)
        entry_arg++;
    entry_count++;
    return 0;
}

static uint64_t reg_worker(uint64_t pattern)
{
    unsigned slot = pattern == 0x13579bdf2468ace0ULL ? 0 : 1;
    int rc = aarch64_sched_reg_probe(pattern);
    reg_status[slot] = (uint64_t)(unsigned)rc;
    reg_fail_mask |= (uint64_t)(unsigned)rc;
    reg_done |= 1u << slot;
    return (uint64_t)rc;
}

/* A waiter body used for explicit wake, the pre-switch lost-wake window,
 * scan-only wake and the production idle pump. */
static bool blocker_condition(task_t *task)
{
    (void)task;
    if (blocker_scan_mode && jiffies >= blocker_tick_deadline)
        blocker_cond = 1;
    if (idle_case_active && blocker_cond && !idle_tick_start)
        idle_tick_start = jiffies;
    return blocker_cond != 0;
}

static uint64_t blocker_waiter(uint64_t unused)
{
    (void)unused;
    if (blocker_scan_mode)
        blocker_scan_target = current;
    blocker_ready = 1;
    int rc = blocker_wait(blocker_condition, BLOCKER_NANOSLEEP, false);
    if (rc == 0 && current->blocker.type == BLOCKER_NONE && !current->on_rq)
        blocker_done++;
    return (uint64_t)rc;
}

static uint64_t blocker_waker(uint64_t unused)
{
    (void)unused;
    /* Determinism gate: blocker_ready is set by the waiter itself
     * immediately before blocker_wait, so (ready && !on_cpu) is
     * reachable ONLY after the waiter's real block switch-out — never
     * from the waiter's never-ran initial state (on_cpu==0 before its
     * first pick would otherwise fake "blocked"). on_cpu stays 1 for
     * the waiter's whole run and drops only in task_finish_switch. */
    uint64_t gate_until = jiffies + 100;
    while (!blocker_ready && jiffies < gate_until)
        kthread_yield();
    uint64_t until = jiffies + 100;
    while (!blocker_switch_seen && blocker_target && blocker_target->on_cpu &&
           jiffies < until)
        kthread_yield();
    if (blocker_target && !blocker_target->on_cpu)
        blocker_switch_seen = 1;
    if (blocker_switch_seen) {
        blocker_cond = 1;
        percpu_t *rq = &percpu_data[blocker_target->cpu];
        uint64_t f = spin_lock_irqsave(&rq->rq_lock);
        uint32_t before = rq->nr_running;
        spin_unlock_irqrestore(&rq->rq_lock, f);
        blocker_wake(blocker_target);
        f = spin_lock_irqsave(&rq->rq_lock);
        uint32_t after = rq->nr_running;
        uint64_t on_rq = blocker_target->on_rq;
        uint64_t cleared = blocker_target->blocker.type == BLOCKER_NONE;
        spin_unlock_irqrestore(&rq->rq_lock, f);
        if (!on_rq || !cleared || after != before + 1)
            blocker_duplicate_enqueue = 1;
    }
    return 0;
}

static uint64_t irq_worker(uint64_t unused)
{
    (void)unused;
    uint64_t daif_before, daif_after;
    for (unsigned i = 0; i < 512 && !(irq_checks & 1); ++i) {
        uint64_t before = switch_count;
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_before));
        kthread_yield();
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_after));
        if (!(daif_before & (1u << 7)) && !(daif_after & (1u << 7)) &&
            switch_count != before) irq_checks |= 1;
    }
    arch_irq_state_t f = arch_local_irq_save();
    for (unsigned i = 0; i < 512 && !(irq_checks & 4); ++i) {
        uint64_t before = switch_count;
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_before));
        kthread_yield();
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_after));
        if ((daif_before & (1u << 7)) && (daif_after & (1u << 7)) &&
            switch_count != before) irq_checks |= 4;
    }
    arch_local_irq_restore(f);
    irq_stop = 1;
    for (unsigned i = 0; i < 512 && !irq_peer_done; ++i) kthread_yield();
    for (unsigned i = 0; i < 512 && !(irq_checks & 2); ++i) {
        uint64_t before = switch_count;
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_before));
        kthread_yield();
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_after));
        if (!(daif_before & (1u << 7)) && !(daif_after & (1u << 7)) &&
            switch_count == before) irq_checks |= 2;
    }
    f = arch_local_irq_save();
    for (unsigned i = 0; i < 512 && !(irq_checks & 8); ++i) {
        uint64_t before = switch_count;
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_before));
        kthread_yield();
        __asm__ __volatile__("mrs %0, daif" : "=r"(daif_after));
        if ((daif_before & (1u << 7)) && (daif_after & (1u << 7)) &&
            switch_count == before) irq_checks |= 8;
    }
    arch_local_irq_restore(f);
    irq_case_done = 1;
    return 0;
}

static uint64_t irq_peer(uint64_t unused)
{
    (void)unused;
    irq_peer_started = 1;
    while (!irq_stop)
        kthread_yield();
    irq_peer_done = 1;
    return 0;
}

static uint64_t ap_cpu_probe_worker(uint64_t unused)
{
    (void)unused;
    if (current->allowed_cpus != 1)
        ap_worker_bad_affinity++;
    if (cpu_id() != 0 || current->cpu != 0)
        ap_worker_bad_cpu++;
    else
        ap_workers_on_cpu0++;
    ap_worker_finished = 1;
    return 0;
}

static uint64_t timer_pressure_worker(uint64_t unused)
{
    (void)unused;
    uint64_t deadline = timer_pressure_until + 100;
    while (jiffies < timer_pressure_until && jiffies < deadline)
        kthread_yield();
    uint64_t target = aarch64_sched_timer_tick_count + 1;
    uint64_t cycles = arch_cycle_freq();
    uint64_t cycle_deadline = arch_cycle_counter() +
                              (cycles ? cycles * 2 : 100000000);
    log_info("[sched-diag] timer probe start target=%lu ticks=%lu\n",
             (unsigned long)target,
             (unsigned long)aarch64_sched_timer_tick_count);
    timer_probe_status = (uint64_t)(unsigned)aarch64_sched_timer_irq_probe(
        target, cycle_deadline);
    log_info("[sched-diag] timer probe done status=%lu ticks=%lu\n",
             (unsigned long)timer_probe_status,
             (unsigned long)aarch64_sched_timer_tick_count);
    timer_stop = 1;
    uint64_t peer_deadline = jiffies + 100;
    while (!timer_peer_done && jiffies < peer_deadline)
        kthread_yield();
    timer_probe_done = 1;
    return 0;
}

static uint64_t timer_yield_peer(uint64_t unused)
{
    (void)unused;
    while (!timer_stop)
        kthread_yield();
    timer_peer_done = 1;
    return 0;
}

void aarch64_sched_before_block_switch(task_t *task)
{
    if (blocker_preswitch_armed) {
        blocker_preswitch_armed = 0;
        blocker_cond = 1;
        blocker_preswitch_task = task;
        blocker_wake(task);
    }
    if (idle_case_active && task && task->blocker.type != BLOCKER_NONE &&
        task->state == TASK_INTERRUPTIBLE && task->on_cpu)
        idle_blocked_observed = 1;
}

void aarch64_sched_observe_scan_wake(task_t *task)
{
    if (task && task == blocker_scan_target)
        blocker_scan_wake_seen = 1;
}

void aarch64_sched_observe_block_enqueue(task_t *task)
{
    if (!task || task != blocker_preswitch_task || !blocker_cond)
        return;
    unsigned matches = 0;
    percpu_t *rq = &percpu_data[task->cpu];
    uint64_t f = spin_lock_irqsave(&rq->rq_lock);
    for (rbtree_node_t *n = rbtree_first(&rq->run_queue); n; n = rbtree_next(n))
        if (container_of(n, task_t, rb_node) == task)
            matches++;
    if (!blocker_preswitch_observed && task->state == TASK_RUNNING &&
        task->blocker.type == BLOCKER_NONE && task->on_rq && matches == 1) {
        blocker_preswitch_observed = 1;
        blocker_preswitch_matches = matches;
        blocker_preswitch_state = task->state;
        blocker_preswitch_on_rq = task->on_rq;
    }
    if (matches > 1) blocker_duplicate_enqueue = 1;
    spin_unlock_irqrestore(&rq->rq_lock, f);
}

void aarch64_sched_observe_switch_tasks(task_t *prev, task_t *next)
{
    aarch64_sched_observe_switch();
    if (scheduler_observation_active) {
        cpu0_observation_count++;
        if (cpu_id() != 0 || (current && current->cpu != 0) ||
            (prev && prev->cpu != 0) || (next && next->cpu != 0))
            cpu0_bad_observations++;
    }
    if (prev == blocker_target && next != prev &&
        prev->state == TASK_INTERRUPTIBLE && prev->blocker.type != BLOCKER_NONE)
        blocker_switch_seen = 1;
    if (prev == blocker_scan_target && next != prev && blocker_scan_wake_seen)
        blocker_scan_switch_seen = 1;
}

void aarch64_sched_observe_resource(const void *ptr, unsigned kind, bool alloc)
{
    if (!allocation_active || !ptr || kind > RESOURCE_FPU)
        return;
    if (alloc) {
        if (resource_count[kind] < 1000) {
            unsigned n = resource_count[kind]++;
            resource_ptrs[kind][n] = ptr;
            resource_freed[kind][n] = 0;
            resource_live[kind]++;
        }
        if (kind == RESOURCE_STACK && reclaim_raw_count < 1000)
            reclaim_raw[reclaim_raw_count++] = (void *)ptr;
        return;
    }
    for (unsigned i = 0; i < resource_count[kind]; ++i) {
        if (resource_ptrs[kind][i] == ptr && !resource_freed[kind][i]) {
            resource_freed[kind][i] = 1;
            resource_live[kind]--;
            break;
        }
    }
    if (kind == RESOURCE_STACK) {
        for (unsigned i = 0; i < reclaim_raw_count; ++i) {
            if (reclaim_raw[i] == ptr && !reclaim_freed[i]) {
                reclaim_freed[i] = 1;
                reclaim_freed_count++;
                break;
            }
        }
    }
}

void aarch64_sched_observe_reap(task_t *prev, bool next_stack)
{
    if (!allocation_active || !prev)
        return;
    void *raw = prev->stack_alloc_base;
    uintptr_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    bool valid_next = next_stack && current && current != prev && current->thread &&
        (sp & ~(uintptr_t)(STACK_SIZE - 1)) == (uintptr_t)current &&
        sp >= current->thread->usable_low && sp < current->thread->stack_top;
    for (unsigned i = 0; i < reclaim_raw_count; ++i) {
        if (reclaim_raw[i] == raw) {
            if (valid_next)
                next_stack_count++;
            else
                reclaim_bad_stack++;
            return;
        }
    }
}

void aarch64_sched_observe_switch(void)
{
    switch_count++;
    if (timer_irq_active)
        irq_switch_count++;
}

void aarch64_sched_observe_nested_schedule(void)
{
    if (timer_irq_active)
        irq_nested_seen++;
}

void aarch64_sched_observe_timer_irq(struct pt_regs *regs, bool entering)
{
    if (entering) {
        timer_irq_active++;
        aarch64_sched_timer_tick_count++;
        if (regs && !((uintptr_t)regs & 15)) {
            uint64_t *words = (uint64_t *)regs;
            uint64_t sum = 0;
            for (unsigned i = 0; i < 34; ++i) sum ^= words[i] + i;
            frame_checksum = sum;
            irq_frame_seen = 1;
        }
    } else if (timer_irq_active) {
        if (regs && !((uintptr_t)regs & 15)) {
            uint64_t *words = (uint64_t *)regs;
            uint64_t sum = 0;
            for (unsigned i = 0; i < 34; ++i) sum ^= words[i] + i;
            if (sum != frame_checksum) {
                irq_frame_bad = 1;
                irq_frame_seen = 0;
            }
        }
        timer_irq_active--;
    }
}

static uint64_t idle_blocker_worker(uint64_t unused)
{
    (void)unused;
    blocker_scan_mode = 1;
    blocker_tick_deadline = jiffies + 2;
    idle_case_active = 1;
    int rc = blocker_wait(blocker_condition, BLOCKER_NANOSLEEP, false);
    idle_worker_done = (rc == 0);
    return (uint64_t)rc;
}

static void idle_pump_until_worker(void)
{
    uint64_t deadline = jiffies + 10;
    while (!idle_worker_done && jiffies < deadline) {
        if (percpu_data[0].idle == current && !current->on_rq &&
            rbtree_empty(&percpu_data[0].run_queue)) {
            if (idle_blocked_observed)
                idle_observed = 1;
        }
        arch_aarch64_idle_step();
    }
}

static unsigned task_list_count(void)
{
    unsigned count = 0;
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    for (list_t *p = init_task_union.task.list.next;
         p != &init_task_union.task.list; p = p->next)
        ++count;
    spin_unlock_irqrestore(&task_list_lock, flags);
    return count;
}

static int case_yield(void)
{
    yield_counts[0] = yield_counts[1] = 0;
    yield_done = 0;
    uint64_t before = switch_count;
    if (!create_kthread(test_yielder, 0, "sched-yield-a") ||
        !create_kthread(test_yielder, 1, "sched-yield-b"))
        return -1;
    uint64_t deadline = jiffies + 500;
    while ((yield_done < 2 || yield_counts[0] < TARGET_YIELDS ||
            yield_counts[1] < TARGET_YIELDS) && jiffies < deadline)
        kthread_yield();
    uint64_t switches = switch_count - before;
    serial_printk("[sched-evidence] id=aarch64_sched_yield a_yields=%lu b_yields=%lu switches=%lu\n",
                  (unsigned long)yield_counts[0], (unsigned long)yield_counts[1],
                  (unsigned long)switches);
    return yield_done == 2 && yield_counts[0] >= TARGET_YIELDS && yield_counts[1] >= TARGET_YIELDS &&
           switches > 0 ? 0 : -1;
}

static int case_regs(void)
{
    /* C stack sentinels are distinct for each worker and checked after
     * production switches by the assembly probe. */
    reg_done = reg_fail_mask = 0;
    reg_status[0] = reg_status[1] = UINT64_MAX;
    if (!create_kthread(reg_worker, 0x13579bdf2468ace0ULL, "sched-reg-a") ||
        !create_kthread(reg_worker, 0xfedcba9876543210ULL, "sched-reg-b"))
        return -1;
    uint64_t deadline = jiffies + 100;
    while (reg_done != 3 && jiffies < deadline)
        kthread_yield();
    uint64_t reg_fail = ((reg_status[0] | reg_status[1]) & 1) != 0;
    uint64_t stack_fail = ((reg_status[0] | reg_status[1]) & 2) != 0;
    uint64_t sp_fail = ((reg_status[0] | reg_status[1]) & 4) != 0;
    serial_printk("[sched-evidence] id=aarch64_sched_regs reg_fail=%lu stack_fail=%lu sp_fail=%lu\n",
                  (unsigned long)reg_fail, (unsigned long)stack_fail,
                  (unsigned long)sp_fail);
    return reg_done == 3 && reg_status[0] == 0 && reg_status[1] == 0 ? 0 : -1;
}

static int case_entry(void)
{
    entry_count = entry_arg = 0;
    aarch64_sched_entry_pre_c_sp = 0;
    aarch64_sched_entry_pre_c_base = 0;
    aarch64_sched_entry_pre_c_ok = 0;
    if (!create_kthread(test_entry, 0x51a7, "sched-entry"))
        return -1;
    uint64_t deadline = jiffies + 100;
    while (!entry_count && jiffies < deadline)
        kthread_yield();
    uint64_t pre_c_sp_ok = aarch64_sched_entry_pre_c_ok &&
        aarch64_sched_entry_pre_c_sp >= aarch64_sched_entry_pre_c_base &&
        aarch64_sched_entry_pre_c_sp < aarch64_sched_entry_pre_c_base + STACK_SIZE &&
        !(aarch64_sched_entry_pre_c_sp & 15);
    uint64_t current_ok = entry_current_ok;
    uint64_t irq_enabled = entry_irq_ok;
    serial_printk("[sched-evidence] id=aarch64_sched_entry entries=%lu arg_ok=%lu pre_c_sp_ok=%lu irq_enabled=%lu current_ok=%lu\n",
                  (unsigned long)entry_count, (unsigned long)entry_arg_ok,
                  (unsigned long)pre_c_sp_ok, (unsigned long)irq_enabled,
                  (unsigned long)current_ok);
    return entry_count == 1 && entry_arg == 1 && pre_c_sp_ok &&
           current_ok && irq_enabled ? 0 : -1;
}

static int case_irq(void)
{
    uint64_t on_switch, on_noswitch, off_switch, off_noswitch;
    irq_checks = 0; irq_stop = 0; irq_peer_started = 0;
    irq_peer_done = irq_case_done = 0;
    if (!create_kthread(irq_peer, 0, "sched-irq-peer")) return -1;
    if (!create_kthread(irq_worker, 0, "sched-irq")) return -1;
    uint64_t deadline = jiffies + 100;
    while (!irq_case_done && jiffies < deadline)
        arch_aarch64_idle_step();
    on_switch = (irq_checks & 1) != 0;
    on_noswitch = (irq_checks & 2) != 0;
    off_switch = (irq_checks & 4) != 0;
    off_noswitch = (irq_checks & 8) != 0;
    log_info("[sched-diag] irq raw observations=%lu done=%lu idle_state=%lu idle_on_rq=%lu\n",
             (unsigned long)irq_checks, (unsigned long)irq_case_done,
             (unsigned long)(current == percpu_data[0].idle &&
                             current->state == TASK_RUNNING),
             (unsigned long)current->on_rq);
    serial_printk("[sched-evidence] id=aarch64_sched_irq on_switch=%lu off_switch=%lu on_noswitch=%lu off_noswitch=%lu\n",
                  (unsigned long)on_switch, (unsigned long)off_switch,
                  (unsigned long)on_noswitch, (unsigned long)off_noswitch);
    return irq_case_done && current == percpu_data[0].idle &&
           current->state == TASK_RUNNING && !current->on_rq &&
           on_switch && off_switch && on_noswitch && off_noswitch ? 0 : -1;
}

static int case_blocker(void)
{
    /* Already-true timing is directly checked. The remaining three
     * timings use production blocker_wait/wake and schedule scan paths. */
    blocker_cond = 1;
    uint64_t already = blocker_wait(blocker_condition, BLOCKER_NANOSLEEP,
                                    false) == 0;
    blocker_duplicate_enqueue = blocker_preswitch_observed = 0;
    blocker_preswitch_matches = blocker_preswitch_state = blocker_preswitch_on_rq = 0;
    blocker_cond = 0; blocker_done = blocker_ready = blocker_switch_seen = 0;
    blocker_target = create_kthread(blocker_waiter, 0, "sched-block-waiter");
    if (!blocker_target || !create_kthread(blocker_waker, 0, "sched-block-waker"))
        return -1;
    uint64_t deadline = jiffies + 300;
    while (!blocker_done && jiffies < deadline) kthread_yield();
    /* The waiting task's switch-out is observable through the hook below. */
    uint64_t explicit_wake = blocker_done != 0;
    /* The explicit-subrun waiter has exited and been reaped by the time
     * the loop above observes blocker_done; the pointer is dangling
     * value-only from here on, so drop it before creating the next
     * waiter (observe_switch_tasks compares against blocker_target). */
    blocker_target = NULL;
    blocker_cond = 0; blocker_done = 0; blocker_ready = 0;
    blocker_preswitch_armed = 1;
    if (!create_kthread(blocker_waiter, 0, "sched-block-preswitch")) return -1;
    deadline = jiffies + 100;
    while (!blocker_done && jiffies < deadline) kthread_yield();
    uint64_t pre_switch = blocker_done != 0 && blocker_preswitch_observed;
    blocker_scan_mode = 1; blocker_cond = 0; blocker_done = 0;
    blocker_scan_target = NULL;
    blocker_scan_wake_seen = blocker_scan_switch_seen = 0;
    blocker_tick_deadline = jiffies + 2;
    if (!create_kthread(blocker_waiter, 0, "sched-block-scan")) return -1;
    deadline = jiffies + 100;
    while (!blocker_done && jiffies < deadline) kthread_yield();
    uint64_t scan = blocker_done != 0 && blocker_scan_wake_seen &&
                    blocker_scan_switch_seen;
    uint64_t duplicate_enqueue = blocker_duplicate_enqueue;
    serial_printk("[sched-evidence] id=aarch64_sched_blocker already=%lu explicit=%lu pre_switch=%lu scan=%lu duplicate_enqueue=%lu\n",
                  (unsigned long)already, (unsigned long)explicit_wake,
                  (unsigned long)pre_switch, (unsigned long)scan,
                  (unsigned long)duplicate_enqueue);
    log_info("[sched-diag] blocker requeue observed=%lu matches=%lu state=%lu on_rq=%lu\n",
             (unsigned long)blocker_preswitch_observed,
             (unsigned long)blocker_preswitch_matches,
             (unsigned long)blocker_preswitch_state,
             (unsigned long)blocker_preswitch_on_rq);
    return already && explicit_wake && pre_switch && scan &&
           !duplicate_enqueue ? 0 : -1;
}

static int case_reclaim(void)
{
    unsigned list_before = task_list_count();
    uint32_t rq_before = percpu_data[0].nr_running;
    memset((void *)resource_live, 0, sizeof(resource_live));
    memset(resource_count, 0, sizeof(resource_count));
    memset(resource_ptrs, 0, sizeof(resource_ptrs));
    memset(resource_freed, 0, sizeof(resource_freed));
    next_stack_count = created_count = 0;
    memset(reclaim_raw, 0, sizeof(reclaim_raw));
    memset(reclaim_freed, 0, sizeof(reclaim_freed));
    reclaim_raw_count = reclaim_freed_count = 0;
    reclaim_bad_stack = 0;
    allocation_active = 1;
    for (unsigned i = 0; i < 1000; ++i) {
        if (!create_kthread(test_entry, i, "sched-reclaim")) {
            allocation_active = 0;
            return -1;
        }
        created_count++;
        if ((i & 31) == 31) kthread_yield();
    }
    uint64_t deadline = jiffies + 1000;
    while ((reclaim_freed_count < created_count ||
            percpu_data[0].nr_running > rq_before) && jiffies < deadline)
        kthread_yield();
    allocation_active = 0;
    unsigned list_after = task_list_count();
    int64_t list_delta_signed = (int64_t)list_after - (int64_t)list_before;
    int64_t rq_delta_signed = (int64_t)percpu_data[0].nr_running - (int64_t)rq_before;
    uint64_t list_delta = list_delta_signed < 0 ? UINT64_MAX : (uint64_t)list_delta_signed;
    uint64_t rq_delta = rq_delta_signed < 0 ? UINT64_MAX : (uint64_t)rq_delta_signed;
    uint64_t stack_live = (uint64_t)resource_live[RESOURCE_STACK];
    uint64_t thread_live = (uint64_t)resource_live[RESOURCE_THREAD];
    uint64_t fpu_live = (uint64_t)resource_live[RESOURCE_FPU];
    uint64_t next_stack = reclaim_freed_count == created_count &&
                          next_stack_count == created_count && !reclaim_bad_stack;
    serial_printk("[sched-evidence] id=aarch64_sched_reclaim created=%lu stack_live=%lu thread_live=%lu fpu_live=%lu next_stack=%lu list_delta=%lu rq_delta=%lu\n",
                  (unsigned long)created_count, (unsigned long)stack_live,
                  (unsigned long)thread_live, (unsigned long)fpu_live,
                  (unsigned long)next_stack, (unsigned long)list_delta,
                  (unsigned long)rq_delta);
    return created_count >= 1000 && !stack_live && !thread_live && !fpu_live &&
           next_stack && !list_delta && !rq_delta ? 0 : -1;
}

static int case_idle(void)
{
    idle_observed = idle_tick_start = idle_worker_done = 0;
    idle_blocked_observed = 0;
    blocker_scan_mode = 0; blocker_cond = 0; idle_case_active = 1;
    if (!create_kthread(idle_blocker_worker, 0, "sched-idle-waiter")) return -1;
    /* Schedule the worker once. It blocks and returns control to this
     * idle continuation, which pumps the same production idle step. */
    schedule();
    idle_pump_until_worker();
    uint64_t wake_ticks = jiffies - idle_tick_start;
    idle_case_active = 0;
    uint64_t idle_in_tree = current->on_rq;
    serial_printk("[sched-evidence] id=aarch64_sched_idle idle_in_tree=%lu idle_observed=%lu wake_ticks=%lu\n",
                  (unsigned long)idle_in_tree, (unsigned long)idle_observed,
                  (unsigned long)wake_ticks);
    return !idle_in_tree && idle_observed && idle_worker_done && wake_ticks <= 10
           ? 0 : -1;
}

static int case_timer(void)
{
    uint64_t before = jiffies;
    irq_frame_seen = irq_frame_bad = irq_nested_seen = irq_switch_count = 0;
    aarch64_sched_timer_tick_count = 0;
    timer_pressure_until = before + 3;
    timer_stop = timer_probe_done = timer_peer_done = 0;
    timer_probe_status = 0;
    timer_switch_baseline = switch_count;
    if (!create_kthread(timer_yield_peer, 0, "sched-timer-yield-peer")) return -1;
    if (!create_kthread(timer_pressure_worker, 0, "sched-timer-pressure")) return -1;
    uint64_t deadline = before + 100;
    while (!timer_probe_done && jiffies < deadline) kthread_yield();
    timer_stop = 1;
    while (!timer_peer_done && jiffies < deadline) kthread_yield();
    uint64_t tick_delta = jiffies - before;
    uint64_t irq_frame_ok = irq_frame_seen && !irq_frame_bad &&
                            timer_probe_done && timer_probe_status == 0;
    uint64_t nested_schedule = irq_nested_seen;
    uint64_t irq_switches = irq_switch_count;
    uint64_t pressure_switches = switch_count - timer_switch_baseline;
    serial_printk("[sched-evidence] id=aarch64_sched_timer tick_delta=%lu irq_frame_ok=%lu nested_schedule=%lu irq_switches=%lu\n",
                  (unsigned long)tick_delta, (unsigned long)irq_frame_ok,
                  (unsigned long)nested_schedule, (unsigned long)irq_switches);
    return tick_delta >= 3 && aarch64_sched_timer_tick_count >= 3 &&
           pressure_switches > 0 && irq_frame_ok && !nested_schedule && !irq_switches
           ? 0 : -1;
}

static int case_ap(void)
{
    uint32_t count = num_cpus;
    ap_worker_finished = ap_worker_bad_cpu = ap_worker_bad_affinity = 0;
    ap_workers_on_cpu0 = 0;
    if (!create_kthread(ap_cpu_probe_worker, 0, "sched-ap-cpu-proof")) return -1;
    uint64_t deadline = jiffies + 100;
    while (!ap_worker_finished && jiffies < deadline) kthread_yield();
    uint64_t ap_target_rejected = 1;
    if (count >= 2) {
        /* Build a valid, unpublished task context and call the actual ARM
         * backend. The common constructor's scheduler_ok check would reject
         * CPU1 before reaching this frozen backend guard. */
        void *raw = kmalloc(sizeof(union task_union) + STACK_SIZE);
        thread_t *probe_thread = kmalloc(sizeof(thread_t));
        if (!raw || !probe_thread) {
            if (probe_thread) kfree(probe_thread);
            if (raw) kfree(raw);
            ap_target_rejected = 0;
        } else {
            task_t *probe = (task_t *)(((uintptr_t)raw + STACK_SIZE - 1) &
                                       ~(uintptr_t)(STACK_SIZE - 1));
            memset(probe, 0, sizeof(*probe));
            memset(probe_thread, 0, sizeof(*probe_thread));
            probe->thread = probe_thread;
            probe->cpu = 1;
            int rc = arch_kthread_prepare(probe, ap_cpu_probe_worker, 0);
            ap_target_rejected = rc == -EINVAL && probe->allowed_cpus == 0;
            kfree(probe_thread);
            kfree(raw);
        }
    }
    uint64_t cpu0_only = ap_target_rejected && ap_worker_finished &&
                         !ap_worker_bad_cpu &&
                         !ap_worker_bad_affinity && ap_workers_on_cpu0;
    if (count >= 2)
        aarch64_shootdown_probe_run_late();
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        if (cpu == 0) continue;
        percpu_t *p = &percpu_data[cpu];
        uint32_t sched_ok = __atomic_load_n(&p->scheduler_ok, __ATOMIC_ACQUIRE);
        uint32_t nr = __atomic_load_n(&p->nr_running, __ATOMIC_ACQUIRE);
        uint64_t work_ok = aarch64_shootdown_probe_late_ap_ok(cpu);
        serial_printk("[sched-evidence] id=aarch64_sched_ap cpu=%u scheduler_ok=%u nr_running=%u work_ok=%lu\n",
                      cpu, sched_ok, nr, (unsigned long)work_ok);
        if (sched_ok || nr || !work_ok) cpu0_only = 0;
    }
    log_info("[sched-diag] cpu0 scheduler observations=%lu bad=%lu\n",
             (unsigned long)cpu0_observation_count,
             (unsigned long)cpu0_bad_observations);
    cpu0_only = cpu0_only && cpu0_observation_count > 0 &&
                cpu0_bad_observations == 0;
    serial_printk("[sched-evidence] id=aarch64_sched_ap cpu_count=%u cpu0_only=%lu\n",
                  count, (unsigned long)cpu0_only);
    return count == num_cpus && cpu0_only ? 0 : -1;
}

void test_aarch64_scheduler_run(void)
{
    scheduler_observation_active = 1;
    cpu0_observation_count = cpu0_bad_observations = 0;
    static const char *const ids[CASES] = {
        "aarch64_sched_yield", "aarch64_sched_regs", "aarch64_sched_entry",
        "aarch64_sched_irq", "aarch64_sched_blocker", "aarch64_sched_reclaim",
        "aarch64_sched_idle", "aarch64_sched_timer", "aarch64_sched_ap"
    };
    int (*const bodies[CASES])(void) = {
        case_yield, case_regs, case_entry, case_irq, case_blocker,
        case_reclaim, case_idle, case_timer, case_ap
    };
    for (unsigned i = 0; i < CASES; ++i) {
        int rc = bodies[i]();
        if (rc)
            log_err("[sched] selftest case %s failed rc=%u\n", ids[i],
                    (unsigned)(rc < 0 ? -rc : rc));
        selftest_record_late(ids[i], rc);
    }
}

#endif /* __aarch64__ && OS01_SELFTEST */
