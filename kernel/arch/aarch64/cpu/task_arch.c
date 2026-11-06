/* AArch64 EL1 cooperative task context and BSP handoff. */
#include <sched/task.h>
#include <sched/internal.h>
#include <sched/boot.h>
#include <arch/aarch64/task_arch.h>
#include <arch/aarch64/ap_sched.h>
#include <arch/aarch64/smp.h>
#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/sched_tick.h>  /* aarch64_sched_tick_init/start */
#include <arch/aarch64/ipi.h>   /* P2: need-slot offset cross-check */
#include <percpu/percpu.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <core/panic.h>
#include <log/log.h>
#include <core/printk.h>
#include <errno.h>
#include <string.h>
#ifdef OS01_SELFTEST
#include <selftest/aarch64_scheduler.h>
#include <selftest/aarch64_ap_scheduler.h>
#include <selftest/result.h>
#endif

extern char _text_start, _text_end;

/* Fatal-path ownership arbitration (P1 spec §6): the assembly fatal
 * entry claims this word with ldaxr/stxr BEFORE any stack exists; the
 * winning CPU installs the shared emergency stack, losers park
 * stackless.  Defined here so both C diagnostics and the .S site link
 * against one storage.  The arrivals word records which CPUs reached
 * the fatal entry — only the fatal-owner test build writes it. */
uint32_t aarch64_fatal_owner_word;
uint32_t aarch64_fatal_losers_word;
uint32_t aarch64_fatal_arrivals_word;

static uint8_t aarch64_context_fatal_stack[4096]
    __attribute__((aligned(16), used));
__asm__(".global aarch64_context_fatal_stack_top\n"
        ".set aarch64_context_fatal_stack_top, aarch64_context_fatal_stack + 4096\n");

_Static_assert(sizeof(task_t) + 256 + 16 < STACK_SIZE,
               "aarch64 task union has no usable context stack");
/* Pin the shared assembly-visible per-CPU layout; the SGI handler uses
 * the real C field accessor, never a private copy of this offset. */
_Static_assert(offsetof(percpu_t, need_resched) == PERCPU_NEED_RESCHED_OFFSET,
               "per-CPU assembly offset drifted from percpu_t.need_resched");
_Static_assert((STACK_SIZE & (STACK_SIZE - 1)) == 0,
               "stack mask requires a power-of-two stack size");

thread_t init_thread = {
    .x19 = (uint64_t)idle_resume,
    .x20 = 0,
    .x21 = (uint64_t)&init_thread,
    .x30 = (uint64_t)arch_aarch64_kthread_entry,
};

__attribute__((noreturn)) void aarch64_context_fatal(uint64_t code,
                                                      uint64_t sp,
                                                      uint64_t base,
                                                      uint64_t top)
{
    log_err("[aarch64] context fatal code=%u sp=%lx base=%lx top=%lx\n",
            (unsigned)code, (unsigned long)sp, (unsigned long)base,
            (unsigned long)top);
    for (;;) arch_cpu_halt();
}

static __attribute__((noreturn)) void fatal_context_details(uint64_t code,
                                                             const char *reason,
                                                             uint64_t sp,
                                                             uint64_t pc,
                                                             uint64_t base,
                                                             uint64_t top)
{
    log_err("[aarch64] context reject code=%u reason=%s sp=%lx pc=%lx range=%lx..%lx\n",
            (unsigned)code, reason ? reason : "unknown",
            (unsigned long)sp, (unsigned long)pc,
            (unsigned long)base, (unsigned long)top);
    for (;;) arch_cpu_halt();
}

static bool task_pointer_plausible(task_t *t)
{
    uintptr_t p = (uintptr_t)t;
    return t && (p >> 48) == 0xffff && (p & (STACK_SIZE - 1)) == 0;
}

static bool thread_pointer_plausible(thread_t *thread)
{
    uintptr_t p = (uintptr_t)thread;
    return thread && (p >> 48) == 0xffff && (p & 7) == 0;
}

