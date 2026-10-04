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
 * gic_target_bit_inject() instead of depending on Task 8's hw read. */
static uint8_t gic_target_bit[AARCH64_BOOT_MAX_CPUS];

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
 * target bytes. Bits beyond the table are ignored (same capacity as
 * NR_CPUS on every current build). */
static uint8_t logical_to_gic_targets(uint64_t mask)
{
    uint8_t out = 0;
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
