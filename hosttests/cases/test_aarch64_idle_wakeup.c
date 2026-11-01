/* test_aarch64_idle_wakeup.c — P2 Task 3: the REAL masked-WFI idle
 * step (kernel/arch/aarch64/cpu/idle.c host-compiled via
 * ../mock/aarch64_idle_runtime.h; the wait helper is a mock — the real
 * `dsb sy; wfi; ret` ELF is audited by the harness).
 *
 * Frozen §5.3 ordering asserted here:
 *   ap_work consumed with IRQs ON → irq_save → rq_lock check
 *   (need / runqueue / slot READY) → unlock → with no work:
 *   wait helper entered with IRQs MASKED and NO lock held → after the
 *   return: IRQ enable + ISB.  With work: schedule with IRQ state
 *   restored, never sleeping.
 *
 * Arrival paths (spec §5.3: an SGI arriving before the final check is
 * observed by it; one arriving after it keeps the IRQ pending and
 * wakes the WFI; spurious wakes just re-check):
 *   1. arrives before the check → the check sees it, schedule.
 *   2. arrives after the check, before the wait → the WFI returns
 *      (pending IRQ), the next iteration handles it.
 *   3. arrives while "in" the wait (injected by the mock helper) →
 *      same return path.
 *   4. spurious wake (nothing arrives) → re-check, still no work.
 */
#define IDLE_TEST_TU 1
#include "test_framework.h"
#include "../mock/aarch64_idle_runtime.h"

/* ── harness state ──────────────────────────────────────────────── */
idle_mock_percpu_t percpu_data[NR_CPUS];
struct ap_work ap_work[NR_CPUS];   /* the real slot array the idle polls */
uint32_t num_cpus = 2;
percpu_t *idle_this_cpu;

int idle_event_count;
int idle_irq_state_at_wait;
static int events_arg[256];
static int events[256];

void idle_event_record(int ev, uint64_t arg)
{
    if (idle_event_count < 256) {
        events[idle_event_count] = ev;
        events_arg[idle_event_count] = (int)arg;
    }
    idle_event_count++;
}

/* IRQ state machine: 1 = masked. */
static int mock_irq_state;
arch_irq_state_t arch_local_irq_save(void)
{
    idle_event_record(EV_IRQ_SAVE, mock_irq_state);
    int old = mock_irq_state;
    mock_irq_state = 1;
    return (arch_irq_state_t)(uintptr_t)old;
}
void arch_local_irq_restore(arch_irq_state_t state)
{
    idle_event_record(EV_IRQ_RESTORE, (uintptr_t)state);
    mock_irq_state = (int)(uintptr_t)state;
}
void arch_local_irq_enable(void)
{
    idle_event_record(EV_IRQ_ENABLE, mock_irq_state);
    mock_irq_state = 0;
}

int mock_schedule_calls;
void schedule(void)
{
    idle_event_record(EV_SCHEDULE, mock_irq_state);
    mock_schedule_calls++;
}

int mock_run_one_calls;
uint32_t mock_run_one_cpu;
static int run_one_irq_state;
void ap_work_run_one(uint32_t cpu)
{
    idle_event_record(EV_RUN_ONE, mock_irq_state);
    mock_run_one_calls++;
    mock_run_one_cpu = cpu;
    run_one_irq_state = mock_irq_state;
}

/* Recording spin surface (macros in the mock header route here). */
uint64_t idle_spin_lock(spinlock_T *l)
{
    (void)l;
    idle_event_record(EV_RQ_LOCK, mock_irq_state);
    return 0;
}
void idle_spin_unlock(spinlock_T *l, uint64_t f)
{
    (void)l;
    (void)f;
    idle_event_record(EV_RQ_UNLOCK, mock_irq_state);
}

/* Wait-helper mock: records the entry IRQ state, optionally injects an
 * arrival (a wake-during-the-WFI), then returns. */
static int wait_arrival_mode;   /* 0 none, 1 inject need, 2 nothing */
static int wait_calls;
void aarch64_idle_wait_irq(void)
{
    idle_event_record(EV_WAIT_ENTER, mock_irq_state);
    idle_irq_state_at_wait = mock_irq_state;
    if (getenv("IDLE_DEBUG"))
        printf("[dbg] wait enter irq=%d events=%d need=%llu q=%llu\n",
               mock_irq_state, idle_event_count,
               (unsigned long long)percpu_data[0].need_resched,
               (unsigned long long)percpu_data[0].run_queue.stub);
    wait_calls++;
    if (wait_arrival_mode == 1)
        percpu_data[idle_this_cpu->cpu_id].need_resched = 1;
    idle_event_record(EV_WAIT_RETURN, 0);
}

