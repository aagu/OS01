// kernel/memory/tlb.c — TLB shootdown, serialized generation-ack protocol
// (M2/M3 Task 12).  Shared between x86_64 and aarch64 (whitelisted in
// kernel/Makefile for both).
//
// Protocol (spec §5.4):
//
//   1. The initiator takes tlb_sd_lock (a PLAIN spin_lock — waiters keep
//      IRQs enabled so they can still answer a TLB IPI while spinning;
//      never take this lock with spin_lock_irqsave on aarch64 — that
//      would block the SGI response and self-deadlock the ack wait).
//   2. Snapshot the target set = online ∧ ¬self ∧ ipi_ready (load-acquire
//      on ipi_ready: a CPU that has not published its IPI channel yet
//      must never be targeted — its IRQs may still be masked).
//   3. Snapshot per-target ack generations: target_gen[cpu] =
//      load(tlb_ack_gen) + 1.
//   4. Flush the LOCAL TLB and bump the local tlb_ack_gen (release).
//   5. ipi_broadcast(IPI_VECTOR_TLB, mask) if the mask is non-empty.
//   6. Spin until every target's tlb_ack_gen == its target_gen.  The
//      comparison is `!=`-based equality, NOT `<=`: uint32 wraparound
//      (gen 0xFFFFFFFF → 0) must still satisfy the wait.
//   7. Timeout (arch_cycle_counter deadline) → tlb_shootdown_panic()
//      (weak FATAL hook; kpanic is not in the aarch64 kernel whitelist,
//      pmm.c/slab.c convention).
//
// Includes kept deliberately dependency-light so the hosttest can
// compile this file against mock headers (mock/tlb_test_runtime.h).

#include <stdint.h>

#include <percpu/percpu.h>      /* percpu_data, this_cpu, cpu_id, num_cpus */
#include <intr/ipi.h>           /* ipi_broadcast, IPI_VECTOR_TLB */
#include <arch/spinlock.h>      /* spinlock_T, spin_lock, spin_unlock */
#include <arch/mmu.h>           /* arch_flush_tlb_all */
#include <arch/cpu.h>           /* arch_cycle_counter, arch_cpu_pause */

// ── Tunables ─────────────────────────────────────────────
// Ack-wait deadline in cycle-counter ticks.  Sized for QEMU TCG, whose
// virtual counter runs far below real hardware frequency (~62.5 MHz on
// aarch64 QEMU, ~1 GHz nominal on x86 TSC): 1e9 cycles ≈ 16 s on QEMU
// aarch64, ≈ 0.3 s on a 3 GHz host.  A live target acknowledges in
// microseconds; only a wedged/dead CPU hits this, and continuing would
// silently tolerate stale TLB entries — hence FATAL.
#define TLB_SD_TIMEOUT_CYCLES  1000000000ULL

// ── Serialization lock (Task 12; later vmm-change entry paths share it)
// Plain spin_lock: waiters keep IRQs enabled (invariant I2 — the TLB
// handler itself takes no lock), so an IPI arriving while a waiter
// spins is still serviced.
spinlock_T tlb_sd_lock = { .lock = 1UL };

// ── FATAL path ───────────────────────────────────────────
// kpanic is not in the aarch64 kernel source whitelist (pmm.c/slab.c
// convention).  Weak hook: the kernel default spins forever, the
// hosttest overrides it with a longjmp capture.
__attribute__((weak)) void tlb_shootdown_panic(const char *reason)
{
    (void)reason;
    for (;;)
        ;
}

// Target snapshot = online ∧ ipi_ready ∧ ¬self.  ipi_ready is the
// one-shot publication from Task 11 / x86 boot (plain uint32_t, read
// with acquire so the target's handler setup is visible before we
// aim an IPI at it).
static uint64_t build_target_mask_excl_self(void)
{
    uint32_t self = cpu_id();
    uint64_t ready = 0;

    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        if (cpu == self || !percpu_data[cpu].online)
            continue;
        if (__atomic_load_n(&percpu_data[cpu].ipi_ready,
                            __ATOMIC_ACQUIRE))
            ready |= (1UL << cpu);
    }
    return ready;
}

void tlb_shootdown(void)
{
    // Serializes shootdowns: every initiator observes a consistent
    // (gen snapshot → broadcast → ack wait) window per target CPU.
    spin_lock(&tlb_sd_lock);

    uint64_t mask = build_target_mask_excl_self();

    // Snapshot the per-target ack generations BEFORE flushing/broadcast:
    // each target must bump its counter exactly once (to gen+1, mod 2^32).
    uint32_t target_gen[NR_CPUS] = {0};
    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        if (mask & (1UL << cpu))
            target_gen[cpu] =
                __atomic_load_n(&percpu_data[cpu].tlb_ack_gen,
                                __ATOMIC_RELAXED) + 1;
    }

    // Local invalidation + local ack first: our own TLB is stale too,
    // and the release increment orders the flush before our reads of
    // target gens can complete the handshake.
    arch_flush_tlb_all();
    __atomic_fetch_add(&this_cpu()->tlb_ack_gen, 1, __ATOMIC_RELEASE);

    if (mask)
        ipi_broadcast(IPI_VECTOR_TLB, mask);

    // Wait for every target to reach its snapshot generation.  Equality
    // (not <=): uint32 wraparound must not resurrect a stale ack.
    uint64_t start = arch_cycle_counter();
    while (mask) {
        uint64_t done = 0;
        for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
            if (!(mask & (1UL << cpu)))
                continue;
            uint32_t g = __atomic_load_n(&percpu_data[cpu].tlb_ack_gen,
                                         __ATOMIC_ACQUIRE);
            if (g == target_gen[cpu])
                done |= (1UL << cpu);
        }
        mask &= ~done;
        if (!mask)
            break;
        if (arch_cycle_counter() - start > TLB_SD_TIMEOUT_CYCLES) {
            spin_unlock(&tlb_sd_lock);
            tlb_shootdown_panic("tlb_shootdown: TIMEOUT (stale targets)");
            return;                 /* not reached in kernel builds */
        }
        arch_cpu_pause();
    }

    spin_unlock(&tlb_sd_lock);
}
