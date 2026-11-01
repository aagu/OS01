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

struct pt_regs;

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

/* ── P2: resched SGI4 (spec §5.1) ───────────────────────────────
 * aarch64_sched_kick(): deliver the remote reschedule request for
 * logical @cpu as SGI 4.  Prerequisite: the target published
 * ipi_ready (scheduler_ok is NOT required — an AP may take the SGI
 * while still polling its old work loop).  An out-of-range or
 * not-IPI-ready target is a logged fail-stop: nothing is sent.
 * Local targets return immediately (the caller already published
 * need_resched; a local ap_work producer never sleeps).
 *
 * aarch64_resched_sgi_handler(): the GIC handler registered by
 * gic_init BEFORE any AP starts.  Release-stores THIS CPU's
 * need_resched=1 and nothing else: no current access (safe on the
 * boot stack), no locks, no schedule, no EOI (gic_dev_dispatch owns
 * the single EOI with the raw IAR).  Merged/duplicate SGIs are
 * inherently idempotent. */
void aarch64_sched_kick(unsigned cpu);
void aarch64_resched_sgi_handler(uint32_t intid, uint64_t param,
                                 struct pt_regs *regs);

/* Per-CPU need_resched slot resolution for the handler.  The target
 * build inlines the arch per-CPU pointer read (TPIDR_EL1 +
 * PERCPU_NEED_RESCHED_OFFSET); the host build overrides this seam. */
#if !defined(__aarch64__)
uint64_t *aarch64_resched_need_slot(void);
#endif

/* Weak hooks (defaults: spin forever / no-op; hosttests override):
 *  - ipi_fatal_itargets: ITARGETSR0 byte was 0, multi-bit, or
 *    non-identity (v1 review item 11 — FATAL, not WARN).
 *  - ipi_warn_mask_bit_out_of_range: a broadcast mask had bits beyond
 *    AARCH64_BOOT_MAX_CPUS, which are dropped (M7.3). */
void ipi_fatal_itargets(uint32_t cpu_id, uint8_t byte);
void ipi_warn_mask_bit_out_of_range(uint64_t mask);

#endif /* _AARCH64_IPI_H */
