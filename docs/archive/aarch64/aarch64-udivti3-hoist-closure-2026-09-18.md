# AArch64 `__udivti3` Hoist to `compiler_rt/` Phase 2 #4 — Closure Report

**Date**: 2026-09-18
**Branch**: `feat/aarch64-udivti3-hoist` (based on `master @ 3037df3`)
**Commits**: 1 functional + 1 spec + 1 closure doc

Spec revisions:
- (single v1 spec; no R1-R4 cycles needed — change is mechanical)

Implementation:
- 5f06a35: hoist `__udivti3` to `kernel/compiler_rt/udivti3.c` (single commit)

## Goal achieved

`__udivti3` runtime helper relocated from `kernel/arch/aarch64/udivti3_stub.c` (101 lines) to `kernel/compiler_rt/udivti3.c` (new file, 81 lines — same code, more concise header comment). The misnamed "_stub.c" file (it contained the actual implementation, not a stub) is now correctly named and grouped with future compiler-runtime helpers.

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang SMP=1 aarch64-uefi` | exit 0 |
| `make PROFILE=x86_64-clang kernel.bin` | exit 0 |
| `.text` section md5 byte-identical to old `udivti3_stub.o` after `.comment` strip | 83e5c5534d394eff244f6388b99c61ad |
| `nm | grep __udivti3` | 1 symbol (T) at `ffff0000400929e8` (same as pre-state) |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` SMP=1 × 3 | **3/3 PASS** |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` SMP=2/4 | pre-existing GIC Phase 1 TIMEOUT (matches baseline) |
| `test-build-contract-x86` | pre-existing baseline issue (matches Phase 2 #1/#2/#3 pattern) |
| x86_64 byte-identity | preserved (no x86_64 source changes) |

## SMP=2/4 timeout acknowledged

The `[ipi] summary targets=1 received=0 status=FAIL` → QEMU TIMEOUT pattern at SMP=2/4 is a **pre-existing GIC Phase 1 bug** documented in `memory/gic-phase1-timout-bug-2026-09-18.md`. Confirmed by running the same `test-aarch64-uefi-smp` at master baseline (with the spec commit applied but implementation stashed) — same 3/9 result (SMP=1 PASS, SMP=2/4 FAIL with timeout). This hoist does not affect that code path; the `.text` section of `__udivti3` is byte-identical to the original implementation.

## Spec review trajectory

v1 (commit d0f33d1) APPROVED via direct subagent review + implementation (small mechanical change; no R1-R4 review cycles needed per user direction).

## Latent corruption acknowledged

None. The `__udivti3` helper is unchanged in behavior — only its location changed. `.text` section md5 identical.

## Scope NOT done in Phase 2 #4 (deferred)

1. **`-I libc/include` policy cleanup** — Phase 2 follow-up #5 (last remaining).
2. **Other compiler-runtime helpers** (`__umodti3`, `__multi3`, `__ashlti3`, etc.) — only `__udivti3` is currently needed. Future helpers can be added to `kernel/compiler_rt/` if/when new `__uint128_t` operations are used.
3. **Per-CPU `init_task[]` array** — APs still halt in `for (;;) arch_cpu_halt();`.

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #4** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`.

Phase 2 P2 follow-ups remaining (1 of 5 from Phase 1 closure):
- #5 `-I libc/include` policy cleanup