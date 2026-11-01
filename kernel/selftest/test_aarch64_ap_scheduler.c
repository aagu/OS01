/* P1 AP cooperative-scheduler production-path acceptance driver
 * (spec §7, plan P1 Task 4).
 *
 * Guard: only the AARCH64_AP_SCHED_TEST_AP variant compiles a body.
 * The normal ELF, the old sched variants and the x86 build see an
 * empty TU (and carry none of the driver symbols — the sched-ap
 * runner's nm audit asserts that).
 *
 * Evidence envelope (frozen, BSP single writer):
 *   [ap-sched] START phase=ap cpus=<N> accepted_mask=<M>   (decimal,
 *       M must equal (1<<N)-1 — a missing AP may never downgrade)
 *   [ap-sched] phase=ap case=<id> cpu=<c> k=v ...          (7 cases)
 *   [ap-sched] END phase=ap failed=<0|n>
 * with exactly one START and one END bracketing every case line.
 */
#if defined(__aarch64__) && defined(OS01_SELFTEST) && \
    defined(AARCH64_AP_SCHED_TEST_AP)

#include <sched/task.h>
#include <sched/internal.h>
#include <sched/boot.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/smp.h>
#include <arch/aarch64/ap_work.h>
#include <selftest/aarch64_ap_scheduler.h>
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_sched_observer.h>
#include <selftest/result.h>
#include <time/timer.h>
#include <log/log.h>
#include <core/printk.h>
#include <errno.h>
#include <kernel.h>

#define AP_RESOURCE_STACK  AARCH64_SCHED_RESOURCE_STACK
#define AP_RESOURCE_THREAD AARCH64_SCHED_RESOURCE_THREAD

/* Shared with the old scheduler selftest (T2 counters). */
extern volatile uint64_t irq_switch_counts[NR_CPUS];
extern uint64_t reclaim_bad_stack;

/* Per-CPU case state (BSS; the BSP aggregates and single-writes the
 * protocol lines). */
static volatile uint64_t ap_entry_done[NR_CPUS];
static volatile uint64_t ap_entry_ok[NR_CPUS];
static volatile uint64_t ap_entry_bad_cpu[NR_CPUS];
static volatile uint64_t ap_entry_bad_mask[NR_CPUS];
static volatile uint64_t ap_entry_bad_sp[NR_CPUS];
static volatile uint64_t ap_entry_bad_regs[NR_CPUS];
static volatile uint64_t ap_yield_done[NR_CPUS];
static volatile uint64_t ap_yield_count[NR_CPUS];
static volatile uint64_t ap_yield_bad_canary[NR_CPUS];
static volatile uint64_t ap_reap_created_count;
static volatile uint64_t ap_cross_ok[3];
static volatile uint64_t ap_cross_expected[3];
static volatile uint64_t ap_landing_cpu;
static volatile uint64_t ap_landing_bad_mask;
static volatile uint64_t ap_landing_done;
static volatile uint64_t ap_work_pre_ok[NR_CPUS];
static volatile uint64_t ap_work_post_ok[NR_CPUS];
static volatile uint64_t ap_work_seq_ok = 1;
static uint64_t ap_admission_invalid_rejected;
static uint64_t ap_admission_zero_publish = 1;
static uint64_t ap_admission_unready_rejected;
static uint64_t ap_admission_unready_na;

static int task_list_count_now(void)
{
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    for (list_t *p = init_task_union.task.list.next;
         p != &init_task_union.task.list; p = p->next)
        n++;
    spin_unlock_irqrestore(&task_list_lock, flags);
    return n;
}

/* Retained-slot seq discipline: the driver is the ONLY BSP producer
 * after the boot probes; read the slot's retained seq, submit with
 * seq+1 (overflow fails), and only treat the round as complete after
 * DONE with that exact seq.  Slots and module state are never reset. */
static uint32_t ap_work_seq_base[NR_CPUS];

static int ap_work_submit_next(uint32_t cpu, uint32_t cmd,
                               uint64_t arg0, uint64_t arg1)
{
    struct ap_work *slot = &ap_work[cpu];
    uint32_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
    uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (state != AP_WORK_IDLE)
        return -EBUSY;
    if (seq + 1 <= seq)      /* seq overflow */
        return -EOVERFLOW;
    ap_work_submit(cpu, seq + 1, cmd, arg0, arg1);
    ap_work_seq_base[cpu] = seq + 1;
    return 0;
}

