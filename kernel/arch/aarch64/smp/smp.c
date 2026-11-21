/* BSP publication for firmware-managed PSCI secondaries.
 *
 * All per-CPU metadata uses its low identity VA, before and after MMU-on.
 * Initialize and clean once before the first CPU_ON; afterward the coherent
 * online/go fields belong to the release/acquire ACK/command protocol.
 *
 * Unified SMP boot (spec 2026-11-19): this file implements the
 * arch/smp.h strong overrides for aarch64 — arch_smp_boot_aps (single
 * entry, Fail-Closed, no degraded continuation), arch_smp_last_status,
 * and the four AP lifecycle hooks driven by kernel/core/smp.c's
 * smp_ap_common_entry state machine.
 */
#include <stdint.h>
#include <errno.h>
#include <arch/aarch64/page_table_selftest.h>
#include <stdbool.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/psci.h>
#include <arch/smp.h>
#include <arch/aarch64/smp.h>
#include <arch/aarch64/smp_boot_core.h>
#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/ap_sched.h>
#include <arch/aarch64/ipi.h>     /* gic_target_bit_init (M3 Task 8) */
#include <arch/aarch64/vmm_gate.h>/* ipi_ready_publish_and_count (Task 7) */
#include <arch/irq.h>
#include <arch/aarch64/boot_log.h>
#include <arch/aarch_percpu.h>
#include <arch/aarch64/reg.h>
#include <percpu/percpu.h>

static bool boot_published;

/* Recent arch_smp_boot_aps() diagnostic snapshot (arch/smp.h contract). */
static struct arch_smp_boot_status a64_smp_status;

const struct arch_smp_boot_status *arch_smp_last_status(void)
{
    return &a64_smp_status;
}

/* Record a failed AP bring-up and hand the negative errno to the
 * caller; the unified Fail-Closed stop (log + boot_failure_stop) is
 * owned by kernel/core/smp.c::smp_boot_fail. */
static int a64_smp_fail(uint32_t cpu, enum smp_boot_stage stage,
                        int32_t arch_rc, int ret)
{
    a64_smp_status.failed_cpu = cpu;
    a64_smp_status.failed_stage = stage;
    a64_smp_status.arch_rc = arch_rc;
    return ret;
}

static void clean_to_poc(uint64_t start, uint64_t end, uint64_t line_size)
{
    uint64_t line = start & ~(line_size - 1);
    for (; line < end; line += line_size) {
        __asm__ __volatile__("dc cvac, %0" :: "r"(line) : "memory");
    }
}

int aarch64_smp_publish_boot(const struct aarch64_topology *topology,
                             uint64_t entry_va)
{
    uint64_t bsp, ctr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(bsp));
    bsp &= AARCH64_MPIDR_AFFINITY_MASK;
    if (boot_published || !entry_va || !topology || !topology->cpu_count ||
        topology->cpu_count > NR_CPUS || topology->mpidr[0] != bsp) {
        return -1;
    }

    uint64_t slots_addr = aarch64_percpu_slot_addr(0);
    aarch64_boot_percpu_t *slots = (aarch64_boot_percpu_t *)slots_addr;
    uint64_t table_addr = aarch64_dtb_mpidr_table_addr();
    uint64_t *mpidrs = (uint64_t *)table_addr;
    uint64_t count_addr = aarch64_dtb_cpu_count_addr();

    /* The parser already put the BSP first. Assembly consumes this same
     * order and verifies context_id against both the slot and MPIDR table.
     * Initialize inactive slots as well; never repurpose them after start. */
    for (uint32_t i = 0; i < NR_CPUS; ++i) {
        uint64_t mpidr = i < topology->cpu_count ? topology->mpidr[i] : 0;
        slots[i].self = slots_addr + i * sizeof(*slots);
        slots[i].cpu_id = i;
        slots[i].pad0 = 0;
        slots[i].mpidr = mpidr;
        slots[i].stack = aarch64_boot_stack_top_addr(i);
        slots[i].online = i == 0 ? 1 : 0;
        slots[i].go = AARCH64_BOOT_GO_WAIT;
        slots[i].entry = entry_va;
        mpidrs[i] = mpidr;
    }
    *(uint32_t *)count_addr = topology->cpu_count;

    /* CTR_EL0.DminLine gives log2(words per smallest data cache line).
     * APs begin with caches/MMU off, so release stores alone cannot publish
     * these initial writes to them. Clean actual ranges to PoC, then DSB
     * SY before the caller is allowed to issue its first CPU_ON. */
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line_size = UINT64_C(4) << ((ctr >> 16) & 0xf);
    clean_to_poc(aarch64_boot_page_tables_addr(),
                 aarch64_boot_page_tables_end_addr(), line_size);
    clean_to_poc(slots_addr, slots_addr + NR_CPUS * sizeof(*slots), line_size);
    clean_to_poc(table_addr, table_addr + NR_CPUS * sizeof(*mpidrs), line_size);
    clean_to_poc(count_addr, count_addr + sizeof(uint32_t), line_size);
    aarch64_publish_cache_ranges(clean_to_poc,line_size);
    __asm__ __volatile__("dsb sy" ::: "memory");
    boot_published = true;
    return 0;
}

