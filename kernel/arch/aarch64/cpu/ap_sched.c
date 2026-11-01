/* aarch64 AP cooperative-scheduler handoff (P1 spec §3).
 *
 * The static idle storage is kernel-owned forever: NR_CPUS-1
 * STACK_SIZE-aligned task unions plus one thread context each.  The
 * unions never consume a PID and never enter the self-reap path.
 *
 * Release/acquire discipline:
 *   - ap_release_gate: BSP release-stores it open; APs acquire-load it
 *     through aarch64_ap_sched_released(), which ALSO checks the
 *     frozen admission bit so a late or never-ACKed AP can never
 *     enter.
 *   - scheduler_ok: the AP handoff release-stores it as the LAST step
 *     (after IRQ enable + ISB); the BSP wait acquire-loads it.
 */
#include <arch/aarch64/ap_sched.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/smp.h>
#include <arch/context.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/spinlock.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <core/panic.h>
#include <log/log.h>
#include <errno.h>
#include <string.h>
#ifdef AARCH64_AP_SCHED_WAKE_TEST
#include <selftest/aarch64_ap_scheduler.h>
#endif

_Static_assert(sizeof(task_t) + 256 <= STACK_SIZE,
               "AP idle union cannot host the task metadata reserve");

/* Permanent AP idle storage (BSS, covered by the kernel reservation):
 * indexed by logical cpu minus one — the BSP (cpu 0) uses
 * init_task_union. */
static union {
    task_t task;
    uint8_t stack[STACK_SIZE];
} ap_idle_union[NR_CPUS - 1] __attribute__((aligned(STACK_SIZE)));
static thread_t ap_idle_ctx[NR_CPUS - 1];
static bool ap_prepared[NR_CPUS - 1];

/* The one-way release gate.  0 = closed; 1 = open. */
static uint32_t ap_release_gate;

static task_t *ap_idle_task(uint32_t cpu)
{
    if (cpu == 0 || cpu >= NR_CPUS)
        return NULL;
    return &ap_idle_union[cpu - 1].task;
}

static thread_t *ap_idle_thread_ctx(uint32_t cpu)
{
    if (cpu == 0 || cpu >= NR_CPUS)
        return NULL;
    return &ap_idle_ctx[cpu - 1];
}

__attribute__((noreturn)) static void ap_sched_fatal(const char *reason,
                                                     uint32_t cpu)
{
    log_err("[ap-sched] FATAL: %s (cpu=%u)\n", reason ? reason : "?", cpu);
    for (;;)
        arch_cpu_halt();
}

/* ── BSP: construct the permanent AP idles (boot stack, no current) ── */
void aarch64_ap_sched_prepare(void)
{
    uint64_t accepted = aarch64_smp_accepted_mask();

    for (uint32_t cpu = 1; cpu < NR_CPUS; ++cpu) {
        if (!((accepted >> cpu) & 1ULL))
            continue;
        task_t *idle = ap_idle_task(cpu);
        thread_t *ctx = ap_idle_thread_ctx(cpu);
        if (!idle || !ctx || ap_prepared[cpu - 1])
            ap_sched_fatal("prepare: invalid or repeated AP slot", cpu);

        memset(idle, 0, sizeof(*idle));
        memset(ctx, 0, sizeof(*ctx));
        idle->thread = ctx;
        idle->stack_alloc_base = idle;   /* the union base IS the stack */
        idle->pid = 0;                   /* idles own no PID */
        idle->flags = PF_KTHREAD;
        idle->cpu = cpu;
        idle->allowed_cpus = 1ULL << cpu;
        idle->mm = &init_mm;
        idle->files = NULL;
        idle->fpu_save = NULL;
        idle->priority = 2;
        idle->state = TASK_RUNNING;
        idle->on_cpu = 0;                /* handoff publishes on_cpu=1 */
        idle->on_rq = 0;
        idle->in_schedule = 0;
        idle->blocker.type = BLOCKER_NONE;
        task_comm_set(idle, "idle");

        /* Independent list/rbtree nodes: never copy a BSP link.  The
         * idle joins the global task list exactly once, under the
         * same lock every other publisher uses. */
        list_init(&idle->list);
        uint64_t flags = spin_lock_irqsave(&task_list_lock);
        list_add_to_before(&init_task_union.task.list, &idle->list);
        spin_unlock_irqrestore(&task_list_lock, flags);

        /* First-entry context: the handoff C function continues into
         * the common polling idle loop. */
        uintptr_t base = (uintptr_t)idle;
        uintptr_t top = base + STACK_SIZE;
        uintptr_t low = (base + sizeof(task_t) + 256 + 15) &
                        ~(uintptr_t)15;
        ctx->x30 = (uintptr_t)idle_resume;
        ctx->sp = top - 16;
        ctx->sched_flags = 0;
        ctx->stack_base = base;
        ctx->stack_top = top;
        ctx->usable_low = low;

        ap_prepared[cpu - 1] = true;
        log_info("[ap-sched] prepared idle cpu=%u base=%lx top=%lx\n",
                 cpu, (unsigned long)base, (unsigned long)top);
    }
}

