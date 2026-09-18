# AArch64 `cntp_tick_handler → tick_handler()` Integration — Design (R2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.
>
> **R1 review**: sonnet aa8cf8d7 found 6 CRITICAL items. R1 verdict: **R2 required (major redesign)**. R2 rewrites the spec with explicit stubs + per-arch gates. R2-R4 review cycles may continue; this is the R2 baseline.

**Goal:** Make `kernel/time/tick.c::tick_handler()` (the unified tick semantic) run on aarch64 by routing `cntp_tick_handler()` through it. **Design revision (R2)**: rather than turning `tick_handler()` into a single-source cross-arch function, gate the x86_64-specific sections with `#if defined(__x86_64__)` and provide aarch64 stubs for the x86_64-only symbols (`idle_resume`, `softirq_status` lock-or inline asm, `clocksource_read_ns()`).

**Architecture (R2):**

The unified tick semantic stays single-source, but is now an arch-conditional compilation. Three layers:

1. **Common path** (runs on both arches): `jiffies++` → `this_cpu()->need_resched = 1` → `this_cpu()->watchdog_counter++` → timer_list_head scan → `set_softirq_status(TIMER_SIRQ)`.
2. **x86_64-only path** (`#if defined(__x86_64__)`): poll-timeout scan using `clocksource_read_ns()` + `poll_timeout_head` + `wait_queue_wake_all` + `spin_lock_irqsave(&poll_timeout_lock)`.
3. **aarch64-only stub layer**: `idle_resume`, `softirq_status` R/W (without x86 `lock orq` asm), `clocksource_read_ns()` (returns jiffies-based fallback or zero), `poll_timeout_head/lock` externs (NULL).