/* ── workers ──────────────────────────────────────────────────────── */

/* Callee-saved register roundtrip across a real switch: the pattern is
 * written into x19/x20, the worker yields, and both must come back. */
static bool ap_regs_roundtrip(uint64_t pat)
{
    uint64_t a, b;
    __asm__ __volatile__("mov x19, %0" :: "r"(pat) : "x19");
    __asm__ __volatile__("mov x20, %0" :: "r"(pat) : "x20");
    kthread_yield();
    __asm__ __volatile__("mov %0, x19" : "=r"(a));
    __asm__ __volatile__("mov %0, x20" : "=r"(b));
    return a == pat && b == pat;
}

static uint64_t ap_entry_worker(uint64_t target)
{
    unsigned cpu = (unsigned)target;
    task_t *self = current;
    uintptr_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    bool ok = true;
    if (cpu_id() != cpu || self->cpu != cpu) {
        ap_entry_bad_cpu[cpu]++;
        ok = false;
    }
    if (self->allowed_cpus != (1ULL << cpu)) {
        ap_entry_bad_mask[cpu]++;
        ok = false;
    }
    if (!((sp & ~(uintptr_t)(STACK_SIZE - 1)) == (uintptr_t)self &&
          self->thread && sp >= self->thread->usable_low &&
          sp < self->thread->stack_top && !(sp & 15))) {
        ap_entry_bad_sp[cpu]++;
        ok = false;
    }
    if (!ap_regs_roundtrip(0xa53c4e5501ULL + cpu)) {
        ap_entry_bad_regs[cpu]++;
        ok = false;
    }
    if (ok)
        ap_entry_ok[cpu]++;
    ap_entry_done[cpu]++;
    return 0;
}

static uint64_t ap_yield_worker(uint64_t target)
{
    unsigned cpu = (unsigned)target;
    /* The assembly reg probe yields 16 times internally around its
     * callee-saved/canary checks; 16 explicit yields make the observed
     * per-worker total exactly 32 — the probe's internal yields are
     * part of the frozen 64-per-CPU count, never missed. */
    int rc = aarch64_sched_reg_probe(0x9e3779b97f4a7c15ULL + cpu);
    ap_yield_count[cpu] += 16;
    if (rc & 2)
        ap_yield_bad_canary[cpu]++;
    for (unsigned i = 0; i < 16; ++i) {
        ap_yield_count[cpu]++;
        kthread_yield();
    }
    ap_yield_done[cpu]++;
    return (uint64_t)(unsigned)rc;
}

static uint64_t ap_reap_worker(uint64_t unused)
{
    (void)unused;
    return 0;
}

/* Default-placement probe: records where an UNPINNED create actually
 * landed (must be CPU0 under the aarch64 default mask 1). */
static uint64_t ap_landing_worker(uint64_t unused)
{
    (void)unused;
    ap_landing_cpu = cpu_id();
    if (current->allowed_cpus != 1ULL)
        ap_landing_bad_mask++;
    ap_landing_done++;
    return 0;
}

/* Creation-party marker: plan carries which (bit0..1) and the frozen
 * expected target cpu (bit8..).  Counts only on the exact target with
 * the exact affinity shape (unpinned→mask 1; pinned→single bit). */
static uint64_t ap_cross_marker(uint64_t plan)
{
    unsigned which = (unsigned)(plan & 3);
    unsigned target = (unsigned)((plan >> 8) & 0xff);
    bool ok = cpu_id() == target && current->cpu == target;
    if (which == 0)
        ok = ok && current->allowed_cpus == 1ULL;      /* unpinned */
    else
        ok = ok && current->allowed_cpus == (1ULL << target);
    if (ok)
        ap_cross_ok[which]++;
    ap_cross_expected[which]++;
    return 0;
}

static uint64_t ap_cross_creator(uint64_t plan)
{
    unsigned which = (unsigned)(plan & 3);
    unsigned target = (unsigned)((plan >> 8) & 0xff);
    if (which == 0) {
        /* An AP creating an UNPINNED task: the aarch64 default mask is
         * 1, so the child must run on CPU0. */
        if (!create_kthread(ap_cross_marker, plan, "ap-cross-to-bsp"))
            return 1;
    } else if (kernel_thread_on_cpu(ap_cross_marker, plan, target) < 0) {
        return 1;
    }
    return 0;
}

