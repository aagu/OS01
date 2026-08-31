# OS01 Toolchain Refactor v4 Implementation Plan

Goal: make x86_64 Clang-version independent without ELF settings leaking into UEFI or aarch64.

Spec: docs/superpowers/specs/2026-08-31-toolchain-refactor-design.md, v4.

Constraints: do not change libc/include/stdint.h, aarch64, CMake/Meson, CI, Make structure, or thirdpart/posix-uefi/uefi/Makefile. Use discovered LLVM variables in checks. The series has exactly 10 commits: A, B, C, D, E, F, G, H, K, M; and exactly 17 steps: S1-S17.

## Commit A: shared contract and x86 dispatch

Files: new toolchain.mk; Makefile:1-22; kernel/Makefile:9-14; kernel/arch/x86_64/make.config:1-5; libc/Makefile:1-18; user/Makefile:1-24; tools/Makefile:1-12; test/Makefile:1-17.

- [ ] S1. Add root-anchored include paths and x86 dispatch from spec sections 2 and 7. Verify standalone kernel/libc/user resolves repository sysroot.
- [ ] S2. Implement origin CC policy and conditional tool discovery from spec section 3. Verify CC=gcc, CC='ccache gcc', CC=cc, and CC=gcc-12 reject while explicit Clang succeeds.
- [ ] S3. Remove immediate tool assignments at kernel config:4-5, libc:10-12, user:6-8; do not use generic CC ?= or LD ?=. Keep libc arch config include-free.
- [ ] S4. Keep tools/test host-only with HOST_CC ?= cc and HOST_CC ?= clang, retaining local flags.
- [ ] Commit A: git add toolchain.mk Makefile kernel/Makefile kernel/arch/x86_64/make.config libc/Makefile user/Makefile tools/Makefile test/Makefile; git commit -m "build(toolchain): centralize x86_64 Clang discovery"

## Commit B: raw links

Files: kernel/arch/x86_64/make.config:38-40; kernel/Makefile:77,158-176,185-206; user/Makefile:23-24,69-93.

- [ ] S5. Add exact driver/raw groups; convert Wl syntax and replace direct kernel library path with TARGET_LIBDIR.
- [ ] S6. Keep stage 1 driver-linked; raw-link final kernel, font, trampoline ELF/embed, and all user ELFs. Build kernel and user.
- [ ] Commit B: git add kernel/arch/x86_64/make.config kernel/Makefile user/Makefile; git commit -m "build(kernel): separate driver and raw linker flags"

## Commit C: owned varargs

Files: new libc/include/stdarg.h; libc/Makefile:88-95.

- [ ] S7. Add builtin va_list and va operations and prove sysroot installs stdarg.h.
- [ ] Commit C: git add libc/include/stdarg.h libc/Makefile; git commit -m "libc: provide freestanding stdarg header"

## Commit D: kallsyms host boundary

Files: kernel/Makefile:188-201.

- [ ] S8. Use HOST_CC/HOST_NM while retaining environment clearing. If c221c89 is equivalent, record no-op verification instead of a fake diff.
- [ ] Commit D: git add kernel/Makefile; git commit -m "build(kernel): use configured host kallsyms tools"

## Commit E: cc.h rationale

Files: kernel/include/net/arch/cc.h:5-23.

- [ ] S9. Preserve local types and avoid stdint.h; add the future-review rationale from spec section 5.
- [ ] Commit E: git add kernel/include/net/arch/cc.h; git commit -m "net: document lwip integer typedef contract"

## Commit F: BusyBox and mbedTLS audit

Files: Makefile:87-167; config BusyBox template rename; .gitignore.

- [ ] S10. Replace every hard-coded clang, llvm-ar, and direct sysroot usr path identified in spec section 5.
- [ ] S11. Render BusyBox config to temporary, cmp, and atomically move only when changed; prove stable timestamp for identical output.
- [ ] Commit F: git add Makefile config/busybox.config.in .gitignore; git commit -m "build(busybox): derive target toolchain and config include"

## Commit G: UEFI isolation

Files: boot/uefi/Makefile:11-31,57-58.

- [ ] S12. For x86_64 pass MAKEOVERRIDES= USE_GCC= CC UEFI_CLANG LD UEFI_LD CFLAGS= LDFLAGS= to inner make, and validate UEFI_LD_TOOL. Preserve inner COFF flags.
- [ ] Commit G: git add boot/uefi/Makefile; git commit -m "build(uefi): isolate recursive Clang contract"

## Commit H: artifact checks

Files: root Makefile and toolchain.mk.

- [ ] S13. Add selected-tool kernel/UEFI validators. Require GLOBAL binding for _start, kernel_main, and _text.
- [ ] Commit H: git add Makefile toolchain.mk; git commit -m "build: add selected-tool artifact validation"

## Commit K: gated libgcc removal

Files: kernel/arch/x86_64/make.config:40.

- [ ] S14. Remove -lgcc, clean-build kernel, and revert if link fails.
- [ ] S15. Run spec section 8 discovered-tool proof: no undefined symbols, no INTERP/DYNAMIC, X86-64, three named GLOBAL symbols. Revert if any check fails.
- [ ] Commit K: git add kernel/arch/x86_64/make.config; git commit -m "build(kernel): remove unnecessary libgcc dependency"

## Commit M: docs and matrix

Files: new docs/build/toolchain.md, AGENTS.md, build documentation.

- [ ] S16. Where installed, clean-build CLANG=clang-18 then CLANG=clang-22 and run S15 proof. Confirm aarch64 dry run has no x86 triple.
- [ ] S17. On each available host/version run make test, UEFI/PE validation, kernel, user, disk image, and headless QEMU boot.
- [ ] Commit M: git add docs/build/toolchain.md AGENTS.md docs; git commit -m "docs(toolchain): document x86_64 overrides"

## Matrix

| Step | Commit | Pass criterion |
|---|---|---|
| S1 | A | root-anchored standalone x86 includes |
| S2 | A | built-in CC replaced; named GCC overrides fail |
| S3 | A | basename/absolute LLVM override validates |
| S4 | A | host builds have no target flags |
| S5 | B | no Wl syntax reaches raw linker |
| S6 | B | kernel/user raw links build |
| S7 | C | sysroot owns stdarg.h |
| S8 | D | kallsyms uses host variables |
| S9 | E | local cc.h ABI decision retained |
| S10 | F | no hard-coded tool/path in mbedTLS/BusyBox |
| S11 | F | identical config does not replace output |
| S12 | G | isolated UEFI Clang/COFF build |
| S13 | H | selected LLVM artifact checks |
| S14 | K | kernel links without libgcc |
| S15 | K | static identity and GLOBAL symbols |
| S16 | M | same-host version matrix and aarch64 proof |
| S17 | M | full test/disk/QEMU matrix |
