# AArch64 Generic Timer Phase 1 — Closure Report

**Date**: 2026-09-18
**Branch**: `feat/aarch64-timer` (based on `master @ 6be7735`)
**Commits**: 5 new commits + Phase 0 deferred doc
- `b6ac05f` Task 1.1: clocksource unit test framework + Suite A (algorithm) / B / C / D
- `31e436d` docs: Phase 0 deferred follow-ups (submodule init + clean residue)
- `6f37a21` Task 1.2: Suite E — aarch64 clocksource integration contract (7 asserts)
- `e7bc952` Task 2.1: QEMU harness `--expect-clk` marker (RED → harness ready)
- `12d3720` Task 2.2: kernel-side `[clocksource]` markers + `__udivti3` stub + Makefile/main.c/clocksource.h changes

## Goal achieved

The Generic Timer (CNTP + CNTVCT) is wired into the aarch64 kernel's
unified time framework. The QEMU harness can verify aarch64 timer
init end-to-end via three [clocksource] markers:
- `[clocksource] active=true`
- `[clocksource] freq=<Hz>`  (62,500,000 on QEMU virt)
- `[clocksource] mult=<N> shift=<N>`  (2147483648 / 27 at 62.5 MHz)

`clocksource_init()` is called explicitly from `aarch64_main` (Option B
fallback) because the aarch64 linker.ld has no `.subsys_init` section
and there is no aarch64 equivalent of `kernel/arch/x86_64/subsys.c`.

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang SMP=1 KERNEL_SELFTEST=1 aarch64-uefi` | PASS |
| `test-aarch64-uefi-smp` (SMP=1/2/4 × 3, --expect-selftest --expect-gic --expect-clk) | 9/9 PASS |
| `test-aarch64-gic-spi` (Task 2.3b regression) | PASS |
| `test_gic_driver` hosttest | 69/69 PASS |
| `test_gic_probe` hosttest | 5/5 PASS |
| `test_gic_marker_lines` hosttest | 4/4 PASS |
| `test_clocksource` hosttest (Suite E = 7 asserts at 62.5 MHz) | 49/49 PASS |
| `test_lwip_rand` hosttest | ALL PASSED |
| `aarch64_uefi_smp.py --self-test` | passed |

## Scope NOT done in Phase 1 (deferred to Phase 2 / unification kernel_main)

1. **aarch64 SUBSYS_INITCALL machinery** — add `.subsys_init` section
   to `kernel/arch/aarch64/linker.ld`; add aarch64 equivalent of
   `kernel/arch/x86_64/subsys.c`; flip the `#ifdef __x86_64__` guards
   in `kernel/time/clocksource.c` (and other subsystem files) to
   `#if defined(__x86_64__) || defined(__aarch64__)`. Once this
   lands, `aarch64_main`'s explicit `clocksource_init()` call can be
   removed (subsystem initcall does it).
2. **`cntp_tick_handler` → `tick_handler()` integration** —
   `kernel/include/time/timer.h` includes `<list.h>` (libc header)
   and `kernel/Makefile:93-99` deliberately does not consume the
   libc sysroot for aarch64. Phase 1 only wires `clocksource_init()`
   because that doesn't pull `<list.h>`. The full unified tick
   semantic (`jiffies++`, poll timeout scan, need_resched,
   TIMER_SIRQ) lives in `kernel/time/tick.c::tick_handler()` and
   requires the include-path fix.
3. **Timer per-CPU affinity / SMP timer** — Phase 1 GIC's secondary
   AP bring-up (TASK 3.2 of the GIC plan) disables AP CNTP
   (`kernel/arch/aarch64/smp.c:219-225` masks CNTP_CTL_EL0.EN).
   BSP-only CNTP tick is intentional in Phase 1; per-CPU tick is
   the unification kernel_main spec's "context switch" / "timer
   unified" sub-tasks. Defer until (1) and (2) are resolved.
4. **`__udivti3` hoist** — currently `kernel/arch/aarch64/udivti3_stub.c`.
   Move to `kernel/compiler_rt/` once other aarch64 TUs start using
   `__uint128_t` (lwIP, COW, Event, etc.).
5. **`-I libc/include` on aarch64 kernel** — required today for
   `kernel/include/time/timer.h`'s transitive `<list.h>` resolution
   (only triggered when a TU pulls `clocksource.h`). Long-term,
   split the `<list.h>` dependency out of kernel into either
   `kernel/include/` (breaking kernel/libc boundary) or aarch64
   sysroot. The current `-I libc/include` is documented as a
   deliberate compromise.

## Concern: Phase 1 GIC evidence gate unchanged

The QEMU harness `qemutests/aarch64_uefi_smp.py:485/514` still
requires `≥3 [tick] N` lines — these come from the per-second
print inside `kernel/arch/aarch64/time.c::cntp_tick_handler` (the
BSP-only 100 Hz demo). This print is unrelated to the framework
integration and survives solely because of the evidence gate. When
the harness is rewritten to look for arch-neutral markers (e.g.
`[clocksource]`), this debug print can be deleted.

## Connection to roadmap P2

This closes the **Generic Timer "roadmap P2" entry** (roadmap.md §P2
Generic Timer row). The next P2 sub-item is **unified kernel_main**
(interrupt dispatch / SMP bring-up / context switch unified —
each is a separate spec). The `aarch64 .subsys_init` item above
(Task 1 of Phase 2) is one of the prerequisites for unified
kernel_main.
