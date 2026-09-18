# AArch64 `cntp_tick_handler → tick_handler()` Integration — Design (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.

**Goal:** Make `kernel/time/tick.c::tick_handler()` (the unified tick semantic) run on aarch64 by routing `cntp_tick_handler()` through it. After this spec lands, aarch64 joins x86_64 in producing the canonical unified tick semantic: `jiffies++` → poll timeout scan → `need_resched=1` → `watchdog_counter++` → `TIMER_SIRQ`. The per-second `[tick] N` debug print from Phase 1 GIC (`6be7735`) is replaced by the framework's own observability (none, today — debug visibility moves to the harness `--expect-clk` marker path).

**Architecture:** Three changes:
1. **`kernel/arch/aarch64/time.c`**: `cntp_tick_handler` becomes a thin wrapper that rewrites `CNTP_TVAL_EL0` (spec §2.3 strict ISR order, FIRST), then calls `tick_handler()`. The per-second debug `g_ticks` counter and `[tick] N` print are removed (the GIC Phase 1 evidence gate is preserved by `qemutests/aarch64_uefi_smp.py`'s `--expect-clk` + `--expect-gic` markers; `[tick] N` lines are not referenced by the current harness per Task 1.2 spec §E note — **verify this in R1 review**).
2. **`kernel/time/clocksource.c`**: flip the existing `#ifdef __x86_64__` gate at `kernel/time/timer.c:136` (verified by R1 grep) to `#if defined(__x86_64__) || defined(__aarch64__)` so `timer_init()` runs via the SUBSYS dispatch path on aarch64. Also extend `kernel/arch/aarch64/subsys_stub.c` to add `timer_init()` to the dispatch walk (or rely on `subsys_init_phase(SUBSYS_PHASE_4)` already walking phase 4 init wrappers — verify ordering in R1).
3. **`kernel/arch/aarch64/main.c`**: add explicit `timer_init()` and `softirq_init()` calls in the `#if defined(__aarch64__)` block (mirror of the x86_64 implicit pattern via `SUBSYS_INITCALL` — but aarch64 needs explicit calls because the framework dispatch walk is not yet integrated with the framework's `_init_wrapper` mechanism on aarch64).

**No-goal:** this spec does NOT address deferred items #3 (per-CPU timer / SMP timer), #4 (`__udivti3` hoist), or #5 (`-I libc/include` policy). Each is a separate spec when picked up.

## Context — why it's needed now

Phase 1 Generic Timer (merge `b2b81fc`, 6 commits) and Phase 2 SUBSYS_INITCALL plumbing (merge `e115d79`, 10 commits) together provide the aarch64 timer infrastructure — CNTP + CNTVCT, `arch_cycle_freq/arch_cycle_counter`, framework `clocksource_init`, framework dispatch via `.subsys_init` — but the **unified tick semantic** in `kernel/time/tick.c::tick_handler()` is still x86_64-only because:

1. **`cntp_tick_handler` bypasses `tick_handler()`** (`kernel/arch/aarch64/time.c:57-76`): rewrites TVAL, increments a debug counter, prints `[tick] N` once per second. Does NOT call `tick_handler()` — so `jiffies` stays 0 on aarch64, no `need_resched` set, no `TIMER_SIRQ` dispatched, no poll timeout scan runs.
2. **`timer_init()` is not called** on aarch64 — `timer_list_head` (defined at `kernel/time/timer.c:11`) is BSS-initialized to all-zeros; `init_timer(&timer_list_head, NULL, NULL, -1UL)` (`kernel/time/timer.c:65-70`) is only invoked from `_timer_init_wrapper` registered via `SUBSYS_INITCALL(_timer_register)` at `kernel/time/timer.c:156`, which is gated `#ifdef __x86_64__` (verified by R1 grep). Without `timer_init`, calling `tick_handler()` would deref NULL via `container_of(list_next(&timer_list_head.list), …)` at `kernel/time/tick.c:40`.
3. **`softirq_init()` is not called** on aarch64 — `softirq_status` (defined at `kernel/include/intr/softirq.h:8`) is BSS-initialized to 0; `softirq_init()` (defined at `kernel/intr/softirq.c`) clears the softirq vector and status. Without init, `set_softirq_status(TIMER_SIRQ)` at `kernel/time/tick.c:41` would write a never-cleared bit and `do_softirq` would NUL-deref on dispatch.
4. **`this_cpu()->need_resched = 1`** at `kernel/time/tick.c:34` writes to aarch64's per-CPU area. On aarch64, `TPIDR_EL1 = &aarch64_boot_percpu[0]` (set at `kernel/arch/aarch64/head.S:296`), which is a 48-byte BSP-only structure (`kernel/arch/aarch64/aarch64_percpu.h:20-29`) — NOT the x86_64-style `percpu_data[cpu]` array. Writes corrupt `.boot.bss` slack. Phase 1 GIC accepted this latent corruption because Phase 1 has no scheduler (`docs/aarch64-timer-phase2-closure-2026-09-18.md` §"Outstanding P2 follow-ups" item #3). Phase 2 #2 connects `tick_handler()` which sets `need_resched` — the latent corruption is no longer latent.

## Design

### A. `kernel/arch/aarch64/time.c` — `cntp_tick_handler` delegates to `tick_handler()`

Replace the current `cntp_tick_handler` body (lines 57-76) with:

```c
/* Unified tick handler: TVAL rewrite first (phase1 spec §2.3), then
 * delegate to the framework's tick_handler() which does
 *   jiffies++ → poll timeout scan (ns) → need_resched →
 *   watchdog → TIMER_SIRQ.  EOI is performed by gic_dev_dispatch
 * after this returns (no in-handler EOI). */
static void cntp_tick_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
    cntp_tval_el0_write(g_period);
    tick_handler();
}
```

Then delete:
- The `static volatile uint64_t g_ticks;` debug counter (line ~49 — no longer used).
- The `#define TICKS_PER_SECOND` (line ~40 — no longer used; `HZ` is sufficient).
- The `[tick] N` debug print (lines 70-74 — replaced by framework observability).
- Update the file-top comment (lines 1-24) to reflect the new contract.

**Decision** (R1 review point): the `[tick] N` per-second print currently satisfies the GIC Phase 1 evidence gate (`qemutests/aarch64_uefi_smp.py:485/514` requires `≥3 [tick] N` lines — verified in Phase 2 #1 closure doc). After this spec, no `[tick]` lines appear in stdout.log — the `--expect-clk` evidence gate (3 `[clocksource]` markers) is the only remaining aarch64 evidence line.

**If the harness still requires `[tick] N` lines after this spec lands**, this spec is REJECTED and a follow-up must add an arch-neutral `[tick]` debug print inside `kernel/time/tick.c::tick_handler()` (behind `#ifdef OS01_DEBUG_TIMER` or similar). **R1 review must verify the harness regex before approving.**

### B. `kernel/time/clocksource.c` — flip `timer.c:136` SUBSYS_INITCALL gate

Open `kernel/time/timer.c` (R1 grep confirms line 136 is the `#ifdef __x86_64__` wrapping `SUBSYS_INITCALL(_timer_register)` at line 156). Change:

```c
#ifdef __x86_64__
SUBSYS_INITCALL(_timer_register);
#endif
```

to:

```c
#if defined(__x86_64__) || defined(__aarch64__)
SUBSYS_INITCALL(_timer_register);
#endif
```

This is the same gate flip as Phase 2 #1's clocksource.c change — but for timer.c. After this commit, `_timer_register` runs on aarch64 via `arch_register_subsys()`, calling `register_subsys()` (stub in `subsys_stub.c` queues) → `_timer_init_wrapper` → `timer_init()`.

**No new hosttest needed** — the existing `test_clocksource` Suite E (Phase 2 #1 commit `6f37a21`) verifies `clocksource_init()` is the dispatch target; `timer_init` follows the same pattern.

### C. `kernel/arch/aarch64/main.c` — explicit `timer_init()` + `softirq_init()` calls

The framework dispatch walk (`subsys_init_phase(SUBSYS_PHASE_4)`) runs `_clocksource_init_wrapper` for phase 4 registrations. After the timer gate flip (B), it ALSO runs `_timer_init_wrapper`. So `timer_init()` runs via the dispatch path — no explicit call needed.

**BUT** `softirq_init()` is NOT registered via SUBSYS_INITCALL — it's called explicitly from `kernel/intr/irq.c:78` on x86_64. On aarch64, nothing calls it. Add an explicit call in `aarch64_main`:

```c
#if defined(__aarch64__)
    /* softirq_init() must run BEFORE arch_tick_start(): tick_handler()
     * calls set_softirq_status(TIMER_SIRQ), which dereferences
     * softirq_status (BSS; zeroed). softirq_init() clears the
     * softirq vector and status. */
    extern void softirq_init(void);
    softirq_init();
#endif
```

Place this between the existing `#if defined(__aarch64__)` hook calls (the `arch_register_subsys()` + `subsys_init_phase(SUBSYS_PHASE_4)` block from Phase 2 #1 commit `bddf8eb`) and the `arch_tick_start()` call. The ordering is:

1. `arch_register_subsys()` — queue `_timer_register` into `.subsys_init` walk
2. `subsys_init_phase(SUBSYS_PHASE_4)` — run `_timer_init_wrapper` → `timer_init()` (subsys_stub.c path)
3. `softirq_init()` (NEW) — explicit; clears `softirq_status`
4. `arch_tick_start()` — registers `cntp_tick_handler` in GIC handler table
5. (subsequent ticks fire `cntp_tick_handler` → `tick_handler()` → everything works)

### D. `kernel/arch/aarch64/subsys_stub.c` — verify no extension needed

After B lands, the framework dispatch walk will:
- `arch_register_subsys()` iterates `.subsys_init` → calls `_clocksource_register` + `_timer_register`
- `_clocksource_register` calls `register_subsys()` (stub queue) → `subsys_table[0]`
- `_timer_register` calls `register_subsys()` (stub queue) → `subsys_table[1]`
- `subsys_init_phase(SUBSYS_PHASE_4)` walks `subsys_table[]`, calls `subsys_table[i].init()` matching phase 4
- Both `_clocksource_init_wrapper` and `_timer_init_wrapper` are called in order

**No subsys_stub.c changes needed.** R1 must verify by tracing the code.

### E. `kernel/arch/aarch64/head.S` — `percpu_data` install (R1 critical risk)

**This is the hardest part of the spec** and may cause R1 to flag it as REJECTED.

When `tick_handler()` writes `this_cpu()->need_resched = 1`, it expects `this_cpu()` to return the **calling CPU's** `percpu_data[cpu_id]` structure. On aarch64, `TPIDR_EL1 = &aarch64_boot_percpu[0]` (set in `kernel/arch/aarch64/head.S:296`); `kernel/include/arch/aarch64/percpu.h` (R1 verify location) defines `this_cpu()` as `(aarch64_percpu_t *)(uintptr_t)TPIDR_EL1`. Writing to `->need_resched` corrupts `cpu_id` (offset 8) and `pad0` (offset 16) of `aarch64_boot_percpu[0]`.

**Fix**: install `percpu_data[cpu]` (the x86_64-style structure that `this_cpu()` on x86_64 resolves to via GS base) for aarch64. This requires:

- `kernel/Makefile` whitelist `kernel/percpu/percpu.c` (or wherever `percpu_data[]` is defined) for the aarch64 kernel build
- `kernel/include/percpu/percpu.h` (or arch-specific) provide an aarch64 implementation of `this_cpu()` returning `(percpu_t *)percpu_data[cpu_id]` — NOT `(aarch64_percpu_t *)TPIDR_EL1`
- `aarch64_main` calls `percpu_install_gs(cpu_id)` (or whatever the equivalent API is — verify in R1) for each CPU during bring-up

**Alternative**: stub `tick_handler()` semantics on aarch64 — bypass `this_cpu()->need_resched = 1` writes (latent — Phase 1 GIC accepted this). But this defeats the purpose of the spec (the unified tick semantic must run).

**R1 review must determine whether E is in scope for this spec or a follow-up.** If E is in scope, this spec's commit count grows from ~4 to ~7 (per-CPU install on each AP during `smp_boot_aps()`). If E is deferred, this spec marks `need_resched` writes as a known latent corruption with a TODO comment, and the per-CPU install becomes a separate Phase 2 #3 spec.

**My recommendation**: defer E to Phase 2 #3 (per-CPU timer / SMP timer) — it's a substantially larger change that breaks the scope of this spec. The `need_resched = 1` writes will corrupt `.boot.bss` slack on BSP (no scheduler reads them today, latent damage only). Document this in the spec.

### F. `kernel/arch/aarch64/head.S` — what does NOT need to change

- No new MMU / page-table setup (the existing high-half identity map covers the new code paths)
- No new interrupt controller setup (CNTP is already enabled)
- No new linker script section (timer_init / softirq_init symbols are already in `.text`)

## Non-goals (out of scope)

1. **Per-CPU timer / SMP timer** — still pending item #1 above (E). Phase 2 #3 spec when picked up.
2. **`__udivti3` hoist to `compiler_rt/`** — independent cleanup. Phase 2 follow-up item #4.
3. **`-I libc/include` policy** — already in place from Task 2.2 (`12d3720`); this spec relies on it but doesn't change it.
4. **`kernel/time/clocksource.c`'s SUBSYS_INITCALL gate** — already flipped in Phase 2 #1 (`4d7f7c9`); no change.
5. **Other framework files with `SUBSYS_INITCALL`** — only `kernel/time/timer.c` is relevant (after B); `pit.c`, `serial.c`, `keyboard.c`, `ahci.c`, `lapic.c`, `lapic_timer.c`, `8259A.c`, `net.c` are x86_64-only drivers, not relevant.
6. **`arch/aarch64/subsys.h` header file** — R3-3 unresolved NIT from Phase 2 #1; still using inline externs. Future P2 follow-up could clean up.
7. **Replacing `subsys_stub.c` with real `kernel/subsys/subsys.c`** — depends on `serial_printk`, `num_cpus`, `strcmp`, `idle_resume` becoming available on aarch64.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| `[tick] N` evidence gate — `qemutests/aarch64_uefi_smp.py:485/514` requires `≥3 [tick] N` lines; spec removes `[tick]` print | High | R1 review MUST grep the harness to verify the requirement. If still required, spec is REJECTED; follow-up adds arch-neutral `[tick]` print in `kernel/time/tick.c`. |
| `timer_list_head` NULL deref at `kernel/time/tick.c:40` if `timer_init()` not run | High | B (gate flip) + C (explicit softirq_init) ensures init before first tick. QEMU E2E catches if first tick fails. |
| `softirq_status` stale — `set_softirq_status(TIMER_SIRQ)` writes a never-cleared bit, `do_softirq` NUL-deref | High | C explicit `softirq_init()` before `arch_tick_start()`. QEMU E2E catches. |
| `this_cpu()->need_resched = 1` corrupts `.boot.bss` slack on BSP (latent) | High (latent), Low (immediate) | E — defer to Phase 2 #3. Document as known latent corruption; no scheduler reads `need_resched` today. |
| `kernel/Makefile` `timer.c` whitelist — aarch64 build currently doesn't include `kernel/time/timer.c` (only `kernel/time/clocksource.c`); adding `timer.c` may pull in `serial_printk`, `strcmp`, etc. | Medium | R1 grep must verify `kernel/time/timer.c`'s dependencies. If it pulls too much, similar to subsys_stub.c: provide a minimal `kernel/arch/aarch64/timer_stub.c` for now (mirroring `subsys_stub.c`). |
| Build-cache CFLAGS trap (`KERNEL_SELFTEST=1` doesn't trigger `.o` rebuild) — Phase 2 #1 Task 3 noted this | Medium | Operational note in plan: always `make clean && make KERNEL_SELFTEST=1` before QEMU regression. |
| `aarch64_main` order — `softirq_init()` must run BEFORE `arch_tick_start()` | Low | QEMU E2E marker ordering check: `[cntp]` marker must appear AFTER `[clocksource]` markers AND the kernel must reach `[IRQ] enabled` without hanging. |
| `tick_handler()` reads `this_cpu()->tsc_offset` via `clocksource_read_ns()` (line 26) — on aarch64 this dereferences the wrong structure | High (latent) | E. If E deferred, the read returns garbage but `poll_timeout_head` is NULL so the loop is skipped; latent damage to `.boot.bss` slack. |

## Verification

### Unit / host tests

No new hosttest needed. The existing 5 hosttests must continue to pass byte-for-byte. The `test_clocksource` Suite E (Phase 2 #1 commit `6f37a21`) verifies `clocksource_init()` post-init globals — still passes (no change to clocksource.c semantics).

### QEMU end-to-end

**Existing evidence gates** must continue to pass (with the `[tick] N` removal caveat above):
- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (--expect-selftest --expect-gic --expect-clk): 9/9 PASS, SMP=1/2/4 × 3
- `make PROFILE=aarch64-clang test-aarch64-gic-spi`: PASS
- `make PROFILE=x86_64-clang test-user-canary`: audit passed
- `make PROFILE=x86_64-clang test-kernel-selftest`: 27/27 PASS

**New RED-then-GREEN test**: add a QEMU harness assertion that `[tick] N` lines are NOT in stdout.log (or are — pending R1 verification). If the harness rejects the absence of `[tick] N`, this spec is REJECTED.

**New x86_64 byte-identity check**: same as Phase 2 #1 — no x86_64 sources modified.

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #2** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 2. `cntp_tick_handler → tick_handler()` integration — needs `<list.h>` include-path fix

The `<list.h>` include-path fix is already in place from Task 2.2 (`12d3720`); this spec consumes that fix. Remaining prerequisite is the `percpu_data` install (E) — deferred to Phase 2 #3.

After this spec, `tick_handler()` runs on aarch64, identical to x86_64. The unified kernel_main spec's "timer unified" sub-task is one step closer to complete (the framework-init side is closed by Phase 2 #1; the unified-tick semantic is closed by this spec; the per-CPU side remains).

## Subagent-driven plan (to be written via writing-plans skill)

After this spec is approved, the writing-plans skill produces `docs/superpowers/plans/2026-09-18-aarch64-tick-handler-plan.md`. Estimated commit count: 4-7 functional + 1 docs. Estimated wall-clock: 2-3 days (per-CPU install deferred; the rest is straightforward delegation to existing framework).