#ifndef _KERNEL_ARCH_X86_64_INTR_FAULT_PROBE_H
#define _KERNEL_ARCH_X86_64_INTR_FAULT_PROBE_H

// ── Isolated destructive dispatch probe (Task 5, spec §9.3) ─────
//
// The x86 phase-2 work needs *isolated* evidence for the two fatal
// entries that cannot be exercised through the ordinary boot or the
// kernel selftest image:
//
//   * #BP — a kernel-mode `int3` that reaches the original fatal
//           handler (kernel/arch/x86_64/intr/trap.c::x86_handle_int3).
//   * NMI — a REAL hardware NMI delivered over QMP `inject-nmi`
//           (kernel/arch/x86_64/intr/trap.c::x86_handle_nmi).
//
// The probe lives in its own kernel/image variant
// (`intr-x86-fault-bp` / `intr-x86-fault-nmi`); the normal build
// never compiles it and never links it.
//
// The caller must invoke this AFTER the per-CPU bring-up has published
// `num_cpus` (x86_64_boot_percpu()) and BEFORE the AP bring-up
// (x86_64_boot_aps()), the FS/TTY bring-up and the user-space init —
// the probe halts the CPU, so nothing may follow it.
//
// The probe checks that precondition itself (`num_cpus == 1`, i.e. the
// BSP is the only CPU the kernel knows about) and prints a
// `stage=post-percpu` marker that the acceptance runner REQUIRES, so
// the placement is verified at RUN time instead of asserted in prose.
//
// The NMI case additionally needs firmware-described NMI routing. LAPIC
// initialization programs MADT type-4 LINT inputs for the matching CPU;
// unspecified inputs remain masked (see arch/x86_64/intr/apic/nmi.c).

void x86_intr_fault_probe_run(void);

#endif