bool arch_task_context_valid(task_t *t, const char **reason)
{
    if (reason) *reason = "invalid task pointer";
    if (!task_pointer_plausible(t))
        return false;
    thread_t *ctx = t->thread;
    if (!thread_pointer_plausible(ctx)) {
        if (reason) *reason = "invalid thread pointer";
        return false;
    }
    uintptr_t base = (uintptr_t)t;
    uintptr_t top = base + STACK_SIZE;
    uintptr_t low = (uintptr_t)ctx->usable_low;
    uintptr_t sp = (uintptr_t)ctx->sp;
    uintptr_t pc = (uintptr_t)ctx->x30;
    if (ctx->stack_base != base || ctx->stack_top != top ||
        low < base + sizeof(*t) + 256 || low >= top) {
        if (reason) *reason = "invalid stack metadata";
        return false;
    }
    if (sp < low || sp >= top) {
        if (reason) *reason = "saved sp outside usable stack";
        return false;
    }
    if ((sp & 15) != 0) {
        if (reason) *reason = "saved sp is not aligned";
        return false;
    }
    if (sp < low + 16) {
        if (reason) *reason = "saved sp lacks switch-tail headroom";
        return false;
    }
    if ((pc & 3) || pc < (uintptr_t)&_text_start ||
        pc >= (uintptr_t)&_text_end) {
        if (reason) *reason = "continuation outside executable kernel text";
        return false;
    }
    if (reason) *reason = "valid";
    return true;
}

void arch_dump_task_context(task_t *t)
{
    const char *reason = 0;
    if (!arch_task_context_valid(t, &reason)) {
        log_err("[sched] aarch64 context invalid: %s\n", reason);
        return;
    }
    log_info("[sched] aarch64 context sp=%lx pc=%lx low=%lx top=%lx\n",
             (unsigned long)t->thread->sp, (unsigned long)t->thread->x30,
             (unsigned long)t->thread->usable_low,
             (unsigned long)t->thread->stack_top);
}

static __attribute__((noreturn)) void fatal_context(uint64_t code,
                                                    uint64_t sp,
                                                    uint64_t base,
                                                    uint64_t top)
{
    aarch64_context_fatal(code, sp, base, top);
}

void arch_task_check_prev(task_t *t)
{
    uint64_t live_sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(live_sp));
    if (!task_pointer_plausible(t) || !thread_pointer_plausible(t->thread))
        fatal_context(4, live_sp, (uintptr_t)t, (uintptr_t)t + STACK_SIZE);
    uintptr_t base = (uintptr_t)t;
    uintptr_t top = base + STACK_SIZE;
    thread_t *ctx = t->thread;
    uintptr_t low = ctx->usable_low;
    if (ctx->stack_base != base || ctx->stack_top != top ||
        live_sp < low || live_sp >= top || (live_sp & 15) != 0)
        fatal_context(5, live_sp, base, top);
}

/* ── Compile-gated fault injection (P5 Task 3 / spec §10.2) ────────
 * Exactly ONE numeric define selects the corruption (Ruling P5-1):
 *   AARCH64_SCHED_FAULT_SP_TOP  — saved SP == stack top
 *   AARCH64_SCHED_FAULT_SP_LOW  — saved SP below usable_low (-16)
 *   AARCH64_SCHED_FAULT_PC_LOW  — continuation = aligned unmapped low addr
 *   AARCH64_SCHED_FAULT_PC_DATA — continuation = non-executable data object
 * Without a define (normal / ordinary selftest / sched positive builds)
 * the hook is a no-op inline and NONE of these symbols exist — the
 * harness nm audit asserts `aarch64_sched_fault_inject` is present in
 * every sched-fault-<value> ELF and absent from the other three.
 *
 * The dedicated victim is the FIRST kernel thread the published idle
 * task (the selftest driver on init_task_union) switches to: that is
 * the driver's first kthread_yield() in case_yield.  The corruption is
 * written immediately BEFORE the production pre-switch validator, so
 * the rejection and the production fatal reason fire BEFORE the stack
 * switch; the observation counters (victim first-entry via
 * arch_aarch64_entry_prepare + committed switches to the victim) are
 * printed from the rejection path, so a build that printed the marker
 * and then transferred shows switches_to_target != 0 and fails. */
#if defined(AARCH64_SCHED_FAULT_SP_TOP) || defined(AARCH64_SCHED_FAULT_SP_LOW) \
    || defined(AARCH64_SCHED_FAULT_PC_LOW) || defined(AARCH64_SCHED_FAULT_PC_DATA)
#define AARCH64_SCHED_FAULT_ACTIVE 1
static task_t *aarch64_sched_fault_target;
static unsigned aarch64_sched_fault_target_entries;
static unsigned aarch64_sched_fault_target_switches;
static int aarch64_sched_fault_injected;

