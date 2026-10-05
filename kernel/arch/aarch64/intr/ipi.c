/* kernel/arch/aarch64/intr/ipi.c — IPI broadcast via GICv2 SGIs (M3 Task 7).
 *
 * Maps the arch-neutral logical IPI vectors (<intr/ipi.h>: IPI_VECTOR_TLB
 * = 0x40 …) onto GICv2 SGI INTIDs and translates a logical-CPU bitmask
 * into the GICD_SGIR 8-bit TargetListFilter byte.
 *
 * Dependency-light by design (like gic_driver.c): no UART logging, no
 * arch inline asm — so the hosttest compiles this file unmodified against
 * mock MMIO. gic_dev_current() lives in gic.c (which pulls DTB/boot-log
 * dependencies the host cannot build); hosttests supply their own.
 */

#include <stdint.h>

#include <intr/ipi.h>            /* IPI_VECTOR_TLB = 0x40 (shared with x86) */
#include <arch/aarch64/ipi.h>    /* public surface of this file */
#include <arch/aarch64/gic.h>    /* gic_send_sgi, gic_dev_current, FILTER_LIST */
#include <arch/aarch64/dtb.h>    /* AARCH64_BOOT_MAX_CPUS */

/* Fatal path. kpanic is not in the aarch64 kernel source whitelist
 * (pmm.c/slab.c convention), so the halt is behind this hook: the weak
 * default just spins; the hosttest overrides it to observe the call. */
__attribute__((weak)) void ipi_panic_unsupported_vector(uint32_t vector)
{
    (void)vector;
    for (;;)
        ;
}

/* Logical vector → SGI INTID map (M3: only the TLB shootdown vector is
 * wired; further vectors get entries here as their consumers land). */
#define AARCH64_IPI_SGI_TLB 3u

/* Per-CPU GIC target byte cache. Static zero-init: before SMP bring-up
 * populates it (from GICD_ITARGETSR, Task 8) every entry is 0, which
 * makes logical_to_gic_targets() return 0 and ipi_broadcast() a no-op —
 * the safe pre-SMP behavior. Hosttests write entries via
 * gic_target_bit_inject() instead of depending on Task 8's hw read.
 *
 * M7.2 ordering contract: a CPU must have its gic_target_bit[] entry
 * published (gic_target_bit_init or gic_target_bit_inject) BEFORE it
 * sets ipi_ready / enters smp_starting_enter. The plain stores here are
 * ordered before the later release-store that publishes ipi_ready, so
 * no other CPU can observe a broadcast-targeting this one while its
 * entry is still zero. */
static uint8_t gic_target_bit[AARCH64_BOOT_MAX_CPUS];

/* FATAL path for gic_target_bit_init (M3 Task 8): the banked
 * GICD_ITARGETSR0 byte did not match the required identity topology.
 * Weak hook so the hosttest can capture it (kpanic is not in the
 * aarch64 kernel source whitelist); the default spins forever. */
__attribute__((weak)) void ipi_fatal_itargets(uint32_t cpu_id, uint8_t byte)
{
    (void)cpu_id; (void)byte;
    for (;;)
        ;
}

/* M7.3 violation hook: logical_to_gic_targets() received a mask bit
 * beyond gic_target_bit[]'s capacity and must drop it. Weak, no-op by
 * default (preserves Task 7's drop-silently behavior); the hosttest
 * overrides it to observe the violation. */
__attribute__((weak)) void ipi_warn_mask_bit_out_of_range(uint64_t mask)
{
    (void)mask;
}

void gic_target_bit_init(uint32_t cpu_id)
{
    /* Bounds guard (review fix round 1): same capacity check as
     * gic_target_bit_inject/get. The multi-core path below already
     * FATALs for cpu_id >= AARCH64_BOOT_MAX_CPUS only by coincidence
     * (sgi_byte == 1u << cpu_id can never match); the single-core path
     * would write out of bounds. Route to the fatal hook. */
    if (cpu_id >= AARCH64_BOOT_MAX_CPUS) {
        ipi_fatal_itargets(cpu_id, 0);
        return; /* unreachable: hook does not return */
    }
    if (dtb_cpu_count() == 1) {
        /* Spec §6.3: on a single-core GICv2, GICD_ITARGETSR0 may be
         * RAZ/WI — reading it back 0 proves nothing. Skip the check. */
        gic_target_bit[cpu_id] = 1u;
        return;
    }
    /* Multi-core: read this CPU's banked GICD_ITARGETSR0 (offset 0x800),
     * SGI byte 0 (SGI INTIDs 0..3 share byte 0 of ITARGETSR0). The GICD
     * base comes from the parsed DTB — never a fixed GIC_DIST_BASE
     * constant. QEMU virt must give exactly one bit = 1u << cpu_id;
     * anything else (0, multiple bits, or non-identity) is FATAL, not
     * WARN (v1 review item 11): a wrong target byte silently drops IPIs,
     * which we refuse to boot with. The store happens only after the
     * byte is validated (see M7.2 above for the publish ordering). */
    volatile uint8_t *itargets =
        (volatile uint8_t *)((uintptr_t)dtb_gicd_base() + 0x800);
    uint8_t sgi_byte = itargets[0];
    if (sgi_byte == 0 || (sgi_byte & (sgi_byte - 1)) != 0 ||
        sgi_byte != (1u << cpu_id)) {
        ipi_fatal_itargets(cpu_id, sgi_byte);
        return; /* unreachable: hook does not return */
    }
    gic_target_bit[cpu_id] = sgi_byte;
}

void gic_target_bit_inject(uint32_t cpu, uint8_t byte)
{
    if (cpu < AARCH64_BOOT_MAX_CPUS)
        gic_target_bit[cpu] = byte;
}

uint8_t gic_target_bit_get(uint32_t cpu)
{
    return cpu < AARCH64_BOOT_MAX_CPUS ? gic_target_bit[cpu] : 0;
}

static uint32_t ipi_vector_to_sgi(uint32_t vector)
{
    switch (vector) {
    case IPI_VECTOR_TLB: return AARCH64_IPI_SGI_TLB;
    default:
        ipi_panic_unsupported_vector(vector);
        return 0; /* unreachable (hook does not return) */
    }
}

/* Logical CPU bitmask → GIC target byte: OR of the cached per-CPU
 * target bytes. Bits beyond the table are dropped (same capacity as
 * NR_CPUS on every current build) and reported via the weak
 * ipi_warn_mask_bit_out_of_range hook (M7.3). */
static uint8_t logical_to_gic_targets(uint64_t mask)
{
    uint8_t out = 0;
    uint64_t out_of_range = mask & ~((UINT64_C(1) << AARCH64_BOOT_MAX_CPUS) - 1);
    if (out_of_range)
        ipi_warn_mask_bit_out_of_range(out_of_range);
    for (uint32_t cpu = 0; cpu < AARCH64_BOOT_MAX_CPUS; cpu++) {
        if (mask & (UINT64_C(1) << cpu))
            out |= gic_target_bit[cpu];
    }
    return out;
}

void ipi_broadcast(uint32_t vector, uint64_t target_mask)
{
    uint32_t sgi = ipi_vector_to_sgi(vector);
    uint8_t targets = logical_to_gic_targets(target_mask);
    if (targets == 0)
        return; /* empty/unmapped mask: nothing to send */
    gic_send_sgi(gic_dev_current(), sgi, targets, GICD_SGIR_FILTER_LIST);
}
