# AArch64 SUBSYS_INITCALL Architecture — Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.

**Goal:** Make `SUBSYS_INITCALL()` work on aarch64 so the kernel time framework (clocksource / timer / softirq / pmm) registers itself via the same mechanism as x86_64, eliminating the Option B fallback explicit calls in `aarch64_main`.

**Architecture:** Three artifacts:
1. `kernel/arch/aarch64/linker.ld` — add `.subsys_init : { PROVIDE(__subsys_init_start = .); KEEP(*(.subsys_init)); PROVIDE(__subsys_init_end = .); } :data` (mirror x86_64)
2. `kernel/arch/aarch64/subsys.c` — NEW ~25 lines, mirror x86_64: `void arch_register_subsys(void) { for (p = __subsys_init_start; p < __subsys_init_end; p++) (*p)(); }`
3. `kernel/Makefile` — add `arch/aarch64/subsys.c` to aarch64 KERNEL_C_SOURCES whitelist

After that, flip `#ifdef __x86_64__` to `#if defined(__x86_64__) || defined(__aarch64__)` in:
- `kernel/time/clocksource.c:45` (`SUBSYS_INITCALL(_clocksource_register)`)
- (other files in Phase 2 follow-ups #2-#3 — not in this spec's scope)

Finally, **remove the Option B fallback block** from `kernel/arch/aarch64/main.c` (the `#if defined(__aarch64__) clocksource_init();` block from commit `12d3720`). The framework now registers via `.subsys_init`.

## Context — why it's needed now

Phase 1 Generic Timer (merge `b2b81fc`, 6 commits) landed `clocksource_init()` as an **Option B explicit call** from `aarch64_main` because the aarch64 linker script `kernel/arch/aarch64/linker.ld` has no `.subsys_init` section, so `SUBSYS_INITCALL()` calls in framework code (e.g. `kernel/time/clocksource.c:65`) are silently discarded by the linker — the static function pointer is in a section the linker doesn't know about, and `arch_register_subsys()` doesn't exist on aarch64.

Phase 1 worked around this by calling `clocksource_init()` directly. Two known limitations of that workaround:
1. **Future subsystems** (timer / softirq / pmm / future items) need the same explicit-call dance.
2. **Initialization order** is determined by source-file order in `aarch64_main`, not by phase / priority (which `SUBSYS_INITCALL(SUBSYS_PHASE_4, ...)` expresses on x86_64).

The `unified kernel_main` spec (roadmap §P2, "距离单一 kernel_main 还差多远" section) requires a single `kernel_main` that walks `arch_early_init() / arch_late_init() / scheduler_init()` once across both architectures. The `SUBSYS_INITCALL` gap is one of the prerequisites for that unification — the other prerequisites (`cntp_tick_handler → tick_handler()` integration / per-CPU timer / `-I libc/include` policy cleanup) are documented as the remaining 4 deferred items in `docs/aarch64-timer-phase1-closure-2026-09-18.md`.

## Design

### A. linker script addition

**File:** `kernel/arch/aarch64/linker.ld`

Add a `.subsys_init` output section near the existing `.data` block. Mirror the x86_64 linker script verbatim:

```ld
/* KEEP-wrapped: drivers register a function pointer here via
 * SUBSYS_INITCALL(); arch_register_subsys() iterates this range
 * to call each one. */
.subsys_init :
{
    PROVIDE(__subsys_init_start = .);
    KEEP(*(.subsys_init))
    PROVIDE(__subsys_init_end = .);
} :data
```

Placement: just before the existing `.data` section, so the `.subsys_init` table sits inside the high-half data region the kernel can address post-MMU.

### B. New file: `kernel/arch/aarch64/subsys.c`

~25 lines, byte-mirror of `kernel/arch/x86_64/subsys.c` minus the `arch_boot_rsdp` global (RSDP is an x86-specific concept):

```c
/* kernel/arch/aarch64/subsys.c — arch_register_subsys() iterator.
 *
 * Drivers place a function pointer in the .subsys_init linker section
 * via SUBSYS_INITCALL(). arch_register_subsys() iterates this range
 * and calls each — kernel_main does not need a hardcoded driver list.
 *
 * This is the libc-free kernel style: __attribute__((constructor))
 * puts pointers in .init_array, but no startup code iterates
 * .init_array in this freestanding aarch64 build. SUBSYS_INITCALL is
 * what Linux uses for the same reason.
 */
#include <stdint.h>
#include <subsys/subsys.h>

void arch_register_subsys(void)
{
    for (subsys_initcall_t *p = __subsys_init_start;
         p < __subsys_init_end; p++) {
        (*p)();
    }
}
```

### C. Kernel Makefile whitelist

**File:** `kernel/Makefile`

Add `arch/aarch64/subsys.c` to the aarch64-specific KERNEL_C_SOURCES whitelist (whichever list Phase 1 GIC added `arch/aarch64/gic.c` to — currently around line 42-43, the `ifeq ($(ARCH),aarch64)` block). x86_64's whitelist is untouched (separate `ifeq ($(ARCH),x86_64)` block).

### D. Flip the framework gate

**File:** `kernel/time/clocksource.c`

Change line 45 from:
```c
#ifdef __x86_64__
SUBSYS_INITCALL(_clocksource_register);
#endif
```
to:
```c
#if defined(__x86_64__) || defined(__aarch64__)
SUBSYS_INITCALL(_clocksource_register);
#endif
```

After this commit, `clocksource_init()` is invoked by `arch_register_subsys()` on aarch64, just like x86_64.

### E. Remove Option B fallback from `aarch64_main`

**File:** `kernel/arch/aarch64/main.c`

After commits B/C/D land, remove the `#if defined(__aarch64__) clocksource_init();` block (lines 267-285 of the post-Phase-1 main.c). Also remove the `#include <time/clocksource.h>` line — `aarch64_main` no longer calls `clocksource_init()` directly.

Replace with a one-line comment pointing to `kernel/arch/aarch64/subsys.c`:
```c
/* Clocksource / timer / softirq are registered via SUBSYS_INITCALL
 * and invoked by arch_register_subsys() — called by kernel_main later
 * in this flow (after smp_boot_aps + before arch_tick_start). */
```

### F. Hook `arch_register_subsys()` into the boot flow

`kernel/arch/aarch64/main.c` does NOT yet call `arch_register_subsys()`. Today it doesn't need to (Option B fallback). After this spec, it must — without the call, `clocksource_init()` (and any future framework subsystem) won't run.

Insert one call after `smp_boot_aps()` and before `arch_tick_start()`:

```c
#if defined(__aarch64__)
extern void arch_register_subsys(void);
arch_register_subsys();
#endif
```

(Forward declaration because `arch/aarch64/subsys.h` doesn't exist yet — phase 2 follow-up could add it. For now the extern inline is fine; it's a 1-line addition.)

## Non-goals (out of scope)

1. **`cntp_tick_handler → tick_handler()` integration** — still blocked on `<list.h>` transitive include. Phase 2 follow-up item #2.
2. **Per-CPU timer affinity / SMP timer** — still pending item #2 above. Phase 2 follow-up item #3.
3. **`__udivti3` hoist to `compiler_rt/`** — independent cleanup. Phase 2 follow-up item #4.
4. **`-I libc/include` policy on aarch64 kernel** — long-term cleanup; once this spec lands, future items can revisit. Phase 2 follow-up item #5.
5. **Other framework files with `SUBSYS_INITCALL`** (timer / softirq / pmm) — they're already gated on `__x86_64__`. This spec doesn't touch them; a single follow-up after this spec could flip their gates uniformly if needed.
6. **`arch/aarch64/subsys.h` header file** — currently `subsys.h` is at `kernel/include/subsys/subsys.h` (shared); adding an `arch/*` variant would be over-engineering for one extern. Use the inline `extern void arch_register_subsys(void);` form in F above.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Adding `.subsys_init` section to aarch64 linker.ld shifts other symbols and changes the kernel binary byte-for-byte, breaking build contracts | High | Run `make test-build-contract-aarch64` (existing target) immediately after step A lands. If binary grows > N KiB or layout shifts, compare with master baseline. |
| `SUBSYS_INITCALL` macro emits a static function pointer in `.subsys_init`; if aarch64 linker silently discards it again (e.g. KEEP missing), the framework appears to run but doesn't | Medium | Step B/C require the KEEP directive (mirroring x86_64 verbatim). Verify via `nm build/aarch64-clang/kernel.elf | grep subsys_init` to confirm `__subsys_init_start`/`__subsys_init_end` are present. |
| Order of `arch_register_subsys()` vs. `arch_tick_start()` matters: clocksource_init() must run before the first CNTP tick so `clocksource_read_ns()` works during `cntp_tick_handler` | Medium | Step F's placement (after `smp_boot_aps`, before `arch_tick_start`) is the same hook point Phase 1 had for the Option B explicit call. QEMU E2E test verifies by inspecting the `[clocksource] active=true` marker order in stdout.log (must appear before `[cntp]`). |
| Removing Option B block in step E without replacing it would silently break aarch64 timer | Low | Step F adds the explicit `arch_register_subsys()` call first. E removes the now-redundant Option B block. Both commits land in the same plan; both verified by full regression before merge. |

## Verification

### Unit / host tests (target: hosttest framework)

No new hosttest needed — the existing 5 hosttests must continue to pass byte-for-byte after each commit. Specifically:
- `test_gic_driver` 69/69 (aarch64-clang profile) — relies on aarch64 kernel still producing GIC marker; depends on `gic_init()` running, which is wired through `arch_register_subsys()` once Step D lands.
- `test_clocksource` 49/49 (aarch64-clang profile) — asserts the framework's `compute_mult_shift` algorithm; not affected.
- `test_lwip_rand`, `test_gic_probe`, `test_gic_marker_lines` — unchanged.

### QEMU end-to-end (target: existing regression matrix)

**Existing evidence gate** (must continue to pass byte-for-byte):
- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` → 9/9 PASS
  - `--expect-selftest --expect-gic --expect-clk` all green
  - 3 `[clocksource]` markers per case (active=true, freq=62500000, mult=2147483648 shift=27)
  - 3 `[tick]` markers per case (GIC Phase 1 evidence gate preserved)
  - SMP=1/2/4 × 3 cases
- `make PROFILE=aarch64-clang test-aarch64-gic-spi` → PASS (PL011 RX → SPI handler still works)
- `make PROFILE=x86_64-clang test-user-canary` → audit passed
- `make PROFILE=x86_64-clang test-kernel-selftest` → 27/27 PASS
- `make PROFILE=x86_64-clang test-pmm-boot-reservation` → 6/6 PASS
- `make PROFILE=x86_64-clang test-kernel-canary-contract` → 5/5 PASS

**New test:** `nm build/aarch64-clang/image/kernel.elf | grep __subsys_init` must show both `__subsys_init_start` and `__subsys_init_end` defined (proves the linker KEEP is real, not silently stripped).

### Architecture invariants

x86_64 byte-identity: `make PROFILE=x86_64-clang test-build-contract-x86` and `make test-kernel-canary-contract` and `make test-user-canary` — all must pass byte-for-byte. None of this spec touches x86_64 sources.

aarch64 byte-identity: `make PROFILE=aarch64-clang test-build-contract-aarch64` — should still pass (binary may grow by <1 KiB to accommodate the `.subsys_init` table, which is acceptable).

## Connection to roadmap §P2

This closes the **interrupts/exceptions dispatch unified** sub-item (roadmap §P2, "中断/异常 dispatch 统一" entry: "`arch_intr_dispatch(vector, pt_regs*)` 单入口抽象"). Not because we touch `arch_intr_dispatch()` (already on the GIC side), but because the SUBSYS_INITCALL infrastructure is the platform-side prerequisite for the unified `kernel_main` to walk both architectures identically.

The remaining Phase 2 P2 follow-ups (items #2-#5 in the closure report) are NOT this spec's responsibility. They become their own specs when picked up.

## Subagent-driven plan (to be written via writing-plans skill)

After this spec is approved by the user, the writing-plans skill produces `docs/superpowers/plans/2026-09-18-aarch64-subsys-init-plan.md` with ~5-7 RED/GREEN tasks mirroring the GIC Phase 1 plan structure. Estimated commit count: 5-7 (one docs, three functional, one refactor). Estimated wall-clock: 1-2 days (no new framework code; mostly linker + mirroring).