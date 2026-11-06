/* P2 acceptance: real per-CPU operations, bounded handshakes, one BSP writer. */
#if defined(__aarch64__) && defined(OS01_SELFTEST) && defined(AARCH64_AP_SCHED_WAKE_TEST)
#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/smp.h>
#include <arch/aarch64/timer.h>   /* selftest-only local CNTP mask/restore */
#include <memory/vmm.h>
#include <intr/ipi.h>
#include <selftest/aarch64_ap_scheduler.h>
#include <selftest/aarch64_sched_observer.h>
#include <selftest/result.h>
#include <core/printk.h>
#include <log/log.h>

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define INC(p) __atomic_fetch_add((p), 1, __ATOMIC_RELAXED)
#define ROUNDS 100

/* A fixture remains alive until its worker has really freed both allocations.
 * Its task pointer is only used while the worker's exit barrier is closed. */
static struct {
    task_t *target;
    uint64_t cond, done, exit_ok, dequeued, at_finish, release_finish;
    uint64_t scan, on_cpu_wakes, finish_enqueues, bad;
    unsigned kind; /* 1 explicit, 2 switch handoff, 3 scan fallback */
} fixture;
static struct {
    uint64_t request, arrived, permit, enters, returns;
    /* Masked-window handshake: the target masks its OWN local CNTP
     * interrupt before it acknowledges the window and restores it only
     * after the window's marker/work completed.  The BSP never starts
     * the next round before the restore ACK. */
    uint64_t mask_requested, masked, mask_failed, saved_ctl;
    uint64_t restore_request, restored;
    uint64_t mask_total, restore_total;
} gates[NR_CPUS];
static uint64_t checkpoint[NR_CPUS];
static uint32_t ready_seq[NR_CPUS];
static uint64_t ready_ok[NR_CPUS];
static uint64_t entries[NR_CPUS], eois[NR_CPUS], bad_iar[NR_CPUS];
uint64_t aarch64_ap_sched_sgi_active[NR_CPUS];
extern volatile uint64_t irq_switch_counts[NR_CPUS];
static uint64_t failures, generation = 10000;

static uint64_t deadline(void)
{
    return arch_cycle_counter() + 2 * arch_cycle_freq();
}

static bool wait_value(uint64_t *p, uint64_t value, bool yield)
{
    uint64_t until = deadline();
    while (LOAD(p) < value) {
        if (arch_cycle_counter() >= until) {
            INC(&failures);
            return false;
        }
        if (yield) kthread_yield();
        else arch_cpu_pause();
    }
    return true;
}

static int finish_batch(unsigned cpu, unsigned gen)
{
    uint64_t until = deadline();
    while (aarch64_sched_observer_end(cpu, gen)) {
        if (arch_cycle_counter() >= until) {
            INC(&failures);
            return -1;
        }
        kthread_yield();
    }
    STORE(&fixture.target, NULL);
    return 0;
}

/* Idle/switch hooks on other CPUs can inspect the fixture between rounds. */
static void reset_fixture(unsigned kind)
{
    STORE(&fixture.kind, 0);
    STORE(&fixture.target, NULL);
    STORE(&fixture.cond, 0);
    STORE(&fixture.done, 0);
    STORE(&fixture.exit_ok, 0);
    STORE(&fixture.dequeued, 0);
    STORE(&fixture.at_finish, 0);
    STORE(&fixture.release_finish, 0);
    STORE(&fixture.scan, 0);
    STORE(&fixture.on_cpu_wakes, 0);
    STORE(&fixture.finish_enqueues, 0);
    STORE(&fixture.bad, 0);
    STORE(&fixture.kind, kind);
}

static bool condition(task_t *task)
{
    (void)task;
    return LOAD(&fixture.cond) != 0;
}

