#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

/*
 * Public, arch-neutral VMM surface (mm-architecture-boundaries Task 2).
 *
 * The ONLY thing shared across architectures through this header is
 * the TLB broadcast protocol (tlb_shootdown, implemented in
 * memory/tlb.c for both ports).  It must NOT define or transitively
 * provide: semantic VM_* bits, arch_vmm_* APIs, raw page-table
 * prototypes, ptbl_t/kernel_map, PTE bits, MMU instructions or uaccess
 * declarations.  Each architecture owns those in its private headers:
 *   - x86: <arch/x86_64/vmm.h> (raw API) + <arch/x86_64/mmu.h>
 *   - ARM: <arch/aarch64/vmm_backend.h> (semantic backend)
 */

#include <stdint.h>

// ── TLB shootdown (SMP) ─────────────────────────────
// When modifying shared kernel page tables (kernel_map), other CPUs
// may have stale TLB entries.  Call this instead of a local flush to
// notify all online cores via IPI.  Internally falls through to the
// local full-TLB reload when num_cpus ≤ 1.  Shared mappings and the
// existing legal call sites keep obeying the IRQ / lock / target-CPU
// constraints documented in docs/smp/smp.md.
void tlb_shootdown(void);

#endif /* _KERNEL_VMM_H */
