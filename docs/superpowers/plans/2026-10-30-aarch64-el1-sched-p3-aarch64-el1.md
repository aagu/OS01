# Phase 3 — aarch64 EL1 context switch, BSP handoff, and CPU0-only scheduling

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give aarch64 a real EL1 cooperative context switch, hand the BSP stack over to a proper idle task, and publish a CPU0-only scheduler — so a normal aarch64 image boots into a scheduling BSP with APs still isolated.

**Architecture:** Three new aarch64 files provide the machine layer: `kernel/arch/aarch64/cpu/switch.S` saves and restores the EL1 callee-saved set and drives the common switch tail; `kernel/arch/aarch64/cpu/thread_entry.S` provides the first-entry trampoline and the BSP stack-handoff trampoline; `kernel/arch/aarch64/cpu/task_arch.c` provides the C-side context initialisation, validation, and the handoff preparation. The BSP's existing boot flow ends with a single non-returning handoff into the scheduler idle, exactly once, after all the pre-SMP and post-SMP probes have run.

**Tech Stack:** AArch64 assembly, C, Clang/LLD (`-march=armv8-a`, `-mgeneral-regs-only`), profile Make builds, QEMU `virt` machine, the protocol-v1 kernel selftest.

**Spec:** [2026-10-30-aarch64-el1-kthread-scheduler-design.md](../specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md) §5.2 (aarch64 save set and ABI), §5.3 (IRQ state), §6 (BSP idle, current, boot order), §7 (CPU set and AP isolation), §9 (diagnostics). Series index: [2026-10-30-aarch64-el1-sched-series.md](2026-10-30-aarch64-el1-sched-series.md).

## Global Constraints

All series constraints from the index apply. Additionally, in this phase:

- **The aarch64 save set is exactly**: `x19–x29`, `x30` (the return continuation), and `SP`. `x0–x17` and NZCV are caller-volatile. `x18` has no platform TLS/current use in this kernel and is treated as a temporary — **never** as a substitute for `TPIDR_EL1`. Saving `x0–x17` is forbidden: it would be a bug, not conservatism.
- **Every assembly-to-C boundary keeps SP 16-byte aligned**, and every C function called from assembly follows AAPCS64.
- **The first thread entry uses `top-16`**: the initial SP is the task-union top minus 16, holding one aligned entry frame, and it never pops back to `top` for the life of the thread.
- **First entry passes `fn`/`arg` in `x19`/`x20`** and the entry point in `x30`. The entry trampoline calls `fn(arg)` with the argument in `x0` and hands the return value to the kernel-thread exit path. There is no fake `pt_regs_t`, no `eret`, and the entry never returns into a fabricated caller.
- **The saved SP must satisfy `usable_low <= SP < top`** — the half-open interval, with `usable_low` covering the `task_t` and the diagnostic reserve. `SP == top` is invalid.
- **The common tail runs on the next task's stack.** `task_switch_complete()` clobbers the link register, so the continuation is spilled to the next stack first and restored afterwards; the spill must land in the next task's usable region and must not overwrite the `task_t` or the saved context.
- **The BSP handoff is one-way and happens once**, after the existing pre-SMP and post-SMP selftests and the shootdown probe have run in their current order and count. The boot stack is not reclaimed this phase.
- **The BSP idle task must be `current` before any C call.** The handoff sets SP into the idle task's stack before the first C instruction runs there — `current` is never "set to top and left to a C prologue to fix".
- **No FP/SIMD state is saved.** No thread that needs FP/SIMD state may be admitted. New assembly uses no FP/SIMD registers.
- **`current` must never be dereferenced on the boot stack, the AP boot/work stacks, or an exception stack.** Any helper reachable before the handoff must not touch `current`.

---

## Review Focus

