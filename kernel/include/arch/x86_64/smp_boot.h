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
// Call order in kernel_main (post-Task-6):
//     x86_64_boot_subsystems(bootctx);
//     x86_64_boot_percpu();           // BSP registration (Task 6: brought
//                                     // forward — see below)
//     random_init(bootctx);
//     fs_boot_prepare();
//     pty_init();
//     x86_64_boot_device_nodes();
//     fs_boot_mounts();
//     tty_boot_init();
//     fs_boot_probe_devfs();
//     x86_64_boot_aps();              // AP bringup (still after the devfs
//                                     // smoke probe)
//
// BSP per-CPU registration moved forward in Task 6 so the CSPRNG, FS,
// and TTY code paths see a valid this_cpu() (num_cpus != 0 doubles as
// the BSP GS-readiness gate used by kernel/driver/keyboard.c).  AP
// bringup stays after fs_boot_probe_devfs() so the percpu marker in
// the boot log still appears after /dev/null.

void x86_64_boot_percpu(void);
void x86_64_boot_aps(void);

#endif // _ARCH_X86_64_SMP_BOOT_H
