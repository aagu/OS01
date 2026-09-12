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

// Broadcast an IPI to all online CPUs (optionally excluding self).
void ipi_broadcast(uint8_t vector, int exclude_self);

// Register IPI vectors in the IDT.  Called once during SMP init.
void ipi_init(void);

#endif
