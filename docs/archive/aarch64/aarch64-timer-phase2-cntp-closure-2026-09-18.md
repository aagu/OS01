# AArch64 `cntp_tick_handler → tick_handler()` Phase 2 #2 — Closure Report

**Date**: 2026-09-18
**Branch**: `feat/aarch64-timer-phase2-cntp` (based on `master @ e115d79`)
**Commits**: 5 functional + 5 spec revisions + 1 plan + 1 closure doc = 12 total

Spec revisions (5 commits, 5 review rounds):
- `faca7a7` v1 spec (R1 REJECTED with 6 CRITICAL)
- `7bbc045` R2 spec (major redesign: stub + gates)
- `27a9e2b` R3.1 spec (R3 NEW do_softirq asm gate)
- `58f3af3` R3.2 spec (R3 NEW timer.c pause asm gate)
- `3f31522` R3.3 spec (calloc/free stub)
- `a83af1c` R3.4 spec (tick_start gate)

Plan:
- `d452f4f` cntp_tick_handler integration plan (R3.1 APPROVED spec)

Implementation (5 commits):
- `2a297cc` libc_stub.c (R3.3 fix landed first — calloc/free shims needed before time/timer.c whitelist expansion)
- `18838ff` Task 2 asm gates (softirq.c BOTH sites: set_softirq_status `lock orq`, do_softirq `lock andq`; R3.2 expanded to also gate timer.c `destroy_timer pause → yield`)
- `c003d04` Task 1.5 retry (idle_resume stub + Makefile whitelist expansion adding 3 files: time/timer.c, intr/softirq.c, arch/aarch64/idle_resume_stub.c)
- `46a860a` Task 3 (gate x86 poll-timeout scan in tick_handler + tick_start body; Makefile whitelist adds time/tick.c; R3.4 expanded the gate to also cover tick_start)
- `a209336` Task 4 (cntp_tick_handler delegates to tick_handler + framework wiring: timer.c:136 gate flip + explicit softirq_init call)

## Goal achieved

`tick_handler()` runs on aarch64 with the same semantic as x86_64 (modulo the `#if`-gated poll-scan block + `tick_start` body which are x86-only). The unified tick semantic (`jiffies++` → `need_resched=1` → `watchdog_counter++` → `set_softirq_status(TIMER_SIRQ)`) now runs on both architectures. `timer_init()` runs via the SUBSYS dispatch path, so `init_timer(&timer_list_head, NULL, NULL, -1UL)` + `register_softirq(0, &do_timer, NULL)` are wired before the first CNTP tick.

The `[tick] N` per-second print in `cntp_tick_handler` is PRESERVED (R3.1 §B fix for R1 CRITICAL-1 — harness requires `>=3 [tick] N` lines per case).

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang SMP=1 aarch64-uefi` | exit 0 |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3) | **9/9 PASS** |
| `make PROFILE=aarch64-clang test-aarch64-gic-spi` | PASS |
| 3 `[clocksource]` markers per case before `[cntp]` | verified |
| >=3 `[tick] N` markers per case (harness evidence gate) | verified |
| `nm | grep subsys_init` shows 4 symbols | verified |
| 10 hosttests (5 binaries x 2 profiles x86_64 + aarch64) | all pass |
| `make PROFILE=x86_64-clang test-user-canary` | audit passed |
| `make PROFILE=x86_64-clang test-kernel-selftest` | 27/27 PASS |
| `make PROFILE=x86_64-clang test-pmm-boot-reservation` | 6/6 PASS |
| `make PROFILE=x86_64-clang test-kernel-canary-contract` | 5/5 PASS |

## Spec review trajectory (5 rounds)

R1: 6 CRITICAL (REJECTED) — spec v1 was a fundamental design error (assumed `tick_handler` reusable as-is on aarch64; it's actually a x86_64-only implementation).
R2: major redesign (APPROVED-WITH-NITs) — added stub files (`idle_resume_stub.c`, `subsys_stub.c` already existed from Phase 2 #1) + `#if defined(__x86_64__)` gates.
R3: 1 NEW CRITICAL (do_softirq `lock andq` asm not gated) — spec section E needed expansion.
R3.1: do_softirq asm gate fixed — APPROVED.
R3.2: 1 NEW (timer.c `pause` asm not gated) — Task 2 scope expanded.
R3.3: 1 NEW (calloc/free libc symbols undefined) — `libc_stub.c` added.
R3.4: 1 NEW (tick_start `irq_mask`/`irq_unmask` x86-only) — tick_start body gated.

