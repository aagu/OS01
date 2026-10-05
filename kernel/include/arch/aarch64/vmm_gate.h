#ifndef OS01_AARCH64_VMM_GATE_H
#define OS01_AARCH64_VMM_GATE_H

/* AArch64 VMM change-entry gate + shootdown foundations (M3 Task 7).
 *
 * Four primitives, all defined in kernel/arch/aarch64/memory/vmm_gate.c:
 *
 *   - Root registry: aarch64_pt_root_publish / aarch64_pt_root_is_published.
 *     Every translation root that is (or was) installed into TTBR must be
 *     published BEFORE install; later shootdown backends (Task 16+) use
 *     is_published to decide whether a root still needs the IPI protocol.
 *     Capacity 8, idempotent, root_pa==0 rejected, overflow is a violation.
 *
 *   - smp_starting_enter: one-way latch set by the BSP when AP bring-up
 *     starts. Once set, the pre-SMP grace period in vmm_gate_check is over.
 *
 *   - ipi_ready_publish_and_count: BSP and APs all publish through this
 *     single function (one-shot per CPU, counted in ipi_ready_count).
 *
 *   - vmm_gate_check: entry gate for every arch_vmm_* / aarch64_pt_*
 *     public primitive. Before SMP starts it is a no-op (pre-SMP例外);
 *     after smp_starting_enter it panics unless every CPU discovered from
 *     the DTB has published ipi_ready — a VMM change must never race a
 *     half-started SMP system.
 *
 * Invariant violations reach the weak hook vmm_gate_violation() (default:
 * spin forever — kpanic is not in the aarch64 kernel source whitelist;
 * hosttests override the hook).
 */

#include <stdint.h>
#include <stdbool.h>

/* Registry capacity: 8 concurrently published translation roots. */
#define AARCH64_PT_MAX_PUBLISHED_ROOTS 8

/* Publish `root_pa` as an installed/installable translation root.
 * Idempotent (re-publishing the same PA returns true); root_pa == 0
 * returns false; a full registry is a violation (and returns false). */
bool aarch64_pt_root_publish(uint64_t root_pa);

/* True iff *root is a published root PA. *root == 0 → false. */
bool aarch64_pt_root_is_published(const uint64_t *root);

/* One-way latch: SMP bring-up has started. A second call is a violation. */
void smp_starting_enter(void);

/* One-shot per-CPU ipi_ready publication + global counter increment.
 * BSP and APs call the same function; a second call for the same CPU is
 * a violation. */
void ipi_ready_publish_and_count(uint32_t cpu);

/* Acquire-load the global ipi_ready counter (M3.5 Task 25 shootdown probe).
 * Counted via ipi_ready_publish_and_count(); the BSP starts at 1 once it
 * publishes its own ipi_ready after arch_local_irq_enable() (Task 11 Step 2
 * ordering) and APs add themselves from secondary_idle (Task 10 Step 4).
 * Returns 0 before any CPU has published. The probe waits for this to
 * reach dtb_cpu_count() — counting the BSP itself — before starting work. */
uint32_t ipi_ready_count_get(void);

/* Entry gate for arch_vmm_* / aarch64_pt_* public primitives. */
void vmm_gate_check(void);

/* Weak violation hook (see file comment); defined in vmm_gate.c. */
void vmm_gate_violation(const char *reason);

#endif /* OS01_AARCH64_VMM_GATE_H */