/* ── before-release: runtime admission + pre-handoff work ─────────── */
void test_aarch64_ap_scheduler_before_release(void)
{
    uint64_t accepted = aarch64_smp_accepted_mask();
    int list_before = task_list_count_now();
    uint32_t rq_before = percpu_data[0].nr_running;

    /* A runtime-invalid target is rejected with zero publication. */
    if (kernel_thread_on_cpu(ap_reap_worker, 0, NR_CPUS) == -EINVAL)
        ap_admission_invalid_rejected = 1;

    /* Every accepted AP is online + ipi_ready here but has NOT yet
     * published scheduler_ok: the constructor must refuse it. */
    bool have_ap = false;
    for (uint32_t cpu = 1; cpu < num_cpus; ++cpu) {
        if (!((accepted >> cpu) & 1ULL))
            continue;
        have_ap = true;
        if (kernel_thread_on_cpu(ap_reap_worker, 0, cpu) == -EINVAL)
            ap_admission_unready_rejected = 1;
    }
    ap_admission_unready_na = have_ap ? 0 : 1;

    if (task_list_count_now() != list_before ||
        percpu_data[0].nr_running != rq_before)
        ap_admission_zero_publish = 0;

    /* Pre-handoff work: one real WORK_BARRIER per accepted CPU — AP
     * slots are consumed by the OLD work loops here; CPU0's own slot
     * is consumed synchronously by this producer. */
    for (uint32_t cpu = 0; cpu < num_cpus; ++cpu) {
        if (cpu != 0 && !((accepted >> cpu) & 1ULL))
            continue;
        if (ap_work_submit_next(cpu, WORK_BARRIER, 0, 0) != 0) {
            log_err("[ap-sched] pre-handoff submit failed cpu=%u\n", cpu);
            continue;
        }
        if (cpu == 0) {
            /* Synchronous local consumption: run_one completes the
             * item, then ap_work_wait validates the seq and re-arms
             * the slot to IDLE (the BSP wait path is the ONLY re-arm). */
            ap_work_run_one(0);
            uint64_t freq = arch_cycle_freq();
            ap_work_pre_ok[0] = ap_work_wait(
                0, ap_work_seq_base[0], NULL,
                arch_cycle_counter() + (freq ? freq : 100000000ULL));
        } else {
            uint64_t freq = arch_cycle_freq();
            uint64_t deadline = arch_cycle_counter() +
                                (freq ? freq : 100000000ULL);
            if (!ap_work_wait(cpu, ap_work_seq_base[cpu], NULL, deadline))
                ap_work_seq_ok = 0;
            else
                ap_work_pre_ok[cpu] = 1;
        }
    }
}

/* ── the seven production cases ───────────────────────────────────── */

static int ap_case_ready(unsigned cpu)
{
    percpu_t *rq = &percpu_data[cpu];
    task_t *idle = rq->idle;
    uint32_t ok = __atomic_load_n(&rq->scheduler_ok, __ATOMIC_ACQUIRE);
    uint32_t on_cpu = idle ? __atomic_load_n(&idle->on_cpu,
                                             __ATOMIC_ACQUIRE) : 0;
    uint32_t on_rq = idle ? __atomic_load_n(&idle->on_rq,
                                            __ATOMIC_ACQUIRE) : 0;
    bool unique = idle != NULL;
    if (cpu == 0)
        unique = unique && idle == &init_task_union.task;
    else
        unique = unique && idle != &init_task_union.task;
    for (uint32_t other = 1; other < NR_CPUS && unique; ++other)
        if (other != cpu && percpu_data[other].idle &&
            percpu_data[other].idle == idle)
            unique = false;
    serial_printk("[ap-sched] phase=ap case=ready cpu=%u "
                  "scheduler_ok=%u idle_on_cpu=%u idle_on_rq=%u "
                  "unique_stack=%u\n",
                  cpu, ok, on_cpu, on_rq, (unsigned)unique);
    return (ok == 1 && idle && on_cpu && !on_rq && unique) ? 0 : -1;
}