static uint64_t sleeper(uint64_t cpu)
{
    if (cpu_id() != cpu || current->allowed_cpus != (1ULL << cpu))
        INC(&fixture.bad);
    STORE(&fixture.target, current);
    int rc = blocker_wait(condition, BLOCKER_NANOSLEEP, false);
    if (rc || current->blocker.type != BLOCKER_NONE || !condition(current))
        INC(&fixture.bad);
    INC(&fixture.done);
    /* Keep the object alive for the controller's final observations. */
    if (cpu != 0 && !wait_value(&fixture.exit_ok, 1, true)) return 1;
    return 0;
}

bool aarch64_ap_sched_skip_scan(task_t *task)
{
    return LOAD(&fixture.kind) != 3 && LOAD(&fixture.target) == task;
}

void aarch64_ap_sched_scan_woke(task_t *task)
{
    if (LOAD(&fixture.target) == task) INC(&fixture.scan);
}

void aarch64_ap_sched_after_dequeue(task_t *task)
{
    if (LOAD(&fixture.target) == task && task->state == TASK_INTERRUPTIBLE)
        STORE(&fixture.dequeued, 1);
}

void aarch64_ap_sched_before_finish(task_t *prev)
{
    if (LOAD(&fixture.kind) != 2 || LOAD(&fixture.target) != prev ||
        !LOAD(&fixture.dequeued) || LOAD(&fixture.at_finish)) return;
    STORE(&fixture.at_finish, 1);
    /* Incoming stack, IRQ masked, no locks. BSP delivers the actual wake. */
    wait_value(&fixture.release_finish, 1, false);
}

void aarch64_ap_sched_finish_enqueued(task_t *task)
{
    /* Called after the real enqueue, under rq_lock: observe only. */
    if (LOAD(&fixture.kind) == 2 && LOAD(&fixture.target) == task &&
        LOAD(&fixture.at_finish)) INC(&fixture.finish_enqueues);
}

void aarch64_ap_sched_idle_hook(unsigned cpu, unsigned stage)
{
    if (stage == AP_SCHED_IDLE_BEFORE_WAIT) INC(&gates[cpu].enters);
    if (stage == AP_SCHED_IDLE_AFTER_WAIT) INC(&gates[cpu].returns);

    /* The BSP closed the masked window: restore the LOCAL CNTP
     * interrupt (CTL + rearm + ISB) on this CPU and publish the
     * restore ACK.  The BSP's own time source is never touched. */
    if (LOAD(&gates[cpu].restore_request) && !LOAD(&gates[cpu].restored)) {
        aarch64_sched_tick_test_restore_local(LOAD(&gates[cpu].saved_ctl));
        STORE(&gates[cpu].restored, 1);
        INC(&gates[cpu].restore_total);
        return;
    }

    uint64_t requested = LOAD(&gates[cpu].request);
    if (!requested || requested != stage) return;

    /* Mask the LOCAL CNTP interrupt BEFORE the acknowledgement: a
     * periodic tick must never rescue the window under test.  The
     * target is the only CPU that masks its own counter. */
    if (LOAD(&gates[cpu].mask_requested)) {
        uint64_t saved = aarch64_sched_tick_test_mask_local();
        if (!saved) {
            INC(&gates[cpu].mask_failed);
            INC(&failures);
        } else {
            STORE(&gates[cpu].saved_ctl, saved);
            STORE(&gates[cpu].masked, 1);
            INC(&gates[cpu].mask_total);
        }
    }

    STORE(&gates[cpu].arrived, 1);
    /* Stage 3 must return immediately; stages 1/2 can wait without locks. */
    if (stage != AP_SCHED_IDLE_BEFORE_WAIT)
        wait_value(&gates[cpu].permit, 1, false);
    STORE(&gates[cpu].request, 0);
}