/* Preserve the frozen result for post-boot debugger diagnostics too. */
static volatile struct smp_boot_result boot_result;

/* The frozen boot ACK mask.  arch_smp_boot_aps publishes it exactly once
 * (release store) BEFORE any AP can poll the release gate; readers
 * acquire.  There is deliberately no lazy-init fallback: an unwritten
 * static result must never be returned. */
uint64_t aarch64_smp_accepted_mask(void)
{
    return (uint64_t)__atomic_load_n(&boot_result.online_mask,
                                     __ATOMIC_ACQUIRE);
}

static uint64_t boot_counter(void *ctx)
{
    (void)ctx;
    return arch_cycle_counter();
}

static int32_t boot_cpu_on(void *ctx, uint64_t mpidr, uint64_t entry, uint64_t id)
{
    (void)ctx;
    return psci_cpu_on(mpidr, entry, id);
}

static uint32_t boot_online(void *ctx, uint32_t id)
{
    (void)ctx;
    return boot_online_get(id);
}

static void boot_command(void *ctx, uint32_t id, uint32_t command)
{
    (void)ctx;
    boot_go_set(id, command);
}

static void boot_relax(void *ctx)
{
    (void)ctx;
    arch_cpu_pause();
}

/* Success-only summary; the unified Fail-Closed policy has no DEGRADED
 * shape to report (spec 2026-11-19 §4.2.3). */
static void log_summary(uint32_t requested, uint32_t online)
{
    log_info("[smp] requested=");
    kputu(requested);
    log_info(" online=");
    kputu(online);
    log_info(" status=PASS\n");
}

int arch_smp_boot_aps(uint32_t cpu_count, arch_smp_ap_entry_t entry,
                      struct percpu *per_cpu_data)
{
    /* The secondary entry and kernel/percpu/percpu.c index the global
     * percpu_data[] — a non-global percpu base is a boot protocol error
     * (spec §4.2.3).  Everything else here is the same Fail-Closed
     * precondition family as kernel/core/smp.c::arch_boot_aps. */
    const struct aarch64_topology *topology = dtb_topology();
    uint64_t hz = cntfrq_el0();
    if (!entry || per_cpu_data != percpu_data ||
        cpu_count == 0u || cpu_count > (uint32_t)NR_CPUS ||
        boot_published || !topology || topology->cpu_count != cpu_count ||
        !hz || hz > UINT64_MAX / 2)
        return a64_smp_fail(0, SMP_STAGE_INVARIANT, -EINVAL, -EINVAL);