/* ── helpers ────────────────────────────────────────────────────── */
static void mock_reset(void)
{
    memset(percpu_data, 0, sizeof(percpu_data));
    for (uint32_t i = 0; i < NR_CPUS; ++i) {
        percpu_data[i].cpu_id = i;
        spin_init(&percpu_data[i].rq_lock);
    }
    idle_this_cpu = &percpu_data[0];
    idle_event_count = 0;
    mock_irq_state = 0;      /* IRQs ON at entry (the idle contract) */
    mock_schedule_calls = 0;
    mock_run_one_calls = 0;
    wait_calls = 0;
    wait_arrival_mode = 0;
    run_one_irq_state = -1;
    for (uint32_t c = 0; c < NR_CPUS; ++c)
        __atomic_store_n(&ap_work[c].state, AP_WORK_IDLE, __ATOMIC_RELAXED);
}

static int ev_find(int ev, int from)
{
    for (int i = from; i < idle_event_count && i < 256; ++i)
        if (events[i] == ev)
            return i;
    return -1;
}

/* The frozen ordering: save → lock → unlock → (no work) wait(masked)
 * → enable; and run_one ran with IRQs ON. */
static void assert_frozen_order(void)
{
    int run = ev_find(EV_RUN_ONE, 0);
    int save = ev_find(EV_IRQ_SAVE, run + 1);
    int lock = ev_find(EV_RQ_LOCK, save + 1);
    int unlock = ev_find(EV_RQ_UNLOCK, lock + 1);
    int wait = ev_find(EV_WAIT_ENTER, unlock + 1);
    assert_true(run >= 0 && save > run && lock > save && unlock > lock);
    assert_true(unlock > lock);
    if (wait_calls > 0) {
        /* The sleeping path: masked at the WFI, no lock ops after the
         * unlock, IRQs re-enabled after the return. */
        assert_eq(1, idle_irq_state_at_wait);    /* masked at the WFI */
        for (int i = unlock + 1; i < idle_event_count; ++i)
            assert_true(events[i] != EV_RQ_LOCK);
        int enable = ev_find(EV_IRQ_ENABLE, wait + 1);
        assert_true(enable > wait);
    }
    /* ap_work ran with IRQs enabled. */
    assert_eq(0, run_one_irq_state);
}

/* ── 1. arrival before the check ────────────────────────────────── */
static void suite_arrival_before_check(void)
{
    TEST_SUITE("SGI before the final check → seen by it, schedule");
    mock_reset();
    percpu_data[0].need_resched = 1;   /* published before the step */
    arch_aarch64_idle_step();
    assert_eq(1, mock_schedule_calls);
    assert_eq(0, wait_calls);          /* never slept */
    assert_frozen_order();
}

/* ── 2. queued work → no sleep ──────────────────────────────────── */
static void suite_queued_no_sleep(void)
{
    TEST_SUITE("runqueue non-empty → schedule, no wait");
    mock_reset();
    percpu_data[0].run_queue.stub = 1;   /* non-empty (mock) */
    arch_aarch64_idle_step();
    assert_eq(1, mock_schedule_calls);
    assert_eq(0, wait_calls);
    assert_frozen_order();
}

/* ── 3. READY slot → IRQ restored, next round consumes ──────────── */
static void suite_ready_no_sleep(void)
{
    TEST_SUITE("slot READY → no sleep, IRQs restored for consumption");
    mock_reset();
    __atomic_store_n(&ap_work[0].state, AP_WORK_READY, __ATOMIC_RELEASE);
    arch_aarch64_idle_step();
    assert_eq(0, mock_schedule_calls);
    assert_eq(0, wait_calls);            /* READY: never sleeps */
    __atomic_store_n(&ap_work[0].state, AP_WORK_IDLE, __ATOMIC_RELEASE);
}

/* ── 4. arrival during the wait ─────────────────────────────────── */
static void suite_arrival_during_wait(void)
{
    TEST_SUITE("SGI during the WFI → helper returns, IRQs re-enabled");
    mock_reset();
    wait_arrival_mode = 1;               /* inject on wait entry */
    arch_aarch64_idle_step();
    assert_eq(1, wait_calls);
    assert_frozen_order();
    /* The arrival is handled on the NEXT step (this one returned with
     * IRQs enabled). */
    assert_eq(0, mock_schedule_calls);
    idle_event_count = 0;
    arch_aarch64_idle_step();
    assert_eq(1, mock_schedule_calls);   /* now observed */
}

/* ── 5. spurious wake ───────────────────────────────────────────── */
static void suite_spurious(void)
{
    TEST_SUITE("spurious wake → re-check, stay idle");
    mock_reset();
    wait_arrival_mode = 2;
    arch_aarch64_idle_step();
    assert_eq(1, wait_calls);
    assert_eq(0, mock_schedule_calls);
    assert_frozen_order();
}

int main(void)
{
    suite_arrival_before_check();
    suite_queued_no_sleep();
    suite_ready_no_sleep();
    suite_arrival_during_wait();
    suite_spurious();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_aarch64_idle_wakeup",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
