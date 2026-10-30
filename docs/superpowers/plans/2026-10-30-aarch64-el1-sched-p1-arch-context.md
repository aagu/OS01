# Phase 1 — Extract arch context facade and common switch tail

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the architecture-specific parts of thread context out of `kernel/include/sched/task.h` into an `arch/context.h` facade with per-arch headers, and turn the x86 switch epilogue into a common `task_switch_complete()` — with zero behaviour change on x86.

**Architecture:** `kernel/include/arch/context.h` becomes the single place that knows about `thread_t` layout, the `current` accessor, the switch operation, context validation, and the architecture-specific part of a hang dump. `kernel/include/sched/task.h` keeps the task-flavoured API and includes the facade. The x86 inline assembly moves verbatim into `kernel/include/arch/x86_64/context.h`; `kernel/arch/x86_64/cpu/switch.c` keeps the TSS/FS/CR3/FPU work and the PF_SELF_REAP epilogue, but the epilogue's ordering-sensitive part becomes the common `task_switch_complete()` in `kernel/sched/core.c`.

**Tech Stack:** C, x86-64 assembly, Clang/LLD, profile Make builds, host C unit tests, kernel protocol-v1 selftests.

**Spec:** [2026-10-30-aarch64-el1-kthread-scheduler-design.md](../specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md) §2 (current-state table), §4 (file boundaries), §5.1 (context and switch contract), §9 (diagnostics). Series index: [2026-10-30-aarch64-el1-sched-series.md](2026-10-30-aarch64-el1-sched-series.md).

## Global Constraints

All series constraints from the index apply. Additionally, in this phase:

- **This phase preserves successful scheduling behavior.** Preserve x86 startup and successful scheduling behavior, stable selftest IDs, diagnostic marker names and payload fields. The spec-mandated self-switch safety guard is intentional. Compare stable evidence, excluding timestamps, addresses, revision banners and logger prefixes; do not require byte-identical complete boot logs.
- x86 `thread_t` field order and every field offset must not move. `thread_t` currently is: `rsp0, rip, rsp, fs, gs, cr3, cr2, trap_nr, error_code, sched_flags` (`kernel/include/sched/task.h:101`). Keep that order.
- The x86 `switch_to` inline assembly keeps its current machine-level shape (it pushes `next->thread->rip` and jumps to `__switch_to`). The aarch64 backend will differ at the machine level; the spec permits this (§5.1).
- `task.h` keeps `switch_to(prev, next)` and `current` as working aliases so no caller outside the facade has to change in this phase.

## Review Focus

1. **The x86 switch epilogue's read-after-free window.** `kernel/arch/x86_64/cpu/switch.c:100-115` reads `prev->flags`, then calls `task_finish_switch(prev)` (which clears `on_cpu` and may let a waiter free `prev`), then reads `prev->thread` / `prev->fpu_save` / `prev->stack_alloc_base` again to free them. That is the read-after-free the spec (§8.3) calls out. The extraction must capture those three pointers *before* `task_finish_switch()`, and the test must prove it.
2. **Two `current` implementations.** `get_current_task()` masks `%rsp` against `STACK_SIZE`; after extraction there must be exactly one definition per architecture, and every existing caller (including `kernel/arch/x86_64/intr/trap.c`, which deliberately does *not* use `current` on IST stacks) must keep its current semantics.
3. **Diagnostics must not degrade to always-true.** The spec (§9) forbids turning the x86 stack/UAF checks into no-ops. The extracted `arch_task_context_valid()` must keep the existing `SWITCH-BAD` and `SCHED-PREV-BAD` conditions, including the `0xffff800000000000..0xffff800020000000` x86 heap window.
4. **`current` on a non-task stack.** The BSP boot stack and AP stacks have no `task_t`. After extraction, `arch_current_task()` must still be undefined-by-contract there — the aarch64 handoff in Phase 3 is where that becomes load-bearing, so Phase 1 must document the constraint rather than paper over it.
5. **`GET_CURRENT` and the `switch_to` macro are used outside `task.h`.** `GET_CURRENT` is referenced by `kernel/arch/x86_64/intr/entry.S`; `switch_to` is called from `kernel/sched/core.c:480`. Moving them must not leave a dangling include.