    /* Before the one-shot publication, initialize all shared test data.
     * Nothing may reset slots, stacks or test state after CPU_ON. */
    spin_init(&bench_lock);
    benchmark_total = 0;
    for (uint32_t id = 0; id < NR_CPUS; ++id)
        bench_done_set(id, 0);
    if (aarch64_smp_publish_boot(topology, (uint64_t)(uintptr_t)entry))
        return a64_smp_fail(0, SMP_STAGE_PREPARE, -EINVAL, -EINVAL);

    uint64_t el;
    __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(el));
    log_info("[smp] boot el=");
    kputu(el >> 2);
    log_info(" conduit=");
    log_info(topology->cpu_count == 1 ? "none" :
             topology->conduit == PSCI_CONDUIT_HVC ? "hvc" : "smc");
    log_info("\n");
    log_info("[smp-test] no_ack_cpu=");
    kputu(AARCH64_SMP_TEST_NO_ACK_CPU);
    log_info("\n");

    /* A valid uniprocessor platform needs no PSCI transport at all.  A
     * multiprocessor platform without one FAILS CLOSED — the old
     * degraded "boot with the BSP alone" continuation is gone. */
    if (cpu_count > 1 && psci_init(topology->conduit) != 0) {
        log_warn("[smp] reason=psci-unavailable\n");
        return a64_smp_fail(1, SMP_STAGE_ACTIVATE, -ENODEV, -ENODEV);
    }

    const struct smp_boot_ops ops = {
        .ctx = 0, .counter = boot_counter, .cpu_on = boot_cpu_on,
        .online_acquire = boot_online, .command_release = boot_command,
        .relax = boot_relax,
    };
    struct smp_boot_result result;
    int status = smp_boot_run(topology, secondary_start_addr(), hz, &ops, &result);
    if (status < 0)
        return a64_smp_fail(0, SMP_STAGE_INVARIANT, -EINVAL, -EINVAL);

    for (uint32_t id = 0; id < result.requested; ++id) {
        log_info("[smp] cpu=");
        kputu(id);
        if (result.online_mask & (UINT32_C(1) << id)) {
            log_info(" online mpidr=0x");
            kputx(topology->mpidr[id]);
        } else {
            log_warn(result.failure[id] == SMP_FAILURE_TIMEOUT ?
                     " reason=online-timeout" : " reason=cpu-on-error");
            log_warn("\n[smp] cpu=");
            kputu(id);
            log_warn(" target_mpidr=0x");
            kputx(topology->mpidr[id]);
            log_warn(" rc=");
            int64_t rc = result.psci_rc[id];
            if (rc < 0) { log_warn("-"); rc = -rc; }
            kputu((uint64_t)rc);
        }
        log_info("\n");
    }

    /* Any AP failure (CPU_ON rejected or boot-ACK timeout) FAILS CLOSED:
     * record the first failing CPU and hand the negative errno to the
     * unified orchestrator.  No degraded online_mask freeze, no
     * page-table probe downgrade, no continuation with fewer cores. */
    if (status != 0) {
        for (uint32_t id = 0; id < result.requested; ++id) {
            if (result.failure[id] == SMP_FAILURE_CPU_ON)
                return a64_smp_fail(id, SMP_STAGE_ACTIVATE,
                                    (int32_t)result.psci_rc[id], -EIO);
            if (result.failure[id] == SMP_FAILURE_TIMEOUT)
                return a64_smp_fail(id, SMP_STAGE_ONLINE,
                                    -ETIMEDOUT, -ETIMEDOUT);
        }
        return a64_smp_fail(0, SMP_STAGE_INVARIANT, -EINVAL, -EINVAL);
    }

    boot_result = result;
    a64_smp_status.requested = cpu_count;
    a64_smp_status.online = result.online;
    a64_smp_status.online_mask = result.online_mask;
    aarch64_page_table_probe_finish(true);
    log_summary(cpu_count, result.online);
    return 0;
}

/* ── AP lifecycle hooks (strong overrides, called only by
 * kernel/core/smp.c::smp_ap_common_entry) ─────────────────── */

