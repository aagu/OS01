# aarch64 SUBSYS_INITCALL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `SUBSYS_INITCALL()` on aarch64 so kernel time framework (currently `clocksource.c`) registers itself via the same mechanism as x86_64, eliminating the Option B explicit `clocksource_init()` call from `aarch64_main`. 4 functional commits mirroring the x86_64 template; x86_64 byte-identity preserved.

**Architecture:** Three artifacts (linker section + iterator TU + Makefile whitelist) enable `SUBSYS_INITCALL`-registered initcalls on aarch64. Then a single hook in `aarch64_main` does the **register+dispatch** pair (mirroring `kernel/core/main.c:193-194` on x86_64): `arch_register_subsys()` runs the queued `_register` wrappers (which populate `subsys_table[]` via `register_subsys()`); `subsys_init_phase(SUBSYS_PHASE_4)` then dispatches the table, running `_clocksource_init_wrapper` (which calls `clocksource_init()`). Without the dispatch step, no registered initcall ever runs — that's the R3-1 critical fix. Finally flip the `SUBSYS_INITCALL` gate in `clocksource.c` to enable aarch64 registration, and trim the now-redundant explicit `clocksource_init()` call from `aarch64_main`.

**Tech Stack:** freestanding C (clang -target aarch64-none-elf), GNU ld.lld linker script, GNU Make profile build (`mk/profiles/aarch64-clang.mk`), host clang tests (`hosttests/`), Python QEMU harness (`qemutests/`).

**Spec:** `docs/superpowers/specs/2026-09-18-aarch64-subsys-init-design.md` (R5 APPROVED, commit `0ae0709`)

## Global Constraints

