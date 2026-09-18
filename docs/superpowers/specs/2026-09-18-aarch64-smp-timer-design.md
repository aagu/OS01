# AArch64 Per-CPU Timer / SMP Timer — Design (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.

**Goal:** Close Phase 2 follow-up item #3 from `docs/aarch64-timer-phase1-closure-2026-09-18.md` — install real per-CPU data on aarch64 (`percpu_data[cpu]` indexed by TPIDR_EL1-injected CPU ID), enable CNTP on application processors (currently disabled by Phase 1 GIC `kernel/arch/aarch64/smp.c:223`), register per-CPU timer init via `SUBSYS_INITCALL` phase 4 walk. After this spec lands, aarch64 multi-core tick semantic is correct (no more latent `.boot.bss` corruption on `need_resched=1` writes), AP CNTP fires, and `tick_handler()` runs on each CPU.

**Architecture — 4 components:**

1. **`percpu_data[cpu]` array install** — define aarch64-aligned `percpu_data[]` array with `__attribute__((section(".data..percpu")))` or BSS allocation; modify `kernel/include/arch/aarch64/percpu.h` `this_cpu()` implementation to read `TPIDR_EL1` (which is set by `head.S` per-CPU bring-up to point at `&percpu_data[cpu_id]`) and return `(percpu_t *)TPIDR_EL1`. Field layout matches `kernel/include/percpu/percpu.h:25-50` (self at offset 0, need_resched at offset 8).
2. **AP CNTP enable** — remove the `cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1))` mask at `kernel/arch/aarch64/smp.c:223`. AP CNTP becomes enabled; AP receives CNTP PPI interrupts on each tick.
3. **Per-CPU timer dispatch** — extend `kernel/arch/aarch64/subsys_stub.c::subsys_init_phase(int phase)` (or add new dispatch in main.c) to also walk `subsys_percpu_table[]` (Phase 4 already exists per `kernel/include/subsys/subsys.h:32-40`; the framework has `_percpu_register` slot ready). Register a per-CPU timer init that runs `timer_init()` on each AP after `smp_boot_aps()`.
4. **per-CPU init_task array expansion** — currently `kernel/include/sched/task.h:241` has `init_task[NR_CPUS] = {&init_task_union.task, 0};` (only BSP). Expand to all-NR_CPUS entries via `__attribute__((__section__(".data.init_task")))` per-CPU copies; update `init_thread.rip = idle_resume` per-CPU entry. Phase 2 #3 makes `idle_resume_stub.c` provide per-CPU variants.

## Context — why it's needed now

Phase 2 #2 `cntp_tick_handler → tick_handler()` integration (commit `10fd3d2`) wired the framework, but three latent corruptions remain (closure report §"Latent corruption acknowledged"):