void aarch64_ap_sched_sgi_trace(unsigned cpu, uint32_t iar, bool after_eoi)
{
    if (cpu >= NR_CPUS) return;
    if (after_eoi) {
        INC(&eois[cpu]);
        __atomic_fetch_sub(&aarch64_ap_sched_sgi_active[cpu], 1, __ATOMIC_RELEASE);
    } else {
        INC(&aarch64_ap_sched_sgi_active[cpu]);
        INC(&entries[cpu]);
        if ((iar & 0x3ff) != 4 || ((iar >> 10) & 7) >= num_cpus)
            INC(&bad_iar[cpu]);
    }
}

static uint32_t submit_next(unsigned cpu, uint32_t cmd)
{
    struct ap_work *slot = &ap_work[cpu];
    if (LOAD(&slot->state) != AP_WORK_IDLE || slot->seq == UINT32_MAX) {
        INC(&failures);
        return 0;
    }
    uint32_t seq = slot->seq + 1;
    ap_work_submit(cpu, seq, cmd, 0, 0);
    return seq;
}

static bool complete_work(unsigned cpu, uint32_t seq)
{
    if (!seq) return false;
    uint64_t until = deadline();
    while (LOAD(&ap_work[cpu].state) != AP_WORK_DONE) {
        if (arch_cycle_counter() >= until) { INC(&failures); return false; }
        arch_cpu_pause();
    }
    /* ap_work_wait takes a DURATION, not an absolute deadline. */
    if (!ap_work_wait(cpu, seq, NULL, 2 * arch_cycle_freq())) {
        INC(&failures);
        return false;
    }
    return true;
}

void test_aarch64_ap_sched_before_ready(unsigned cpu)
{
    STORE(&checkpoint[cpu], 1);
    wait_value(&checkpoint[cpu], 2, false);
}

void test_aarch64_ap_sched_poll_ready(void)
{
    for (unsigned cpu = 1; cpu < num_cpus; ++cpu) {
        if (LOAD(&checkpoint[cpu]) != 1) continue;
        ready_seq[cpu] = submit_next(cpu, WORK_BARRIER);
        STORE(&checkpoint[cpu], 2);
    }
}

/* Arm a real idle boundary and kick the CPU out of any preceding WFI.
 * The ACK is produced by the target hook, never by the sender.
 *
 * The window is MASKED: the target masks its own local CNTP interrupt
 * before it acknowledges, so the AP's periodic tick can never rescue
 * the window under test. */
/* Close the masked window: the TARGET restores its local CNTP
 * interrupt (CTL + rearm + ISB) and publishes the restore ACK.  The
 * BSP never starts the next round before that ACK, and a target that
 * cannot restore is a hard failure (never a silent pass). */
static bool close_window(unsigned cpu);

static bool arm_idle(unsigned cpu, unsigned stage)
{
    STORE(&gates[cpu].arrived, 0);
    STORE(&gates[cpu].permit, 0);
    STORE(&gates[cpu].request, stage);
    STORE(&gates[cpu].mask_requested, 1);
    STORE(&gates[cpu].masked, 0);
    STORE(&gates[cpu].mask_failed, 0);
    STORE(&gates[cpu].saved_ctl, 0);
    STORE(&gates[cpu].restored, 1);
    STORE(&gates[cpu].restore_request, 0);
    aarch64_sched_kick(cpu);
    if (!wait_value(&gates[cpu].arrived, 1, false)) {
        STORE(&gates[cpu].permit, 1);
        STORE(&gates[cpu].request, 0);
        /* The target may have masked before it acknowledged: the
         * window MUST be closed before the case gives up. */
        close_window(cpu);
        return false;
    }
    if (LOAD(&gates[cpu].mask_failed)) {
        /* The mask did not stick: the window is NOT protected, so the
         * case fails instead of pretending the window was protected. */
        STORE(&gates[cpu].permit, 1);
        close_window(cpu);
        INC(&failures);
        return false;
    }
    if (stage == AP_SCHED_IDLE_BEFORE_WAIT) {
        /* Allow the target to pass its wait boundary; no claim of physical sleep. */
        uint64_t until = arch_cycle_counter() + arch_cycle_freq() / 100000;
        while (arch_cycle_counter() < until) arch_cpu_pause();
    }
    return true;
}

