# AArch64 SUBSYS_INITCALL Architecture — Design (R2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.
>
> **R1 review:** sonnet R1 pass produced 3 REJECTED + 8 NIT. This R2 version fixes all REJECTEDs and most NITs. R3 (codex) will re-verify.

**Goal:** Make `SUBSYS_INITCALL()` work on aarch64 so the kernel time framework (clocksource / timer / softirq / pmm) registers itself via the same mechanism as x86_64, eliminating the Option B fallback explicit `clocksource_init()` call in `aarch64_main`.

**Architecture — 6 functional commits + 1 docs commit (per R1 fix §2):**

| Step | File | What | Commit group |
|---|---|---|---|
| **A** | `kernel/arch/aarch64/linker.ld` | Add `.subsys_init` section after `.data` with `AT(ADDR(...) - _kernel_page_offset)` for high-half | **Group 1** |
| **B** | `kernel/arch/aarch64/subsys.c` (NEW) | `arch_register_subsys()` iterator, ~25 lines byte-mirror of x86_64 | **Group 1** |
| **C** | `kernel/Makefile` | Add `arch/aarch64/subsys.c` to aarch64 KERNEL_C_SOURCES whitelist | **Group 1** |
| **F** | `kernel/arch/aarch64/main.c` | Insert `arch_register_subsys()` call (after `smp_boot_aps`, before `arch_tick_start`) | **Group 2** |
| **D** | `kernel/time/clocksource.c` | Flip `#ifdef __x86_64__` → `#if defined(__x86_64__) \|\| defined(__aarch64__)` at line 50 | **Group 3** |
| **E** | `kernel/arch/aarch64/main.c` | Remove only the `clocksource_init()` call line in Option B block; **keep** marker prints + 4 forward decls | **Group 3** |

**Commit-order constraint** (R1 fix §7/§9): Group 1 (linker+subsys.c+Makefile) MUST land before Group 2 (hook `arch_register_subsys()` in main.c); otherwise the build fails with `undefined reference to 'arch_register_subsys'`. Group 3 (flip + remove explicit call) MUST land AFTER Group 2 — flipping the gate first would cause `arch_register_subsys()` to invoke `_clocksource_register()` while the explicit call is also still there, which is a double-init (the framework would call `clocksource_init()` twice; double-init is benign on clocksource but sets a bad precedent).

The Option B block at `kernel/arch/aarch64/main.c:279-303` (R1 fix §8) contains both the explicit `clocksource_init()` call AND the 3 `[clocksource]` marker prints required by the `--expect-clk` harness evidence gate (`qemutests/aarch64_uefi_smp.py:401-403`). The marker prints MUST survive — removing them would regress `--expect-clk`. Only the `clocksource_init()` call is redundant (the framework now calls it via `arch_register_subsys()`).

## Context — why it's needed now

Phase 1 Generic Timer (merge `b2b81fc`, 6 commits) landed `clocksource_init()` as an **Option B explicit call** from `aarch64_main` because the aarch64 linker script `kernel/arch/aarch64/linker.ld` has no `.subsys_init` section, so `SUBSYS_INITCALL()` calls in framework code (e.g. `kernel/time/clocksource.c:65`) are silently discarded by the linker — the static function pointer is in a section the linker doesn't know about, and `arch_register_subsys()` doesn't exist on aarch64.