- **Worktree:** all work in `feat/aarch64-timer-phase2` worktree (`/home/aagu/aarch64-timer-phase2`); do NOT touch master or `feat/aarch64-timer`.
- **AGENTS.md is PROTECTED.** Do not edit.
- **No push.** Local commits only.
- **x86_64 byte-identity:** `make PROFILE=x86_64-clang test-build-contract-x86`, `test-user-canary`, `test-kernel-canary-contract`, `test-kernel-selftest` must pass byte-for-byte. None of this plan touches x86_64 sources.
- **aarch64 build contract:** `make PROFILE=aarch64-clang test-build-contract-aarch64` must pass (binary may grow ≤1 KiB to accommodate `.subsys_init` table).
- **Strict commit order** (R1 fix §7/§9, R3-1 critical): **Group 1 → Group 2 → Group 3a → Group 3b**. Tasks below are numbered to enforce this. Verify the git log between commits matches.
- **SPEC.md line numbers** are read from the R5-approved spec at `docs/superpowers/specs/2026-09-18-aarch64-subsys-init-design.md`. If the spec evolved, re-verify line numbers from the file before each step.
- **No tests added for plumbing** (Group 1 has no new test — it's pure plumbing; the verification is `nm | grep __subsys_init`).
- **No host tests for SUBSYS_INITCALL dispatch path** — the critical regression (R3-1: missing `subsys_init_phase`) would ONLY be caught by QEMU E2E `--expect-clk`. Always run `make PROFILE=aarch64-clang test-aarch64-uefi-smp` after Group 2 lands.
- **Phase 1 GIC evidence gate unchanged** — the existing harness `qemutests/aarch64_uefi_smp.py:485/514` requires `≥3 [tick] N` lines. The `[tick]` per-second print survives from `cntp_tick_handler` (unrelated to this spec).
- **R5 NIT (un-fixed):** R4's Non-goal #6 wording claims `<subsys/subsys.h>` transitively includes `<arch/subsys.h>` — that's wrong. The actual concern: `<arch/subsys.h>` is the canonical declaration site for `arch_register_subsys`; including it directly would expose `arch_boot_rsdp` (x86 ACPI concept). Conclusion unchanged: use inline externs.

---

## File Structure

**Created:**
- `kernel/arch/aarch64/subsys.c` (~30 lines) — `arch_register_subsys()` iterator, byte-mirror of `kernel/arch/x86_64/subsys.c` minus `arch_boot_rsdp`.

**Modified:**
- `kernel/arch/aarch64/linker.ld` — add `.subsys_init : AT(ADDR(.subsys_init) - _kernel_page_offset) { ... } :data` after `.data` block.
- `kernel/Makefile` — add `arch/aarch64/subsys.c` to the aarch64 KERNEL_C_SOURCES whitelist (lines 42-44).
- `kernel/arch/aarch64/main.c` — add hook calls in Group 2 (Task 2); remove redundant `clocksource_init()` call + delete its forward decl in Group 3b (Task 4).
- `kernel/time/clocksource.c` — flip `#ifdef __x86_64__` to `#if defined(__x86_64__) || defined(__aarch64__)` in Group 3a (Task 3).

**Not modified (verified preserved):**
- `kernel/arch/x86_64/subsys.c`, `kernel/arch/x86_64/linker.ld`, `kernel/core/main.c`, all x86_64-only drivers — all untouched.
- `kernel/include/subsys/subsys.h`, `kernel/include/arch/subsys.h` — both untouched.

---

### Task 1: Group 1 — aarch64 SUBSYS_INITCALL plumbing

**Files:**
- Create: `kernel/arch/aarch64/subsys.c`
- Modify: `kernel/arch/aarch64/linker.ld`
- Modify: `kernel/Makefile`

**Interfaces:**
- Produces: `void arch_register_subsys(void)` (no args, no return). Symbol defined in `kernel/arch/aarch64/subsys.c`; callable from `aarch64_main.c` in Task 2.
- Produces: `__subsys_init_start[]` / `__subsys_init_end[]` externs provided by the linker script's `PROVIDE` directives (mirroring `kernel/arch/x86_64/linker.ld:52-57`).

- [ ] **Step 1: Verify baseline builds clean**

```sh
cd /home/aagu/aarch64-timer-phase2
rm -rf build/aarch64-clang/
make PROFILE=aarch64-clang SMP=1 aarch64-uefi 2>&1 | tail -10
```

Expected: kernel.elf and aarch64-uefi.img produced; no errors.

- [ ] **Step 2: Create `kernel/arch/aarch64/subsys.c`**

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

- [ ] **Step 3: Modify `kernel/arch/aarch64/linker.ld` — add `.subsys_init` after `.data`**

Open the file. Locate the end of the `.data` block. **After** the existing `.data` block (not before — R1 fix §4 byte-mirror with x86_64 placement), insert:

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

Verify `_kernel_page_offset` is defined elsewhere in the same linker script (it's used by `.data` and `.bss`). Open the file and grep: `grep -n _kernel_page_offset kernel/arch/aarch64/linker.ld`. Expect at least 3 hits: the constant definition (e.g., `= 0xffff000000000000`), the `.data` AT expression, the `.bss` AT expression.

- [ ] **Step 4: Modify `kernel/Makefile` — add `arch/aarch64/subsys.c` to aarch64 whitelist**

Open the file. Locate the `ifeq ($(ARCH),aarch64)` block (around lines 42-44). Add `arch/aarch64/subsys.c` to the KERNEL_C_SOURCES list. Example (verify exact line content first):

```make
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES += time/clocksource.c arch/aarch64/udivti3_stub.c arch/aarch64/subsys.c
endif
```

(Do not touch the `ifeq ($(ARCH),x86_64)` block — x86_64's whitelist is independent.)

- [ ] **Step 5: Build**

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang SMP=1 aarch64-uefi 2>&1 | tail -10
```

Expected: kernel.elf + aarch64-uefi.img produced; no errors. The new `subsys.c` compiles and links.

- [ ] **Step 6: Verify linker produced the `__subsys_init` symbols**

```sh
nm build/aarch64-clang/image/kernel.elf | grep __subsys_init
```

Expected output (BOTH symbols present):

```
0xffff000000XXXXXX A __subsys_init_start
0xffff000000XXXXXX A __subsys_init_end
```

If either symbol is missing, the linker script is wrong — re-check Step 3. (Note: at this point the count will be 2 symbols, not 3, because Task 3's gate flip hasn't landed yet; `clocksource.c` still gates `SUBSYS_INITCALL` on `__x86_64__` only. The third expected symbol `__clocksource_register` function pointer will appear after Task 3.)

- [ ] **Step 7: Commit**

```sh
git add kernel/arch/aarch64/subsys.c \
        kernel/arch/aarch64/linker.ld \
        kernel/Makefile
git commit -m "feat(aarch64): SUBSYS_INITCALL plumbing — linker .subsys_init + arch_register_subsys iterator

- kernel/arch/aarch64/linker.ld: add .subsys_init section AFTER .data
  (byte-mirror kernel/arch/x86_64/linker.ld:52-57 placement, R1
  fix §4) with AT(ADDR(.subsys_init) - _kernel_page_offset) for
  high-half dual VMA/LMA mapping (aarch64-specific; x86_64
  doesn't have dual mapping). PROVIDE __subsys_init_start/end
  inside KEEP(*(.subsys_init)) so the linker doesn't strip
  registered initcalls.

- kernel/arch/aarch64/subsys.c (NEW, ~30 lines): byte-mirror of
  kernel/arch/x86_64/subsys.c minus arch_boot_rsdp (x86 ACPI
  concept). Iterates __subsys_init_start..__subsys_init_end and
  calls each registered initcall. x86_64-style libc-free
  SUBSYS_INITCALL pattern.

- kernel/Makefile: add arch/aarch64/subsys.c to aarch64
  KERNEL_C_SOURCES whitelist (lines 42-44, in the
  ifeq (\$(ARCH),aarch64) block). x86_64 whitelist untouched
  (separate ifeq block).

Build verification:
- make PROFILE=aarch64-clang SMP=1 aarch64-uefi: build OK
- nm build/aarch64-clang/image/kernel.elf | grep __subsys_init:
  BOTH __subsys_init_start and __subsys_init_end symbols
  present. After Task 3 lands, the third symbol
  __clocksource_register function pointer will also appear.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: Group 2 — hook `arch_register_subsys()` + `subsys_init_phase(SUBSYS_PHASE_4)` into `aarch64_main`

**Files:**
- Modify: `kernel/arch/aarch64/main.c`

**Interfaces:**
- Consumes: `void arch_register_subsys(void)` from Task 1's `subsys.c`.
- Consumes: `void subsys_init_phase(int phase)` from `kernel/subsys/subsys.c` (architecture-neutral, no new include needed).
- Consumes: `SUBSYS_PHASE_4` constant from `kernel/include/subsys/subsys.h`.

- [ ] **Step 1: RED — verify pre-state**

Run QEMU on the Task 1 binary (no Group 2 hook yet). Expected: 9/9 PASS still (Task 1 doesn't break anything; Group 2 is the next hook). Pre-state evidence:

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
```

Expected: 9 case JSONs, all `"result":"PASS"`. Save this output as Task 2's RED evidence.

- [ ] **Step 2: Modify `kernel/arch/aarch64/main.c`** — insert two hook calls

Open `kernel/arch/aarch64/main.c`. Locate `smp_boot_aps()` call (around line 272) and `arch_tick_start()` call (around line 304). Between them, after the existing Option B block, insert:

```c
#if defined(__aarch64__)
    /* R3-1 critical: register queues wrappers; phase dispatch runs them.
     * On x86_64 (kernel/core/main.c:193-194) this is the consecutive
     * register+dispatch pair. Without subsys_init_phase, no SUBSYS_INITCALL
     * registered initcall ever runs — clocksource_init never called,
     * [clocksource] markers never appear, --expect-clk FAILS. */
    extern void arch_register_subsys(void);
    extern void subsys_init_phase(int phase);
    arch_register_subsys();
    subsys_init_phase(SUBSYS_PHASE_4);
#endif
```

Verify the `#include` directives at top of main.c already include `<arch/aarch64/smp.h>` (yes — line 12). No additional `#include` needed; `SUBSYS_PHASE_4` is defined in `<subsys/subsys.h>` but we're using inline externs to avoid pulling in `<arch/subsys.h>` transitively (R3-3 NIT unresolved — see Global Constraints).

- [ ] **Step 3: Build**

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang SMP=1 aarch64-uefi 2>&1 | tail -10
```

Expected: kernel.elf + aarch64-uefi.img produced; no errors. The two new calls compile and link.

- [ ] **Step 4: Verify QEMU 9/9 PASS still holds (Phase 1 GIC evidence gate)**

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
```

Expected: 9 case JSONs, all `"result":"PASS"`. The Option B fallback in main.c is still in place (Task 4 removes it), so `clocksource_init()` runs via two paths (explicit + framework) — benign double-init on clocksource but bad precedent. Task 3 flips the gate so only one path runs.

- [ ] **Step 5: Verify `[clocksource]` markers still appear before `[cntp]`**

```sh
cd /home/aagu/aarch64-timer-phase2
grep -n "\[clocksource\]\|\[cntp\]" \
    test-results/aarch64-uefi-smp/$(ls -t test-results/aarch64-uefi-smp/ | head -1)/cpus-1-run-1.stdout.log | head -10
```

Expected: 3 `[clocksource]` markers (lines 19-21), then `[cntp]` (line 22+). Order matters — `clocksource_init()` must run BEFORE the first CNTP tick fires.

- [ ] **Step 6: Commit**

```sh
git add kernel/arch/aarch64/main.c
git commit -m "feat(aarch64): hook arch_register_subsys + subsys_init_phase(SUBSYS_PHASE_4) in aarch64_main

- kernel/arch/aarch64/main.c: insert two consecutive calls
  between smp_boot_aps() and arch_tick_start():
    arch_register_subsys()       — runs queued SUBSYS_INITCALL _register
                                   wrappers, which call register_subsys()
                                   to populate subsys_table[]
    subsys_init_phase(SUBSYS_PHASE_4) — walks subsys_table[] and calls
                                        each registered init wrapper,
                                        including _clocksource_init_wrapper
                                        which invokes clocksource_init()

  R3-1 critical fix: on x86_64 (kernel/core/main.c:193-194) this
  is the consecutive register+dispatch pair. Without the second
  call, no SUBSYS_INITCALL registered initcall ever runs.

  Note: clocksource.c's SUBSYS_INITCALL gate is still
  #ifdef __x86_64__ at this point (Task 3 flips it). On aarch64
  today, arch_register_subsys() iterates an empty __subsys_init
  table (zero initcalls registered). subsys_init_phase(4) is
  therefore a no-op for now — but once Task 3 lands, this
  becomes the dispatch path that actually calls clocksource_init().

  At this commit, clocksource_init() still runs via the
  Option B fallback (lines 279-303); double-init is benign on
  clocksource but bad precedent. Task 4 removes the explicit
  call once Task 3's gate flip is in.

Verification:
- make PROFILE=aarch64-clang test-aarch64-uefi-smp: 9/9 PASS
- [clocksource] markers appear before [cntp] in stdout.log

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: Group 3a — flip `clocksource.c` SUBSYS_INITCALL gate to include aarch64

**Files:**
- Modify: `kernel/time/clocksource.c`

**Interfaces:**
- Modifies: `SUBSYS_INITCALL(_clocksource_register)` macro expansion. After this task, `_clocksource_register` is invoked by `arch_register_subsys()` on aarch64 (via `Task 1`'s `.subsys_init` section + `Task 2`'s iterator).

- [ ] **Step 1: Verify current `clocksource.c` line numbers**

```sh
grep -n "__x86_64__\|SUBSYS_INITCALL" /home/aagu/aarch64-timer-phase2/kernel/time/clocksource.c
```

Expected: shows line ~50 with `#ifdef __x86_64__` and line ~70 with `SUBSYS_INITCALL(_clocksource_register)`. If line numbers drift (e.g., future commits), re-verify before editing — this plan's line numbers assume the R5 spec baseline.

- [ ] **Step 2: Modify `kernel/time/clocksource.c` — flip the gate**

Open the file. Locate line ~50 (the `#ifdef __x86_64__` that wraps the `SUBSYS_INITCALL` at line ~70). Change:

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

- [ ] **Step 3: Build**

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang SMP=1 aarch64-uefi 2>&1 | tail -10
```

Expected: kernel.elf + aarch64-uefi.img produced; no errors. (The `-I libc/include` for aarch64 kernel build, added in commit `12d3720`, ensures `<list.h>` transitive include via `<time/timer.h>` resolves correctly — see Global Constraints.)

- [ ] **Step 4: Verify linker now produces 3 `subsys_init` symbols (start, end, _clocksource_register)**

```sh
cd /home/aagu/aarch64-timer-phase2
nm build/aarch64-clang/image/kernel.elf | grep -c subsys_init
```

Expected: `3` (the two `PROVIDE` symbols plus `_clocksource_register` function pointer). This is the R3-1 canonical detection: before Task 3 the count is 2 (no registered initcalls); after Task 3 the count is 3.

- [ ] **Step 5: Verify QEMU 9/9 PASS (Phase 1 GIC evidence gate preserved)**

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
```

Expected: 9 case JSONs, all `"result":"PASS"`. The harness `--expect-clk` evidence gate still sees 3 `[clocksource]` markers per case. At this commit, `clocksource_init()` runs via the dispatch path (Task 2's `subsys_init_phase(SUBSYS_PHASE_4)` triggers `_clocksource_init_wrapper` → `clocksource_init()`). The Option B fallback in main.c also runs (Task 4 will remove it) — same double-init state as Task 2's Step 4.

- [ ] **Step 6: Commit**

```sh
git add kernel/time/clocksource.c
git commit -m "feat(aarch64): flip clocksource.c SUBSYS_INITCALL gate for aarch64

- kernel/time/clocksource.c: change #ifdef __x86_64__ to
  #if defined(__x86_64__) || defined(__aarch64__) wrapping
  SUBSYS_INITCALL(_clocksource_register). After this commit,
  _clocksource_register is in the aarch64 .subsys_init section.

This is the register half of the x86_64 register+dispatch
pair (kernel/core/main.c:193-194). The dispatch half
(subsys_init_phase(SUBSYS_PHASE_4)) was added in commit
<Task 2's hash>.

Verification:
- nm | grep -c subsys_init: 3 (was 2 before this commit)
- test-aarch64-uefi-smp: 9/9 PASS
- --expect-clk: 3 [clocksource] markers per case

Note: clocksource_init() now runs twice (once via dispatch, once
via Option B fallback in main.c). Double-init is benign on
clocksource but sets a bad precedent; Task 4 removes the
explicit call.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: Group 3b — remove redundant Option B `clocksource_init()` call from `aarch64_main`

**Files:**
- Modify: `kernel/arch/aarch64/main.c`

**Interfaces:**
- Removes: the `clocksource_init()` call inside the `#if defined(__aarch64__)` block (line ~291) — now redundant since the framework dispatches it.
- Removes: the `extern void clocksource_init(void);` forward declaration (line ~33) — becomes dead after the call removal.

- [ ] **Step 1: RED — verify pre-state**

Run QEMU on the post-Task 3 binary. Expected: 9/9 PASS still; `[clocksource]` markers appear; `clocksource_init()` runs twice (dispatch + Option B). Save this as Task 4's RED evidence:

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
grep -n "\[clocksource\]\|\[cntp\]" \
    test-results/aarch64-uefi-smp/$(ls -t test-results/aarch64-uefi-smp/ | head -1)/cpus-1-run-1.stdout.log | head -10
```

Expected: 9 PASS + 3 `[clocksource]` markers + `[cntp]` marker after them.

- [ ] **Step 2: Modify `kernel/arch/aarch64/main.c` — remove the explicit `clocksource_init()` call**

Open the file. Locate the Option B block (lines 279-303 per R1 fix §8). The block contains both the explicit `clocksource_init()` call (line ~291) AND the 3 `[clocksource]` marker prints.

Change the `#if defined(__aarch64__)` block to remove ONLY the `clocksource_init()` call. Keep the marker prints + the 4 forward decls (`clocksource_active`, `clocksource_freq_hz`, `clocksource_mult`, `clocksource_shift`).

Before (lines 148-166 per R5 spec):

```c
#if defined(__aarch64__)
    clocksource_init();
#endif
    {
        extern bool clocksource_active;
        extern uint64_t clocksource_freq_hz(void);
        extern uint32_t clocksource_mult;
        extern uint32_t clocksource_shift;
        if (clocksource_active) {
            kputs("[clocksource] active=true\n");
            ...
        }
    }
```

After:

```c
    /* Clocksource / timer / softirq are registered via SUBSYS_INITCALL
     * and dispatched by arch_register_subsys() + subsys_init_phase(SUBSYS_PHASE_4)
     * (called between smp_boot_aps and arch_tick_start). The 3 [clocksource]
     * markers below remain because qemutests/aarch64_uefi_smp.py --expect-clk
     * requires them as evidence. */
    {
        extern bool clocksource_active;
        extern uint64_t clocksource_freq_hz(void);
        extern uint32_t clocksource_mult;
        extern uint32_t clocksource_shift;
        if (clocksource_active) {
            kputs("[clocksource] active=true\n");
            ...
        }
    }
```

(The exact diff: delete the `#if defined(__aarch64__)`, the `clocksource_init();` line, and the matching `#endif`. Add a comment above the block explaining the new dispatch path.)

- [ ] **Step 3: Remove the now-dead `clocksource_init` forward declaration**

Open the file. Locate line 33 (`extern void clocksource_init(void);`). Delete this line.

Verify it was the only call site for `clocksource_init` — grep:

```sh
grep -n "clocksource_init" /home/aagu/aarch64-timer-phase2/kernel/arch/aarch64/main.c
```

Expected: only the comment (e.g. line 169 "no longer calls clocksource_init() directly"), no remaining call or forward decl.

- [ ] **Step 4: Build**

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang SMP=1 aarch64-uefi 2>&1 | tail -10
```

Expected: kernel.elf + aarch64-uefi.img produced; no errors.

- [ ] **Step 5: GREEN — verify QEMU 9/9 PASS with `[clocksource]` markers (single dispatch path now)**

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
```

Expected: 9 case JSONs, all `"result":"PASS"`. This is the GREEN evidence — Task 4 completes the framework unification; no double-init.

- [ ] **Step 6: Verify the same `[clocksource]` markers appear**

```sh
cd /home/aagu/aarch64-timer-phase2
grep -n "\[clocksource\]\|\[cntp\]" \
    test-results/aarch64-uefi-smp/$(ls -t test-results/aarch64-uefi-smp/ | head -1)/cpus-1-run-1.stdout.log | head -10
```

Expected: 3 `[clocksource]` markers (lines 19-21) and `[cntp]` marker after them. If fewer than 3 `[clocksource]` markers, `clocksource_active` was false at the point of marker emission — meaning the dispatch path didn't run `clocksource_init()`. **This would indicate R3-1 is not actually fixed**; re-verify Task 2's hook is present (the `subsys_init_phase(SUBSYS_PHASE_4)` call). DO NOT commit if markers are missing.

- [ ] **Step 7: Full regression — hosttests + QEMU E2E + build contracts**

```sh
cd /home/aagu/aarch64-timer-phase2
for t in test_gic_driver test_gic_probe test_gic_marker_lines test_clocksource test_lwip_rand; do
    echo "--- $t ---"
    (cd hosttests && make PROFILE=aarch64-clang \
        OS01_PROFILE_FILE=$PWD/../mk/profiles/aarch64-clang.mk $t) 2>&1 | tail -2
done
```

Expected: 5 hosttests all pass (69/69, 5/5, 4/4, 49/49, ALL PASSED).

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
make PROFILE=aarch64-clang test-aarch64-gic-spi 2>&1 | tail -5
make PROFILE=x86_64-clang test-user-canary 2>&1 | tail -5
make PROFILE=x86_64-clang test-kernel-selftest 2>&1 | tail -5
```

Expected: 9/9 QEMU aarch64 PASS; SPI PASS; x86_64 canary PASS; x86_64 kernel selftest 27/27 PASS.

```sh
cd /home/aagu/aarch64-timer-phase2
make PROFILE=aarch64-clang test-build-contract-aarch64 2>&1 | tail -5
make PROFILE=x86_64-clang test-build-contract-x86 2>&1 | tail -5
```

Expected: both build contracts pass; aarch64 binary may grow ≤1 KiB; x86_64 byte-identical.

- [ ] **Step 8: Commit**

```sh
git add kernel/arch/aarch64/main.c
git commit -m "refactor(aarch64): remove redundant clocksource_init() Option B call

- kernel/arch/aarch64/main.c: remove the explicit clocksource_init()
  call inside the #if defined(__aarch64__) block. clocksource_init()
  is now invoked exclusively via the dispatch path:
    1. arch_register_subsys() (in Task 2's hook) runs
       _clocksource_register which calls register_subsys() to
       populate subsys_table[]
    2. subsys_init_phase(SUBSYS_PHASE_4) (also in Task 2's hook)
       walks subsys_table[] and calls _clocksource_init_wrapper
       which calls clocksource_init()
  The explicit call is now redundant.

  Also remove the dead 'extern void clocksource_init(void);'
  forward declaration (line 33).

  KEEP the 4 other forward decls (clocksource_active,
  clocksource_freq_hz, clocksource_mult, clocksource_shift) and
  the 3 [clocksource] marker prints — the harness
  qemutests/aarch64_uefi_smp.py --expect-clk requires them.

After this commit, aarch64 SUBSYS_INITCALL machinery is
fully wired: linker section + iterator + Makefile whitelist
(Task 1) + main.c hook (Task 2) + gate flip (Task 3) +
Option B removal (this commit).

Full regression:
- 5 hosttests (test_gic_driver/probe/marker_lines +
  test_clocksource + test_lwip_rand): all pass
- test-aarch64-uefi-smp: 9/9 PASS
- test-aarch64-gic-spi: PASS
- x86_64 test-user-canary / test-kernel-selftest / build
  contracts: all pass byte-for-byte (x86_64 untouched)

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage** — every spec requirement maps to a task:
- §A linker script → Task 1 Step 3 ✓
- §B subsys.c (mirror minus RSDP) → Task 1 Step 2 ✓
- §C Makefile whitelist → Task 1 Step 4 ✓
- §D flip gate (line 50, SUBSYS_INITCALL line 70) → Task 3 Step 2 ✓
- §E trim Option B (remove ONLY `clocksource_init()` line; keep markers + 4 of 5 forward decls) → Task 4 Steps 2-3 ✓
- §F hook `arch_register_subsys()` + `subsys_init_phase(SUBSYS_PHASE_4)` → Task 2 Step 2 ✓
- §Risks empty-loop case mitigation (nm -c ≥ 3) → Task 3 Step 4 ✓
- §Risks missing phase-init mitigation (QEMU E2E regression detection) → Task 4 Step 6 ✓
- §Verification matrix (hosttests + QEMU E2E + build contracts) → Task 4 Step 7 ✓
- §Commit ordering (Group 1 → Group 2 → Group 3a → Group 3b) → enforced by task numbering + Step 7 commit message cross-references ✓

**2. Placeholder scan** — no "TBD", "TODO", "implement later", "similar to Task N". All step instructions are concrete.

**3. Type consistency** —
- `arch_register_subsys(void)` declared in spec §B body, defined in Task 1 Step 2, called in Task 2 Step 2. Identical signature across all references.
- `subsys_init_phase(int phase)` declared in `kernel/subsys/subsys.c`, called in Task 2 Step 2 as `subsys_init_phase(SUBSYS_PHASE_4)`. Identical.
- `SUBSYS_PHASE_4` defined in `kernel/include/subsys/subsys.h`, used in Task 2 Step 2 + Task 3 Step 6 commit message. Identical.
- `clocksource_active` / `clocksource_freq_hz` / `clocksource_mult` / `clocksource_shift` / `clocksource_init` referenced identically across spec §B, §E, §F, and Tasks 2/4.
- `nm | grep __subsys_init` expected output identical in Task 1 Step 6 + Task 3 Step 4.

**4. Risks NOT covered by plan tasks**:
- GIC `gic_init()` interaction (R3 mitigation: explicit call at main.c:270 is BEFORE Group 2 hook, no conflict) — verification lives in Task 4 Step 7's full regression (test-aarch64-gic-spi + test-aarch64-uefi-smp 9/9 cover this).
- Test harness `[clocksource]` regex mismatch with marker format — Task 4 Step 6's grep verification catches this.
- Binary layout shift breaking kernel-image walk in pmm.c — Task 4 Step 7's `make test-build-contract-aarch64` catches this.

**5. Out-of-scope items documented in spec as Non-goals** (re-verified here):
- `cntp_tick_handler` → `tick_handler()` integration (item #2)
- Per-CPU timer / SMP timer (item #3)
- `__udivti3` hoist (item #4)
- `-I libc/include` policy cleanup (item #5 — already in place, no change)
- Other framework files' SUBSYS_INITCALL gates (only `timer.c:136` exists; gated today; flip is a future P2 follow-up)
- `arch/aarch64/subsys.h` header (uses inline externs; R5 NIT unresolved)

Self-review: PASS. Ready for execution.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-09-18-aarch64-subsys-init-plan.md`.

Two execution options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** — execute tasks in this session using `executing-plans`, batch execution with checkpoints

Per project convention (Phase 1 GIC used subagent-driven) and user direction (`subagent-driven 模式`), recommend option 1.