The `[tick] N` per-second print stays in `cntp_tick_handler` (R2 keeps it; it's the harness evidence gate — see `Risks §H1`).

## R1 critical findings (recap, fixes in R2)

| R1 finding | R2 fix |
|---|---|
| CRITICAL-1: harness requires `≥3 [tick] N` lines; spec §A removed the print | R2 keeps `[tick] N` print in `cntp_tick_handler` (NOT `tick_handler`). Print stays GIC-Phase-1-style (per-second counter). |
| CRITICAL-2: `time/tick.c` + `time/timer.c` not in aarch64 whitelist | R2 explicitly adds both. |
| CRITICAL-3: `<sched/task.h>` file-scope `init_thread` references `idle_resume` | R2 provides `kernel/arch/aarch64/idle_resume_stub.c` (no-op). |
| CRITICAL-4: `intr/softirq.c` uses x86 `lock orq` inline asm; not in aarch64 whitelist | R2 adds `intr/softirq.c` to aarch64 whitelist + gates the asm with `#if defined(__x86_64__)`. |
| CRITICAL-5: `tick.c` references `fs/poll.c`'s `poll_timeout_head/lock`; poll.c not in aarch64 whitelist | R2 gates the poll-scan block in `tick.c` with `#if defined(__x86_64__)`. aarch64's `tick_handler` does the common path only. |
| CRITICAL-6: `clocksource_read_ns()` gated `#if defined(__x86_64__)` in `kernel/include/time/clocksource.h:34` | R2 keeps the existing gate (x86_64-only); `tick.c`'s `clocksource_read_ns()` call is already inside the `#if defined(__x86_64__)` poll-scan block — so the call is naturally gated. |

## Design (R2)

### A. `kernel/time/tick.c` — gate the poll-scan block AND `tick_start()`

Open `kernel/time/tick.c`. The current `tick_handler()` body (lines 17-41) AND `tick_start()` body (lines 41-51) both become gated.

**`tick_handler()`** becomes:

```c
void tick_handler(void)
{
    jiffies++;

#if defined(__x86_64__)
    // x86_64-only poll-timeout scan. aarch64 phase 1 has no userland
    // processes (no init_thread, no scheduler, no /dev/poll); the
    // poll-timeout scan is dead code on aarch64.
    if (poll_timeout_head) {
        uint64_t flags = spin_lock_irqsave(&poll_timeout_lock);
        for (poll_timeout_node_t *n = poll_timeout_head; n; n = n->next)
            if (clocksource_read_ns() >= n->deadline)
                wait_queue_wake_all(n->wq);
        spin_unlock_irqrestore(&poll_timeout_lock, flags);
    }
#endif

    this_cpu()->need_resched = 1;
    this_cpu()->watchdog_counter++;

    if ((container_of(list_next(&timer_list_head.list), timer_t, list)->expire_jiffies <= jiffies))
        set_softirq_status(TIMER_SIRQ);
}
```

**`tick_start()`** also gets gated (R3.4 NEW finding — Task 3 originally stopped because `tick_start` calls `irq_mask`/`irq_unmask` which are x86-only via `kernel/intr/irq.c` not in aarch64 whitelist):

```c
void tick_start(void)
{
#if defined(__x86_64__)
    // x86_64-only PIT/LAPIC handoff ceremony. aarch64 phase 1 has
    // no PIT, no LAPIC; kernel/arch/aarch64/main.c:334 calls
    // arch_tick_start() directly without going through tick_start().
    // tick_start() is dead code on aarch64.
    irq_mask(0);
    if (arch_tick_start()) {
        // LAPIC 接管成功，PIT 保持掩蔽。
    } else {
        // LAPIC 未校准/失败：回退 PIT。
        irq_unmask(0);
    }
#endif
}
```

Result on aarch64:
- `jiffies++` ✓
- poll scan SKIPPED (gated)
- `this_cpu()->need_resched = 1` → writes to `.boot.bss` slack (Phase 2 #3 per-CPU install will fix)
- `this_cpu()->watchdog_counter++` → same (latent)
- timer_list_head scan → if `timer_init()` ran via SUBSYS dispatch, `timer_list_head.list.next` is a sentinel (per `init_timer(... -1UL)`). `list_next` returns the sentinel, `container_of(sentinel, timer_t, list)->expire_jiffies` is `UINT64_MAX` (because `init_timer` sets `expire_jiffies = -1UL`). Comparison `UINT64_MAX <= jiffies` is false (jiffies is small). Skip softirq set. Latent OK.
- `tick_start()` body SKIPPED (gated). `tick_start` symbol still resolves (empty function on aarch64, can be called from anywhere that links against it; currently nothing on aarch64 calls it, dead code).

Result on x86_64: byte-identical (the `#if` blocks are identical to the current code).

### B. `kernel/arch/aarch64/time.c` — keep `[tick] N` print in `cntp_tick_handler`

**R2 revision**: do NOT remove the per-second `[tick] N` print. Keep it as GIC Phase 1 evidence gate.

Replace `cntp_tick_handler` (lines 57-76) with:

```c
static void cntp_tick_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
    cntp_tval_el0_write(g_period);  /* TVAL rewrite FIRST (phase1 spec §2.3) */
    tick_handler();                   /* unified tick semantic */
    /* GIC Phase 1 evidence gate: qemutests/aarch64_uefi_smp.py:545/574
     * requires >=3 [tick] N lines per case. */
    uint64_t t = g_ticks + 1;
    g_ticks = t;
    if ((t % TICKS_PER_SECOND) == 0) {
        kputs("[tick] ");
        kputu(t / TICKS_PER_SECOND);
        kputs("\n");
    }
}
```

Keep `g_ticks` (line ~49), `TICKS_PER_SECOND` (line ~40), and the `[tick] N` print block. `tick_handler()` runs first, then the per-second print. The order: TVAL → tick_handler → debug print. `tick_handler()`'s common path (`jiffies++`, `need_resched=1`, `watchdog_counter++`, timer scan) runs every tick; the per-second print fires once per second.

This satisfies BOTH:
- The framework integration (`tick_handler()` actually runs, aarch64 now has unified tick semantic — Phase 2 #2 goal achieved).
- The GIC Phase 1 evidence gate (`[tick] N` lines still appear in stdout.log).

### C. `kernel/Makefile` — expand aarch64 whitelist

Open `kernel/Makefile`. Locate the `ifeq ($(ARCH),aarch64)` block (around lines 42-45). Replace:

```make
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c time/clocksource.c \
                   arch/aarch64/udivti3_stub.c arch/aarch64/subsys.c \
                   arch/aarch64/subsys_stub.c
endif
```

with:

```make
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c \
                   time/clocksource.c time/timer.c \
                   arch/aarch64/udivti3_stub.c arch/aarch64/subsys.c \
                   arch/aarch64/subsys_stub.c \
                   arch/aarch64/idle_resume_stub.c
endif
```

Adds:
- `time/timer.c` — the framework timer init (needed for `_timer_register` to run on aarch64 via SUBSYS dispatch)
- `arch/aarch64/idle_resume_stub.c` — provides `idle_resume` symbol for `<sched/task.h>`'s file-scope `init_thread` initializer

**Not added** (deliberately):
- `time/tick.c` — see D below; tick.c is NOT directly linked on aarch64. The framework `tick_handler()` symbol is consumed via `kernel/time/tick.c`'s header path... wait, that's wrong; `tick_handler()` is **defined** in `kernel/time/tick.c`. Need to link tick.c.

**Correction**: `time/tick.c` MUST be in the aarch64 whitelist (the `cntp_tick_handler` calls `tick_handler()`). Updated whitelist:

```make
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c \
                   time/clocksource.c time/tick.c time/timer.c \
                   arch/aarch64/udivti3_stub.c arch/aarch64/subsys.c \
                   arch/aarch64/subsys_stub.c \
                   arch/aarch64/idle_resume_stub.c
endif
```

This expands aarch64's compiled TU set by **3 files** (`time/tick.c`, `time/timer.c`, `arch/aarch64/idle_resume_stub.c`).

`kernel/intr/softirq.c` is **NOT** in this whitelist expansion. R2 relies on the existing `#if defined(__x86_64__)` gate in `kernel/intr/softirq.c::set_softirq_status` (R2 §E adds this gate) so that x86-only `lock orq` asm is only compiled on x86_64. On aarch64, the gate selects the aarch64 branch (no-op or arch-neutral). Verify in R3 review: `kernel/intr/softirq.c` is currently x86_64 whitelist only (`$(wildcard intr/*.c)` block — line 58). Adding it to aarch64 KERNEL_C_SOURCES is **not required** if `tick.c`'s `set_softirq_status(TIMER_SIRQ)` call (line 41) is wrapped in `#if defined(__x86_64__)` too. R3 review must trace this.

### D. `kernel/arch/aarch64/idle_resume_stub.c` (NEW, ~10 lines)

Create `kernel/arch/aarch64/idle_resume_stub.c`:

```c
// ── kernel/arch/aarch64/idle_resume_stub.c ──────────────────────
//
// <sched/task.h> file-scope declares
//   thread_t init_thread = { ..., .rip = (uint64_t)idle_resume, ... };
// unconditionally — no #ifdef __aarch64__ guard. On x86_64,
// kernel/arch/x86_64/entry.S provides `idle_resume` as the entry
// point that returns from idle. On aarch64 phase 2 we have no
// scheduler and no idle thread (Phase 2 #3 = per-CPU timer / SMP
// timer is the prerequisite for an actual scheduler), so this
// stub is never called — its existence is solely to satisfy the
// linker when time/tick.c and time/timer.c are pulled into the
// aarch64 build (which transitively pulls <sched/task.h>'s
// init_thread initializer).
//
// Phase 2 #3 follow-up: replace this stub with a real aarch64
// idle_resume that re-enters the idle loop on each CPU.

#include <stdint.h>

void idle_resume(void)
{
    /* no-op: aarch64 phase 2 has no scheduler; init_thread is
     * dead code (its .rip is never dereferenced because nothing
     * schedules init_thread onto a CPU). */
    for (;;) {
        __asm__ __volatile__("wfi" ::: "memory");
    }
}
```

(`wfi` loop matches `kernel/arch/aarch64/smp.c:223-225`'s AP idle pattern.)

### E. `kernel/intr/softirq.c` — gate x86 inline asm + make arch-neutral

Open `kernel/intr/softirq.c`. There are **two** x86-only inline asm sites that must be gated:

**Site 1**: `set_softirq_status` (lines 11-13):

```c
void set_softirq_status(uint64_t status)
{
#if defined(__x86_64__)
    __asm__ __volatile__("lock orq %0, softirq_status(%%rip)"
                         :: "r"(status) : "memory");
#else
    /* aarch64 (and other arches): plain write. SMP-safe in practice
     * because tick_handler() runs at IRQ context with IRQs masked
     * (no concurrent set_softirq_status); softirq_status is single
     * uint64_t written by tick + cleared by do_softirq, no race. */
    softirq_status |= status;
#endif
}
```

**Site 2** (R3 NEW finding): `do_softirq` (lines 40-42) — also uses x86-only asm:

```c
void do_softirq(void)
{
    for (int i = 0; i < 64; i++) {
        if (softirq_status & (1ULL << i)) {
#if defined(__x86_64__)
            __asm__ __volatile__("lock andq %0, softirq_status(%%rip)"
                                 :: "r"(~(1ULL << i)) : "memory");
#else
            softirq_status &= ~(1ULL << i);
#endif
            if (softirq_vector[i].action)
                softirq_vector[i].action(softirq_vector[i].data);
        }
    }
}
```

Both sites must be gated, because R2 §C adds `intr/softirq.c` to aarch64 KERNEL_C_SOURCES (so this file will be compiled under `clang -target aarch64-none-elf`, and the x86 `lock andq`/`lock orq` syntax will fail to assemble). `do_softirq` is invoked only from `kernel/arch/x86_64/entry.S:78` (x86_64-only); on aarch64 phase 2 it's dead code, but the asm-gated branch must still compile cleanly.

`get_softirq_status()`, `register_softirq()`, `unregister_softirq()`, `softirq_init()` don't need gating (no inline asm).

### E-bis. `kernel/time/timer.c` — gate x86 `pause` instruction (Task 1.5 deviation)

**R3.2 NEW finding**: `kernel/time/timer.c:124` contains an unconditional `__asm__ volatile("pause");` inside `destroy_timer()` (wait-for-timer-done spin loop). The `pause` instruction is x86-only; on aarch64 it must be either gated or replaced with an arch-neutral spin-wait hint.

Gate it with `#if defined(__x86_64__)`:

```c
        /* x86_64 `pause` instruction: power-saving + inter-thread
         * politeness hint in spin loops. aarch64 maps to `yield`
         * (a v8.0-A hint; same semantic family — give up the
         * current execution slice in a spin). */
#if defined(__x86_64__)
        __asm__ volatile("pause");
#else
        __asm__ volatile("yield");
#endif
```

(`yield` is the canonical aarch64 replacement for x86 `pause` in spin-wait contexts; both are v8-A-defined low-latency hints that improve SMT throughput without serializing the pipeline.)

### E-ter. `kernel/arch/aarch64/libc_stub.c` — provide `calloc`/`free` libc shims (R3.3 NEW)

**R3.3 NEW finding** (Task 1.5 retry STOPPED): `kernel/time/timer.c` calls libc functions:

- **line 25** `create_timer()`: `timer_t * timer = (timer_t *)calloc(1, sizeof(timer_t));`
- **line 134** `destroy_timer()`: `free(timer);`

aarch64 phase 2 has no userspace libc (the aarch64 kernel is freestanding). x86_64 pulls these symbols from the libc sysroot (`-isystem $(SYSROOT_GENERATION_DIR)/usr/include` per `kernel/Makefile:99`). aarch64 does NOT use the libc sysroot (per `kernel/Makefile:93-99`'s `ifeq ($(ARCH),x86_64)` guard).

Provide minimal shims in NEW `kernel/arch/aarch64/libc_stub.c` (mirrors `subsys_stub.c` pattern):

```c
/* kernel/arch/aarch64/libc_stub.c — libc alloc/free shims for aarch64.
 *
 * kernel/time/timer.c uses calloc/free (lines 25, 134) for timer
 * object allocation. x86_64 pulls these from the libc sysroot
 * (kernel/Makefile:99); aarch64 phase 2 deliberately does NOT
 * use the libc sysroot (kernel/Makefile:93-99 guards -isystem
 * under ifeq x86_64). This stub provides minimal in-kernel
 * replacements that delegate to the slab allocator.
 *
 * Mirrors the arch/aarch64/subsys_stub.c pattern (Phase 2 #1).
 * Phase 2 follow-up: replace with real libc when one lands.
 */

#include <stdint.h>
#include <stddef.h>

void *calloc(size_t nmemb, size_t size)
{
    /* overflow check omitted (caller validates nmemb * size before
     * calling calloc; timer.c callers pass 1, sizeof(timer_t))
     */
    size_t total = nmemb * size;
    void *p = kmalloc(total);  /* slab allocator (arch/aarch64/slab_stub.c) */
    if (p)
        memset(p, 0, total);
    return p;
}

void free(void *ptr)
{
    kfree(ptr);
}
```

**Required includes** (verify R3 review): the prototype for `calloc`/`free` is declared in `<stdlib.h>` (libc header) which aarch64 kernel doesn't include directly. Options:
- (a) Forward-declare in the stub file (cleanest).
- (b) Define the prototype at top of `libc_stub.c` matching `<stdlib.h>` signatures.

Option (a) recommended: `extern void *kmalloc(size_t); extern void kfree(void *); extern void *memset(void *, int, size_t);` at the top of the stub.

`tick.c` line 41 `set_softirq_status(TIMER_SIRQ)` is **inside** the common-path tail, NOT inside the `#if defined(__x86_64__)` poll-scan block. So `set_softirq_status()` is always called on aarch64 — the §E Site 1 asm gate handles it.

`kernel/intr/softirq.c` was previously only in x86_64 whitelist. Adding it to aarch64 KERNEL_C_SOURCES is now **required** (since `tick.c` calls `set_softirq_status`):

```make
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c \
                   time/clocksource.c time/timer.c \
                   intr/softirq.c \
                   arch/aarch64/udivti3_stub.c arch/aarch64/subsys.c \
                   arch/aarch64/subsys_stub.c \
                   arch/aarch64/idle_resume_stub.c \
                   arch/aarch64/libc_stub.c
endif
```

### F. `kernel/time/clocksource.c` — flip `timer.c:136` SUBSYS_INITCALL gate

Same as v1 §B — flip `#ifdef __x86_64__` to `#if defined(__x86_64__) || defined(__aarch64__)`. After this lands, `_timer_register` runs on aarch64 via framework dispatch, calling `_timer_init_wrapper` → `timer_init()` (which calls `init_timer(&timer_list_head, NULL, NULL, -1UL)` + `register_softirq(0, &do_timer, NULL)`).

`timer_init()` calls `register_softirq(0, ...)`. This is now arch-neutral (no asm gate needed). The softirq `do_timer` callback runs when `do_softirq` is invoked — but on aarch64 phase 2 there's no `do_softirq` invocation path yet (no scheduler to call it). **Documented as latent**: TIMER_SIRQ bit may stay set indefinitely on aarch64. Phase 2 #3 (scheduler integration) fixes this. No immediate crash.

### G. `kernel/arch/aarch64/main.c` — verify no explicit `softirq_init()` needed

After E, `softirq_init()` is in `kernel/intr/softirq.c` (now in aarch64 KERNEL_C_SOURCES). `softirq_init()` zeroes `softirq_status` + `softirq_vector`. It's called explicitly from `kernel/intr/irq.c:78` on x86_64. On aarch64, the `intr/irq.c` is x86_64-only — not linked. So `softirq_init()` is never called on aarch64.

**Add explicit call in `aarch64_main`** (between the SUBSYS hook and `arch_tick_start()`):

```c
#if defined(__aarch64__)
    extern void softirq_init(void);
    softirq_init();
#endif
```

Order:
1. `arch_register_subsys()` (Phase 2 #1 commit `bddf8eb`)
2. `subsys_init_phase(SUBSYS_PHASE_4)` (Phase 2 #1) — runs `_clocksource_init_wrapper` + `_timer_init_wrapper`
3. `softirq_init()` (NEW, R2) — explicit; clears `softirq_status`
4. `arch_tick_start()` — registers `cntp_tick_handler` in GIC handler table

### H. What does NOT change

- `kernel/time/clocksource.c` — Phase 2 #1 already flipped its gate.
- `kernel/arch/aarch64/linker.ld` — already has `.subsys_init` (Phase 2 #1 commit `6a6026b`).
- `kernel/arch/aarch64/subsys.c` — already exists (Phase 2 #1).
- `kernel/arch/aarch64/subsys_stub.c` — already provides `register_subsys` etc. (Phase 2 #1 + commit `069e632`). Need to verify it can handle 2 initcalls (clocksource + timer) without cap issues — `MAX_SUBSYS = 16`, plenty of room.

## Non-goals (out of scope)

1. **Per-CPU timer / SMP timer** — Phase 2 #3.
2. **`__udivti3` hoist to `compiler_rt/`** — Phase 2 follow-up #4.
3. **`-I libc/include` policy cleanup** — already in place from Task 2.2.
4. **Real aarch64 scheduler** — much larger follow-up.
5. **Removing `idle_resume` stub** — replaced when scheduler lands.
6. **Replacing `subsys_stub.c`** — replaced when `serial_printk`/`strcmp`/`num_cpus` available on aarch64.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| `tick_handler` `this_cpu()->need_resched = 1` corrupts `aarch64_boot_percpu[0]` (offset 8/16) | High (latent), Low (immediate) | Phase 2 #3 per-CPU install fixes; documented as latent corruption (no scheduler reads today). |
| `tick_handler` `this_cpu()->watchdog_counter++` writes out of bounds of 48-byte struct | High (latent), Low (immediate) | Same. |
| `kernel/intr/softirq.c` adds `intr/` to aarch64 whitelist — verify `intr/irq.c` etc. aren't transitively pulled | Medium | R3 review: grep aarch64 build for `intr/irq.c` references. If pulled, add more stubs. |
| `do_softirq` second asm site in `kernel/intr/softirq.c` (R3 NEW finding) | High | §E Site 2 gate handles it. Verified the function is invoked only from x86_64 entry.S:78 (dead code on aarch64); the asm-gated branch must still assemble cleanly. |
| `set_softirq_status` plain write race on SMP aarch64 | Low (latent) | Documented; tick_handler runs at IRQ context with IRQs masked, no concurrent write. |
| TIMER_SIRQ bit never cleared on aarch64 (no `do_softirq` invocation) | Medium (latent) | Documented; Phase 2 #3 scheduler fix. |
| `kernel/time/timer.c` builds OK on aarch64 — verify all its `#include` headers resolve | High | R3 review: trace `#include` chain. If any header pulls x86-only types, gate. |
| `idle_resume` stub never invoked — link OK but linker may strip unused symbols | Low | `kernel/arch/aarch64/idle_resume_stub.c` is in aarch64 whitelist via Makefile change; linker keeps it. |

## Verification

### Unit / host tests

- Existing 5 hosttests must continue to pass byte-for-byte.
- `test_clocksource` 49/49 PASS (no change to clocksource semantics).

### QEMU end-to-end

- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (--expect-selftest --expect-gic --expect-clk): 9/9 PASS
- `[tick] N` lines still appear (R2 §B preserves the print)
- `[clocksource]` markers still appear (Phase 2 #1 unchanged)
- New: `timer_init()` is now invoked via SUBSYS dispatch — verifiable via `nm | grep -c subsys_init` going from 3 (Phase 2 #1) to 4 (R2 adds `_timer_register`)

### x86_64 byte-identity

- `kernel/intr/softirq.c` edit is `#if defined(__x86_64__)` gate — x86_64 path unchanged.
- `kernel/time/tick.c` edit is `#if defined(__x86_64__)` gate — x86_64 path unchanged.
- `kernel/Makefile` x86_64 `else` block untouched.
- `kernel/time/timer.c` gate flip is `||` — x86_64 still works.

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #2** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 2. `cntp_tick_handler → tick_handler()` integration — needs `<list.h>` include-path fix

`<list.h>` is already in place from Task 2.2 (`12d3720`); this spec consumes that fix. After this spec, `tick_handler()` runs on aarch64 (common path only — jiffies/need_resched/watchdog/TIMER_SIRQ set). x86_64 also gets the gated poll-scan block but the behavior is byte-identical (the `#if` only protects x86_64-only code).

## Subagent-driven plan (to be written via writing-plans skill)

Estimated commit count: 4-7 functional + 1 docs.

1. **Commit 1**: `feat(aarch64): provide idle_resume stub` — `kernel/arch/aarch64/idle_resume_stub.c` (NEW, ~10 lines) + `kernel/Makefile` aarch64 whitelist expansion (adds `idle_resume_stub.c`, `time/tick.c`, `time/timer.c`, `intr/softirq.c`). Verify build OK; verify `idle_resume` symbol present.

2. **Commit 2**: `feat(kernel): gate x86 inline asm in softirq.c` (R3 NEW) — `#if defined(__x86_64__)` gates around **BOTH** `lock orq` (Site 1: `set_softirq_status`) AND `lock andq` (Site 2: `do_softirq`). Verify x86_64 byte-identical + aarch64 build OK.

3. **Commit 3**: `feat(kernel): gate x86 poll-scan block in tick.c` — `#if defined(__x86_64__)` around poll-scan. Verify x86_64 byte-identical + aarch64 build OK.

4. **Commit 4**: `feat(aarch64): cntp_tick_handler delegates to tick_handler() with [tick] print kept` — modify `cntp_tick_handler` per §B. Add explicit `softirq_init()` call per §G. Flip `timer.c:136` gate per §F.

5. **Commit 5** (if needed): fixup if Step 1's whitelist expansion breaks something.

Total: 4-5 commits. Wall-clock: 1-2 days (mostly verification + QEMU runs).

Each commit is RED→GREEN→QEMU verification → next commit.