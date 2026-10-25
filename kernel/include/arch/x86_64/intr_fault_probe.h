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
// The caller must invoke this AFTER the IDT, the serial port and the
// per-CPU area are usable, and BEFORE the AP bring-up and the
// user-space init — the probe halts the CPU, so nothing may follow it.

void x86_intr_fault_probe_run(void);

#endif