/* ── AP-side gate poll ─────────────────────────────────────────────── */
bool aarch64_ap_sched_released(uint32_t cpu)
{
    if (__atomic_load_n(&ap_release_gate, __ATOMIC_ACQUIRE) == 0)
        return false;
    /* The gate alone is not admission: a CPU outside the frozen boot
     * ACK mask stays in its old work loop forever. */
    return (aarch64_smp_accepted_mask() >> cpu) & 1ULL;
}

/* ── AP: leave the work stack through the validating trampoline ────── */
void aarch64_ap_sched_enter(uint32_t cpu)
{
    task_t *idle = ap_idle_task(cpu);
    thread_t *ctx = ap_idle_thread_ctx(cpu);
    if (!idle || !ctx || !ap_prepared[cpu - 1])
        ap_sched_fatal("enter: unprepared AP", cpu);
    arch_local_irq_disable();
    aarch64_ap_stack_enter(ctx->sp, ctx, cpu);
}

/* ── C handoff on the new idle task stack ──────────────────────────── */
void aarch64_ap_idle_handoff(uint32_t cpu)
{
    task_t *idle = ap_idle_task(cpu);
    percpu_t *rq = &percpu_data[cpu];

    /* current is derived from the SP mask: landing here on the idle
     * union means the trampoline installed the right stack. */
    if (cpu == 0 || cpu >= NR_CPUS || !idle ||
        arch_current_task() != idle || current != idle ||
        idle->thread != ap_idle_thread_ctx(cpu))
        ap_sched_fatal("handoff: wrong stack or context", cpu);

    /* Install the runqueue idle pointer, verify the metadata and that
     * this AP's runqueue is empty (never cleared here), then publish
     * the scheduling-visible state. */
    rq->idle = idle;
    if (!rbtree_empty(&rq->run_queue) || rq->nr_running)
        ap_sched_fatal("handoff: runqueue not empty", cpu);
    if (idle->cpu != cpu || idle->allowed_cpus != (1ULL << cpu) ||
        !(idle->flags & PF_KTHREAD) || idle->pid != 0 ||
        idle->mm != &init_mm || !idle->thread ||
        idle->thread->sp < idle->thread->usable_low ||
        idle->thread->sp >= idle->thread->stack_top)
        ap_sched_fatal("handoff: invalid idle metadata", cpu);
    idle->state = TASK_RUNNING;
    idle->on_cpu = 1;
    idle->on_rq = 0;
    idle->in_schedule = 0;

    /* IRQs on (with the ordering barrier) BEFORE the readiness
     * publication: a pending SGI must be takeable the instant another
     * CPU may consider this AP schedulable. */
#ifdef AARCH64_AP_SCHED_WAKE_TEST
    /* The ready-overlap checkpoint: the AP is on its legal idle stack,
     * IRQs still masked, scheduler_ok not yet published. */
    test_aarch64_ap_sched_before_ready(cpu);
#endif
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");
    __atomic_store_n(&rq->scheduler_ok, 1, __ATOMIC_RELEASE);
    log_info("[ap-sched] idle-ready cpu=%u scheduler_ok=1 irq=on\n", cpu);

    idle_resume();
    __builtin_unreachable();
}

/* ── BSP: open the gate, then wait for every accepted AP ───────────── */
void aarch64_ap_sched_release(void)
{
    uint64_t accepted = aarch64_smp_accepted_mask();

    /* IRQs on and no lock held while polling: the wait must not
     * deadlock against an AP that needs the BSP to make progress. */
    arch_irq_state_t irq_flags = arch_local_irq_save();
    arch_local_irq_enable();
    __atomic_store_n(&ap_release_gate, 1, __ATOMIC_RELEASE);

    for (uint32_t cpu = 1; cpu < NR_CPUS; ++cpu) {
        if (!((accepted >> cpu) & 1ULL))
            continue;
        uint64_t freq = arch_cycle_freq();
        uint64_t deadline = arch_cycle_counter() + (freq ? 2 * freq : 0);
        if (!freq)
            deadline = arch_cycle_counter() + 100000000ULL; /* ~2s guard */
        while (!__atomic_load_n(&percpu_data[cpu].scheduler_ok,
                                __ATOMIC_ACQUIRE)) {
            if (arch_cycle_counter() > deadline)
                ap_sched_fatal("AP scheduler_ok deadline (2s)", cpu);
            arch_cpu_pause();
#ifdef AARCH64_AP_SCHED_WAKE_TEST
            test_aarch64_ap_sched_poll_ready();
#endif
        }
    }
    arch_local_irq_restore(irq_flags);
    log_info("[ap-sched] release gate open, accepted_mask=%lx all ready\n",
             (unsigned long)accepted);
}