1. **The first-entry SP must be inside the task union before any C prologue.** A C function that adjusts SP before `current` is computed gives a wrong `current` for the first entry. The handoff must establish SP in assembly, and the check must happen before the first C call on the new stack.
2. **`SP == top` must be rejected.** A stack pointer exactly at the top of the union is one past the last usable byte. The half-open interval `[usable_low, top)` must be enforced, including at the *initial* `top-16`.
3. **The common tail must not clobber the continuation.** `task_switch_complete()` is a C call; its link-register use must not destroy the next task's resume point. Getting this wrong produces a task that returns to a garbage address instead of hanging.
4. **APs must stay untouched.** After the handoff, AP `scheduler_ok` is still 0, AP runqueues are still empty, APs still serve the TLB shootdown, and no AP ever calls `schedule()` or `current`. The shootdown probe must still pass at `-smp >= 2`.
5. **The handoff must not disturb the existing probe order.** The pre-SMP selftests, the SMP bring-up, the GIC probes, the shootdown probe, and the post-SMP multicore selftest all keep their current order, count, and markers. A moved or skipped probe shows up as a changed `[selftest]` marker set in the `MODE=smp` log — that log is the regression oracle for this phase.

---

## File Responsibilities

Tasks 1–4 are a single integration unit. Keep their task/step structure, compile intermediate objects where possible, but defer product-link gates and commits until Task 4 is wired; then run the listed gates against the complete phase. No broken intermediate commit is required.


| File | Change | Responsibility after this phase |
|---|---|---|
| `kernel/arch/aarch64/cpu/switch.S` | create | Save/restore the EL1 context, spill the continuation, call the common tail, resume or enter the first-entry trampoline |
| `kernel/arch/aarch64/cpu/thread_entry.S` | create | First-entry trampoline (`fn`/`arg` → `fn(arg)` → exit path) and the BSP stack-handoff trampoline. Does not reuse the exception-return frame |
| `kernel/arch/aarch64/cpu/task_arch.c` | create | C-side context init/validation, idle/kthread construction, BSP handoff preparation |
| `kernel/arch/aarch64/boot/main.c` | modify | End the boot flow with the one-way handoff; keep every existing probe in place |
| `kernel/arch/aarch64/linker.ld` | modify | Reserve and align the aarch64 `init_task_union` per `STACK_SIZE` |
| `kernel/arch/aarch64/cpu/idle.c` | modify | Replace the bare `wfi` loop with the scheduler-aware idle loop |
| `kernel/include/arch/aarch64/context.h` | modify | Fill in the aarch64 `thread_t` offsets the assembly uses; add the aarch64 implementations of the facade hooks |
| `kernel/sched/boot.c` | modify | Provide the common idle publication the aarch64 handoff drives |

---

### Task 1: AArch64 context switch assembly

**Files:**
- Create: `kernel/arch/aarch64/cpu/switch.S`
- Modify: `kernel/include/arch/aarch64/context.h`

**Interfaces:**
- Consumes: `task_switch_complete(task_t *actual_prev)` from Phase 1/2; the aarch64 `thread_t` layout and offset constants from Phase 1 Task 1 Step 2.
- Produces: C wrapper `arch_task_switch(prev, next)` in Task 3 and assembly `aarch64_switch_context(actual_prev, prev_ctx, next_ctx)`, matching the index contract.

- [ ] **Step 1: Write the assembly skeleton with the offsets wired to the header**

`kernel/arch/aarch64/cpu/switch.S` must include the offset header so the constants have exactly one definition:

```asm
#include <arch/aarch64/context.h>
```

Confirm the header is safe for the assembler: it may define `typedef`/`struct` only inside `#ifndef __ASSEMBLER__`, with the offset `#define`s outside it — the same shape `kernel/include/arch/aarch64/regs.h` already uses.

- [ ] **Step 2: Implement the save half**

`aarch64_switch_context(actual_prev, prev_ctx, next_ctx)` receives x0=actual_prev, x1=prev->thread, x2=next->thread. The C wrapper owns task validation; assembly reads only `thread_t` offsets, never an unprovided `task_t` member offset. Save x19–x29, x30 (the caller continuation), and SP through x1. Use x3–x17 as documented scratch; keep x0 as the incoming actual-prev argument until the common tail consumes it, and x2 until restoration finishes. Do not change x18 or the saved registers before saving them.

