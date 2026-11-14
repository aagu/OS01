#ifndef _ARCH_X86_64_VMA_H
#define _ARCH_X86_64_VMA_H

/*
 * x86_64 VMA leaf conversion (mm-architecture-boundaries Task 1).
 *
 * The VMA's logical permission bits (VMA_PROT_*) are the SINGLE source
 * of truth for user leaf permissions; no hardware PTE word is stored in
 * the shared vma_t metadata.  This conversion is the one place where
 * the logical bits become x86 hardware leaf bits.
 */

#include <stdint.h>
#include <memory/vma.h>

/* Convert validated VMA logical permission bits into an x86 user 4 KiB
 * PTE flag word:
 *   - zero permission  → USER-only template (no PRESENT; the PROTNONE
 *                        stash path adds PAGE_PROTNONE itself);
 *   - R / RW           → NX (PTE permissions plan);
 *   - RX / RWX         → executable (prot_to_vma_flags already granted
 *                        EXEC its implicit READ);
 *   - WRITE            → from the VMA. */
uint64_t x86_vma_pte_flags(uint64_t vm_flags);

#endif /* _ARCH_X86_64_VMA_H */
