# Architecture source grouping

## Intent

Group the flat C and assembly files under both `kernel/arch` architecture directories by responsibility, while preserving all symbols, boot order, generated binary symbol names, and architecture interfaces. This is a path-only refactor.

## Layout

Keep `make.config`, `linker.ld`, and `head.S` at each architecture root. Keep x86_64 `trampoline.S` and `trampoline.ld` at its root because the special build rules and embedded binary symbols depend on their current build path.

Move x86_64 files into `intr/`, `memory/`, `smp/`, `cpu/`, and `platform/`: interrupt entry, trap, IRQ hooks and controllers in `intr`; PMM in `memory`; SMP and per-CPU subsystem setup in `smp`; atomic, entropy, auxv, task switch and thread entry in `cpu`; time, RTC, subsystem registration, and early print in `platform`.

Move aarch64 files into `boot/`, `intr/`, `memory/`, `smp/`, `cpu/`, `platform/`, and `runtime/`: boot fixup, boot per-CPU setup, and main in `boot`; exception entry, trap, GIC and IRQ tests in `intr`; PMM, page tables and RAM normalization in `memory`; SMP, PSCI and spinlock test in `smp`; atomic, entropy, auxv and idle resume stub in `cpu`; DTB, PL011, time and subsystem registration in `platform`; freestanding library, logging and print stubs in `runtime`.

## Build and references

Use explicit one-level source globs in `kernel/Makefile`; avoid recursive discovery and duplicate object inputs. Update the aarch64 explicit source whitelist to relocated paths. Preserve the x86_64 trampoline build output path and binary-symbol names. Update fixed source paths in host tests, QEMU tests, and current architecture documentation. Public headers remain at `kernel/include/arch/`; this change does not alter their interfaces.

## Verification

Clean and build both profile kernels, run directly affected host and QEMU tests, verify source-to-object coverage and no duplicate object inputs, and check the x86_64 trampoline embedded symbols.