1. **`tick_handler()` writes `this_cpu()->need_resched = 1`** at `kernel/time/tick.c:34` (x86_64 `#if`-gated path removed on aarch64, so the write fires on every tick). On aarch64, `this_cpu()` resolves via `TPIDR_EL1` (set in `kernel/arch/aarch64/head.S:296` to `&aarch64_boot_percpu[0]`); `aarch64_boot_percpu[0]` is a 48-byte BSP-only structure (`kernel/arch/aarch64/aarch64_percpu.h:20-29`) with NO `need_resched` field. Writes corrupt `cpu_id` (offset 8 of the BSP struct) and adjacent slack. Latent damage — no scheduler reads `need_resched` today (closure §"Outstanding P2 follow-ups" #3).

2. **`tick_handler()` writes `this_cpu()->watchdog_counter++`** at `kernel/time/tick.c:35`. Same `this_cpu()` mismatch; writes out of bounds of the 48-byte struct. Latent.

3. **`tick_handler()` writes `set_softirq_status(TIMER_SIRQ)`** at `kernel/time/tick.c:41`. `softirq_status` is BSS-initialized to 0; bit stays set indefinitely (no `do_softirq` invocation on aarch64 — `kernel/arch/x86_64/entry.S:78` is the only caller). Latent.

Phase 2 #3 closes these by installing real `percpu_data[]` and per-CPU init paths so `this_cpu()` returns a real per-CPU struct with `need_resched` + `watchdog_counter` fields, and AP CNTP fires so the latent corruption is no longer latent.

Additionally:
- `kernel/arch/aarch64/smp.c:223` explicitly disables AP CNTP via `cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1))`. This was Phase 1 GIC's choice to simplify (only BSP CNTP fires). Phase 2 #3 reverses this so APs participate in the tick.
- `kernel/include/sched/task.h:241` defines `init_task[NR_CPUS] = {&init_task_union.task, 0}` — only BSP entry; APs have no `init_task`. After Phase 2 #3, each AP needs its own idle thread.

## Design

### A. `percpu_data[cpu]` install on aarch64

**Files:**
- Modify: `kernel/arch/aarch64/percpu.c` (NEW, ~30 lines) — define `percpu_data[NR_CPUS]` array, BSS-allocated, with proper alignment.
- Modify: `kernel/include/arch/aarch64/percpu.h` — `this_cpu()` reads `TPIDR_EL1` (already does), but `kernel/arch/aarch64/head.S:296` must set TPIDR_EL1 to `&percpu_data[cpu_id]` instead of `&aarch64_boot_percpu[0]`.

Implementation:

```c
// kernel/arch/aarch64/percpu.c
#include <percpu/percpu.h>

/* Per-CPU data array, BSS-allocated. On BSP, head.S:296 sets
 * TPIDR_EL1 = &percpu_data[0]. On APs, head.S:650 sets
 * TPIDR_EL1 = &percpu_data[cpu_id]. */
percpu_t percpu_data[NR_CPUS] __attribute__((aligned(64)));
```

Modify `head.S:296` (BSP) and `head.S:650` (AP):
- BSP: `msr tpidr_el1, x9` where `x9 = &percpu_data[0]`
- AP: `msr tpidr_el1, x9` where `x9 = &percpu_data[cpu_id]`

This requires passing `&percpu_data[cpu_id]` to the AP via the per-CPU slot mechanism (`aarch64_boot_percpu[cpu_id].tpidr_el1`).

### B. AP CNTP enable

**Files:**
- Modify: `kernel/arch/aarch64/smp.c:223` — remove the CNTP mask

Change:

```c
cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));
```

to:

```c
/* Phase 2 #3: AP CNTP enabled. Each AP receives its own CNTP
 * PPI interrupt on each tick. tick_handler() runs per-CPU
 * (per-CPU jiffies counters are future work — current
 * jiffies is a single global; Phase 2 #4 if needed). */
```

(or just remove the line entirely)

Also remove the duplicate CNTP-mask line at `smp.c:230` (the same mask repeated after `smp_bench_iter`).

### C. Per-CPU timer dispatch

**Files:**
- Modify: `kernel/arch/aarch64/subsys_stub.c` — add `subsys_init_percpu()` walk (already in scope per `subsys.h:45`)
- Modify: `kernel/arch/aarch64/main.c` — call `subsys_init_percpu()` after `smp_boot_aps()` (mirrors x86_64 `kernel/core/main.c:325-326`)

The framework already has `subsys_percpu_table[MAX_SUBSYS_PERCPU]` (subsys_stub.c:48). Add a registration call:

```c
// In aarch64_main.c, after smp_boot_aps() + before arch_tick_start():
#if defined(__aarch64__)
    extern void subsys_init_percpu(void);
    subsys_init_percpu();  /* Run per-CPU timer init on each AP */
#endif
```

`subsys_init_percpu()` walks `subsys_percpu_table[]`, calling each `init_percpu(cpu_id)`. We register a stub `init_percpu` that no-ops (timer.c `timer_init()` is BSP-only via `SUBSYS_INITCALL`; per-CPU timer init is not yet implemented).

### D. per-CPU init_task array

**Files:**
- Modify: `kernel/include/sched/task.h:241` — expand to all NR_CPUS

Change:

```c
task_t *init_task[NR_CPUS] = {&init_task_union.task,0};
```

to:

```c
extern task_t init_task_percpu[NR_CPUS];  /* defined in kernel/core/init_task_percpu.c (x86_64) */
task_t *init_task[NR_CPUS] = {&init_task_union.task, 0, 0, 0};  /* NR_CPUS=8 hardcoded */
```

Wait — `init_task[]` is a pointer array; the per-CPU task structs themselves need to live somewhere. On x86_64, this is `kernel/core/init_task_percpu.c` (R1 R2 verify). On aarch64, provide analogous file.

Actually, looking at `kernel/include/sched/task.h:241`: `init_task[NR_CPUS]` is a `task_t *` array. So I just need to fill in NR_CPUS pointer entries. Phase 2 #3 provides:

```c
// kernel/arch/aarch64/percpu_init_task.c (NEW)
#include <sched/task.h>

/* Per-CPU init_task entries. On x86_64 these come from
 * kernel/core/init_task_percpu.c; on aarch64 we provide per-CPU
 * copies in this file. Each entry mirrors init_task_union.task. */
static task_union_t init_task_union_percpu[NR_CPUS]
    __attribute__((__section__(".data.init_task")));
task_t *init_task[NR_CPUS] = { [0] = &init_task_union.task };
```

Hmm — too complex. Defer D to a follow-up spec. The main goal of Phase 2 #3 is **per-CPU timer**, not per-CPU idle thread. The idle thread can remain BSP-only for now; APs halt in `for (;;) arch_cpu_halt();` loop (smp.c:233) until scheduler lands (Phase 2 follow-up #3.5 or 3.6).

**Scope reduction**: Drop D from Phase 2 #3. Document as future work.

## Non-goals

1. **Per-CPU jiffies counters** — current `jiffies` is a single global. Per-CPU jiffies would require changing `tick_handler()` to use `this_cpu()->jiffies`. Future spec.
2. **Per-CPU scheduler integration** — `init_task[cpu]` for APs (idle threads). Future spec.
3. **`__udivti3` hoist to `compiler_rt/`** — Phase 2 follow-up #4.
4. **`-I libc/include` policy cleanup** — Phase 2 follow-up #5.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| `percpu_data[]` array size mismatch (NR_CPUS=8 vs QEMU `-smp 4` test config) | Low | `NR_CPUS=8` covers QEMU 1/2/4; x86_64 already uses the same constant |
| TPIDR_EL1 set too early or late on AP | Medium | Existing Phase 1 GIC code at `head.S:650` sets TPIDR_EL1 before AP enters C; verify the new `&percpu_data[cpu_id]` works at that point |
| AP CNTP enable breaks GIC selftest (which assumed BSP-only tick) | Medium | QEMU 9/9 PASS still requires `--expect-gic` (GIC dispatch) + `--expect-clk` (3 [clocksource] markers) + `[tick] N` markers. AP CNTP adds new tick sources — verify no harness confusion |
| `subsys_init_percpu()` walks empty table (no per-cpu init registered) | Low | No-op; same as Phase 2 #1's `subsys_init_phase(SUBSYS_PHASE_4)` walking empty table |
| Cross-CPU IPI tick broadcast | N/A | Out of scope for Phase 2 #3 |
| Latent corruption fix incomplete (e.g., AP doesn't yet set `need_resched` to `per_cpu` correctly) | Medium | QEMU E2E catches: if AP `tick_handler` writes corrupt memory, kernel crashes or `nm` test fails |

## Verification

### Unit / host tests

- Existing 5 hosttests must continue to pass byte-for-byte.
- New: `test_percpu.aarch64` hosttest (write after spec approved) verifies `this_cpu()` returns distinct addresses for `cpu_id=0` vs `cpu_id=1`.

### QEMU end-to-end

- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3): **9/9 PASS** with 3 `[clocksource]` + ≥3 `[tick] N` markers per case. NEW for Phase 2 #3: SMP=2/4 cases should now show `[tick] N` lines appearing from BOTH BSP (line 31-33) AND APs (multiple CPUs contributing).
- `make PROFILE=aarch64-clang test-aarch64-gic-spi`: PASS.
- x86_64 byte-identity preserved (10 hosttests + 4 x86_64 regression targets).

### `nm` verification

- `nm build/aarch64-clang/image/kernel.elf | grep percpu_data`: 8 entries (one per CPU).
- `nm | grep idle_resume`: 8 entries (one per CPU; replaces single BSP-only entry).

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #3** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 3. Timer per-CPU affinity / SMP timer

After this spec lands, AP CNTP fires, `tick_handler()` runs per-CPU (with `this_cpu()` pointing at real per-CPU data), and the latent corruption from Phase 2 #2 is closed.

## Subagent-driven plan (to be written via writing-plans skill)

Estimated commit count: 3-5 functional + 1 docs.

1. **Commit 1**: `percpu_data[]` array install — `kernel/arch/aarch64/percpu.c` (NEW) + `kernel/arch/aarch64/head.S` (BSP TPIDR_EL1 set) + `kernel/Makefile` aarch64 whitelist adds `arch/aarch64/percpu.c`.
2. **Commit 2**: AP CNTP enable — `kernel/arch/aarch64/smp.c` removes the mask line(s).
3. **Commit 3**: Per-CPU timer dispatch — `kernel/arch/aarch64/main.c` calls `subsys_init_percpu()`.
4. **Commit 4** (optional): per-CPU `init_task[]` array expansion.

Each commit RED→GREEN→QEMU 9/9→next.

This is Phase 2 P2 follow-up #3; the remaining #4 (`__udivti3` hoist) and #5 (`-I libc/include` policy) are separate specs after Phase 2 #3 closes.