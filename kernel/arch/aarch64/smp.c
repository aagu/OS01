/* BSP publication for firmware-managed PSCI secondaries.
 *
 * All per-CPU metadata uses its low identity VA, before and after MMU-on.
 * Initialize and clean once before the first CPU_ON; afterward the coherent
 * online/go fields belong to the release/acquire ACK/command protocol.
 */
#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/dtb.h>
#include <arch/aarch64/psci.h>
#include <arch/aarch64/smp.h>
#include <arch/aarch64/smp_boot_core.h>
#include <arch/irq.h>
#include <arch/aarch64/boot_log.h>
#include "aarch64_percpu.h"
#include "reg.h"

static bool boot_published;

static void clean_to_poc(uint64_t start, uint64_t end, uint64_t line_size)
{
    uint64_t line = start & ~(line_size - 1);
    for (; line < end; line += line_size) {
        __asm__ __volatile__("dc cvac, %0" :: "r"(line) : "memory");
    }
}

int aarch64_smp_publish_boot(const struct aarch64_topology *topology)
{
    uint64_t bsp, ctr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(bsp));
    bsp &= AARCH64_MPIDR_AFFINITY_MASK;
    if (boot_published || !topology || !topology->cpu_count ||
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
        slots[i].reserved = 0;
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
    __asm__ __volatile__("dsb sy" ::: "memory");
    boot_published = true;
    return 0;
}

/* Preserve the frozen result for post-boot debugger diagnostics too. */
static volatile struct smp_boot_result boot_result;

static __attribute__((noreturn)) void smp_fatal(const char *reason)
{
    log_err("[smp] FATAL: ");
    log_err(reason);
    log_err("\n");
    for (;;) arch_cpu_halt();
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

static void log_summary(uint32_t requested, uint32_t online)
{
    log_info("[smp] requested=");
    kputu(requested);
    log_info(" online=");
    kputu(online);
    if (online == requested) log_info(" status=PASS\n");
    else log_warn(" status=DEGRADED\n");
}

uint32_t smp_boot_aps(void)
{
    if (boot_published) smp_fatal("repeated SMP boot");
    const struct aarch64_topology *topology = dtb_topology();
    uint64_t hz = cntfrq_el0();
    if (!hz || hz > UINT64_MAX / 2)
        smp_fatal("invalid counter frequency");

    /* Before the one-shot publication, initialize all shared test data.
     * Nothing may reset slots, stacks or test state after CPU_ON. */
    spin_init(&bench_lock);
    benchmark_total = 0;
    for (uint32_t id = 0; id < NR_CPUS; ++id)
        bench_done_set(id, 0);
    if (aarch64_smp_publish_boot(topology))
        smp_fatal("invalid or repeated boot publication");

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

    /* A valid uniprocessor platform needs no PSCI transport at all. */
    if (topology->cpu_count > 1 && psci_init(topology->conduit)) {
        boot_result.requested = topology->cpu_count;
        boot_result.online = 1;
        boot_result.online_mask = 1;
        for (uint32_t id = 1; id < topology->cpu_count; ++id) {
            boot_result.psci_rc[id] = -1;
            boot_result.failure[id] = SMP_FAILURE_CPU_ON;
            boot_go_set(id, AARCH64_BOOT_GO_IDLE);
        }
        log_warn("[smp] reason=psci-unavailable\n");
        log_summary(topology->cpu_count, 1);
        return 1;
    }

    const struct smp_boot_ops ops = {
        .ctx = 0, .counter = boot_counter, .cpu_on = boot_cpu_on,
        .online_acquire = boot_online, .command_release = boot_command,
        .relax = boot_relax,
    };
    struct smp_boot_result result;
    int status = smp_boot_run(topology, secondary_start_addr(), hz, &ops, &result);
    if (status < 0) smp_fatal("invalid boot state machine input");
    boot_result = result;
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
    log_summary(result.requested, result.online);
    return result.online;
}

void secondary_idle(uint32_t cpu_id)
{
    /* Assembly validated our slot/MPIDR, SP, TPIDR and vectors. All
     * accesses below retain the slot's identity VA. IRQs stay masked. */
    gic_cpu_init();
    /* Phase 2 #3: install real per-CPU data for AP. head.S:649
     * already set TPIDR_EL1 = &percpu_data[cpu_id]; percpu_install_gs
     * re-confirms (idempotent) and percpu_init populates
     * self/cpu_id/arch_processor_id/online/rq_lock. Inline asm
     * 'mrs xN, mpidr_el1' (no helper function — does not exist
     * in codebase, R3 NIT-2). */
    extern void percpu_install_gs(uint32_t cpu);
    extern void percpu_init(uint32_t cpu, uint32_t apic_id);
    uint32_t mpidr_ap;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr_ap));
    percpu_install_gs(cpu_id);
    percpu_init(cpu_id, mpidr_ap);
    /* Phase 2 #3 Commit 2: AP CNTP enabled. APs receive their own
     * CNTP PPI tick on each timer cycle. tick_handler() runs per-CPU
     * via this_cpu() (now pointing at percpu_data[cpu] from Commit 1). */
    /* Test-only loss of ACK: the AP still reaches C initialization and
     * observes the persistent idle command after the BSP times out. */
    if (cpu_id != AARCH64_SMP_TEST_NO_ACK_CPU)
        boot_online_set(cpu_id);
    uint32_t command;
    do {
        command = boot_go_get(cpu_id);
        if (command == AARCH64_BOOT_GO_WAIT) arch_cpu_pause();
    } while (command == AARCH64_BOOT_GO_WAIT);
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);

#if OS01_SELFTEST
    /* R11 + Task 2.2 simplification: only the selftest build unmask DAIF.I
     * on APs so they can take SGIs through el1_irq (GIC Phase 1, spec §7.5).
     * Production build stays unchanged — APs remain IRQ-masked. */
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");
#endif
    for (;;) arch_cpu_halt();
}
