/* Live IDT/IRQ probes. No symbols or observations in production builds. */
#if defined(OS01_SELFTEST) && defined(__x86_64__)
#include <selftest/x86_intr.h>
#include <arch/x86_64/gate.h>
#include <arch/mmu.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <percpu/percpu.h>
#include <intr/ipi.h>
#include <memory/vmm.h>
#include <sched/task.h>
#include <time/timer.h>
#include <core/printk.h>
#include <string.h>

/* Permanent test storage: a failed cleanup quarantines it until reboot.
 * No pages may be freed while a delayed AP could still use its CR3. */
static uint64_t scratch_root[512] __attribute__((aligned(4096)));
static uint64_t scratch_pdpt[512] __attribute__((aligned(4096)));
static uint64_t scratch_pd[512] __attribute__((aligned(4096)));
static uint64_t scratch_pt[512] __attribute__((aligned(4096)));
static uint64_t page_a[512] __attribute__((aligned(4096)));
static uint64_t page_b[512] __attribute__((aligned(4096)));
#define PROBE_VA 0xfffffe0000000000ULL
#define BUDGET 5000000000ULL
static uint64_t ticks[NR_CPUS], resched[NR_CPUS];
uint64_t selftest_x86_intr_returns[NR_CPUS];
static unsigned target;
static int armed, ready, release_worker, restored, worker_result;
static uint64_t flush_value;
static unsigned flush_cpu;
static uint32_t flush_generation;
static uint64_t progress;
static uint64_t before_tick, after_tick;
static uint64_t before_resched, after_resched;
static int resched_armed;

void selftest_x86_intr_tick(unsigned cpu)
{
    __atomic_fetch_add(&ticks[cpu], 1, __ATOMIC_RELAXED);
}
void selftest_x86_intr_resched(unsigned cpu)
{
    __atomic_fetch_add(&resched[cpu], 1, __ATOMIC_RELEASE);
}
void selftest_x86_intr_tlb(unsigned cpu)
{
    if (__atomic_load_n(&armed, __ATOMIC_ACQUIRE) && cpu == target) {
        /* Called AFTER the actual CR3 reload and BEFORE the ack. This
         * checks a translation, not a mock generation or handler call. */
        flush_value = *(volatile uint64_t *)PROBE_VA;
        flush_cpu = cpu;
        flush_generation = __atomic_load_n(&percpu_data[cpu].tlb_ack_gen,
                                            __ATOMIC_RELAXED);
    }
}

static bool wait_event(const int *event)
{
    uint64_t start = arch_cycle_counter();
    int state;
    while (!(state = x86_intr_wait_state(
                __atomic_load_n(event, __ATOMIC_ACQUIRE),
                arch_cycle_counter(), start, BUDGET))) {
        arch_local_irq_enable();
        arch_cpu_pause();
    }
    return state == 1;
}

static uint64_t remote_worker(uint64_t arg)
{
    (void)arg;
    uint64_t flags = arch_local_irq_save();
    uint64_t *old_root = arch_get_page_table();
    uint32_t guard = current->in_schedule;
    /* Keep this scratch CR3 installed across timer/IPI returns. The
     * existing nested-schedule guard suppresses a context switch only
     * during this bounded test window; IRQ handlers still run normally. */
    current->in_schedule = 1;
    arch_switch_mm((uint64_t *)((uint64_t)scratch_root - ARCH_PAGE_OFFSET));
    uint64_t initial = *(volatile uint64_t *)PROBE_VA;
    before_tick = __atomic_load_n(&ticks[cpu_id()], __ATOMIC_RELAXED);
    before_resched = __atomic_load_n(&resched[cpu_id()], __ATOMIC_ACQUIRE);
    __atomic_store_n(&armed, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&ready, initial == X86_INTR_PRE_FLUSH_VALUE ? 1 : -1,
                     __ATOMIC_RELEASE);
    arch_local_irq_enable();
    uint64_t start = arch_cycle_counter();
    while (!x86_intr_wait_state(
                __atomic_load_n(&release_worker, __ATOMIC_ACQUIRE),
                arch_cycle_counter(), start, BUDGET)) {
        /* Executed in task context after real interrupt return. */
        if (__atomic_load_n(&resched_armed, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&resched[cpu_id()], __ATOMIC_ACQUIRE) > before_resched)
            __atomic_fetch_add(&progress, 1, __ATOMIC_RELEASE);
        arch_cpu_pause();
    }
    arch_local_irq_disable();
    after_tick = __atomic_load_n(&ticks[cpu_id()], __ATOMIC_RELAXED);
    after_resched = __atomic_load_n(&resched[cpu_id()], __ATOMIC_ACQUIRE);
    worker_result = __atomic_load_n(&release_worker, __ATOMIC_ACQUIRE) ? 0 : -1;
    __atomic_store_n(&armed, 0, __ATOMIC_RELEASE);
    arch_switch_mm(old_root);
    current->in_schedule = guard;
    __atomic_store_n(&restored, 1, __ATOMIC_RELEASE);
    arch_local_irq_restore(flags);
    return 0;
}

