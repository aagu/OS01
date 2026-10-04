#ifndef _AARCH64_IPI_H
#define _AARCH64_IPI_H

/* AArch64 IPI: logical vector → SGI mapping (M3 Task 7).
 *
 * The logical IPI vector space is shared with x86_64 and defined in
 * <intr/ipi.h> (IPI_VECTOR_TLB = 0x40, IPI_VECTOR_RESCHED = 0x41 —
 * include that header for the constants; they are NOT redefined here).
 * GICv2 SGIs occupy INTIDs 0-15, so the logical vector number itself can
 * never be delivered: this layer maps IPI_VECTOR_TLB → SGI 3 and refuses
 * (kpanic) anything else until later M3 tasks define more mappings.
 *
 * ipi_broadcast() takes a LOGICAL CPU bitmask (bit i = percpu_data[i]),
 * which is translated to the GICv2 8-bit TargetListFilter byte via the
 * per-CPU gic_target_bit[] cache. The cache is zero-initialized; on real
 * hardware it is populated from GICD_ITARGETSR during SMP bring-up
 * (Task 8). Hosttests populate it via gic_target_bit_inject().
 */

#include <stdint.h>

/* Broadcast the IPI `vector` to every logical CPU set in `target_mask`.
 * An empty mask sends nothing. Unsupported vectors reach the weak hook
 * ipi_panic_unsupported_vector() (default: spin forever; the hosttest
 * overrides it — kpanic is not in the aarch64 kernel source whitelist). */
void ipi_broadcast(uint32_t vector, uint64_t target_mask);

/* Weak fatal hook (see above); defined in intr/ipi.c. */
void ipi_panic_unsupported_vector(uint32_t vector);

/* Per-CPU GIC target byte cache accessors. `gic_target_bit_inject` is
 * the single write path (SMP bring-up on hardware; hosttests directly).
 * Out-of-range cpu indices are ignored. */
void gic_target_bit_inject(uint32_t cpu, uint8_t byte);
uint8_t gic_target_bit_get(uint32_t cpu);

/* Probe this CPU's banked GICD_ITARGETSR0 and publish gic_target_bit[]
 * (M3 Task 8). Call once per CPU BEFORE that CPU publishes ipi_ready /
 * enters smp_starting_enter (M7.2 ordering contract). Single-core builds
 * take the RAZ/WI exception (bit[cpu] = 1 without a hardware read);
 * multi-core builds require ITARGETSR0 SGI byte 0 == 1u << cpu_id and
 * otherwise FATAL via the weak ipi_fatal_itargets hook. Not yet called
 * from boot — SMP bring-up (Task 11) owns the call site. */
void gic_target_bit_init(uint32_t cpu_id);

/* Weak hooks (defaults: spin forever / no-op; hosttests override):
 *  - ipi_fatal_itargets: ITARGETSR0 byte was 0, multi-bit, or
 *    non-identity (v1 review item 11 — FATAL, not WARN).
 *  - ipi_warn_mask_bit_out_of_range: a broadcast mask had bits beyond
 *    AARCH64_BOOT_MAX_CPUS, which are dropped (M7.3). */
void ipi_fatal_itargets(uint32_t cpu_id, uint8_t byte);
void ipi_warn_mask_bit_out_of_range(uint64_t mask);

#endif /* _AARCH64_IPI_H */