---

## File Responsibilities

| File | Change | Responsibility after this phase |
|---|---|---|
| `kernel/include/arch/context.h` | create | Facade: selects the per-arch context header, declares `arch_current_task`, `arch_task_switch`, `arch_task_context_valid`, `arch_dump_task_context` |
| `kernel/include/arch/x86_64/context.h` | create | x86 `thread_t` layout (unchanged order) + the x86 `switch_to` inline assembly + `arch_current_task()` |
| `kernel/include/arch/aarch64/context.h` | create | aarch64 `thread_t` layout + offset constants + real SP-mask current accessor for Phase 2 compile checks. Context-switch assembly lands in Phase 3. Selected only on aarch64; assembler guards keep constants available without C types |
| `kernel/include/sched/task.h` | modify | Drop the inline `thread_t` definition and the inline asm; include the facade; keep `switch_to`/`current` aliases and all task-flavoured declarations |
| `kernel/arch/x86_64/cpu/switch.c` | modify | Keep TSS/FS/CR3/FPU; replace the PF_SELF_REAP epilogue with a call to the common `task_switch_complete()` |
| `kernel/sched/core.c` | modify | Add `task_switch_complete()`; replace direct `thread->rip`/`thread->rsp` reads with the arch validation/dump calls |

---

### Task 1: Move `thread_t` and the x86 switch asm into the arch facade

**Files:**
- Create: `kernel/include/arch/context.h`, `kernel/include/arch/x86_64/context.h`, `kernel/include/arch/aarch64/context.h`
- Modify: `kernel/include/sched/task.h`

**Interfaces:**
- Consumes: nothing (first task in the series).
- Produces: the facade declared in the index, plus `kernel/include/sched/internal.h` gaining `void task_switch_complete(task_t *actual_prev);` for Task 2.

- [ ] **Step 1: Record the x86 `thread_t` layout as the reference**

Before touching anything, write down the current layout so the "offsets must not drift" claim in the index is checkable. In `kernel/include/sched/task.h` the struct is `rsp0, rip, rsp, fs, gs, cr3, cr2, trap_nr, error_code, sched_flags`, all `uint64_t`, so offsets are 0, 8, 16, 24, 32, 40, 48, 56, 64, 72 and `sizeof(thread_t) == 80`.

Add to the new `kernel/include/arch/x86_64/context.h`:

```c
_Static_assert(offsetof(thread_t, rsp0)       ==  0, "x86 thread_t rsp0 offset drift");
_Static_assert(offsetof(thread_t, rip)        ==  8, "x86 thread_t rip offset drift");
_Static_assert(offsetof(thread_t, rsp)        == 16, "x86 thread_t rsp offset drift");
_Static_assert(offsetof(thread_t, fs)         == 24, "x86 thread_t fs offset drift");
_Static_assert(offsetof(thread_t, gs)         == 32, "x86 thread_t gs offset drift");
_Static_assert(offsetof(thread_t, cr3)        == 40, "x86 thread_t cr3 offset drift");
_Static_assert(offsetof(thread_t, cr2)        == 48, "x86 thread_t cr2 offset drift");
_Static_assert(offsetof(thread_t, trap_nr)    == 56, "x86 thread_t trap_nr offset drift");
_Static_assert(offsetof(thread_t, error_code) == 64, "x86 thread_t error_code offset drift");
_Static_assert(offsetof(thread_t, sched_flags)== 72, "x86 thread_t sched_flags offset drift");
_Static_assert(sizeof(thread_t) == 80, "x86 thread_t size drift");
```

Use `offsetof` from `<stddef.h>`.

- [ ] **Step 2: Create the aarch64 context header and current accessor**

`kernel/include/arch/aarch64/context.h` defines the aarch64 `thread_t`. The spec (§5.2) fixes the saved set: `x19–x29`, `x30` (the return continuation), and `SP`; plus the per-task IRQ storage `sched_flags`; plus stack-top/range fields used by the Phase 3 validation. Use this exact layout — Phase 3's assembly depends on it:

