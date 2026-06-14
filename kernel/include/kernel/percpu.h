#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H

#include <stdint.h>
#include <kernel/task.h>

// ──────────────────────────────────────────────
//  Per-CPU data structure
//
//  One instance per logical CPU.  Accessed via
//  GS segment base (IA32_GS_BASE MSR).
//
//  IMPORTANT: field offsets 0 and 8 are
//  hardcoded in kernel/arch/x86_64/entry.S:
//    offset 0  → self pointer (GS:0)
//    offset 8  → need_resched (GS:8)
//  Do NOT reorder or insert fields before these
//  without updating entry.S.
// ──────────────────────────────────────────────

#define PERCPU_NEED_RESCHED_OFFSET  8

typedef struct percpu {
    // ── Assembly-accessed fields ──
    uint64_t self;              // offset 0: self-pointer (GS:0 loads this)
    uint64_t need_resched;      // offset 8: per-CPU reschedule flag
    // ── C-only fields ──
    uint32_t cpu_id;            // logical CPU ID (0 .. NR_CPUS-1)
    uint32_t apic_id;           // Local APIC ID (from MADT)
    uint32_t online;            // 1 when CPU is fully initialized
    uint32_t scheduler_ok;      // per-CPU scheduler_initialized guard
    struct tss_struct *tss;     // this CPU's TSS (in GDT slot 7)
    // ── IPI / TLB shootdown ──
    uint32_t tlb_wanted;        // atomic flag: TLB invalidate requested
    uint32_t tlb_ack;           // atomic counter: shootdown ACK
} percpu_t;

// Number of CPUs supported (from kernel/include/kernel/arch/x86_64/cpu.h)
#ifndef NR_CPUS
#error "NR_CPUS must be defined (see kernel/include/kernel/arch/x86_64/cpu.h)"
#endif

extern percpu_t percpu_data[NR_CPUS];

// Number of CPUs actually discovered from MADT (≤ NR_CPUS).
// Set by main.c after percpu_init loop.  All runtime loops
// should iterate over num_cpus, not NR_CPUS.
extern uint32_t num_cpus;

// ── Per-CPU accessors ──────────────────────────────

// Return a pointer to the current CPU's percpu struct.
// Reads the self-pointer at GS:0 — GS base must already
// be installed via percpu_install_gs().
static inline percpu_t *this_cpu(void)
{
    percpu_t *cpu;
    __asm__ __volatile__(
        "movq %%gs:0, %0"
        : "=r"(cpu)
    );
    return cpu;
}

// Convenience: logical CPU ID of the executing core.
// Returns 0 if GS base is not yet set up.
static inline uint32_t cpu_id(void)
{
    percpu_t *cpu = this_cpu();
    return cpu ? cpu->cpu_id : 0;
}

// ── Initialisation ─────────────────────────────────

// Set up percpu_data[cpu] with the given APIC ID.
// Does NOT install GS base — call percpu_install_gs() for that.
void percpu_init(uint32_t cpu, uint32_t apic_id);

// Write IA32_GS_BASE MSR to point GS at this CPU's percpu struct.
// After this call, this_cpu() / cpu_id() work on this core.
void percpu_install_gs(uint32_t cpu);

#endif // _KERNEL_PERCPU_H