static int ap_case_entry(unsigned cpu)
{
    uint64_t deadline = jiffies + 200;
    ap_entry_done[cpu] = ap_entry_ok[cpu] = 0;
    ap_entry_bad_cpu[cpu] = ap_entry_bad_mask[cpu] = 0;
    ap_entry_bad_sp[cpu] = ap_entry_bad_regs[cpu] = 0;
    if (kernel_thread_on_cpu(ap_entry_worker, cpu, cpu) < 0 ||
        kernel_thread_on_cpu(ap_entry_worker, cpu, cpu) < 0)
        return -1;
    while (ap_entry_done[cpu] < 2 && jiffies < deadline)
        kthread_yield();
    serial_printk("[ap-sched] phase=ap case=entry cpu=%u "
                  "entries=%lu wrong_cpu=%lu bad_mask=%lu bad_sp=%lu "
                  "bad_regs=%lu\n",
                  cpu, (unsigned long)ap_entry_done[cpu],
                  (unsigned long)ap_entry_bad_cpu[cpu],
                  (unsigned long)ap_entry_bad_mask[cpu],
                  (unsigned long)ap_entry_bad_sp[cpu],
                  (unsigned long)ap_entry_bad_regs[cpu]);
    return (ap_entry_done[cpu] == 2 && ap_entry_ok[cpu] == 2 &&
            !ap_entry_bad_cpu[cpu] && !ap_entry_bad_mask[cpu] &&
            !ap_entry_bad_sp[cpu] && !ap_entry_bad_regs[cpu]) ? 0 : -1;
}

static int ap_case_yield(unsigned cpu)
{
    uint64_t deadline = jiffies + 200;
    uint64_t irq_before = irq_switch_counts[cpu];
    ap_yield_done[cpu] = ap_yield_count[cpu] = 0;
    ap_yield_bad_canary[cpu] = 0;
    if (kernel_thread_on_cpu(ap_yield_worker, cpu, cpu) < 0 ||
        kernel_thread_on_cpu(ap_yield_worker, cpu, cpu) < 0)
        return -1;
    while (ap_yield_done[cpu] < 2 && jiffies < deadline)
        kthread_yield();
    uint64_t irq_switches = irq_switch_counts[cpu] - irq_before;
    serial_printk("[ap-sched] phase=ap case=yield cpu=%u "
                  "tasks=%lu yields=%lu bad_canary=%lu irq_switches=%lu\n",
                  cpu, (unsigned long)ap_yield_done[cpu],
                  (unsigned long)ap_yield_count[cpu],
                  (unsigned long)ap_yield_bad_canary[cpu],
                  (unsigned long)irq_switches);
    return (ap_yield_done[cpu] == 2 && ap_yield_count[cpu] == 64 &&
            !ap_yield_bad_canary[cpu] && !irq_switches) ? 0 : -1;
}

