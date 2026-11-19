// kernel/arch/x86_64/smp/boot.c
//
// x86_64 SMP bring-up helpers — extracted from kernel/core/main.c
// as part of the kernel-main refactor (Task 5).
//
// Owns the per-CPU construction phase that previously lived inline in
// kernel_main:
//
//   x86_64_boot_percpu() — single-CPU MADT traversal with NR_CPUS
//                          drop-out branch, percpu_init() per enabled
//                          APIC entry, BSP (cpu_idx==0) TSS/tss_hw/GS/
//                          online writes in that exact order, and the
//                          num_cpus publication only AFTER the loop
//                          completes.  The "publish at end" ordering
//                          matters: AP spin-ups in the next stage read
//                          num_cpus to know how many APs to start.
//
// The stage-8 AP bring-up is the unified kernel/core/smp.c
// arch_boot_aps() since the SMP boot unification (spec 2026-11-19);
// the x86_64 side of it lives in smp/smp.c (arch/smp.h strong
// overrides).

#include <arch/x86_64/smp_boot.h>
#include <arch/boot.h>
#include <selftest/result.h>
#include <core/selftest.h>

#include <stdint.h>

#include <percpu/percpu.h>
#include <core/panic.h>
#include <arch/x86_64/tss.h>
#include <intr/apic.h>
#include <intr/ipi.h>         /* IPI_VECTOR_TLB */
#include <driver/serial.h>
#include <core/printk.h>
#include <log/log.h>
#include <arch/thread.h>
#include <time/clockevent.h>
#include <sched/accounting.h>
#include <subsys/subsys.h>
#include <arch/subsys.h>
#include <arch/x86_64/platform/boot_irq.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <core/assert.h>

// ── M3 Task 12: ipi_ready publication (x86 side) ─────────
// Mirrors aarch64's ipi_ready_publish_and_count (vmm_gate.c): a CPU must
// only be a TLB-shootdown target (ipi_ready ∧ online in
// build_target_mask_excl_self) after its IDT carries the IPI handler and
// its IRQs are unmasked — otherwise a broadcast IPI would arrive with
// IF=0, never be acked, and the initiator would spin to its FATAL
// timeout.  x86 has NO smp_starting gate: publication happens directly
// at the two points below.

static int x86_irqs_enabled(void)
{
    arch_irq_state_t flags = arch_local_irq_save();
    arch_local_irq_restore(flags);
    return (flags & (1UL << 9)) != 0;   /* RFLAGS.IF */
}

void ipi_ready_publish_bsp(void)
{
    /* Handler table entry for vector 0x40 is written by ipi_init()
     * (smp_boot_aps) — publishing before that would ack nothing. */
    ASSERT(intr_handler_table[IPI_VECTOR_TLB] != 0);
    ASSERT(this_cpu() == &percpu_data[0]);
    /* BOOT-1: ipi_ready=1 ⇒ online=1 (spec §4 invariant 4). */
    ASSERT(__atomic_load_n(&percpu_data[0].online, __ATOMIC_ACQUIRE) == 1);
    ASSERT(x86_irqs_enabled());
    __atomic_store_n(&percpu_data[0].ipi_ready, 1, __ATOMIC_RELEASE);
}

void ipi_ready_publish_ap(uint32_t cpu)
{
    ASSERT(cpu < num_cpus);
    /* BOOT-1: ipi_ready=1 ⇒ online=1 (spec §4 invariant 4). */
    ASSERT(__atomic_load_n(&percpu_data[cpu].online, __ATOMIC_ACQUIRE) == 1);
    ASSERT(x86_irqs_enabled());
    __atomic_store_n(&percpu_data[cpu].ipi_ready, 1, __ATOMIC_RELEASE);
}

void x86_64_boot_percpu(void)
{
    /* BOOT-2: construct exactly once.  The x86 boot calls this right
     * after phase 3 (before any device IRQ can be enabled); any later
     * call must go through the verify-only late facade. */
    if (num_cpus != 0) {
        log_warn("percpu: already constructed (num_cpus=%u); ignoring "
                 "repeat call\n", num_cpus);
        return;
    }
    uint32_t cpu_idx = 0;
    for (uint32_t i = 0; i < apic_info.lapic_count; i++) {
        if (!(apic_info.lapics[i].flags & 1))
            continue;

        if (cpu_idx >= NR_CPUS) {
            log_warn("percpu: APIC id=%u DROPPED (NR_CPUS=%u)\n",
                     apic_info.lapics[i].apic_id, (unsigned)NR_CPUS);
            continue;
        }

        percpu_init(cpu_idx, apic_info.lapics[i].apic_id);

        if (cpu_idx == 0) {
            percpu_data[0].tss = &init_tss[0];
            percpu_data[0].tss_hw = arch_task_boot_state();
            percpu_install_gs(0);
            /* BOOT-1: the BSP's own runtime online publication is the
             * ONLY place besides the AP paths where online is written.
             * percpu_init() deliberately leaves it 0 — a constructed
             * CPU is not a running CPU (spec §5.1). */
            __atomic_store_n(&percpu_data[0].online, 1, __ATOMIC_RELEASE);
            log_info("percpu: BSP  (cpu=%u, apic_id=%u) online\n",
                     cpu_idx, apic_info.lapics[i].apic_id);
        } else {
            log_info("percpu: AP   (cpu=%u, apic_id=%u) registered\n",
                     cpu_idx, apic_info.lapics[i].apic_id);
        }
        cpu_idx++;
    }
    log_info("percpu: %u CPU(s) registered (%u in MADT)\n",
             cpu_idx, apic_info.lapic_count);
    num_cpus = cpu_idx;
}

void arch_boot_smp_prepare(void)
{
    // Monitor plan Task 2: initialize the per-CPU tick accounting ONCE,
    // here, BEFORE tick_start(): GS/percpu are already usable (BSP's
    // percpu_install_gs(0) ran in x86_64_boot_percpu) and the APs are
    // not up yet.  Until sched_accounting_init() release-publishes
    // accounting_ready, early tick interrupts (the PIT may already be
    // armed) stay inert in sched_account_tick() and never touch the
    // still-zeroed usage spinlocks.
    sched_accounting_init();

    // 显式启动 tick 源：GS base 已装（x86_64_boot_percpu 中的
    // percpu_install_gs(0)），this_cpu() 可用。tick_start 先掩 PIT
    // 再启 LAPIC，失败回退 PIT。
    tick_start();
}

/* arch_boot_aps() (stage 8 AP bring-up + subsys_prepare_cpus) is the
 * unified kernel/core/smp.c orchestrator since the SMP boot unification
 * (spec 2026-11-19); the x86_64 backend implements the arch/smp.h
 * strong overrides in smp/smp.c. */

// ── Boot Facade Strong Overrides ─────────────────────────────

void arch_boot_percpu(void)
{
    if (num_cpus != 0) {
        if (__atomic_load_n(&percpu_data[0].online, __ATOMIC_ACQUIRE) != 1 ||
            percpu_data[0].self != (uint64_t)&percpu_data[0]) {
            kpanic("BOOT-2: late percpu verify failed\n");
        }
        return;
    }
    x86_64_boot_percpu();
    x86_boot_irq_context_prepare();
}

void arch_boot_runtime(void)
{
    /* x86 runtime bringup completes during earlier phases; no redundant operations */
}

void arch_post_smp_selftest(void)
{
#ifdef OS01_SELFTEST
    serial_printk("[selftest] running built-in tests...\n");
    selftest_begin_run(arch_selftest_expected_late());
    selftest_run_all();
    serial_printk("[selftest] done\n");
#endif
}