```c
typedef struct thread_struct {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    uint64_t x30;        /* return continuation (the "rip" equivalent) */
    uint64_t sp;         /* saved stack pointer */
    uint64_t sched_flags;/* per-task IRQ state, shared with x86 semantics */
    uint64_t stack_base; /* task_union base */
    uint64_t stack_top;  /* task_union base + STACK_SIZE */
    uint64_t usable_low; /* align_up(base + sizeof(task_t) + 256, 16) */
} thread_t;
```

Give it the same `_Static_assert` treatment for each field offset, and publish the offsets as named constants the Phase 3 assembly will `#include`:

```c
#define AARCH64_THREAD_X19_OFF     (0 * 8)
#define AARCH64_THREAD_X30_OFF     (11 * 8)
#define AARCH64_THREAD_SP_OFF      (12 * 8)
#define AARCH64_THREAD_SCHED_FLAGS_OFF (13 * 8)
#define AARCH64_THREAD_STACK_BASE_OFF  (14 * 8)
#define AARCH64_THREAD_STACK_TOP_OFF   (15 * 8)
#define AARCH64_THREAD_USABLE_LOW_OFF  (16 * 8)
#define AARCH64_THREAD_SIZE           (17 * 8)
```

The facade includes exactly one architecture header. Guard all C includes, typedefs, inlines and assertions with `#ifndef __ASSEMBLER__`; keep numeric offsets outside it.

Provide the real aarch64 current accessor in this step, before Phase 2 compiles current-using common TUs:

```c
static inline task_t *arch_current_task(void)
{
    uint64_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    return (task_t *)(sp & ~(STACK_SIZE - 1));
}
```

It is compiled now, but is valid at runtime only on a task stack; Phase 3 validates and reuses it after handoff. It is neither a stub nor an extern pending a later definition.


- [ ] **Step 3: Create the x86 context header with the moved inline assembly**

`kernel/include/arch/x86_64/context.h` holds the x86 `thread_t` (exact current field order and types) and the `switch_to` inline assembly moved verbatim from `kernel/include/sched/task.h:295-329`. Rename the macro to `arch_task_switch(prev, next)` and keep the body byte-for-byte identical, including the `"memory", "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10", "r11", "cc"` clobber list.

Also move `get_current_task()` here and expose it as:

```c
static inline task_t *arch_current_task(void)
{
    uint64_t sp;
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(sp));
    return (task_t *)(sp & ~(STACK_SIZE - 1));
}
```

Note `STACK_SIZE` is currently defined in `task.h`; the context header needs it, so move the `#define STACK_SIZE (32 * 1024)` into the facade `kernel/include/arch/context.h` and have `task.h` keep including it transitively. `STACK_SIZE` must remain 32 KiB for both architectures (spec §6).

- [ ] **Step 4: Create the facade and rewire `task.h`**

`kernel/include/arch/context.h`:

```c
#ifndef KERNEL_ARCH_CONTEXT_H
#define KERNEL_ARCH_CONTEXT_H
#define STACK_SIZE (32 * 1024)
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
struct task_struct;
typedef struct task_struct task_t;
#endif
#ifdef __x86_64__
# include <arch/x86_64/context.h>
#elif defined(__aarch64__)
# include <arch/aarch64/context.h>
#else
# error "Unsupported architecture"
#endif
#ifndef __ASSEMBLER__
/* current is a per-arch static inline; x86 switch is a macro. */
#ifdef __aarch64__
void arch_task_switch(task_t *prev, task_t *next);
#endif
bool arch_task_context_valid(task_t *t, const char **reason);
void arch_dump_task_context(task_t *t);
void arch_task_check_prev(task_t *t);
void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void));
#endif
#endif
```

The per-arch header defines `thread_t`, so `task_t` (which contains `thread_t *thread`) needs `thread_t` complete. Order the includes so `thread_t` is visible before `task_t` is defined: the facade includes the per-arch header, and `task.h` includes the facade *before* defining `task_t`.

In `kernel/include/sched/task.h`: delete the `thread_t` struct definition and the inline assembly; add `#include <arch/context.h>`; and keep the compatibility aliases:

```c
#define switch_to(prev, next) arch_task_switch(prev, next)
#define get_current_task() arch_current_task()
#define current arch_current_task()
```