static int live_remote(void)
{
    int rc = -1;
    uint64_t flags = arch_local_irq_save();
    unsigned self = cpu_id();
    target = NR_CPUS;
    for (unsigned i = 0; i < num_cpus; ++i)
        if (i != self && percpu_data[i].online &&
            __atomic_load_n(&percpu_data[i].ipi_ready, __ATOMIC_ACQUIRE)) {
            target = i;
            break;
        }
    if (target == NR_CPUS || !this_cpu()->scheduler_ok) {
        serial_printk("[selftest] x86_intr: no online/ipi_ready target cpu\n");
        goto out;
    }
    /* Root clone preserves every code/stack/GS/IDT/APIC mapping. Only
     * the previously unused scratch slot is replaced, in the clone. */
    uint64_t *root = (uint64_t *)((uint64_t)arch_get_page_table() + ARCH_PAGE_OFFSET);
    if (root[(PROBE_VA >> 39) & 511]) {
        serial_printk("[selftest] x86_intr: scratch slot %u already mapped\n",
                      (unsigned)((PROBE_VA >> 39) & 511));
        goto out;
    }
    memcpy(scratch_root, root, sizeof(scratch_root));
    scratch_root[(PROBE_VA >> 39) & 511] = ((uint64_t)scratch_pdpt - ARCH_PAGE_OFFSET) | 3;
    scratch_pdpt[0] = ((uint64_t)scratch_pd - ARCH_PAGE_OFFSET) | 3;
    scratch_pd[0] = ((uint64_t)scratch_pt - ARCH_PAGE_OFFSET) | 3;
    scratch_pt[0] = ((uint64_t)page_a - ARCH_PAGE_OFFSET) | 3;
    page_a[0] = X86_INTR_PRE_FLUSH_VALUE;
    page_b[0] = X86_INTR_FLUSHED_VALUE;
    uint64_t bsp_tick = __atomic_load_n(&ticks[self], __ATOMIC_RELAXED);
    uint64_t global_tick = jiffies;
    int pid = kernel_thread_on_cpu(remote_worker, 0, target);
    if (pid < 0) {
        serial_printk("[selftest] x86_intr: worker spawn failed: %d\n", pid);
        goto out;
    }
    arch_local_irq_enable();
    if (!wait_event(&ready) || ready != 1) {
        serial_printk("[selftest] x86_intr: worker ready=%d (timeout or "
                      "wrong initial mapping)\n", ready);
        goto cleanup;
    }
    uint32_t gen = __atomic_load_n(&percpu_data[target].tlb_ack_gen, __ATOMIC_ACQUIRE);
    __atomic_store_n(&scratch_pt[0], ((uint64_t)page_b - ARCH_PAGE_OFFSET) | 3,
                     __ATOMIC_RELEASE);
    /* Uses production serialization, online/ipi_ready target snapshot,
     * remote delivery and equality ack wait; never calls a handler. */
    tlb_shootdown();
    uint32_t ack = __atomic_load_n(&percpu_data[target].tlb_ack_gen, __ATOMIC_ACQUIRE);
    if (!x86_intr_ack_matches(target, flush_cpu, gen, ack, flush_generation,
                              flush_value)) {
        serial_printk("[selftest] x86_intr: shootdown cpu=%u/%u gen=%u ack=%u "
                      "observed_gen=%u flush=%lx FAIL\n", target, flush_cpu,
                      gen, ack, flush_generation, flush_value);
        goto cleanup;
    }
    before_resched = __atomic_load_n(&resched[target], __ATOMIC_ACQUIRE);
    __atomic_store_n(&resched_armed, 1, __ATOMIC_RELEASE);
    ipi_send(percpu_data[target].arch_processor_id, 0x41);
    uint64_t start = arch_cycle_counter();
    while (!x86_intr_wait_state(
               x86_intr_progress_done(
                   __atomic_load_n(&ticks[self], __ATOMIC_RELAXED), bsp_tick,
                   __atomic_load_n(&ticks[target], __ATOMIC_RELAXED), before_tick),
               arch_cycle_counter(), start, BUDGET))
        arch_cpu_pause();
    if (x86_intr_live_verdict(
            __atomic_load_n(&ticks[self], __ATOMIC_RELAXED), bsp_tick,
            __atomic_load_n(&ticks[target], __ATOMIC_RELAXED), before_tick,
            jiffies, global_tick,
            __atomic_load_n(&progress, __ATOMIC_ACQUIRE)))
        rc = 0;
    else
        serial_printk("[selftest] x86_intr: live verdict FAIL (bsp ticks %lu/%lu "
                      "ap ticks %lu/%lu jiffies %lu/%lu progress %lu)\n",
                      (unsigned long)__atomic_load_n(&ticks[self], __ATOMIC_RELAXED),
                      (unsigned long)bsp_tick,
                      (unsigned long)__atomic_load_n(&ticks[target], __ATOMIC_RELAXED),
                      (unsigned long)before_tick, (unsigned long)jiffies,
                      (unsigned long)global_tick,
                      (unsigned long)__atomic_load_n(&progress, __ATOMIC_ACQUIRE));
cleanup:
    /* Always release the worker, then require it to have restored the
     * original CR3 and in_schedule guard before this CPU proceeds. The
     * scratch pages are static storage: on a restore timeout they are
     * quarantined (never reused) instead of freed, because the delayed
     * worker may still switch to that CR3. */
    __atomic_store_n(&release_worker, 1, __ATOMIC_RELEASE);
    if (!wait_event(&restored)) {
        serial_printk("[selftest] x86_intr: cleanup timeout; scratch quarantined\n");
        rc = -1;
    } else if (!x86_intr_worker_verdict(worker_result, before_tick, after_tick,
                                        before_resched, after_resched)) {
        serial_printk("[selftest] x86_intr: worker verdict FAIL (result=%d "
                      "ticks %lu/%lu resched %lu/%lu)\n", worker_result,
                      (unsigned long)before_tick, (unsigned long)after_tick,
                      (unsigned long)before_resched, (unsigned long)after_resched);
        rc = -1;
    }
    serial_printk("[selftest] x86_intr: remote cpu=%u flush=%lx gen=%u ack=%u "
                  "AP ticks=%lu progress=%lu restored=%d\n", target, flush_value,
                  flush_generation, percpu_data[target].tlb_ack_gen,
                  after_tick - before_tick, progress, restored);
out:
    arch_local_irq_restore(flags);
    return rc;
}

