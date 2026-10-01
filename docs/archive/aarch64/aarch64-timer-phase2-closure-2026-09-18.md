# AArch64 SUBSYS_INITCALL Phase 2 — Closure Report

**Date**: 2026-09-18
**Branch**: `feat/aarch64-timer-phase2` (based on `master @ b2b81fc`)
**Commits**: 9 (4 spec revisions + 1 plan + 4 functional)

- `2806519` Phase 2 spec (R1)
- `16fc5fc` Phase 2 spec R2 (fix 3 REJECTED + 8 NIT)
- `0ae0709` Phase 2 spec R4 (fix R3-1 CRITICAL missing dispatch + 3 NIT)
- `5e5b71c` Implementation plan (R5 APPROVED)
- `6a6026b` Group 1 plumbing (linker + subsys.c + Makefile)
- `bddf8eb` Group 2 hook (arch_register_subsys + subsys_init_phase)
- `069e632` fixup: subsys_stub.c also stubs register_subsys
- `4d7f7c9` Group 3a gate flip (clocksource.c)
- `e31ab1b` Group 3b trim Option B + upgrade subsys_stub.c to real queue

## Goal achieved

`SUBSYS_INITCALL()` now works on aarch64, identical to x86_64:
- Linker script has `.subsys_init` section (byte-mirror x86_64, AFTER `.data`)
- `arch_register_subsys()` iterator (`kernel/arch/aarch64/subsys.c`, byte-mirror x86_64 minus RSDP)
- Makefile whitelist includes `arch/aarch64/subsys.c`
- `kernel/time/clocksource.c` gate flipped to `#if defined(__x86_64__) || defined(__aarch64__)`
- `aarch64_main` calls the **register+dispatch pair** (mirrors `kernel/core/main.c:193-194` x86_64 pattern)
- Option B explicit `clocksource_init()` call removed; framework dispatch is the single source of invocation

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang SMP=1 aarch64-uefi` | exit 0 |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3) | **9/9 PASS** |
| `--expect-selftest --expect-gic --expect-clk` evidence gates | all green |
| 3 `[clocksource]` markers per case before `[cntp]` | verified |
| `nm | grep __subsys_init` shows `__subsys_init_start` + `__subsys_init_end` + `__subsys_initcall__clocksource_register` | 3 symbols |
| `test-aarch64-gic-spi` (Phase 1 GIC 2.3b regression) | PASS |
| `test-user-canary` (x86_64) | audit passed |
| `test-kernel-selftest` (x86_64) | 27/27 PASS |
| `test-pmm-boot-reservation` (x86_64) | 6/6 PASS |
| `test-kernel-canary-contract` (x86_64) | 5/5 PASS |
| `test_clocksource` hosttest (aarch64-clang profile) | 49/49 PASS |

## R3-1 critical failure mode — observed mid-state, fixed

The mid-state between Task 4 Step 2 (Option B explicit call removed) and Task 4 Step 7 (`subsys_stub.c` upgraded) reproduced the exact regression predicted by spec §F and the R3 review:

- `arch_register_subsys()` iterates an empty `__subsys_init` table (no drivers registered because `subsys_stub.c::register_subsys` was a no-op)
- `subsys_init_phase(SUBSYS_PHASE_4)` walks an empty dispatch table — no init invoked
- `clocksource_init()` never runs
- `clocksource_active` stays false
- All 3 `[clocksource]` markers disappear
- Harness `--expect-clk` fails; cases time out at 90s

Path (a) — upgrade `subsys_stub.c` to a real queue (`subsys_table[MAX_SUBSYS=16]`, `subsys_init_phase` walks the table) — restored GREEN. The R3-1 critical risk documented in the spec was load-bearing for the plan; without it, Task 4 would have shipped a broken state.

## Plan deviations (documented in commit messages)

1. **Task 2 subagent created `kernel/arch/aarch64/subsys_stub.c`** — needed because `kernel/subsys/subsys.c` (the canonical home of `subsys_init_phase()`) transitively references `serial_printk`, `num_cpus`, `strcmp`, `idle_resume` — none in the strict aarch64 whitelist. Stub initially no-op'd `subsys_init_phase` only.

2. **Task 2 fixup `069e632` extended the stub** — needed because Task 3's gate flip would link-fail on `register_subsys()` reference. Stub now also no-ops `register_subsys`, `register_subsys_percpu`, `subsys_init_all`, `subsys_init_percpu`, `subsys_status`.

3. **Task 4 upgraded the stub (path (a))** — to make the dispatch path actually invoke `clocksource_init()`. `register_subsys` writes to `subsys_table[16]`; `subsys_init_phase(int)` walks the table and calls `init()`. API parity with `kernel/subsys/subsys.c` without the scheduler/percpu/libc-string dependencies.

4. **Task 4 reordered the `[clocksource]` marker prints** — moved to AFTER `subsys_init_phase(SUBSYS_PHASE_4)` and BEFORE `arch_tick_start()` so `clocksource_active` is true when markers print. Spec §E had this implicit but plan §Task 4 didn't make the order explicit; the reorder was forced by the `--expect-clk` evidence gate.

## Build-cache operational note

`make` does not recompile `.o` files on `CFLAGS`-only changes (notably `KERNEL_SELFTEST=1`). Any QEMU regression run MUST be preceded by `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang KERNEL_SELFTEST=1 ... aarch64-uefi` to avoid stale-cache false negatives where GIC E2E probes are stripped. This is a build-system bug; tracked as P2 follow-up.

## Scope NOT done in Phase 2 (deferred to future specs)

1. **Replace `kernel/arch/aarch64/subsys_stub.c` with real `kernel/subsys/subsys.c`** — once the port provides `serial_printk`, `num_cpus`, `strcmp`, and scheduler `idle_resume`. The stub is API-parity with the real file; swap should be ~10-line drop-in.
2. **Add `.subsys_init` section + aarch64 equivalent of `kernel/arch/x86_64/subsys.c::arch_register_subsys_percpu()` for SMP** — Phase 2 follow-up item #3 (per-CPU timer / SMP timer).
3. **Flip `kernel/time/timer.c:136` SUBSYS_INITCALL gate** — currently `#ifdef __x86_64__`. Requires `cntp_tick_handler` → `tick_handler()` integration first (Phase 2 follow-up item #2 from Phase 1).
4. **Fix the build-cache CFLAGS bug** — `KERNEL_SELFTEST=1` doesn't trigger `.o` rebuild. Affects any future QEMU regression workflow.
5. **Add a Phase 2 RED→GREEN hosttest for the framework dispatch path** — currently only QEMU E2E catches R3-1-style regressions (host tests don't exercise the SUBSYS_INITCALL dispatch).
6. **Drop `arch_boot_rsdp` x86-conditional from `kernel/include/arch/subsys.h`** — long-term cleanup so aarch64 sources can directly `#include <arch/subsys.h>` without the inline-extern workaround (R3-3 NIT unresolved).

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #1** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 1. aarch64 `.subsys_init` section + `arch_register_subsys()` — drop explicit `clocksource_init()` call

This unblocks future framework-init work: any new `SUBSYS_INITCALL()` registration on aarch64 will work without explicit-call workarounds, and the unified `kernel_main` spec's "platform-side prerequisite" is satisfied.

## Outstanding P2 follow-ups remaining (4 of 5 from Phase 1 closure)

- Item #2: `cntp_tick_handler` → `tick_handler()` integration (needs `<list.h>` include-path fix)
- Item #3: Timer per-CPU affinity / SMP timer
- Item #4: `__udivti3` hoist to `kernel/compiler_rt/`
- Item #5: `-I libc/include` policy cleanup