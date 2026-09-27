#ifndef _ARCH_X86_64_BOOT_H
#define _ARCH_X86_64_BOOT_H

// Forward declaration — the full definition lives in <core/bootinfo.h>.
// The boot-stage helpers only need a pointer to the boot context; pulling
// the full definition (with all its sub-structs) here would create an
// unwanted include cycle with the arch headers.
struct boot_context;

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

// ── Boot-stage helpers (Task 4) ────────────────────────────────
// The original kernel_main() carried three ordered boot blocks inline:
//   • early      — framebuffer Pos, arch_task_init_early, IDT, serial, NXE
//   • memory     — PMMngr boundaries, early FB + logo, pmm/vmm, FB remap
//   • subsystems — arch_boot_rsdp, arch_register_subsys, subsys_init_all
// Each is now a thin helper that takes the boot context by pointer for
// uniformity (some stages may not reference the pointer; that's fine —
// the parameter is reserved for future symmetry across the family of
// boot_xxx() helpers).  random_init() stays in kernel_main because it
// is a single-shot CSPRNG seed that logically lives next to the canary
// handoff check, not inside an arch-scoped stage.
void x86_64_boot_early(const struct boot_context *bootctx);
void x86_64_boot_memory(const struct boot_context *bootctx);
void x86_64_boot_subsystems(const struct boot_context *bootctx);

#endif // _ARCH_X86_64_BOOT_H