struct x86_intr_snapshot selftest_x86_before, selftest_x86_after;
extern void selftest_x86_sentinel(void);

/* All GPRs have distinct sentinels, including caller-saved registers.
 * DF=1 tests CLD in entry and flags restoration by IRET; C resumes DF=0. */
__asm__(
    ".text\n"
    ".globl selftest_x86_sentinel\n"
    "selftest_x86_sentinel:\n"
    "pushq %rbp\n"
    "pushq %rbx\n"
    "pushq %r12\n"
    "pushq %r13\n"
    "pushq %r14\n"
    "pushq %r15\n"
    "movq $1192960, %rax\n"
    "movq $1192961, %rbx\n"
    "movq $1192962, %rcx\n"
    "movq $1192963, %rdx\n"
    "movq $1192964, %rsi\n"
    "movq $1192965, %rdi\n"
    "movq $1192966, %rbp\n"
    "movq $1192967, %r8\n"
    "movq $1192968, %r9\n"
    "movq $1192969, %r10\n"
    "movq $1192970, %r11\n"
    "movq $1192971, %r12\n"
    "movq $1192972, %r13\n"
    "movq $1192973, %r14\n"
    "movq $1192974, %r15\n"
    "pushq $0x447\n"
    "popfq\n"
    "movq %rax, selftest_x86_before+0(%rip)\n"
    "movq %rbx, selftest_x86_before+8(%rip)\n"
    "movq %rcx, selftest_x86_before+16(%rip)\n"
    "movq %rdx, selftest_x86_before+24(%rip)\n"
    "movq %rsi, selftest_x86_before+32(%rip)\n"
    "movq %rdi, selftest_x86_before+40(%rip)\n"
    "movq %rbp, selftest_x86_before+48(%rip)\n"
    "movq %r8, selftest_x86_before+56(%rip)\n"
    "movq %r9, selftest_x86_before+64(%rip)\n"
    "movq %r10, selftest_x86_before+72(%rip)\n"
    "movq %r11, selftest_x86_before+80(%rip)\n"
    "movq %r12, selftest_x86_before+88(%rip)\n"
    "movq %r13, selftest_x86_before+96(%rip)\n"
    "movq %r14, selftest_x86_before+104(%rip)\n"
    "movq %r15, selftest_x86_before+112(%rip)\n"
    "movq %rsp, selftest_x86_before+120(%rip)\n"
    "pushfq\n"
    "popq selftest_x86_before+128(%rip)\n"
    "movw %ds, selftest_x86_before+136(%rip)\n"
    "movw %es, selftest_x86_before+138(%rip)\n"
    "int $0xff\n"
    "movq %rax, selftest_x86_after+0(%rip)\n"
    "movq %rbx, selftest_x86_after+8(%rip)\n"
    "movq %rcx, selftest_x86_after+16(%rip)\n"
    "movq %rdx, selftest_x86_after+24(%rip)\n"
    "movq %rsi, selftest_x86_after+32(%rip)\n"
    "movq %rdi, selftest_x86_after+40(%rip)\n"
    "movq %rbp, selftest_x86_after+48(%rip)\n"
    "movq %r8, selftest_x86_after+56(%rip)\n"
    "movq %r9, selftest_x86_after+64(%rip)\n"
    "movq %r10, selftest_x86_after+72(%rip)\n"
    "movq %r11, selftest_x86_after+80(%rip)\n"
    "movq %r12, selftest_x86_after+88(%rip)\n"
    "movq %r13, selftest_x86_after+96(%rip)\n"
    "movq %r14, selftest_x86_after+104(%rip)\n"
    "movq %r15, selftest_x86_after+112(%rip)\n"
    "movq %rsp, selftest_x86_after+120(%rip)\n"
    "pushfq\n"
    "popq selftest_x86_after+128(%rip)\n"
    "movw %ds, selftest_x86_after+136(%rip)\n"
    "movw %es, selftest_x86_after+138(%rip)\n"
    "cld\n"
    "popq %r15\n"
    "popq %r14\n"
    "popq %r13\n"
    "popq %r12\n"
    "popq %rbx\n"
    "popq %rbp\n"
    "ret\n"
);