On resumption, `ret` returns to the suspended C wrapper on that task's own stack. Its AAPCS64 compiler frame then returns to its original `schedule()` caller. This is an ordinary function ABI on aarch64; it does not imitate x86's inline macro.



- [ ] **Step 3: Implement the switch and the common tail**

The switch half must restore x19–x29, x30 and SP through x2, then use this definite tail sequence:

```asm
/* x0 still names the task switched out by THIS transfer. */
sub sp, sp, #16
str x30, [sp]
bl task_switch_complete
ldr x30, [sp]
add sp, sp, #16
ret
```

The 16-byte allocation is below the restored SP: AAPCS64 has no red zone, so this newly allocated space is available, while memory at/above the restored SP may be live. Never use a permanent `top-16` spill slot; that can overwrite a suspended task's frame. The C wrapper must validate `saved_sp - 16 >= usable_low` before committing to this allocation. C call depth below the spill must remain above the reserved diagnostic area; context/probe stress checks this margin. The tail runs on next's stack and receives the incoming actual-prev, not the task saved in next's old invocation. It may release actual-prev, so no later instruction dereferences that pointer.



- [ ] **Step 4: Verify the SP interval invariant in the assembly**

The C wrapper `arch_task_switch()` returns immediately on `prev == next`; otherwise it validates next and checks prev's live SP through `arch_task_check_prev()` BEFORE entering the assembly. `thread_t.stack_base` is the union base, `stack_top` is base+STACK_SIZE, and the separate `usable_low` field is `ALIGN_UP(base + sizeof(task_t) + 256, 16)`. Do not conflate stack_base with usable_low. Require alignment, `usable_low <= saved_sp < top`, and the extra 16-byte tail headroom. Context initialization/static assertions must ensure the interval is nonempty.

Immediately after installing next's SP and before the first C tail call, assembly checks SP alignment, `SP & ~(STACK_SIZE-1) == next_ctx->stack_base`, and bounds using thread offsets. Use scratch registers while x2 still holds next_ctx. The same pre-C check is required in the first-entry and BSP trampolines. A failure branches to an arch fatal entry that masks IRQ and logs the supplied raw values without consulting `current`; it never makes a normal C call on an unvalidated stack. Pre-switch rejection occurs in the wrapper, not after committing SP.



- [ ] **Step 5: Build the aarch64 kernel**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi-kernel
```

Expected: the assembly object compiles. Tasks 1–4 are one integration unit: defer final product linking and the listed commits until Task 4 supplies all referenced hooks/globals and the common source whitelist. Do not add unresolved-symbol stubs or forced aliases to make an intermediate link pass.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/aarch64/cpu/switch.S kernel/include/arch/aarch64/context.h
git commit -m "feat(aarch64): add EL1 cooperative context switch"
```

### Task 2: First-entry trampoline and BSP handoff trampoline

**Files:**
- Create: `kernel/arch/aarch64/cpu/thread_entry.S`
- Modify: `kernel/include/arch/aarch64/context.h`

**Interfaces:**
- Consumes: the aarch64 `thread_t` layout; `kthread_exit()` from Phase 2.
- Produces: the first-entry symbol the constructor sets in `thread->x30`, and the BSP handoff entry point.

- [ ] **Step 1: Implement the first-entry trampoline**

The trampoline is entered after the common tail with x19=fn, x20=arg and SP=top-16. Before any C call, assembly checks SP/current mask/alignment/bounds against the initialized context, records the pre-C observation for the selftest, and branches to the raw fatal path on failure. Then normalize `current->on_cpu=1` and `current->in_schedule=0` (a small C helper is legal only AFTER the pre-C check). Enable IRQ explicitly before `blr x19` with x0=x20. This must also happen on the first switch from idle; do not rely on a suspended `schedule()` restoring flags.

