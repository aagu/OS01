#ifndef _KERNEL_IPI_H
#define _KERNEL_IPI_H

#include <stdint.h>

// ── IPI vector allocation (0x40 – 0x4F reserved for IPIs) ────

#define IPI_VECTOR_TLB      0x40   // TLB shootdown
#define IPI_VECTOR_RESCHED  0x41   // reschedule request

// ── API ───────────────────────────────────────────────────

// Send an IPI to a specific APIC ID with the given vector.
// Destination is physical mode, fixed delivery.
void ipi_send(uint32_t dest_apic_id, uint8_t vector);

// Broadcast an IPI to every CPU set in the logical-CPU bitmask
// `target_mask` (bit i = percpu_data[i]). Callers decide self-exclusion
// by clearing their own bit; an empty mask sends nothing.
// x86_64 delivers via LAPIC ICR per target; aarch64 maps the logical
// vector onto a GICv2 SGI (see <arch/aarch64/ipi.h>).
void ipi_broadcast(uint32_t vector, uint64_t target_mask);

// Register IPI vectors in the IDT.  Called once during SMP init.
void ipi_init(void);

// ── M3 Task 12: per-CPU ipi_ready publication (x86_64) ───
// BSP: after ipi_init() in smp_boot_aps(), before any AP is started.
// AP: in ap_entry() after the kernel IDT is loaded and IRQs enabled.
// aarch64 publishes through ipi_ready_publish_and_count() instead
// (<arch/aarch64/vmm_gate.h>).
void ipi_ready_publish_bsp(void);
void ipi_ready_publish_ap(uint32_t cpu);

#endif
