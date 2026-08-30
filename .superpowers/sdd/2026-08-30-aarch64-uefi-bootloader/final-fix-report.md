# Final fix report

**Base:** `0f0084a`
**Scope:** findings from the final review of the AArch64 UEFI bootloader

## Fixes

- Program both BSP and secondary `ELR_EL2` registers with an `adr` to the
  immediately following local continuation before `eret`.
- Add a compiled-object regression check which decodes both `adr`
  instructions and proves that each EL2 return resumes at `eret + 4`.
- Force the AArch64 UEFI path through a fresh kernel sub-build, make the ESP
  depend on that build, add `bootinfo.h` dependencies to both EFI wrapper
  layers, and test that the kernel, EFI app, and ESP all refresh after the ABI
  header changes.  The test also extracts `/kernel.elf` from the ESP and
  compares it byte-for-byte with the rebuilt ELF.
- Replace the machine-specific posix-uefi default with
  `$(ROOT)/thirdpart/posix-uefi/uefi`; retain `UEFI_RUNTIME_SOURCE=...` as the
  explicit external-source override.
- Preserve the UEFI memory descriptor version in the former four-byte tail
  padding of `BOOT_MEMORY_MAP`.  `sizeof(BOOT_MEMORY_MAP) == 24` and
  `sizeof(boot_context) == 104` remain unchanged, so
  `BOOT_CONTEXT_VERSION == 2` remains valid.
- Add a direct unit boundary case proving a load ending at `0x401e0000` is
  accepted while a one-byte crossing is rejected.
- Emit PL011 diagnostics for both handoff-data and trampoline allocation
  failures, with a forced-allocation QEMU negative test.
- Remove three negative assertions for the nonexistent
  `UEFI-A64: handoff success` marker.  Positive boot evidence remains the real
  kernel handoff/banner/tick sequence.
- Reconcile the design, plan, architecture documentation, and implementation:
  a missing/invalid FDT falls back, while a valid FDT that cannot fit is a
  fatal handoff-overflow error.  The raw-map descriptor version is documented.

## Red evidence captured before fixes

- `test-aarch64-el2-return`: failed at the first compiled EL2 `eret` because
  no local `ELR_EL2` continuation was present.
- `test_bootinfo_abi`: failed to compile because `descriptor_version` did not
  exist.
- `test-aarch64-uefi-rebuild`: reported that `BOOTAA64.EFI` was not rebuilt
  after `bootinfo.h` changed.
- `test-handoff-allocation-failure`: failed because no PL011 allocation
  diagnostic was emitted.

## Verification

Verification below was repeated after removing generated kernel, libc, user,
test, sysroot, and AArch64 build outputs (the root `clean` target itself stops
at the unavailable x86 posix-uefi dependency described below).

- `make -C test run` — 17 suites, 0 failed.
- `make test-aarch64-el2-return` — both compiled EL2 continuations pass.
- `make UEFI_RUNTIME_SOURCE=/home/aagu/OS01/thirdpart/posix-uefi/uefi test-aarch64-uefi-rebuild`
  — rebuild freshness and ESP/kernel identity pass.
- `make UEFI_RUNTIME_SOURCE=/home/aagu/OS01/thirdpart/posix-uefi/uefi test-aarch64-uefi-negative`
  — bad ELF, handoff crossing, capacity overflow, and allocation failure all
  stop in the loader with the expected PL011 diagnostic.
- `make UEFI_RUNTIME_SOURCE=/home/aagu/OS01/thirdpart/posix-uefi/uefi check-aarch64-uefi-artifacts`
  — ARM64 PE32+ application and both ESP files verified.
- `make UEFI_RUNTIME_SOURCE=/home/aagu/OS01/thirdpart/posix-uefi/uefi test-aarch64-uefi-smoke`
  — handoff marker, phase-1 banner, and `[tick] 1` observed; no spinlock start.
- Bounded direct boot (`timeout 25 make run-aarch64 AARCH64_SMP=4 DISPLAY=none`)
  — expected timeout 124 after phase-1 banner, four-core total PASS, and ticks
  1 through 24.  Evidence: `build/aarch64/direct-smoke.log`.

## Environmental limitation

`make boot/uefi/BOOTX64.EFI` was executed and stopped before compilation with:

```text
posix-uefi submodule not initialized. Run 'git submodule update --init'
```

The x86 BOOTX64 artifact is therefore **not claimed as passing** in this
worktree.  The submodule was intentionally left untouched.  BusyBox and
posix-uefi have no changes.

## Self-review

- The EL2 checker validates generated instructions, not source-text presence.
- ABI size/offset assertions confirm the new field consumes existing padding.
- Rebuild coverage checks timestamps and packaged bytes, not Makefile text.
- Negative tests assert actual serial outcomes and absence of kernel entry.
- `git diff --check` is clean; no unresolved code finding remains in this fix
  wave.  Existing AArch64 unused-function and staged-posix-uefi linker warnings
  remain non-fatal and are outside this review scope.