Pass the function's return value directly to `kthread_exit()`. It is noreturn; an unexpected return is fatal. There is no fabricated caller, fake pt_regs or eret. Preserve AAPCS64 alignment at every C boundary; x19/x20 carry the entry operands across the switch tail because they are callee-saved.



- [ ] **Step 2: Implement the BSP handoff trampoline**

This is the assembly that performs the one-way transition from the boot stack to the idle task's stack. It must:

1. Enter the distinct assembly symbol `aarch64_bsp_stack_enter(uint64_t idle_sp)` with x0=`top-16`; set SP before any C call. Check the new SP/current-mask/alignment/bounds in assembly using the known idle context, without first dereferencing a computed current.
2. Clear the frame-pointer chain (the spec requires it: "再清理 FP 链").
3. Branch to a non-returning C entry point (`arch_aarch64_bsp_handoff()`) that continues the boot sequence on the new stack.

The SP value must come from a linker-visible symbol or a C-provided constant, and must be provably inside the idle task's union — see Task 3.

- [ ] **Step 3: Build and commit**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi-kernel
git add kernel/arch/aarch64/cpu/thread_entry.S
git commit -m "feat(aarch64): add first-entry and BSP handoff trampolines"
```

### Task 3: aarch64 task_arch.c — context init, validation, handoff preparation

**Files:**
- Create: `kernel/arch/aarch64/cpu/task_arch.c`
- Modify: `kernel/include/arch/aarch64/context.h`

**Interfaces:**
- Consumes: `kthread_alloc_and_publish()` (Phase 2), the trampolines (Tasks 1-2), the facade hooks.
- Produces: the aarch64 implementations of `arch_current_task()`, `arch_task_switch()` (forwarding to the assembly), `arch_task_context_valid()`, `arch_dump_task_context()`, and `arch_aarch64_bsp_handoff()`.

- [ ] **Step 1: Implement `arch_current_task()` for aarch64**

Per spec §6, the aarch64 `current` uses the same stack-mask idea as x86: take SP and mask it against `STACK_SIZE`. The mask assumes the task-union base is `STACK_SIZE`-aligned — Task 4 arranges that in the linker script.

Verify/reuse the real Phase-1 `static inline` in `kernel/include/arch/aarch64/context.h` using `mov %0, sp` plus the mask (available to Phase 2 compile-checks); do not also declare a conflicting extern. It is **only valid on a task stack**.

- [ ] **Step 2: Implement `arch_task_context_valid()` for aarch64**

Per spec §9, the checks are:

- the pointer is non-NULL;
- the saved SP satisfies `usable_low <= SP < top` (reject `SP == top` and `SP < usable_low`);
- SP is 16-byte aligned;
- the continuation (`x30`) is a legal kernel execution address for this architecture.

On the continuation address, the spec is explicit that aarch64's address space differs from x86's and that the old fixed x86 upper bound must not be reused:

- accept only aligned instruction addresses in the linker-defined high-half executable text range(s), including the first-entry/idle code;
- stacks and data may be in the RAM direct map; that does NOT make their addresses valid continuations;
- no scheduler continuation needs the low `.boot` range in this phase, so arbitrary low addresses and `.boot` are rejected. If a future feature needs a named low executable range, that is a separately reviewed change.

`arch_task_context_valid(t, reason)` is a boolean diagnostic predicate: return false and set a static reason, without halting. `arch_task_switch()` owns fatal reporting and halts before the assembly call on false; `arch_task_check_prev()` checks live prev context without validating a stale saved PC/SP. Hang dumps can call the predicate without recursively halting. `arch_task_prepare_first_switch(t, idle_entry)` is a real aarch64 backend: contexts are prepared by the constructor/handoff already; verify their invariants and preserve them, never fabricate an x86 idle_resume address.



- [ ] **Step 3: Implement `arch_dump_task_context()` for aarch64**

Print the architecture-specific fields the hang dump needs: the saved SP, the continuation, and the stack range. The aarch64 log backend cannot format `%d` — use the specifiers it does support and make sure the numbers actually appear.

- [ ] **Step 4: Implement the aarch64 first-entry context initialisation**

Implement `int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg)` and the file-drop hook required by Phase 2. The prepare hook does:

- `mm = &init_mm` (the shared one), `files = NULL`, `fpu_save = NULL`;
- no page table created or destroyed;
- SP = `top-16`;
- `x19 = fn`, `x20 = arg`;
- `x30` = the first-entry trampoline address;
- `stack_base` / `stack_top` recorded in the `thread_t` so the validation and the switch can use them.

Store `usable_low = ALIGN_UP(stack_base + sizeof(task_t) + 256, 16)` in its distinct field; require top-16 to leave the additional 16-byte tail slot. Return 0 only after complete initialization; no arch allocation is needed. `arch_kthread_drop_files()` checks files is NULL; it does not fake a VFS release. Define the aarch64 `init_thread` initializer here with the same range metadata and idle entry; common code retains init_mm/init_task_union ownership.

- [ ] **Step 5: Build and commit**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi-kernel
git add kernel/arch/aarch64/cpu/task_arch.c kernel/include/arch/aarch64/context.h
git commit -m "feat(aarch64): add EL1 task context init, validation and idle construction"
```