Keep the direct-call compatibility alias `get_current_task()` for existing callers, including arch/x86_64/intr/trap.c; removing the old inline body must not leave those calls undeclared.

Keep `GET_CURRENT` where it is reachable for `kernel/arch/x86_64/intr/entry.S`, or move it into `kernel/include/arch/x86_64/context.h` and confirm `entry.S` still finds it — check how `entry.S` includes headers today and match that pattern.

- [ ] **Step 5: Build both architectures**

Run: `make PROFILE=x86_64-clang kernel.bin` and `make PROFILE=aarch64-clang aarch64-uefi-kernel`.

Expected: both link. The aarch64 build now compiles the aarch64 `thread_t` definition (previously it used the x86-shaped one from `task.h` — that is the point of this task). If the aarch64 build fails because some aarch64 TU referenced an x86-only `thread_t` field, that is a real finding: record it and fix it by moving the access into an arch helper, not by widening the aarch64 struct.

- [ ] **Step 6: Commit**

```bash
git add kernel/include/arch/context.h kernel/include/arch/x86_64/context.h \
        kernel/include/arch/aarch64/context.h kernel/include/sched/task.h
git commit -m "refactor(sched): extract arch context facade and per-arch thread_t"
```

### Task 2: Extract the common switch tail as `task_switch_complete()`

**Files:**
- Modify: `kernel/sched/core.c`, `kernel/arch/x86_64/cpu/switch.c`, `kernel/include/sched/internal.h`

**Interfaces:**
- Consumes: `arch_task_switch()` from Task 1.
- Produces: `void task_switch_complete(task_t *actual_prev);` — the contract frozen in the index.

- [ ] **Step 1: Write the failing host test for the capture ordering**

The bug to prevent: reading `prev->thread` / `prev->fpu_save` / `prev->stack_alloc_base` *after* `task_finish_switch(prev)` has cleared `on_cpu`, because a waiter may free `prev` in that window.

Add `hosttests/cases/test_switch_complete.c`, host-compiling the real production `kernel/sched/kthread.c` against mock `task_finish_switch`, `kfree` and current. Create that TU here with just the common tail; Phase 2 adds lifecycle functions.

Assertions: pointers freed are captured before mock finish poisons `prev`; non-self-reap frees nothing; `actual_prev == current` calls neither finish nor free even when PF_SELF_REAP is set. No duplicated test-local implementation.

The host test uses the repo's existing mock pattern: see `hosttests/mock/accounting_test_runtime.h` and `hosttests/cases/` for how production `.c` files are host-compiled against mocks (the `hosttests/Makefile` compiles e.g. `kernel/sched/accounting.c` as `accounting_production.o`). Follow that pattern exactly, and register the new binary in `TEST_BINS`.

- [ ] **Step 2: Run the host test to verify it fails**

Run: `make PROFILE=x86_64-clang test-host` (or the focused hosttests invocation the Makefile supports).

Expected: FAIL, because `task_switch_complete()` does not exist yet.

- [ ] **Step 3: Implement `task_switch_complete()`**

In `kernel/sched/kthread.c`:

```c
#include <sched/task.h>
#include <sched/internal.h>
#include <memory/slab.h>

/* Runs on the *next* task's stack. Captures the self-reap decision and
 * the objects to release BEFORE task_finish_switch() clears on_cpu:
 * after that a waiter may free prev, and reading prev afterwards is a
 * use-after-free. */
void task_switch_complete(task_t *actual_prev)
{
    if (actual_prev == current) return; /* never finish/free our own stack */
    bool self_reap = (actual_prev->flags & PF_SELF_REAP) != 0;

    thread_t *thd  = actual_prev->thread;
    void      *fpu  = actual_prev->fpu_save;
    void      *stk  = actual_prev->stack_alloc_base;

    task_finish_switch(actual_prev);

    if (self_reap) {
        if (thd) kfree(thd);
        if (fpu) kfree(fpu);
        if (stk) kfree(stk);
    }
}
```

The single-argument signature is frozen. On next's stack `current` identifies next. Self-switch is a safe no-op, as required by the spec; no implementer decision to preserve self-reaping is left open.

- [ ] **Step 4: Rewire the x86 switch epilogue to call it**

