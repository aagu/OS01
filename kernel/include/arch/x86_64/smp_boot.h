#ifndef _ARCH_X86_64_SMP_BOOT_H
#define _ARCH_X86_64_SMP_BOOT_H

// ── x86_64 SMP bring-up (Task 5 of kernel-main refactor) ─────────
//
// Pulled out of kernel/core/main.c to keep kernel_main() a thin call
// sequence.  The two helpers together own the per-CPU + SMP stage:
//
//   x86_64_boot_percpu() — MADT traversal, percpu_init(), BSP
//                          TSS/GS/online, num_cpus publication.
//                          Must run BEFORE the AP boot sequence so that
//                          smp_boot_aps() can iterate over a fully
//                          populated percpu_data[] and so that
//                          this_cpu() is valid for the tick source.
//
//   x86_64_boot_aps()    — tick_start() → smp_boot_aps() →
//                          arch_register_subsys_percpu() →
//                          subsys_init_percpu().  Owns the BSP→AP
//                          bring-up and the per-CPU subsystem dispatch.
//
// Call order in kernel_main (preserved verbatim from pre-Task-5):
//     fs_boot_probe_devfs();
//     x86_64_boot_percpu();
//     x86_64_boot_aps();
// The position is significant: percpu registration must follow the
// devfs smoke probe so that the percpu marker still appears AFTER
// /dev/null in the boot log.  Future work (Task 6) may evaluate
// moving x86_64_boot_percpu() earlier; that move is NOT this task.

void x86_64_boot_percpu(void);
void x86_64_boot_aps(void);

#endif // _ARCH_X86_64_SMP_BOOT_H