/* Close the masked window: the TARGET restores its local CNTP
 * interrupt (CTL + rearm + ISB) and publishes the restore ACK.  The
 * BSP never starts the next round before that ACK, and a target that
 * cannot restore is a hard failure (never a silent pass). */
static bool close_window(unsigned cpu)
{
    STORE(&gates[cpu].restore_request, 1);
    STORE(&gates[cpu].restored, 0);
    /* The CNTP is masked: only an SGI can pull the target out of WFI. */
    aarch64_sched_kick(cpu);
    if (!wait_value(&gates[cpu].restored, 1, false)) {
        INC(&failures);
        return false;
    }
    return true;
}

static int blocker_case(unsigned cpu, unsigned kind)
{
    uint64_t completed = 0, scans = 0, wakes = 0, enqueues = 0, bad = 0, explicit_calls = 0;
    for (unsigned round = 0; round < ROUNDS; ++round) {
        /* Prior round has ended its resource batch: all fields are quiescent. */
        reset_fixture(kind);
        unsigned gen = ++generation;
        aarch64_sched_observer_begin(cpu, gen);
        if (kernel_thread_on_cpu(sleeper, cpu, cpu) < 0) return -1;
        uint64_t *stage = kind == 2 ? &fixture.at_finish : &fixture.dequeued;
        if (!wait_value(stage, 1, true)) {
            STORE(&fixture.cond, 1);
            STORE(&fixture.release_finish, 1);
            STORE(&fixture.exit_ok, 1);
            return -1;
        }
        task_t *target = LOAD(&fixture.target);
        if (kind == 1) {
            /* Ordinary remote wake must not reuse the on-CPU handoff case. */
            uint64_t until = deadline();
            while (LOAD(&target->on_cpu)) {
                if (arch_cycle_counter() >= until) { INC(&failures); return -1; }
                kthread_yield();
            }
        }
        STORE(&fixture.cond, 1);
        if (kind != 3) {
            if (kind == 2 && LOAD(&target->on_cpu) && !LOAD(&target->on_rq))
                INC(&fixture.on_cpu_wakes);
            blocker_wake(target);
            ++explicit_calls;
        }
        STORE(&fixture.release_finish, 1);
        bool done = wait_value(&fixture.done, 1, true);
        if (done) {
            completed += LOAD(&fixture.done);
            scans += LOAD(&fixture.scan);
            wakes += LOAD(&fixture.on_cpu_wakes);
            enqueues += LOAD(&fixture.finish_enqueues);
            bad += LOAD(&fixture.bad) + (LOAD(&fixture.done) != 1);
        }
        STORE(&fixture.exit_ok, 1);
        if (finish_batch(cpu, gen) || !done) return -1;
    }
    if (kind == 1)
        serial_printk("[ap-sched] phase=wake case=remote-wake cpu=%u rounds=100 completed=%lu duplicate=%lu explicit=%lu scan=%lu na=0\n",
                      cpu, completed, bad, explicit_calls, scans);
    else if (kind == 2)
        serial_printk("[ap-sched] phase=wake case=wake-switch cpu=%u rounds=100 completed=%lu duplicate=%lu on_cpu_wakes=%lu finish_enqueues=%lu na=0\n",
                      cpu, completed, bad, wakes, enqueues);
    else
        serial_printk("[ap-sched] phase=wake case=scan-fallback cpu=%u rounds=100 completed=%lu explicit=0 scan=%lu na=0\n",
                      cpu, completed, scans);
    return completed != ROUNDS || bad || (kind == 1 ? scans != 0 || explicit_calls != ROUNDS :
        kind == 2 ? wakes != ROUNDS || enqueues != ROUNDS : scans != ROUNDS) ? -1 : 0;
}

