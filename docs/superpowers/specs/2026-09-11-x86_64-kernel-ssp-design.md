# x86_64 Kernel Stack Protector Design

## Goal

Enable reliable x86_64 kernel stack-protector instrumentation without the
historical false `__stack_chk_fail` trips caused by PIE/GOT access to the
global canary.

## Context and root cause

The kernel currently defines and seeds `__stack_chk_guard`, and provides a
non-instrumented `__stack_chk_fail` handler, but it compiles all x86_64 C
sources with `-fpie` and without `-fstack-protector-strong`.

With the existing target flags, `-fpie` produces
`R_X86_64_REX_GOTPCRELX __stack_chk_guard` relocations.  Canary checks then
load the guard address through the GOT.  This was disabled after large
functions produced false stack-smashing failures.  A controlled compile with
non-PIC code produces a direct canary reference instead.

The kernel already has a fixed higher-half link address and static image
contract.  Kernel ASLR is not a current feature; the roadmap's ASLR work is
for user mappings and ET_DYN executables.

## Design

For `ARCH=x86_64`, replace `-fpie` with `-fno-pic`, select
`-mcmodel=large`, and append `-fstack-protector-strong` to `ARCH_CFLAGS`.
The actual link address (`0xffff800000100000`) is outside
`-mcmodel=kernel`'s signed 2 GiB range; the large model emits direct 64-bit
addresses instead of invalid 32-bit absolute relocations.

Retain the existing global guard and the `no_stack_protector` attributes on
`kernel_main` and `__stack_chk_fail`.  They are required respectively to seed
the guard before any protected epilogue runs and to prevent recursion in the
failure path.  No userland flags, aarch64 flags, linker load address, or
runtime ABI change is in scope.

## Build-time audit

Add an x86_64-only validation target that inspects a known large protected
object (`sched/task.o`) and the final `kernel.elf`.

It must fail when:

1. a `__stack_chk_guard` relocation in the selected object is GOT-based;
2. no direct `R_X86_64_PC32` or `R_X86_64_64` `__stack_chk_guard` relocation
   is present; or
3. the selected object has no direct `__stack_chk_fail` relocation, or the
   final image lacks the global failure-handler symbol.

The audit gives a deterministic regression signal for the exact compile/link
property that caused SSP to be disabled.  It supplements, rather than
replaces, runtime tests.

## Verification

Because compiler flags affect every object, verification begins with a clean
x86_64 build.  Required evidence:

1. the new audit fails under the old PIE configuration and passes under the
   new static configuration;
2. `make clean && make PROFILE=x86_64-clang kernel.bin` succeeds;
3. `make KERNEL_SELFTEST=1` succeeds without a spurious canary trip;
4. `make OS01_SYSTEST=1 test-syscall` passes; and
5. a deliberately enabled, test-only stack overwrite invokes
   `__stack_chk_fail`, prints the established diagnostic, and halts QEMU.

The destructive runtime self-test is compile-time gated and is not enabled in
normal builds.

## Non-goals

- Per-CPU or TLS canaries.
- Guard pages for task or IST stacks.
- User-space stack protectors and `AT_RANDOM`.
- Kernel address randomization.
- Aarch64 stack-protector enablement.