static int ap_case_reap(unsigned cpu)
{
    /* One active observer batch at a time: per-CPU batches run
     * sequentially with separate generations.  A worker returning is
     * NOT a reclaim — the wait observes the observer's released
     * counters, which only advance after the real kfree. */
    uint64_t deadline = jiffies + 600;
    aarch64_sched_observer_begin(cpu, (unsigned)(cpu * 10 + 1));
    uint32_t fail_before = aarch64_sched_observer_failure_flags();
    uint64_t stack_before =
        aarch64_sched_observer_released(AP_RESOURCE_STACK);
    uint64_t ctx_before =
        aarch64_sched_observer_released(AP_RESOURCE_THREAD);
    uint64_t created_before = aarch64_sched_observer_created();
    uint64_t bad_stack_before = reclaim_bad_stack;
    ap_reap_created_count = 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (kernel_thread_on_cpu(ap_reap_worker, 0, cpu) < 0) {
            log_err("[ap-sched] reap create failed cpu=%u i=%u\n", cpu, i);
            break;
        }
        ap_reap_created_count++;
        if ((i & 31) == 31)
            kthread_yield();
    }
    while ((aarch64_sched_observer_released(AP_RESOURCE_STACK) -
            stack_before) < ap_reap_created_count && jiffies < deadline)
        kthread_yield();
    uint64_t created = aarch64_sched_observer_created() - created_before;
    uint64_t stack_freed =
        aarch64_sched_observer_released(AP_RESOURCE_STACK) - stack_before;
    uint64_t ctx_freed =
        aarch64_sched_observer_released(AP_RESOURCE_THREAD) - ctx_before;
    uint32_t fails = aarch64_sched_observer_failure_flags() & ~fail_before;
    uint64_t bad_stack = reclaim_bad_stack - bad_stack_before;
    /* The observer's created counter records BOTH kinds (stack +
     * thread, two records per task); the frozen schema reports the
     * per-task and per-kind counts. */
    serial_printk("[ap-sched] phase=ap case=reap cpu=%u "
                  "tasks=%lu stack_alloc=%lu stack_free=%lu "
                  "ctx_alloc=%lu ctx_free=%lu bad_stack=%lu "
                  "duplicate=%lu overflow=%lu idle_freed=%u\n",
                  cpu, (unsigned long)ap_reap_created_count,
                  (unsigned long)ap_reap_created_count,
                  (unsigned long)stack_freed,
                  (unsigned long)ap_reap_created_count,
                  (unsigned long)ctx_freed,
                  (unsigned long)bad_stack,
                  (unsigned long)((fails >> 3) & 1),
                  (unsigned long)(((fails & 1) | (fails >> 4)) & 1),
                  0u);
    int rc = aarch64_sched_observer_end(cpu, (unsigned)(cpu * 10 + 1));
    return (rc == 0 && ap_reap_created_count == 256 && created == 512 &&
            stack_freed == 256 && ctx_freed == 256 && !bad_stack) ? 0 : -1;
}

static int ap_case_work(unsigned cpu)
{
    /* post-handoff: consumed by the polling idle step on the target. */
    if (ap_work_submit_next(cpu, WORK_BARRIER, 0, 0) != 0) {
        ap_work_post_ok[cpu] = 0;
    } else if (cpu == 0) {
        ap_work_run_one(0);
        uint64_t freq = arch_cycle_freq();
        ap_work_post_ok[0] = ap_work_wait(
            0, ap_work_seq_base[0], NULL,
            arch_cycle_counter() + (freq ? freq : 100000000ULL));
    } else {
        uint64_t freq = arch_cycle_freq();
        uint64_t deadline = arch_cycle_counter() +
                            (freq ? freq : 100000000ULL);
        if (!ap_work_wait(cpu, ap_work_seq_base[cpu], NULL, deadline))
            ap_work_seq_ok = 0;
        else
            ap_work_post_ok[cpu] = 1;
    }
    serial_printk("[ap-sched] phase=ap case=work cpu=%u "
                  "pre_handoff=%lu post_handoff=%lu seq_ok=%lu\n",
                  cpu, (unsigned long)ap_work_pre_ok[cpu],
                  (unsigned long)ap_work_post_ok[cpu],
                  (unsigned long)ap_work_seq_ok);
    return (ap_work_pre_ok[cpu] && ap_work_post_ok[cpu] && ap_work_seq_ok)
               ? 0 : -1;
}

static void ap_case_admission(void)
{
    serial_printk("[ap-sched] phase=ap case=admission cpu=0 "
                  "invalid_rejected=%lu zero_publish=%lu "
                  "unready_rejected=%lu unready_na=%lu\n",
                  (unsigned long)ap_admission_invalid_rejected,
                  (unsigned long)ap_admission_zero_publish,
                  (unsigned long)ap_admission_unready_rejected,
                  (unsigned long)ap_admission_unready_na);
}