static const char *aarch64_sched_fault_case(void)
{
#if defined(AARCH64_SCHED_FAULT_SP_TOP)
    return "sp-top";
#elif defined(AARCH64_SCHED_FAULT_SP_LOW)
    return "sp-low";
#elif defined(AARCH64_SCHED_FAULT_PC_LOW)
    return "pc-low";
#else
    return "pc-data";
#endif
}

__attribute__((noinline, used))
static void aarch64_sched_fault_inject(task_t *prev, task_t *next)
{
    if (aarch64_sched_fault_injected)
        return;
    if (prev != &init_task_union.task || next == prev)
        return;
    aarch64_sched_fault_injected = 1;
    aarch64_sched_fault_target = next;
    thread_t *ctx = next->thread;
#if defined(AARCH64_SCHED_FAULT_SP_TOP)
    ctx->sp = ctx->stack_top;
#elif defined(AARCH64_SCHED_FAULT_SP_LOW)
    ctx->sp = ctx->usable_low - 16;
#elif defined(AARCH64_SCHED_FAULT_PC_LOW)
    ctx->x30 = 0x2000;      /* 16-aligned, unmapped, arbitrary low address */
#else
    /* A real kernel data object in .bss: mapped, readable, NOT executable
     * (only the higher-half .text range is executable). */
    ctx->x30 = (uintptr_t)&aarch64_context_fatal_stack;
#endif
}
#else
#define AARCH64_SCHED_FAULT_ACTIVE 0
static inline void aarch64_sched_fault_inject(task_t *prev, task_t *next)
{
    (void)prev;
    (void)next;
}
#endif

/* ── fatal-owner fault variant (P1 Task 3, AARCH64_AP_SCHED_TEST=
 *    fatal-owner build ONLY — the normal ELF carries none of this) ──
 * BSP and CPU1 meet at the no-stack fatal arbitration through a real
 * ap_work item: the BSP submits a test command to CPU1 (whose work
 * loop consumes it and drives straight into the fatal path), then
 * follows it itself.  The asm records arrivals and arbitrates; the
 * winning CPU (on the shared emergency stack) waits until
 * arrivals_mask==3 && losers==1 (2-second cycle deadline), then writes
 * the single evidence line and parks.  A successful END is impossible:
 * both CPUs park forever and the owner never releases. */
#ifdef AARCH64_AP_SCHED_TEST_FATAL_OWNER
#define AARCH64_AP_FATAL_OWNER_CMD 0x57u

extern void aarch64_context_fatal_asm(void) __attribute__((noreturn));

void aarch64_ap_fatal_owner_enter(void)
{
    uint64_t freq = arch_cycle_freq();
    uint64_t deadline = arch_cycle_counter() +
                        (freq ? 2 * freq : 100000000ULL);
    uint32_t arrivals = 0, losers = 0;
    for (;;) {
        arrivals = __atomic_load_n(&aarch64_fatal_arrivals_word,
                                   __ATOMIC_ACQUIRE);
        losers = __atomic_load_n(&aarch64_fatal_losers_word,
                                 __ATOMIC_ACQUIRE);
        if (arrivals == 3 && losers == 1)
            break;
        if (arch_cycle_counter() > deadline)
            break;   /* timeout: park without evidence — runner fails */
        arch_cpu_pause();
    }
    if (arrivals == 3 && losers == 1)
        serial_printk("[ap-fatal] owner=%u arrivals_mask=%u "
                      "entrants=1 losers=%u\n",
                      (unsigned)cpu_id(), (unsigned)arrivals,
                      (unsigned)losers);
    for (;;)
        arch_cpu_halt();   /* owner never releases; no successful END */
}

/* Strong override of the weak <arch/aarch64/ap_work.h> ext hook: the
 * test command drives CPU1 into the fatal path.  Every other command
 * keeps the legacy unknown-cmd semantics. */
bool ap_work_ext_run(uint32_t cmd, uint64_t arg0, uint64_t arg1,
                     uint64_t *out)
{
    (void)arg0;
    (void)arg1;
    (void)out;
    if (cmd != AARCH64_AP_FATAL_OWNER_CMD)
        return false;
    aarch64_context_fatal_asm();
}

static void test_aarch64_ap_fatal_owner_drive(void)
{
    /* seq 1: the first submission this boot makes to CPU1. */
    ap_work_submit(1, 1, AARCH64_AP_FATAL_OWNER_CMD, 0, 0);
    aarch64_context_fatal_asm();
}
#endif