### Task 4: Linker script, idle loop, and the BSP handoff

**Files:**
- Modify: `kernel/arch/aarch64/linker.ld`, `kernel/arch/aarch64/boot/main.c`, `kernel/arch/aarch64/cpu/idle.c`, `kernel/sched/boot.c`, `kernel/Makefile`
- Create: `qemutests/aarch64_scheduler.py` (normal-image idle-smoke skeleton, extended in Phase 5)
- Modify: `kernel/arch/aarch64/make.config` if a new source directory is needed (the aarch64 arch sources are collected by a wildcard over `arch/aarch64/{boot,intr,memory,smp,cpu,platform,runtime}` — `cpu/` is already included, so `cpu/switch.S` and `cpu/thread_entry.S` are picked up automatically; confirm the `.S` wildcard covers `cpu/*.S`).

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: an aarch64 image that boots into a scheduling BSP idle.

- [ ] **Step 1: Reserve and align the aarch64 `init_task_union`**

The spec (§6, item 3) requires the idle task's union to be aligned to `STACK_SIZE` and reserved whole, so the masked `current` accessor resolves it and the stack has room.

In `kernel/arch/aarch64/linker.ld`, add an output section for the idle task union, aligned to 32768, sized to hold the union, and keep it out of the reclaimed-after-boot region. The x86 linker does the analogous thing with `.data.init_task` and `ALIGN(32768)` — mirror that shape.

Confirm the symbol the handoff uses to compute `top-16` is derived from this section, not from a hardcoded address.

- [ ] **Step 2: Replace the bare `wfi` idle loop**

`kernel/arch/aarch64/cpu/idle.c` currently spins in `wfi` forever with the comment that the scheduler has not landed. Replace it with the scheduler-aware idle loop the spec (§6) requires:

- check for a runnable task / `need_resched` **before** sleeping;
- when runnable work exists, save local IRQ state, call `schedule()`, and restore that state after return (real switch resumes masked);
- enable IRQ before sleeping with `wfi`; do not depend on Phase 4's not-yet-existing kthread_yield;
- cover the window between "checked" and "wfi" — re-check after waking.

The spec allows the BSP's periodic CNTP to bound this: at most one tick later the CPU leaves `wfi` and re-checks. The BSP's CNTP timer must stay running for that to hold.

The idle task must never enter the runnable rbtree, and idle exit must be refused.

- [ ] **Step 3: End the aarch64 boot flow with the one-way handoff**

In `kernel/arch/aarch64/boot/main.c`, the current flow ends with `for (;;) arch_cpu_halt();`. Replace that with the handoff, in this exact order (spec §6):