Replace the old capture/finish/free block in `kernel/arch/x86_64/cpu/switch.c` with `task_switch_complete(prev)` after all TSS/FS/CR3/FPU work; include `<sched/internal.h>`. In common schedule, every `next == current` takes the existing no-switch IRQ-restore/guard-clear path, irrespective of vruntime. Keep normal switch behavior and do not read prev after the tail.

- [ ] **Step 5: Run the host test to verify it passes**

Run: `make PROFILE=x86_64-clang test-host`.

Expected: PASS, including the new case.

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/kthread.c kernel/include/sched/internal.h \
        kernel/arch/x86_64/cpu/switch.c hosttests/
git commit -m "refactor(sched): extract common switch tail task_switch_complete"
```

### Task 3: Move context validation and hang-dump fields behind the arch facade

**Files:**
- Modify: `kernel/sched/core.c`, `kernel/arch/x86_64/cpu/switch.c`, `kernel/core/panic.c`

**Interfaces:**
- Consumes: `arch_task_context_valid()`, `arch_dump_task_context()` from Task 1.
- Produces: no new public interface; removes direct `thread->rip` / `thread->rsp` reads from common code.

- [ ] **Step 1: Identify every common-code read of x86-specific thread fields**

Grep the arch-neutral files for direct field access:

```bash
rg -n "thread->rip|thread->rsp|thread->rsp0|thread->cr3|thread->fs|thread->gs" \
  kernel/sched/ kernel/core/panic.c
```

Expected hits: `kernel/sched/core.c` (the `SCHED-PREV-BAD` / `SCHED-PREV-BAD2` diagnostics around lines 445-479, and the `next->thread->rip = idle_task_resume` line around 450), and `kernel/core/panic.c` (`hang_dump_all`, lines ~82-84).

Also grep for `thread->` inside `kernel/sched/fair.c`, `balance.c`, `identity.c` — those are common TUs and must stay arch-neutral.

- [ ] **Step 2: Implement `arch_task_context_valid()` for x86**

Move the `SWITCH-BAD` condition from `kernel/arch/x86_64/cpu/switch.c:30-40` verbatim into `kernel/arch/x86_64/cpu/switch.c` as the implementation of `arch_task_context_valid(task_t *t, const char **reason)`. It must check, in the same order and with the same bounds:

- `t` is within the x86 kernel heap window `0xffff800000000000 .. 0xffff800020000000`;
- `t->thread` is non-NULL and within the same window;
- `t->thread->rsp` lies within `[t, t + STACK_SIZE]`.

On failure set `*reason` to a static string naming the specific violated condition, and keep the existing `log_err("SWITCH-BAD: ...")` output format so the diagnostic text is unchanged. The function returns `bool`; the caller still logs, so the log text stays where it is today.

- [ ] **Step 3: Implement `arch_dump_task_context()` for x86**

Move the per-task `thread->rip` / `thread->rsp` / `thread->cr3` printing out of `hang_dump_all()` in `kernel/core/panic.c` into `arch_dump_task_context(task_t *t)` in `kernel/arch/x86_64/cpu/switch.c` (or a new `kernel/arch/x86_64/cpu/diag.c` if that file is getting long — its job is "hang/watchdog reporting depends on architecture fields, so split it or give it a real backend", spec §9). Keep the printed field order and format identical.

`hang_dump_all()` then calls `arch_dump_task_context(t)` in the same place it printed the fields before.

- [ ] **Step 4: Replace the `SCHED-PREV-BAD` / `SCHED-PREV-BAD2` block in `kernel/sched/core.c`**

Use `void arch_task_check_prev(task_t *t)` to own the two distinct x86 previous-context checks, their payload fields and fatal response; common core calls it without reading arch fields. The bool validator returns false and a reason without halting; callers decide the fatal response. `arch_dump_task_context()` remains the hang/watchdog field dump, not a replacement for the two different previous-context error formats. Use permitted log_* diagnostics in moved arch code and preserve marker names/field order.

- [ ] **Step 5: Verify the `next->thread->rip == 0` idle special case still works**

`kernel/sched/core.c:449-450` sets `next->thread->rip = (uint64_t)idle_task_resume` when the idle task has never run. That is x86-specific (the original C idle_task_resume is in sched/core.c; it differs from assembly idle_resume in arch/x86_64/intr/entry.S). Move it behind an arch hook rather than leaving a direct `thread->rip` write in common code:

```c
/* kernel/include/arch/context.h */
void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void));
```

Call only for `next == rq->idle`, passing the existing common `idle_task_resume` callback. x86 sets rip to that callback only when rip is zero; do not substitute the different assembly `idle_resume`. aarch64 keeps a real extern until Phase 3; it is not referenced by the Phase-1 product and needs no stub.

- [ ] **Step 6: Build and run the x86 gates**

```bash
make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
```

Expected: all green. `test-static` includes the kernel-layout audit; if the `thread_t` move upset it, fix the audit's expectation rather than the layout.

- [ ] **Step 7: Commit**

```bash
git add kernel/sched/core.c kernel/arch/x86_64/cpu/switch.c kernel/core/panic.c \
        kernel/include/arch/context.h kernel/include/arch/x86_64/context.h \
        kernel/include/arch/aarch64/context.h