void arch_task_switch(task_t *prev, task_t *next)
{
    if (prev == next)
        return;
    aarch64_sched_fault_inject(prev, next);
    const char *reason = 0;
    if (!arch_task_context_valid(next, &reason)) {
#if AARCH64_SCHED_FAULT_ACTIVE
        /* Rejection path ONLY (a marker printed before attempting the
         * switch proves nothing): the victim's live observation counters
         * are printed here, so a build that printed the marker and then
         * committed the transfer shows switches_to_target != 0 and the
         * harness rejects it. */
        if (next == aarch64_sched_fault_target) {
            serial_printk("[sched-fault] case=%s phase=pre-switch "
                          "target=%lu switches_to_target=%u\n",
                          aarch64_sched_fault_case(),
                          (unsigned long)next->pid,
                          aarch64_sched_fault_target_entries +
                          aarch64_sched_fault_target_switches);
        }
#endif
        bool safe = task_pointer_plausible(next) &&
                    thread_pointer_plausible(next->thread);
        uint64_t sp = safe ? next->thread->sp : 0;
        uint64_t pc = safe ? next->thread->x30 : 0;
        uint64_t base = safe ? next->thread->stack_base : (uintptr_t)next;
        uint64_t top = safe ? next->thread->stack_top : 0;
        fatal_context_details(6, reason, sp, pc, base, top);
    }
    if (next->thread->sp < next->thread->usable_low + 16)
        fatal_context(7, next->thread->sp, next->thread->stack_base,
                      next->thread->stack_top);
#if AARCH64_SCHED_FAULT_ACTIVE
    /* Switch-commit observer: reaching the assembly call with the
     * victim as `next` means a transfer to it is committing. */
    if (next == aarch64_sched_fault_target)
        aarch64_sched_fault_target_switches++;
#endif
    arch_task_check_prev(prev);
    aarch64_switch_context(prev, prev->thread, next->thread);
}

void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void))
{
    (void)idle_entry; /* aarch64 idle context is fully constructed up front. */
    const char *reason = 0;
    if (!arch_task_context_valid(t, &reason)) {
        bool safe = task_pointer_plausible(t) &&
                    thread_pointer_plausible(t->thread);
        uint64_t sp = safe ? t->thread->sp : 0;
        uint64_t pc = safe ? t->thread->x30 : 0;
        uint64_t base = safe ? t->thread->stack_base : (uintptr_t)t;
        uint64_t top = safe ? t->thread->stack_top : 0;
        fatal_context_details(8, reason, sp, pc, base, top);
    }
}

int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg)
{
    if (!t || !t->thread || !fn)
        return -EINVAL;
    /* P1 Task 3: the common constructor already validated the target
     * (bounds, online, scheduler_ok).  The arch hook freezes the
     * single-bit affinity for the verified target — every aarch64
     * kernel thread stays unmigratable. */
    if (t->cpu >= NR_CPUS)
        return -EINVAL;
    t->allowed_cpus = 1ULL << t->cpu;
    thread_t *ctx = t->thread;
    uintptr_t base = (uintptr_t)t;
    uintptr_t top = base + STACK_SIZE;
    uintptr_t low = (base + sizeof(*t) + 256 + 15) & ~(uintptr_t)15;
    uintptr_t sp = top - 16;
    if (sp < low + 16)
        return -EINVAL;

    t->mm = &init_mm;
    t->files = NULL;
    t->fpu_save = NULL;
    ctx->x19 = (uintptr_t)fn;
    ctx->x20 = arg;
    ctx->x21 = (uintptr_t)ctx;
    ctx->x22 = ctx->x23 = ctx->x24 = ctx->x25 = 0;
    ctx->x26 = ctx->x27 = ctx->x28 = ctx->x29 = 0;
    ctx->x30 = (uintptr_t)arch_aarch64_kthread_entry;
    ctx->sp = sp;
    ctx->sched_flags = 0;
    ctx->stack_base = base;
    ctx->stack_top = top;
    ctx->usable_low = low;
    const char *reason = 0;
    return arch_task_context_valid(t, &reason) ? 0 : -EINVAL;
}

void arch_kthread_drop_files(task_t *t)
{
    if (t && t->files)
        fatal_context(9, 0, (uintptr_t)t, (uintptr_t)t + STACK_SIZE);
}

