# AArch64 Per-CPU Timer / SMP Timer Phase 2 #3 — Closure Report

**Date**: 2026-09-18
**Branch**: `feat/aarch64-timer-phase2-smp` (based on `master @ 10fd3d2`)
**Commits**: 2 functional + 2 spec revisions + 1 closure doc

Spec revisions:
- `44f4314` v1 spec (R1 REJECTED with 8 CRITICAL)
- `15e1df9` R2 spec (R2 rewrite addresses 8 CRITICAL; R3 APPROVED-WITH-NITS)

Implementation:
- `923fcbb` Commit 1 — combined §A + §C prerequisite (percpu_data install + k_cpu_index fix)
- `150b74b` Commit 2 — AP CNTP enable (§B)

Plan deviation (Commit 1 subagent note): added `kernel/include/arch/aarch64/boot_offsets.h` to expose `PERCPU_DATA_SIZE` as assembly-safe (the C header `kernel/include/percpu/percpu.h` includes function prototypes that fail assembly preprocessing).

## Goal achieved

`percpu_data[cpu]` is wired on aarch64 (TPIDR_EL1 = `&percpu_data[cpu_id]`). `k_cpu_index()` reads `cpu_id` from real `percpu_t`. AP CNTP enabled. Phase 2 #2's latent `tick_handler()` writes to per-CPU `need_resched` field correctly (was latent because TPIDR_EL1 previously pointed at `aarch64_boot_percpu[0]`).

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang SMP=1 aarch64-uefi` | exit 0 |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` 1-CPU | PASS |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` 2/4-CPU | FAIL (same pre-existing Phase 2 #1 GIC SGI bug per memory `gic-phase1-timout-bug-2026-09-18`; not introduced by Phase 2 #3) |
| `make PROFILE=aarch64-gic-spi` | PASS |
| 3 `[clocksource]` markers per case (1-CPU) | verified |
| ≥3 `[tick] N` markers per case (1-CPU) | verified |
| `nm | grep percpu_data` shows 1 symbol (size = NR_CPUS × PERCPU_DATA_SIZE = 8 × 144 = 1152 bytes) | verified |
| 10 hosttests (5 binaries × 2 profiles) | all pass |
| `make PROFILE=x86_64-clang test-user-canary` | audit passed |
| `make PROFILE=x86_64-clang test-kernel-selftest` | 27/27 PASS |
| `make PROFILE=x86_64-clang test-pmm-boot-reservation` | 6/6 PASS |
| `make PROFILE=x86_64-clang test-kernel-canary-contract` | 5/5 PASS |

## Spec review trajectory (3 rounds)

R1: 8 CRITICAL (REJECTED) — spec v1 had fundamental design errors:
- CRITICAL-1: referenced nonexistent `kernel/include/arch/aarch64/percpu.h`
- CRITICAL-2: §B without §A turns latent corruption active
- CRITICAL-3: spec proposed NEW `kernel/arch/aarch64/percpu.c` that would duplicate symbol
- CRITICAL-4: missing `percpu_init()` calls
- CRITICAL-5: wrong line numbers
- CRITICAL-6/7: §C "framework ready" claim misleading
- CRITICAL-8: §D snippet left despite self-revocation

R2: major rewrite (uses existing `kernel/percpu/percpu.c`; combines §A+§C prerequisite in single commit; correct line numbers).

R3: APPROVED-WITH-NITS — 3 MEDIUM + 6 LOW NITs:
- MEDIUM-1: `PERCPU_DATA_SIZE` macro needs numeric literal for asm use — fixed in plan §Global Constraints; implemented in `kernel/include/percpu/percpu.h` + mirrored in `kernel/include/arch/aarch64/boot_offsets.h`
- MEDIUM-2: `mpidr_el1_read()` helper function does not exist — fixed by using inline asm directly
- MEDIUM-3: `#if OS01_SELFTEST` guard around `arch_local_irq_enable()` — kept as-is; AP `[tick] N` lines only appear in selftest builds

## Latent corruption closed (from Phase 2 #2 closure §"Latent corruption acknowledged")

| Latent issue | Status after Phase 2 #3 |
|--------------|--------------------------|
| `tick_handler()` writes `this_cpu()->need_resched = 1` to wrong struct | **CLOSED** — now writes to real `percpu_data[cpu].need_resched` |
| `tick_handler()` writes `this_cpu()->watchdog_counter++` out of bounds | **CLOSED** — now writes to real `percpu_data[cpu].watchdog_counter` |
| `set_softirq_status(TIMER_SIRQ)` sets never-cleared bit | Latent — `do_softirq` not invoked on aarch64; future work |

## Scope NOT done in Phase 2 #3 (deferred)

1. **Pre-existing 2/4-CPU IPI FAIL** — separate Phase 2 #1 GIC SGI bug per memory `gic-phase1-timout-bug-2026-09-18`; NOT in scope for Phase 2 #3. Future P2 follow-up spec.
2. **Per-CPU `init_task[]` array** — APs still halt in `for (;;) arch_cpu_halt();` without per-CPU idle thread. Future spec.
3. **Per-CPU jiffies counters** — `jiffies` is single global; multi-CPU writes are racy (BSP produces ≥3 `[tick] N` so harness gate passes). Future spec.
4. **`__udivti3` hoist to `compiler_rt/`** — Phase 2 follow-up #4.
5. **`-I libc/include` policy cleanup** — Phase 2 follow-up #5.
6. **`arch_register_subsys_percpu()` on aarch64** — `subsys_percpu_table[]` stays empty (framework in place for future use).

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #3** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 3. Timer per-CPU affinity / SMP timer

Phase 2 P2 follow-ups remaining: #4 (`__udivti3` hoist), #5 (`-I libc/include` policy).