1. Keep every existing pre-SMP and post-SMP selftest, probe, and marker in its current position and count. **Do not reorder anything.**
2. Mask the BSP IRQ.
3. Prepare the idle task, `init_thread`, an empty runqueue, and the task list. Do **not** call `percpu_init` again and do **not** reset published IPI state.
4. On the boot stack, prepare only metadata. Do not publish `scheduler_ok` here, and do not call any `current`-dependent helper.
5. Jump into the handoff trampoline, which sets SP to the idle task's `top-16`, clears the FP chain, and enters `arch_aarch64_bsp_handoff()`.
6. In `arch_aarch64_bsp_handoff()` (running on the validated idle stack): set `cpu = 0`, the idle identity, `state = TASK_RUNNING`, `on_cpu = 1`, `on_rq = 0`, and the CPU0-only mask; call common `sched_boot_publish_idle()` and returning `sched_boot_start()` to publish CPU0. Create no TTY or file table and load no user init.
7. Enable IRQ, emit `[sched] idle-ready cpu=0 scheduler_ok=1 irq=on` once, then enter idle. Phase 3 keeps the old early coordinator END; Phase 4 atomically adds late registration/execution and moves END.

Add common core/boot/kthread/diag/fair/balance plus their real list/rbtree dependencies to the aarch64 kernel whitelist in this same integration change; resolve each required symbol using real backends. Keep fork/exec/VMA/VFS/TTY/user-init absent and run the expanded symbol audit against this linked product. Tasks 1–4 must be linked and committed together if their intermediate commits would be broken.

The handoff must not return to the old boot stack.

- [ ] **Step 4: Verify `current` is correct at the first C instruction on the new stack**

The assembly trampoline performs the pre-C check described in Task 1 Step 4, including `SP & ~(STACK_SIZE-1) == &init_task_union.task`. Only afterwards may the C continuation assert `current` and metadata as a second check; a C prologue must not be the first check. The pre-C observation feeds Phase 4's entry evidence.

Create the normal-image smoke runner with required `--image PATH --firmware PATH --idle-smoke --cpus 1 2 4 8`. Use the existing per-count `generate_diagnostic_dtb()`/QEMU command pattern. Boot the supplied normal image at each CPU count with a fixed 120s deadline, require the post-publication idle-ready marker and at least three existing `[tick]` records with increasing values AFTER that marker, reject fatal/crash/timeout, retain logs and return nonzero on failure. Use the existing tick reporter; do not add an undefined DTB observation option or infer readiness from pre-handoff SMP PASS. Preserve default console logging policy.



- [ ] **Step 5: Build and boot aarch64**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi-kernel
make PROFILE=aarch64-clang test-aarch64 MODE=smp
```

Also build the normal `aarch64-uefi` image without KERNEL_SELFTEST and run:

```bash
python3 qemutests/aarch64_scheduler.py --idle-smoke --cpus 1 2 4 8 \
  --image build/aarch64-clang/image/aarch64-uefi.img \
  --firmware build/aarch64-clang/image/QEMU_EFI.fd
```

Expected: real idle readiness and subsequent ticks at all four CPU counts. `MODE=smp` separately preserves existing probe markers; it alone does not prove handoff completion.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/aarch64/linker.ld kernel/arch/aarch64/boot/main.c \
        kernel/arch/aarch64/cpu/idle.c kernel/sched/boot.c
git commit -m "feat(aarch64): hand BSP stack to scheduler idle and publish CPU0"
```

### Task 5: AP isolation and placement gate

**Files:**
- Modify: `kernel/arch/aarch64/smp/smp.c`, `kernel/sched/balance.c`, `kernel/sched/boot.c`

**Interfaces:**
- Consumes: Task 4.
- Produces: the AP-isolation and placement guarantees the spec §7 requires.

- [ ] **Step 1: Confirm APs never gain a scheduler**

In `kernel/arch/aarch64/smp/smp.c`, the AP entry path must not set `scheduler_ok`, must not register an idle task, and must not call `schedule()` or `current`. The AP work loop (`ap_work_run_one()`) and the IRQ/SGI handling stay exactly as they are — the spec requires the APs to keep serving TLB shootdown.

Verify with CodeGraph/source inspection, and state the finding in the report.