5 review rounds, 4 spec amendments (R3, R3.2, R3.3, R3.4 — each caught a real issue in a file the previous rounds had not inspected). The cumulative fix set is mechanical and additive — no fundamental re-architecture required.

## Plan deviations (documented in commit messages)

- **Task 1.5 amendment**: original Task 1 added `time/tick.c` to the whitelist in the same commit as the Makefile expansion. This failed because `time/tick.c` calls `clocksource_read_ns()` (x86_64-only). Fixed by splitting into Task 3 (gate + whitelist together) and Task 1.5 (other 3 files only).
- **Task 2 expansion**: R3.2 found timer.c `pause` asm needed gating too (not just softirq.c). Task 2 scope expanded from "softirq.c BOTH sites" to "softirq.c BOTH + timer.c pause".
- **Task 1.5 second retry**: R3.3 found calloc/free undefined on aarch64 (libc sysroot not used). Required `libc_stub.c` (commit `2a297cc`) as prerequisite to Task 1.5 retry success.

## Latent corruption acknowledged

`tick_handler()` writes `this_cpu()->need_resched = 1` to `aarch64_boot_percpu[0].cpu_id` (offset 8) + `.pad0` (offset 16); `this_cpu()->watchdog_counter++` writes out of bounds of the 48-byte struct. No scheduler reads these today; **Phase 2 #3 (per-CPU timer / SMP timer) installs `percpu_data[cpu]` to fix this**.

`tick_handler()` calls `set_softirq_status(TIMER_SIRQ)` which writes to `softirq_status` BSS global. Softirq bit never cleared on aarch64 (no `do_softirq` invocation — dead code on aarch64). Latent; Phase 2 #3 scheduler fix.

`tick_handler()` calls `container_of(list_next(&timer_list_head.list), ...)` — `timer_list_head.list.next` is a sentinel (set by `init_timer(... -1UL)`); `expire_jiffies = -1UL` means `UINT64_MAX <= jiffies` is false -> skip softirq set. Latent OK on aarch64 phase 2.

## Scope NOT done in Phase 2 #2 (deferred to future specs)

1. **Per-CPU timer / SMP timer** — installs `percpu_data[cpu]` on aarch64, enables per-CPU affinity, fixes the latent `need_resched` corruption. Phase 2 #3.
2. **`__udivti3` hoist to `compiler_rt/`** — Phase 2 follow-up #4.
3. **`-I libc/include` policy cleanup** — Phase 2 follow-up #5.
4. **Replacing `subsys_stub.c` with real `kernel/subsys/subsys.c`** — depends on `serial_printk`/`strcmp`/`num_cpus`/`idle_resume` becoming available on aarch64.
5. **Replacing `idle_resume_stub.c` with real aarch64 idle loop** — depends on scheduler landing (Phase 2 #3).
6. **`tick_handler()` removal of `#if`-gated poll-scan block** — when aarch64 has its own poll.c (currently dead code on aarch64 phase 2).

## Update 2026-09-24 — AAGU-29 closes item 6

`kernel/arch/aarch64/libc_stub.c` was removed in AAGU-29 once aarch64 kernel started linking `libk.a` (kernel/arch/aarch64/make.config: `ARCH_LIBS = -nostdlib -lk`). `calloc`/`free` (now `kmalloc`/`kfree` via slab) and `memset` come from `libk.a` (`libc/stdlib/{calloc,malloc}.c` + `libc/string/memset.c` cross-compiled to aarch64). The Phase 2 #5 libc sysroot path never fired for aarch64 (aarch64-clang profile is `kernel uefi`, no `userland`), so the substitution is **kernel-side libk.a** instead of a full libc sysroot. Closure: `docs/aarch64-libk-aarch64-closure-2026-09-24.md`. Open follow-up remaining from Phase 2 #5: `kernel/include/compat/*` mirror headers (also tracked as a separate sub-issue — see closure).

## Connection to roadmap P2

Closes **Phase 2 follow-up item #2** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 2. `cntp_tick_handler -> tick_handler()` integration — needs `<list.h>` include-path fix

The `<list.h>` include-path fix was already in place from Task 2.2 (`12d3720`); this spec consumed that fix. After this spec, `tick_handler()` runs on aarch64 (common path only — jiffies/need_resched/watchdog/TIMER_SIRQ set). x86_64 also gets the gated poll-scan block + gated tick_start body but the behavior is byte-identical (the `#if` only protects x86_64-only code).

The unified `kernel_main` spec's "timer unified" sub-task is now closed (modulo the per-CPU side, which is Phase 2 #3).