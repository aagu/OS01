# AArch64 phantom ENOENT: missing CAS condition-code clobber

Root cause identified on 2026-10-19 in worktree
`.claude/worktrees/aarch64-phantom-enoent`, branch
`fix/aarch64-phantom-enoent`. This supersedes the hypotheses in the
[paused-session handoff](aarch64-phantom-enoent-handoff-2026-10-19.md).

## Root cause

The AArch64 implementation of `arch_atomic_cas()` in
`kernel/include/arch/atomic.h` executes `cmp` but declared only a `"memory"`
clobber. `cmp` changes NZCV. Without a `"cc"` clobber, the compiler may
keep a condition live across the inline assembly, assuming NZCV survives.
Acquire/release memory ordering does not describe this register side effect.

With Clang 23.1.3 at `-O2`, the affected page-table functions inline both
descriptor construction and the CAS-based `spin_lock()`. The compiler
schedules the PROTNONE test before the first CAS, and the descriptor-selecting
`csel` after it. That select actually consumes the CAS comparison's flags.

The pre-fix selftest ELF contains this sequence in `aarch64_pt_map_4k_ext`:

```asm
ffff00004009cc18: tst   x21, #0x80000000000000  // software PROTNONE bit
...
ffff00004009cc24: ldaxr x11, [x23]             // lock: 1=unlocked, 0=locked
ffff00004009cc28: cmp   x11, x13               // x13=1; overwrites NZCV
ffff00004009cc2c: b.ne  ...cc40
...
ffff00004009cc40: mov   w12, #0                // first CAS failed
ffff00004009cc44: orr   x8, x10, x9
ffff00004009cc48: and   x9, x8, #0xfffffffffffffffe
ffff00004009cc4c: csel  x24, x8, x9, eq        // selects V=0 when lock was busy
ffff00004009cc50: cbnz  w12, ...cc84
...                                          // retry lock; x24 is not rebuilt
ffff00004009ccd4: orr   x8, x8, x24
ffff00004009ccd8: orr   x9, x8, x21
ffff00004009ccdc: stlr  x9, [x10]              // publishes malformed descriptor
```

For `software_bits=0`, the original `tst` sets Z=1 and the select should
keep V=1. If the first CAS sees a busy lock, its `cmp 0,1` sets Z=0.
The select then clears V. A later successful lock retry does not reconstruct
the descriptor. The same defect is present in `aarch64_pt_replace_4k`,
so failure can surface either at replace (bad map) or unmap (bad replace).

This is a kernel inline-assembly contract bug. The lock's mutual exclusion
and its LDAXR/STLXR memory ordering can be correct while the surrounding
compiler-generated descriptor calculation is wrong.

## Why the earlier evidence was misleading

- The map readback compares memory against the already-malformed `desc`.
  Both values can have V=0 and compare equal. A quiet diagnostic never
  proved that the published descriptor had V=1.
- Both CPUs can independently encounter a busy lock on their first CAS.
  Two malformed leaves do not require page-wide corruption or reuse.
- The observed `...706` and `...786` endings preserve PA and other fields.
  Their difference, `0x80`, is the AP permission bit (RW versus RO), not AF.
  This matches malformed descriptors originating from map or replace.
- Changing STR to STLR cannot repair a descriptor constructed incorrectly
  before the store. Neither stronger barriers nor disabling MTTCG is needed
  to explain the observed failure.

## Minimal fix and generated code

Add `"cc"` to the AArch64 CAS clobber list. No descriptor algorithm,
barrier, lock algorithm, or test iteration count changes are required.
The pre-existing STLR experiment and diagnostics remain in the worktree.

After this one change, the same function computes its descriptor before
the CAS, keeping the PROTNONE test and its consumer together:

```asm
ffff00004009cc08: tst   x21, #0x80000000000000
...
ffff00004009cc24: and   x10, x9, #0xfffffffffffffffe
ffff00004009cc2c: csel  x24, x9, x10, eq
ffff00004009cc30: ldaxr x9, [x23]
ffff00004009cc34: cmp   x9, x11
```

## Verification

Environment: Clang 23.1.3, QEMU 11.1.2, `cortex-a53`, GICv2, 512 MiB,
diagnostic DTB, existing 500-iteration section 5b.

1. Fresh pre-fix run, 4 CPUs: section 5b fails at BSP iteration 44,
   operation 1 (replace), with BSP/AP both returning -3. The leaf values
   are `0x0060000041609706` and `0x0060000041602706`; no map mismatch fires.
2. Add only the CAS `"cc"` clobber and its explanatory comment. Build with
   `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 -j4 aarch64-uefi`.
   The dependency files trigger recompilation of affected header users,
   including page_table.c. No structs changed. Build exits 0; existing
   asm operand-width warnings in boot/main.c and smp/smp.c remain.
3. Run the full SMP acceptance matrix below: 9/9 cases PASS. Section 5b
   passes in all six 2/4-CPU runs; 1-CPU m3mc correctly SKIPs. The harness
   also checks PMM/PT smoke, slab, M3, GIC, and clock evidence.

```sh
python3 qemutests/aarch64_uefi_smp.py \
  --cpus 1 2 4 --repeat 3 --timeout 45 \
  --expect-selftest --expect-gic --expect-clk \
  --expect-slab-selftest --expect-m3-selftest --expect-m3mc-selftest \
  --diagnostic-dtb auto --qemu qemu-system-aarch64 \
  --firmware build/aarch64-clang/image/selftest/QEMU_EFI.fd \
  --image build/aarch64-clang/image/selftest/aarch64-uefi.img \
  --log-dir /tmp/phantom-cc-green \
  --build-dir build/aarch64-clang --profile aarch64-clang
```

Raw evidence is under `build/aarch64-clang/logs/tests/aarch64-uefi-smp/`:

- RED: `20261019T134026Z-0a170a769d5f4f3d95be7a12acbd19ac`.
- GREEN matrix: runs beginning `20261019T134147Z` through `20261019T134225Z`.
- Last 4-CPU GREEN: `20261019T134225Z-c77d1c7e050d4d31b747f53a2ea1e7ab`.

Runner output and before/after selftest disassembly are also preserved in
`/tmp/phantom-cc-{red,green}.out` and
`/tmp/phantom-selftest-{before,after}-cc.dis`. Generated build and temporary
evidence can disappear after cleanup; the decisive instruction sequences
are recorded above.

The fix has not been committed or merged. M4's concurrent-updater gate and
roadmap state are not changed by this investigation; integration and policy
updates remain separate work.