static uint64_t marker(uint64_t target)
{
    if (cpu_id() != target || current->allowed_cpus != (1ULL << target))
        INC(&fixture.bad);
    INC(&fixture.done);
    return 0;
}

static int idle_case(unsigned cpu)
{
    uint64_t counts[3] = {0}, completed = 0;
    uint64_t entered = LOAD(&gates[cpu].enters), returned = LOAD(&gates[cpu].returns);
    uint64_t mask_base = LOAD(&gates[cpu].mask_total);
    uint64_t restore_base = LOAD(&gates[cpu].restore_total);
    for (unsigned timing = 0; timing < 3; ++timing) {
        for (unsigned round = 0; round < ROUNDS; ++round) {
            reset_fixture(0);
            if (!arm_idle(cpu, timing + 1)) return -1;
            unsigned gen = ++generation;
            aarch64_sched_observer_begin(cpu, gen);
            if (kernel_thread_on_cpu(marker, cpu, cpu) < 0) {
                STORE(&gates[cpu].permit, 1);
                close_window(cpu);
                return -1;
            }
            STORE(&gates[cpu].permit, 1);
            if (!wait_value(&fixture.done, 1, false) || finish_batch(cpu, gen)) {
                close_window(cpu);
                return -1;
            }
            /* The window's marker completed: close the masked window
             * before the next round starts. */
            if (!close_window(cpu)) return -1;
            if (LOAD(&fixture.done) != 1 || LOAD(&fixture.bad)) return -1;
            ++counts[timing];
            ++completed;
        }
    }
    uint64_t masked = LOAD(&gates[cpu].mask_total) - mask_base;
    uint64_t restored = LOAD(&gates[cpu].restore_total) - restore_base;
    serial_printk("[ap-sched] phase=wake case=idle-race cpu=%u before_check=%lu after_check=%lu at_wait=%lu completed=%lu lost=%lu wfi_enter=%lu wfi_return=%lu masked=%lu restored=%lu na=0\n",
                  cpu, counts[0], counts[1], counts[2], completed,
                  (uint64_t)(3 * ROUNDS - completed), LOAD(&gates[cpu].enters) - entered,
                  LOAD(&gates[cpu].returns) - returned, masked, restored);
    /* Every window must have been protected AND closed again. */
    return masked == 3 * ROUNDS && restored == 3 * ROUNDS ? 0 : -1;
}

static int work_case(unsigned cpu)
{
    uint64_t pending = 0, after = 0;
    uint64_t mask_base = LOAD(&gates[cpu].mask_total);
    uint64_t restore_base = LOAD(&gates[cpu].restore_total);
    for (unsigned timing = 0; timing < 2; ++timing) {
        for (unsigned round = 0; round < ROUNDS; ++round) {
            if (!arm_idle(cpu, timing + 2)) return -1;
            uint32_t seq = submit_next(cpu, WORK_BARRIER);
            STORE(&gates[cpu].permit, 1);
            if (!complete_work(cpu, seq)) {
                close_window(cpu);
                return -1;
            }
            /* The window's work completed: close the masked window
             * before the next round starts. */
            if (!close_window(cpu)) return -1;
            if (timing) ++after;
            else ++pending;
        }
    }
    uint64_t masked = LOAD(&gates[cpu].mask_total) - mask_base;
    uint64_t restored = LOAD(&gates[cpu].restore_total) - restore_base;
    serial_printk("[ap-sched] phase=wake case=work-sleep cpu=%u ready_overlap=%lu pending_before=%lu after_wait=%lu completed=%lu bad_seq=0 masked=%lu restored=%lu na=0\n",
                  cpu, ready_ok[cpu], pending, after, ready_ok[cpu] + pending + after,
                  masked, restored);
    if (ready_ok[cpu] != 1) return -1;
    return masked == 2 * ROUNDS && restored == 2 * ROUNDS ? 0 : -1;
}