git commit -m "refactor(sched): move context validation and hang dump behind arch facade"
```

### Task 4: x86 behaviour-preservation gate

**Files:** none (verification only)

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: the gate evidence Phase 2 needs.

- [ ] **Step 1: Run the x86 kernel selftest suite**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
```

Expected: the same set of cases PASS as before this phase, including `kernel_mutex`, `kthread_self_reap`, `fd_refcount`, `thermal_affinity`, `pgrp_signal`, `tty_vintr`, `x86_intr`. `kthread_self_reap` is the direct exercise of the refactored epilogue — it must still reclaim stacks.

- [ ] **Step 2: Run the phase-0 and systest QEMU suites**

```bash
make PROFILE=x86_64-clang test-qemu SUITE=phase-0
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest
```

Expected: both pass. Record the CPU count each ran with.

- [ ] **Step 3: Run the aarch64 build and the aarch64 audit**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi-kernel
make PROFILE=aarch64-clang test-aarch64-audit
```

Expected: aarch64 links and the audit is green (no forbidden `vma_*` / `uaccess_*` / `fork_*` symbols).

- [ ] **Step 4: Diff the observable output**

Compare before/after successful case IDs/results and stable diagnostic marker/payload templates, normalising revision banners, addresses and timestamps. Do not equate dynamic boot-log differences with behavioral regressions. Record both logs and any intended logger formatting change.

- [ ] **Step 5: Write the phase report**

Record per command: exact command line, exit code, CPU count, log path. Note anything not run as explicitly "not run" with the reason. Do not claim the series is complete — only this phase.

---

## Self-Review

- **Spec coverage**: §2 (current-state table — informs Task 1's move list), §4 (file boundaries — the facade and per-arch headers are created here as specified), §5.1 (context/switch contract — the frozen index contract), §8.3 (self-reap ordering — Review Focus 1 and Task 2), §9 (diagnostics — Task 3). §5.2/§5.3 (aarch64 save set, IRQ state), §6 (BSP handoff), §7 (AP isolation), §8.1/§8.2 (kthread create/block), §10 (acceptance) are **not** in this phase — they are Phases 2-5, and the aarch64 `thread_t` layout fixed in Task 1 Step 2 is the only aarch64-specific artifact here.
- **Step scan**: each step names one concrete edit or one command with an expected outcome. Task 2 freezes the single-argument tail and its self-switch no-op; Task 3 passes the original idle continuation explicitly. No signature decision is delegated to the executor.
- **Type consistency**: `task_t`, `thread_t`, `arch_current_task`, `arch_task_switch`, `arch_task_context_valid`, `arch_dump_task_context`, `task_switch_complete` are spelled identically in the index, in Task 1, in Task 2, and in Task 3.
- **Proportion**: this plan is roughly the size of the spec section it implements (§2/§4/§5.1/§8.3/§9), not the size of the whole spec. It deliberately leaves the aarch64 assembly, the BSP handoff, the kthread lifecycle, and the QEMU harness to later phases.

## Execution Handoff

Phase 1 is complete when Task 4's gate is green and the report is written. The next plan is:

`docs/superpowers/plans/2026-10-30-aarch64-el1-sched-p2-core-split.md`