Phase 1 worked around this by calling `clocksource_init()` directly. Two known limitations of that workaround:
1. **Future subsystems** (specifically: only `kernel/time/timer.c:136` has a `#ifdef __x86_64__` gate around `SUBSYS_INITCALL(_timer_register)` — `softirq` and `pmm` are NOT `SUBSYS_INITCALL`-registered on x86_64 either; they're called explicitly from `kernel/intr/irq.c:78` and `kernel/core/main.c:155` respectively. So only timer.c would benefit from a future gate flip; softirq/pmm are out of this spec by virtue of not being registered at all.)
2. **Initialization order** is determined by source-file order in `aarch64_main`, not by phase / priority (which `SUBSYS_INITCALL(SUBSYS_PHASE_4, ...)` expresses on x86_64).

The `unified kernel_main` spec (roadmap §P2, "距离单一 kernel_main 还差多远" section) requires a single `kernel_main` that walks `arch_early_init() / arch_late_init() / scheduler_init()` once across both architectures. The `SUBSYS_INITCALL` gap is **one of the prerequisites** for that unification — it does not directly close the "interrupts/exceptions dispatch unified" sub-item (that's `arch_intr_dispatch(vector, pt_regs*)` which is already wired on the GIC side). The other prerequisites (`cntp_tick_handler → tick_handler()` integration / per-CPU timer / `-I libc/include` policy cleanup / `__udivti3` hoist) are documented as the remaining 4 deferred items in `docs/aarch64-timer-phase1-closure-2026-09-18.md`.

**Dependency on prior Phase 1 Task 2.2** (R1 fix §10 #4): this spec RELIES on the `-I libc/include` for the aarch64 kernel build added in commit `12d3720` (`kernel/Makefile:118` in the `ifeq ($(ARCH),aarch64)` block). Without that `-I`, `kernel/time/clocksource.c` would fail to compile on aarch64 because `kernel/include/time/clocksource.h` → `kernel/include/time/timer.h` transitively pulls `<list.h>` (libc header). The `-I` is in place as of `12d3720`; this spec doesn't change it. It is NOT a follow-up for this spec — but it IS a dependency we should explicitly call out so future reviewers don't think we missed it.

## Design

### A. linker script addition

**File:** `kernel/arch/aarch64/linker.ld`

Add a `.subsys_init` output section **after** `.data` (mirroring x86_64 placement, R1 fix §4) with the dual high-half VMA/LMA mapping (aarch64-specific, since x86_64 doesn't have the dual mapping):

```ld
/* KEEP-wrapped: drivers register a function pointer here via
 * SUBSYS_INITCALL(); arch_register_subsys() iterates this range
 * to call each one. Placement AFTER .data (not before) to byte-mirror
 * kernel/arch/x86_64/linker.ld — same relative position keeps
 * the kernel-image walk in pmm.c consistent across architectures.
 * The AT(ADDR(...) - _kernel_page_offset) mirrors the surrounding
 * .data section's high-half direct-map LMA mapping so the symbols
 * are addressable from the kernel image's high-half VA. */
.subsys_init : AT(ADDR(.subsys_init) - _kernel_page_offset)
{
    PROVIDE(__subsys_init_start = .);
    KEEP(*(.subsys_init))
    PROVIDE(__subsys_init_end = .);
} :data
```

Placement: just **after** the `.data` section in `kernel/arch/aarch64/linker.ld`. The `kernel_page_offset` constant is already defined elsewhere in the linker script (used by `.data`).

### B. New file: `kernel/arch/aarch64/subsys.c`

~25 lines, byte-mirror of `kernel/arch/x86_64/subsys.c` minus the `arch_boot_rsdp` global (RSDP is x86 ACPI concept; verified absent from all aarch64 sources via grep):

```c
// ── kernel/arch/aarch64/subsys.c ──────────────────────────────
//
// arch_register_subsys() iterator — byte-mirror of x86_64 subsys.c
// minus arch_boot_rsdp (RSDP is x86 ACPI concept; not referenced
// anywhere in aarch64 sources).
//
// Drivers place a function pointer in the .subsys_init linker section
// via SUBSYS_INITCALL(). arch_register_subsys() iterates this range
// and calls each — kernel_main does not need a hardcoded driver list.
//
// This is the libc-free kernel style: __attribute__((constructor))
// puts pointers in .init_array, but no startup code iterates
// .init_array in this freestanding aarch64 build. SUBSYS_INITCALL is
// what Linux uses for the same reason.

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

Add `arch/aarch64/subsys.c` to the aarch64-specific KERNEL_C_SOURCES whitelist (currently lines 42-44 in the `ifeq ($(ARCH),aarch64)` block, which already contains `time/clocksource.c`, `arch/aarch64/udivti3_stub.c`, etc.). x86_64's whitelist (the `ifeq ($(ARCH),x86_64)` block at lines 46-67) is untouched.

### D. Flip the framework gate

**File:** `kernel/time/clocksource.c`

Change **line 50** (the `#ifdef __x86_64__` that wraps `SUBSYS_INITCALL(_clocksource_register)` at line 70) from:

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

**Other framework files with `SUBSYS_INITCALL`** (R1 fix §7):
- `kernel/time/timer.c:136` — has `#ifdef __x86_64__` gate around `_timer_register`. A future follow-up spec could flip this to match clocksource. Not this spec's scope (timer depends on the `tick_handler()` integration — see Non-goals §1).
- `kernel/driver/pit.c`, `kernel/driver/serial.c`, `kernel/driver/keyboard.c`, `kernel/driver/ahci.c`, `kernel/intr/apic/lapic.c`, `kernel/intr/apic/lapic_timer.c`, `kernel/intr/pic/8259A.c`, `kernel/net/net.c` — all `SUBSYS_INITCALL`-registered **without** `#ifdef __x86_64__` gates. These compile on x86_64 only because the kernel Makefile doesn't include them on aarch64 (drivers are arch-specific, not gated by `__x86_64__`). After this spec, the linker will still discard them on aarch64 (no `__aarch64__` gate flip), but that's silent-discard which is the same as today. No additional change needed.

### E. Trim Option B fallback in `aarch64_main`

**File:** `kernel/arch/aarch64/main.c`

The Option B block at **lines 279-303** (R1 fix §8) contains BOTH:
- the `clocksource_init()` call (line 291) — becomes redundant after Group 3 lands
- the 3 `[clocksource]` marker prints (`kputs("[clocksource] active=true\n")` etc.) — **must survive** because the `--expect-clk` QEMU harness evidence gate depends on them

Correct change: **only** remove the `clocksource_init()` call line. Keep:
- the 4 forward decls (`clocksource_active`, `clocksource_freq_hz`, `clocksource_mult`, `clocksource_shift`) — still needed by the marker prints
- the marker prints themselves — required by `--expect-clk`
- the long block comment explaining why the explicit call existed — update wording to point at `arch_register_subsys()` instead

Result:

```c
    /* Clocksource / timer / softirq are registered via SUBSYS_INITCALL
     * and invoked by arch_register_subsys() below (called between
     * smp_boot_aps and arch_tick_start). The 3 [clocksource] markers
     * below remain because qemutests/aarch64_uefi_smp.py --expect-clk
     * requires them as evidence. */
#if defined(__aarch64__)
    {
        extern bool clocksource_active;
        extern uint64_t clocksource_freq_hz(void);
        extern uint32_t clocksource_mult;
        extern uint32_t clocksource_shift;
        if (clocksource_active) {
            kputs("[clocksource] active=true\n");
            kputs("[clocksource] freq=");
            kputu(clocksource_freq_hz());
            kputs("\n");
            kputs("[clocksource] mult=");
            kputu((uint64_t)clocksource_mult);
            kputs(" shift=");
            kputu((uint64_t)clocksource_shift);
            kputs("\n");
        }
    }
#endif
```

Note: this block no longer calls `clocksource_init()` — that's done by `arch_register_subsys()` from Group 2. If for any reason `arch_register_subsys()` doesn't run (e.g. linker setup broken), `clocksource_active` will be false and the marker block prints nothing — which itself is a tell that something is wrong (the harness `--expect-clk` will fail because no markers appear, surfacing the regression).

### F. Hook `arch_register_subsys()` into the boot flow

**File:** `kernel/arch/aarch64/main.c`

Insert one call after `smp_boot_aps()` (line 272) and before `arch_tick_start()` (line 304):

```c
#if defined(__aarch64__)
    extern void arch_register_subsys(void);
    arch_register_subsys();
#endif
```

(Forward declaration because `arch/aarch64/subsys.h` doesn't exist yet — the inline `extern` form is a deliberate temporary form, see Non-goals §6 for the follow-up that could add the shared header.)

**Timing requirement**: `arch_register_subsys()` must run BEFORE `arch_tick_start()` so `clocksource_init()` (one of the registered initcalls) sets `clocksource_active = true` before the first CNTP tick fires. The harness `--expect-clk` evidence gate verifies this: the 3 `[clocksource]` markers (emitted by the Option B block in §E) must appear in the QEMU stdout.log BEFORE the `[cntp]` marker.

## Non-goals (out of scope)

1. **`cntp_tick_handler → tick_handler()` integration** — still blocked on `<list.h>` transitive include. Phase 2 follow-up item #2.
2. **Per-CPU timer affinity / SMP timer** — still pending item #1 above. Phase 2 follow-up item #3.
3. **`__udivti3` hoist to `compiler_rt/`** — independent cleanup. Phase 2 follow-up item #4.
4. **`-I libc/include` policy** — already in place from Task 2.2 (`12d3720`); this spec relies on it but doesn't change it.
5. **Other framework files with `SUBSYS_INITCALL`** — `kernel/time/timer.c:136` has a `#ifdef __x86_64__` gate that could be flipped similarly, but only `kernel/time/timer.c:136` is gated today (verified). The other `SUBSYS_INITCALL` users (`pit.c`, `serial.c`, `keyboard.c`, `ahci.c`, `lapic.c`, `lapic_timer.c`, `8259A.c`, `net.c`) are arch-specific x86 drivers — they're not in the aarch64 build at all, so no gate flip is relevant. Future P2 follow-up could flip `timer.c:136` once item #1 is resolved.
6. **`arch/aarch64/subsys.h` header file** — currently `subsys.h` is at `kernel/include/subsys/subsys.h` (shared); adding an `arch/*` variant for one extern declaration would be over-engineering. Using the inline `extern void arch_register_subsys(void);` form in step F is a deliberate temporary form.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Adding `.subsys_init` section to aarch64 linker.ld shifts other symbols and changes the kernel binary byte-for-byte, breaking build contracts | High | Run `make test-build-contract-aarch64` immediately after Group 1 lands. If binary grows > N KiB or layout shifts, compare with master baseline. |
| `SUBSYS_INITCALL` macro emits a static function pointer in `.subsys_init`; if aarch64 linker silently discards it again (e.g. KEEP missing), the framework appears to run but doesn't | Medium | Group 1's `KEEP(*(.subsys_init))` directive (mirroring x86_64 verbatim) makes this not happen. **Double verification** via `nm build/aarch64-clang/kernel.elf | grep __subsys_init` must show BOTH `__subsys_init_start` AND `__subsys_init_end` defined. Additionally, the registered initcall count must match expectations: after Group 3 lands, expect exactly 1 registered initcall on aarch64 (clocksource's `_clocksource_register`). |
| **Empty iteration** (R1 fix §11): `__subsys_init_start == __subsys_init_end` (zero registered initcalls) at first run would make `arch_register_subsys()` silently succeed even on broken linker setup | Medium | The `nm | grep __subsys_init` check must verify BOTH symbols are present. Add a stronger check: `nm build/aarch64-clang/kernel.elf | grep -c subsys_init` should return ≥3 (two PROVIDE symbols + the function pointer). If aarch64 build with KEEP works, expect exactly 3 (start, end, `_clocksource_register`). |
| Order of `arch_register_subsys()` vs. `arch_tick_start()` matters: clocksource_init() must run before the first CNTP tick so `clocksource_read_ns()` works during `cntp_tick_handler` | Medium | Step F's placement (after `smp_boot_aps`, before `arch_tick_start`) is the same hook point Phase 1 had for the Option B explicit call. QEMU E2E test verifies by inspecting the `[clocksource]` marker order in stdout.log (must appear before `[cntp]`). |
| Removing Option B block in step E without replacing it would silently break aarch64 timer | Low | Step E removes ONLY the `clocksource_init()` call line, keeps marker prints. Both commits land in Group 3 with the same plan; both verified by full regression before merge. |
| **Commit-order violation** (R1 fix §7/§9): if Group 2 (hook call) lands before Group 1 (subsys.c defining the symbol), the build fails with `undefined reference to 'arch_register_subsys'`. If Group 3 (flip gate) lands before Group 2 (hook call), framework calls `clocksource_init()` twice (benign on clocksource but bad precedent). | High | The writing-plans skill MUST encode the strict commit order: **Group 1 → Group 2 → Group 3**. Verify the order is preserved when the plan is executed by checking the git log between them. |
| GIC `gic_init()` interaction: `kernel/arch/aarch64/gic.c` does NOT use `SUBSYS_INITCALL` — `gic_init()` is called explicitly at `kernel/arch/aarch64/main.c:270` (BEFORE the new `arch_register_subsys()` hook runs). Will Group 2's hook break GIC? | Low | Verified by grep: gic.c has no SUBSYS_INITCALL. GIC's explicit call at line 270 is BEFORE the new hook at the post-`smp_boot_aps` position. After Group 1 lands, GIC's explicit call still works (no change). After Group 3 lands, GIC still uses the explicit call (no change to gic.c). No break. |

## Verification

### Unit / host tests (target: hosttest framework)

No new hosttest needed — the existing 5 hosttests must continue to pass byte-for-byte after each commit. Specifically:
- `test_gic_driver` 69/69 (aarch64-clang profile) — relies on aarch64 kernel still producing GIC marker; depends on `gic_init()` running, which is wired through Option B explicit call (NOT SUBSYS_INITCALL — see GIC interaction risk above). No regression risk.
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

**New tests:**
- `nm build/aarch64-clang/image/kernel.elf | grep __subsys_init` must show BOTH `__subsys_init_start` AND `__subsys_init_end` defined (proves the linker KEEP is real, not silently stripped). Add a stronger check: `nm | grep -c subsys_init` ≥ 3 after Group 3 lands (start + end + registered initcall).
- `make PROFILE=aarch64-clang test-build-contract-aarch64` — binary may grow slightly to accommodate the `.subsys_init` table; should still pass byte-identity invariant.

### Architecture invariants

x86_64 byte-identity: `make PROFILE=x86_64-clang test-build-contract-x86`, `make test-kernel-canary-contract`, `make test-user-canary` — all must pass byte-for-byte. None of this spec touches x86_64 sources.

aarch64 byte-identity: `make PROFILE=aarch64-clang test-build-contract-aarch64` — binary may grow by <1 KiB to accommodate the `.subsys_init` table.

## Connection to roadmap §P2

This closes the **subsystem init unified** prerequisite (not directly a roadmap item, but required for the "interrupts/exceptions dispatch unified" + "统一 kernel_main" entries to make sense for aarch64). The actual interrupts-dispatch unification (`arch_intr_dispatch(vector, pt_regs*)` single-entry abstraction) is already wired on the GIC side (Phase 1 GIC `entry.S` + `trap.c::el1_irq`); what's missing is the framework-init unification that this spec provides.

The remaining Phase 2 P2 follow-ups (items #2-#5 in the closure report) are NOT this spec's responsibility. They become their own specs when picked up.

## Subagent-driven plan (to be written via writing-plans skill)

After this spec is approved by the user (after R3 codex re-review), the writing-plans skill produces `docs/superpowers/plans/2026-09-18-aarch64-subsys-init-plan.md` with RED/GREEN tasks mirroring the GIC Phase 1 plan structure. Estimated commit count: 6 functional + 1 docs = 7. Estimated wall-clock: 1-2 days (no new framework code; mostly linker + mirroring).

**Commit ORDER is the critical plan document section** (per R1 fix §7/§9):
1. `docs(superpowers): aarch64 SUBSYS_INITCALL architecture spec (R3)` — this doc
2. Group 1: `feat(aarch64): add .subsys_init linker section + arch_register_subsys()` (A+B+C)
3. Group 2: `feat(aarch64): hook arch_register_subsys() into aarch64_main` (F)
4. Group 3a: `feat(aarch64): flip clocksource.c SUBSYS_INITCALL gate` (D)
5. Group 3b: `refactor(aarch64): remove redundant clocksource_init() Option B call in aarch64_main` (E)

After each Group, run the full regression matrix and commit only on green. The plan's RED/GREEN sections must enforce this ordering explicitly.