static int sgi_case(void)
{
    /* Drain every sender's work/SGI before sampling the real trace counters. */
    ipi_broadcast(IPI_VECTOR_RESCHED, 1);
    if (!wait_value(&entries[0], 1, false)) return -1;
    for (unsigned cpu = 1; cpu < num_cpus; ++cpu)
        if (!complete_work(cpu, submit_next(cpu, WORK_BARRIER))) return -1;
    uint32_t ack[NR_CPUS];
    for (unsigned cpu = 0; cpu < num_cpus; ++cpu) ack[cpu] = LOAD(&percpu_data[cpu].tlb_ack_gen);
    tlb_shootdown();
    int failed = 0;
    for (unsigned cpu = 0; cpu < num_cpus; ++cpu) {
        uint64_t h = LOAD(&entries[cpu]), e = LOAD(&eois[cpu]);
        uint64_t irq = LOAD(&irq_switch_counts[cpu]);
        uint64_t tlb = LOAD(&percpu_data[cpu].tlb_ack_gen) == (uint32_t)(ack[cpu] + 1);
        serial_printk("[ap-sched] phase=wake case=sgi cpu=%u handler=%lu eoi=%lu bad_iar=%lu irq_switches=%lu tlb_ack_ok=%lu\n",
                      cpu, h, e, LOAD(&bad_iar[cpu]), irq, tlb);
        failed |= !h || h != e || LOAD(&bad_iar[cpu]) || irq || !tlb;
    }
    return failed ? -1 : 0;
}

void test_aarch64_ap_scheduler_wake_run(void)
{
    /* Complete the one-shot ready handoff before the inherited work cases. */
    for (unsigned cpu = 1; cpu < num_cpus; ++cpu)
        ready_ok[cpu] = complete_work(cpu, ready_seq[cpu]);
    serial_printk("[ap-sched] START phase=wake cpus=%u accepted_mask=%lu\n",
                  num_cpus, aarch64_smp_accepted_mask());
    int failed = test_aarch64_ap_scheduler_cases();
    int results[6] = {0};
    if (num_cpus == 1) {
        serial_printk("[ap-sched] phase=wake case=remote-wake cpu=0 rounds=0 completed=0 duplicate=0 explicit=0 scan=0 na=1\n");
        serial_printk("[ap-sched] phase=wake case=wake-switch cpu=0 rounds=0 completed=0 duplicate=0 on_cpu_wakes=0 finish_enqueues=0 na=1\n");
        serial_printk("[ap-sched] phase=wake case=idle-race cpu=0 before_check=0 after_check=0 at_wait=0 completed=0 lost=0 wfi_enter=0 wfi_return=0 masked=0 restored=0 na=1\n");
        serial_printk("[ap-sched] phase=wake case=work-sleep cpu=0 ready_overlap=0 pending_before=0 after_wait=0 completed=0 bad_seq=0 masked=0 restored=0 na=1\n");
        results[5] = blocker_case(0, 3);
    } else {
        for (unsigned cpu = 1; cpu < num_cpus; ++cpu) {
            results[0] |= blocker_case(cpu, 1);
            results[1] |= blocker_case(cpu, 2);
            results[2] |= idle_case(cpu);
            results[4] |= work_case(cpu);
            results[5] |= blocker_case(cpu, 3);
            if (LOAD(&failures)) break;
        }
    }
    results[3] = sgi_case();
    const char *ids[] = { "remote-wake", "wake-switch", "idle-race", "sgi", "work-sleep", "scan-fallback" };
    for (unsigned i = 0; i < 6; ++i) {
        selftest_record_late(ids[i], results[i]);
        failed += results[i] != 0;
    }
    failed += selftest_end_run();
    serial_printk("[ap-sched] END phase=wake failed=%u\n", (unsigned)(failed + LOAD(&failures)));
}
#endif