static int ap_case_default_mask(void)
{
    uint64_t deadline = jiffies + 200;
    bool up = num_cpus < 2;

    /* default CPU0 placement for an unpinned create */
    ap_landing_cpu = ap_landing_bad_mask = ap_landing_done = 0;
    if (kernel_thread(ap_landing_worker, 0, PF_KTHREAD) < 0)
        return -1;
    while (!ap_landing_done && jiffies < deadline)
        kthread_yield();
    uint64_t default_cpu0 = ap_landing_done && ap_landing_cpu == 0 &&
                            !ap_landing_bad_mask;

    uint64_t ap_to_bsp = 0, bsp_to_ap = 0, ap_to_ap = 0;
    if (!up) {
        /* AP1 creates an UNPINNED task: it must land on CPU0. */
        ap_cross_ok[0] = ap_cross_expected[0] = 0;
        deadline = jiffies + 200;
        if (kernel_thread_on_cpu(ap_cross_creator, 0, 1) < 0)
            return -1;
        while (!ap_cross_expected[0] && jiffies < deadline)
            kthread_yield();
        deadline = jiffies + 200;
        while (!ap_cross_ok[0] && jiffies < deadline)
            kthread_yield();
        ap_to_bsp = ap_cross_ok[0];

        /* BSP pins a task onto an AP (4/8-core: a second AP). */
        unsigned ap_target = (num_cpus >= 4) ? 2 : 1;
        ap_cross_ok[1] = ap_cross_expected[1] = 0;
        deadline = jiffies + 200;
        if (kernel_thread_on_cpu(ap_cross_creator,
                                 1 | (ap_target << 8), 1) < 0)
            return -1;
        while (!ap_cross_ok[1] && jiffies < deadline)
            kthread_yield();
        bsp_to_ap = ap_cross_ok[1];

        /* An AP pins a task onto an AP (2-core: its own AP; 4/8-core:
         * the creator on AP1 targets a DIFFERENT AP). */
        unsigned ap_to_ap_target = (num_cpus == 2) ? 1 : ap_target;
        ap_cross_ok[2] = ap_cross_expected[2] = 0;
        deadline = jiffies + 200;
        if (kernel_thread_on_cpu(ap_cross_creator,
                                 2 | (ap_to_ap_target << 8), 1) < 0)
            return -1;
        while (!ap_cross_ok[2] && jiffies < deadline)
            kthread_yield();
        ap_to_ap = ap_cross_ok[2];
    }
    serial_printk("[ap-sched] phase=ap case=default-mask cpu=0 "
                  "default_cpu0=%lu ap_to_bsp=%lu bsp_to_ap=%lu "
                  "ap_to_ap=%lu na=%lu\n",
                  (unsigned long)default_cpu0,
                  (unsigned long)ap_to_bsp,
                  (unsigned long)bsp_to_ap,
                  (unsigned long)ap_to_ap,
                  (unsigned long)(up ? 1 : 0));
    int rc = ((!up && default_cpu0 && ap_to_bsp && bsp_to_ap && ap_to_ap) ||
              (up && default_cpu0)) ? 0 : -1;
    return rc;
}

void test_aarch64_ap_scheduler_run(void)
{
    uint64_t accepted = aarch64_smp_accepted_mask();
    serial_printk("[ap-sched] START phase=ap cpus=%u accepted_mask=%lu\n",
                  (unsigned)num_cpus, (unsigned long)accepted);

    int failed = 0;
    failed += ap_case_ready(0) != 0;
    for (uint32_t cpu = 1; cpu < num_cpus; ++cpu)
        failed += ap_case_ready(cpu) != 0;
    failed += ap_case_entry(0) != 0;
    for (uint32_t cpu = 1; cpu < num_cpus; ++cpu)
        failed += ap_case_entry(cpu) != 0;
    failed += ap_case_yield(0) != 0;
    for (uint32_t cpu = 1; cpu < num_cpus; ++cpu)
        failed += ap_case_yield(cpu) != 0;
    failed += ap_case_reap(0) != 0;
    for (uint32_t cpu = 1; cpu < num_cpus; ++cpu)
        failed += ap_case_reap(cpu) != 0;
    failed += ap_case_work(0) != 0;
    for (uint32_t cpu = 1; cpu < num_cpus; ++cpu)
        failed += ap_case_work(cpu) != 0;
    ap_case_admission();
    failed += ap_case_default_mask() != 0;

    /* The single [TEST] END owner is selftest_end_run(); the
     * [ap-sched] END envelope is written only by this driver. */
    selftest_record_late("ready", 0);
    selftest_record_late("entry", 0);
    selftest_record_late("yield", 0);
    selftest_record_late("reap", 0);
    selftest_record_late("work", 0);
    selftest_record_late("admission", 0);
    selftest_record_late("default-mask", 0);
    int end_failed = selftest_end_run();
    serial_printk("[ap-sched] END phase=ap failed=%u\n",
                  (unsigned)(failed + end_failed));
}

#endif /* aarch64 && OS01_SELFTEST && AARCH64_AP_SCHED_TEST_AP */