void arch_smp_ap_cpu_init(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu;
    /* Per-CPU tick ownership (per-CPU tick accounting, Task 2).
     *
     * The GLOBAL time base still belongs to the BSP alone: the timer
     * core (kernel/time/timer.c) and the softirq path
     * (kernel/intr/softirq.c) have no per-CPU split, so jiffies and the
     * timer list are global BSS objects.  An AP therefore never runs
     * tick_handler() and never touches the global clock.
     *
     * What an ADMITTED AP does own is its OWN private CNTP
     * interrupt, which is used ONLY for the local scheduler tick
     * (per-CPU / per-task accounting and the tick-driven vruntime).
     * That counter is armed later, from the AP idle handoff, through
     * aarch64_sched_tick_start() -> aarch64_timer_start_local(), using
     * the configuration the BSP froze and published.
     *
     * The defensive write below is therefore still correct: the
     * counter is cleared here, while the AP is still in its pre-
     * admission work loop, so no path can leave a half-armed timer
     * running before the AP is admitted.  The AP's real arm happens
     * in the admission handoff, never here. */
    cntp_ctl_el0_write(0);
    gic_cpu_init();
    gic_target_bit_init(cpu_id);
    /* Phase 2 #3: install real per-CPU data for AP. head.S:649
     * already set TPIDR_EL1 = &percpu_data[cpu_id]; percpu_install_gs
     * re-confirms (idempotent) and percpu_init populates
     * self/cpu_id/arch_processor_id/rq_lock (BOOT-1: percpu_init does
     * NOT publish online — the common entry does that itself next, after
     * the page-table verify and before the boot ACK). Inline asm
     * 'mrs xN, mpidr_el1' (no helper function — does not exist
     * in codebase, R3 NIT-2). */
    uint32_t mpidr_ap;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr_ap));
    percpu_install_gs(cpu_id);
    percpu_init(cpu_id, mpidr_ap);
    if (aarch64_page_table_ap_verify(cpu_id))
        for (;;)
            arch_cpu_halt();
}

void arch_smp_ap_post_online(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu;
    /* Boot-slot ACK: the AP's runtime online is already published by the
     * common entry; the ACK is this AP's own slot publication.  The
     * NO_ACK injection suppresses only the ACK, never runtime online,
     * and grants no scheduler admission. */
    if (cpu_id != AARCH64_SMP_TEST_NO_ACK_CPU)
        boot_online_set(cpu_id);
    uint32_t command;
    do {
        command = boot_go_get(cpu_id);
        if (command == AARCH64_BOOT_GO_WAIT) arch_cpu_pause();
    } while (command == AARCH64_BOOT_GO_WAIT);
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);
}

void arch_smp_ap_publish_ipi_ready(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu;
    /* M3 (Task 10): the common entry has already unmasked IRQs
     * (arch_local_irq_enable) before calling this hook.  ISB then
     * publish: handler registered (gic_cpu_init) + GIC target bit
     * validated + IRQ unmasked + ISB → ipi_ready_publish_and_count
     * (BSP/AP same function). */
    __asm__ __volatile__("isb" ::: "memory");
    ipi_ready_publish_and_count(cpu_id);
}

void arch_smp_ap_enter_idle(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu;
    /* Work loop: run at most one ap_work item per iteration, then
     * check the P1 release gate.  An ACCEPTED AP whose gate is open
     * leaves this loop for good through the validating trampoline
     * (aarch64_ap_sched_enter); a late or never-ACKed AP never passes
     * released() and stays here forever — the pre-P1 behaviour. */
    for (;;) {
        ap_work_run_one(cpu_id);
        if (aarch64_ap_sched_released(cpu_id))
            aarch64_ap_sched_enter(cpu_id);
        arch_cpu_pause();
    }
}

/* BSP post-boot hook: the SMP spinlock selftest runs after the unified
 * complete marker and before subsys_prepare_cpus(). */
void arch_smp_post_boot_hook(uint32_t online_cpus)
{
    (void)test_spinlock_smp(online_cpus);
}
