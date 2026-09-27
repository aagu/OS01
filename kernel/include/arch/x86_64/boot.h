#ifndef _ARCH_X86_64_BOOT_H
#define _ARCH_X86_64_BOOT_H

// ── Boot-time x86 device-node registration ────────────────────
// Pulled out of kernel/core/main.c as part of the kernel-main refactor
// (Task 2).  Registers the keyboard, mouse, and framebuffer chrdevs that
// route through devfs.  Lives under arch/x86_64 because the drivers
// themselves are x86-only (port-I/O keyboard, PS/2 mouse, x86 framebuffer).
//
// The mouse branch is gated on subsys_status("mouse") == 1 because the
// mouse probe only succeeds when the i8042 controller is functional —
// QEMU configurations without a PS/2 mouse leave the subsystem unset.
//
// Call order in kernel_main:  fs_boot_prepare(); pty_init();
//                             x86_64_boot_device_nodes(); fs_boot_mounts();
// TTY + /dev probe + devfs smoke test follow in main.c (Task 3 moves
// them into fs_boot_probe_devfs()).
void x86_64_boot_device_nodes(void);

#endif // _ARCH_X86_64_BOOT_H
