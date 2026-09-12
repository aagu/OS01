#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

// ──────────────────────────────────────────────────────────
//  SMP Boot API
//
//  smp_boot_aps() brings up all secondary CPUs discovered
//  from the MADT/ACPI table.  The implementation is arch-specific
//  (x86_64 uses a real-mode trampoline at 0x8000; aarch64
//  may use PSCI or spin-table protocol).
//
//  Called from kernel_main() once, after percpu_init()
//  has populated percpu_data[] and the BSP is marked online.
// ──────────────────────────────────────────────────────────

void smp_boot_aps(void);

#endif /* _KERNEL_SMP_H */