/* Called only after assembly has validated and installed the initial stack. */
void arch_aarch64_entry_prepare(void)
{
    task_t *t = arch_current_task();
#if AARCH64_SCHED_FAULT_ACTIVE
    /* Target-entry observer: first C instruction on the victim's own
     * stack — reachable only through a committed switch. */
    if (t == aarch64_sched_fault_target)
        aarch64_sched_fault_target_entries++;
#endif
    t->on_cpu = 1;
    t->in_schedule = 0;
}

/* Called on the boot stack after all probes and before the one-way handoff.
 * It writes metadata only; it must never derive or dereference current. */
void arch_aarch64_prepare_bsp_handoff(void)
{
    uintptr_t base = (uintptr_t)&init_task_union;
    uintptr_t top = base + STACK_SIZE;
    uintptr_t low = (base + sizeof(task_t) + 256 + 15) & ~(uintptr_t)15;
    init_task_union.task.thread = &init_thread;
    init_thread.sp = top - 16;
    init_thread.stack_base = base;
    init_thread.stack_top = top;
    init_thread.usable_low = low;
    const char *reason = 0;
    if (!arch_task_context_valid(&init_task_union.task, &reason))
        fatal_context(12, init_thread.sp, base, top);

    /* The per-CPU scheduler tick is initialized HERE, on the boot
     * stack: it only initializes data (the accounting locks and the
     * ready gate) and never reads `current`.  It must run before any
     * AP handoff and before the BSP starts its own tick. */
    aarch64_sched_tick_init();
}

void arch_aarch64_bsp_handoff(void)
{
    task_t *idle = arch_current_task();
    if (idle != &init_task_union.task)
        fatal_context(10, (uintptr_t)__builtin_frame_address(0),
                      (uintptr_t)&init_task_union, (uintptr_t)&init_task_union + STACK_SIZE);
    idle->cpu = 0;
    idle->allowed_cpus = 1;
    idle->state = TASK_RUNNING;
    idle->on_cpu = 1;
    idle->on_rq = 0;
    idle->in_schedule = 0;
    if (!rbtree_empty(&percpu_data[0].run_queue))
        fatal_context(11, (uintptr_t)__builtin_frame_address(0),
                      (uintptr_t)&init_task_union, (uintptr_t)&init_task_union + STACK_SIZE);
    sched_boot_publish_idle();
    /* Start the BSP's LOCAL scheduler tick now: the idle metadata and
     * sched_boot_publish_idle() are in place and IRQs are still
     * disabled.  The BSP's counter was already armed during the early
     * boot phase — the start only verifies it and publishes the
     * accounting lifecycle.  A failure must never publish
     * scheduler_ok: the handoff fails the boot instead. */
    if (!aarch64_sched_tick_start()) {
        log_err("[sched-tick] FATAL: BSP local tick start failed\n");
        for (;;) arch_cpu_halt();
    }
    /* Publish BSP scheduler_ok FIRST, then open the AP release gate and
     * wait for every accepted AP's scheduler_ok (P1 spec §3.2).  The AP
     * variant's runtime-admission check runs in the window where the
     * accepted APs are IPI-ready but not yet schedulable. */
    sched_boot_start();
#if defined(AARCH64_AP_SCHED_TEST_AP) || defined(AARCH64_AP_SCHED_WAKE_TEST)
    test_aarch64_ap_scheduler_before_release();
#endif
    aarch64_ap_sched_release();
    arch_local_irq_enable();
    log_info("[sched] idle-ready cpu=0 scheduler_ok=1 irq=on\n");
#ifdef AARCH64_AP_SCHED_TEST_FATAL_OWNER
    /* fatal-owner variant: both CPUs drive into the illegal-SP path at
     * the cross-CPU barrier and park; the boot never completes. */
    test_aarch64_ap_fatal_owner_drive();
#endif
#if defined(AARCH64_AP_SCHED_WAKE_TEST)
    /* The wake variant runs the P1-ready + six wake cases with the
     * phase=wake envelope. */
    test_aarch64_ap_scheduler_wake_run();
#elif defined(AARCH64_AP_SCHED_TEST_AP)
    /* The AP production-path variant runs ONLY the seven frozen
     * ap-sched cases (the old suite's late probe is not run here). */
    test_aarch64_ap_scheduler_run();
#elif defined(OS01_SELFTEST)
    test_aarch64_scheduler_run();
    int selftest_failed = selftest_end_run();
    serial_printk("[selftest] done (failed=%u)\n",
                  (unsigned)selftest_failed);
#endif
    idle_resume();
    __builtin_unreachable();
}