static int spurious(void)
{
    uint64_t flags = arch_local_irq_save();
    unsigned cpu = cpu_id();
    uint64_t returns = selftest_x86_intr_returns[cpu];
    uint64_t schedules = this_cpu()->schedule_count;
    uint64_t need = this_cpu()->need_resched;
    /* A wrongly routed ret_from_intr would consume this and increment
     * the return-work counter, even if schedule selects this task. */
    this_cpu()->need_resched = 1;
    selftest_x86_sentinel();
    bool ok = x86_intr_spurious_verdict(
        x86_intr_snapshot_equal(&selftest_x86_before, &selftest_x86_after),
        returns, selftest_x86_intr_returns[cpu],
        schedules, this_cpu()->schedule_count,
        this_cpu()->need_resched);
    this_cpu()->need_resched = need;
    arch_local_irq_restore(flags);
    serial_printk("[selftest] x86_intr: int ff GPR/RSP/RFLAGS/DS/ES and return-work %s\n",
                  ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

/* Independent expected descriptors, deliberately not intr_vectors.h. */
struct expected_gate { unsigned vector, attr, ist; void (*stub)(void); };
extern void divide_error(void);
extern void debug(void);
extern void nmi(void);
extern void int3(void);
extern void overflow(void);
extern void bounds(void);
extern void undefined_opcode(void);
extern void dev_not_available(void);
extern void double_fault(void);
extern void coprocessor_segment_overrun(void);
extern void invalid_TSS(void);
extern void segment_not_present(void);
extern void stack_segment_fault(void);
extern void general_protection(void);
extern void page_fault(void);
extern void x87_FPU_error(void);
extern void alignment_check(void);
extern void machine_check(void);
extern void SIMD_exception(void);
extern void virtualization_exception(void);
extern void IRQ0_interrupt(void);
extern void IRQ1_interrupt(void);
extern void IRQ2_interrupt(void);
extern void IRQ3_interrupt(void);
extern void IRQ4_interrupt(void);
extern void IRQ5_interrupt(void);
extern void IRQ6_interrupt(void);
extern void IRQ7_interrupt(void);
extern void IRQ8_interrupt(void);
extern void IRQ9_interrupt(void);
extern void IRQ10_interrupt(void);
extern void IRQ11_interrupt(void);
extern void IRQ12_interrupt(void);
extern void IRQ13_interrupt(void);
extern void IRQ14_interrupt(void);
extern void IRQ15_interrupt(void);
extern void IRQ16_interrupt(void);
extern void IRQ17_interrupt(void);
extern void IRQ18_interrupt(void);
extern void IRQ19_interrupt(void);
extern void IRQ20_interrupt(void);
extern void IRQ21_interrupt(void);
extern void IRQ22_interrupt(void);
extern void IRQ23_interrupt(void);
extern void lapic_timer_stub(void);
extern void _intr_stub_tlb(void);
extern void _intr_stub_resched(void);
extern void lapic_spurious_stub(void);
static const struct expected_gate gates[] = {
    {0, 0x8f, 1, divide_error},
    {1, 0x8f, 1, debug},
    {2, 0x8e, 1, nmi},
    {3, 0xef, 1, int3},
    {4, 0xef, 1, overflow},
    {5, 0xef, 1, bounds},
    {6, 0x8f, 1, undefined_opcode},
    {7, 0x8f, 1, dev_not_available},
    {8, 0x8f, 3, double_fault},
    {9, 0x8f, 1, coprocessor_segment_overrun},
    {10, 0x8f, 1, invalid_TSS},
    {11, 0x8f, 1, segment_not_present},
    {12, 0x8f, 1, stack_segment_fault},
    {13, 0x8f, 1, general_protection},
    {14, 0x8f, 0, page_fault},
    {16, 0x8f, 1, x87_FPU_error},
    {17, 0x8f, 1, alignment_check},
    {18, 0x8f, 1, machine_check},
    {19, 0x8f, 1, SIMD_exception},
    {20, 0x8f, 1, virtualization_exception},
    {0x20, 0x8e, 0, IRQ0_interrupt},
    {0x21, 0x8e, 0, IRQ1_interrupt},
    {0x22, 0x8e, 0, IRQ2_interrupt},
    {0x23, 0x8e, 0, IRQ3_interrupt},
    {0x24, 0x8e, 0, IRQ4_interrupt},
    {0x25, 0x8e, 0, IRQ5_interrupt},
    {0x26, 0x8e, 0, IRQ6_interrupt},
    {0x27, 0x8e, 0, IRQ7_interrupt},
    {0x28, 0x8e, 0, IRQ8_interrupt},
    {0x29, 0x8e, 0, IRQ9_interrupt},
    {0x2a, 0x8e, 0, IRQ10_interrupt},
    {0x2b, 0x8e, 0, IRQ11_interrupt},
    {0x2c, 0x8e, 0, IRQ12_interrupt},
    {0x2d, 0x8e, 0, IRQ13_interrupt},
    {0x2e, 0x8e, 0, IRQ14_interrupt},
    {0x2f, 0x8e, 0, IRQ15_interrupt},
    {0x30, 0x8e, 0, IRQ16_interrupt},
    {0x31, 0x8e, 0, IRQ17_interrupt},
    {0x32, 0x8e, 0, IRQ18_interrupt},
    {0x33, 0x8e, 0, IRQ19_interrupt},
    {0x34, 0x8e, 0, IRQ20_interrupt},
    {0x35, 0x8e, 0, IRQ21_interrupt},
    {0x36, 0x8e, 0, IRQ22_interrupt},
    {0x37, 0x8e, 0, IRQ23_interrupt},
    {0x38, 0x8e, 0, lapic_timer_stub},
    {0x40, 0x8e, 0, _intr_stub_tlb},
    {0x41, 0x8e, 0, _intr_stub_resched},
    {0xff, 0x8e, 0, lapic_spurious_stub},
};
static int idt_snapshot(void)
{
    const unsigned count = sizeof(gates) / sizeof(gates[0]);
    for (unsigned i = 0; i < count; ++i) {
        const struct expected_gate *e = &gates[i];
        const unsigned char *g = IDT_Table[e->vector].x;
        const char *why = x86_intr_gate_mismatch(g, (uint64_t)e->stub,
                                                 e->attr, e->ist);
        if (why) {
            serial_printk("[selftest] x86_intr: IDT vector=%x mismatch=%s "
                          "attr=%x ist=%u FAIL\n", e->vector, why,
                          (unsigned)g[5], (unsigned)g[4]);
            return -1;
        }
    }
    serial_printk("[selftest] x86_intr: runtime IDT %u/%u PASS\n", count, count);
    return 0;
}
int test_x86_intr(void)
{
    if (idt_snapshot() || spurious() || live_remote()) return -1;
    serial_printk("[selftest] x86_intr: live BSP/AP timer, remote TLB and resched PASS\n");
    return 0;
}
#endif