- [ ] **Step 2: Keep `num_cpus` truthful**

`num_cpus` stays the real enumerated count. The BSP-only scheduler must not be implemented by clamping `num_cpus` to 1. Confirm and state in the report.

- [ ] **Step 3: Make placement require `scheduler_ok`**

`sched_pick_cpu_mask()` (Phase 2 Task 3 Step 4) already requires `online && scheduler_ok`. Confirm that on aarch64 it therefore only ever returns CPU0, and that `kernel_thread_on_cpu()` to a non-zero CPU returns `-EINVAL` even when that AP is online (the AP is online but not `scheduler_ok`).

- [ ] **Step 4: Reject remote placement on aarch64**

The aarch64 remote-notify path must report an invariant error and refuse, never silently succeed, and must not use the x86 IPI vector encoding.

- [ ] **Step 5: Run the aarch64 suites at several CPU counts**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp
make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe
make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi
```

Expected: PASS. Record actual CPU counts from existing SMP harness; the normal-image idle-smoke explicitly covers 1/2/4/8. Phase 4 supplies the kernel AP state evidence; do not claim the old harness reads scheduler_ok from RAM.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/aarch64/smp/smp.c kernel/sched/balance.c kernel/sched/boot.c
git commit -m "fix(aarch64): keep APs isolated from the CPU0 scheduler"
```

### Task 6: Two-arch regression gate

**Files:** none (verification only)

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces: the gate evidence Phase 4 needs.

- [ ] **Step 1: Run the x86 gates**

```bash
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
make PROFILE=x86_64-clang test-qemu SUITE=phase-0
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest
```

Expected: all green. Phase 3 should not have touched x86 behaviour at all.

- [ ] **Step 2: Run the aarch64 gates**

```bash
make PROFILE=aarch64-clang test-aarch64-audit
make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault
make PROFILE=aarch64-clang test-aarch64 MODE=m1-ram
```

Expected: PASS.

- [ ] **Step 3: Run the harness regression**

```bash
make test-harness
```

Expected: PASS. Slow; do not kill it.

- [ ] **Step 4: Write the phase report**

Record per command: exact command line, exit code, CPU count, log path. Record the aarch64 marker-set diff against the pre-phase baseline. List anything not run.

---

## Self-Review

- **Spec coverage**: §5.2 (save set, ABI, first entry, SP interval, tail-on-next-stack — Tasks 1-3), §5.3 (IRQ state at the handoff — Task 4), §6 (BSP idle, current, boot order, idle loop window — Task 4), §7 (CPU set, AP isolation, placement — Task 5), §9 (aarch64 validation and diagnostics — Task 3). §8.1/§8.2/§8.3 (kthread lifecycle specifics) and §10 (acceptance) are Phases 2 and 4-5; Phase 2 built the constructor, Phase 4 exercises the lifecycle.
- **Step scan**: each step names one edit or one command with an expected outcome. Task 1 Step 2's "caveat worth deciding explicitly" and Task 4 Step 3's ordered list are the spec's requirements stated as decisions, not as "figure it out".
- **Type consistency**: `arch_task_switch`, `arch_current_task`, `arch_task_context_valid`, `arch_dump_task_context`, `arch_aarch64_bsp_handoff`, `kthread_exit`, `task_switch_complete` are spelled identically here, in the index, and in Phases 1-2.
- **Resolved ABI**: the C wrapper validates before the assembly call; x0/x1/x2 carry actual-prev/prev-context/next-context, and a temporary 16-byte stack allocation protects LR across the next-stack tail. First entry and idle explicitly enable IRQ.


- **Proportion**: this plan implements §5.2, §5.3, §6, §7, and the aarch64 half of §9 — the machine-level surface of the spec. It does not re-specify the kthread lifecycle or the acceptance harness.

## Execution Handoff

Phase 3 is complete when Task 6's gate is green and the report is written. The next plan is:

`docs/superpowers/plans/2026-10-30-aarch64-el1-sched-p4-cooperative.md`
