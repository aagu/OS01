# Phase 2 — Split the scheduler into arch-neutral core, boot, and kthread modules

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `kernel/sched/core.c` into an arch-neutral scheduling core, move system startup into `kernel/sched/boot.c`, move kernel-thread construction/termination into `kernel/sched/kthread.c`, and compile-check the common scheduler TUs on aarch64 (product linking waits for Phase 3).

**Architecture:** After this phase, `kernel/sched/core.c` contains only the runqueue, the EEVDF pick/enqueue, the blocker framework, `task_wake`, and the global task list — no x86 addresses, no x86 inline assembly, no VFS, no user-process startup. `kernel/sched/boot.c` owns the boot-phase publication of the idle task and the scheduler start sequence; the x86 system startup (fd table, user init spawn, thermal worker, late selftest calls) moves out of the common path into an x86-only file. `kernel/sched/kthread.c` owns the arch-neutral kernel-thread constructor, the exit state machine, and the common tail already added in Phase 1.

**Tech Stack:** C, Clang/LLD, profile Make builds, host C unit tests, kernel protocol-v1 selftests.

**Spec:** [2026-10-30-aarch64-el1-kthread-scheduler-design.md](../specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md) §4 (file boundaries), §4.1 (build and headers), §5.1 (context contract), §8.1 (kthread creation contract), §8.2 (blocker), §8.3 (exit and reclaim). Series index: [2026-10-30-aarch64-el1-sched-series.md](2026-10-30-aarch64-el1-sched-series.md).

## Global Constraints

All series constraints from the index apply. Additionally, in this phase:

- **The x86 boot-phase creation exception is a contract, not an accident.** Today `net_lwip_init()` calls `kernel_thread()` before the BSP publishes `scheduler_ok`, and it succeeds because `sched_pick_cpu()` falls back to CPU0. The split must preserve that: an unpinned `kernel_thread`/`create_kthread` during the BSP boot phase (per-CPU ready, current task stack up, CPU0 runqueue up, BSP `scheduler_ok` not yet published) may fall back to CPU0 and be queued. That window closes permanently at the first BSP `scheduler_ok` publish and never reopens. A runtime "no eligible target" is **not** a boot-phase case and must return `-EINVAL`.
- **No success-returning stubs.** The aarch64 build omits VFS, user-MM, fork/exec, and network because the *callers* are omitted, not because a stub returns 0. If a common TU cannot compile on aarch64, the fix is to move the x86-only caller out of the common TU.
- **Public kernel-thread signatures do not change.** `kernel_thread(fn, arg, flags)`, `kernel_thread_on_cpu(fn, arg, cpu)`, `create_kthread(fn, arg, name)` keep their exact signatures and return conventions (`kernel_thread*` return the PID or a negative errno; `create_kthread` returns a non-owning `task_t *` or NULL).
- **`flags` keeps its current call shapes.** Existing callers pass `PF_KTHREAD` or 0; the new common constructor must accept the same values and must not silently reinterpret them. Enumerate the live call sites in the implementation and record them in the report (the spec requires this list).
- **aarch64 uses the shared `init_mm`**: `mm == &init_mm`, `files == NULL`, `fpu_save == NULL`, no page table created or destroyed. x86 keeps its current behaviour: kthreads inherit the mm/CR3, get a files copy, and get an FPU save area.
- **Publication ordering is fixed**: allocate → fill → arch context init → *then* publish under `task_list_lock` → then enqueue under the runqueue lock. Nothing half-initialised is ever visible to another CPU. The lock order is `task_list_lock` before the runqueue lock, everywhere.
- **Rollback on any allocation failure** must leave no runqueue entry, no task-list entry, and no leaked allocation. PIDs and birth IDs may be left as holes.

---

## Review Focus

1. **The boot-phase window must close and stay closed.** A test that creates an unpinned kthread during BSP boot must succeed; the same call after `scheduler_ok` is published with no eligible CPU must fail with `-EINVAL`. A window that reopens after a later CPU goes offline is a bug.
2. **`kernel_thread_on_cpu()` to an offline-but-scheduler-ready CPU** must fail with `-EINVAL` and must not leave a half-published task behind.
3. **Rollback must be total.** Force each allocation step to fail (raw stack, thread struct, arch resource prepare) and assert that after the failure the task list length, the runqueue's `nr_running`, and the slab's `total_using` are all back to their pre-call values.
4. **The aarch64 link must not drag VFS in.** If the aarch64 `kernel.elf` grows `vma_*`, `uaccess_*`, `fork_*`, `spawn_user_task`, or `files_*` symbols, the common TU leaked an x86 dependency. The existing `test-aarch64-audit` gate checks the first three; extend the check to the rest.
5. **Boot stacks differ.** x86 boot-service creation already runs on a valid task stack and may use current. aarch64 metadata preparation before handoff must not use current; its constructor is callable only after scheduler publication on a task stack. `sched_boot_publish_idle()` itself needs no current accessor.

---

## File Responsibilities

| File | Change | Responsibility after this phase |
|---|---|---|
| `kernel/sched/core.c` | modify | Arch-neutral: runqueue, EEVDF pick/enqueue, blocker framework, `task_wake`, `task_finish_switch`, global task list, `alloc_pid`, `task_comm_*`/identity glue. No x86 addresses, no VFS, no system startup |
| `kernel/sched/boot.c` | create | `sched_boot_publish_idle()`, `sched_boot_start()`, `sched_boot_creation_open()`; the common idle-publication and scheduler-start sequence |
| `kernel/sched/kthread.c` | modify | Arch-neutral kernel-thread constructor, `kthread_exit(code)`, the `task_switch_complete()` tail added in Phase 1 |
| `kernel/arch/x86_64/cpu/sched_boot.c` | create | The x86 system startup moved out of `core.c`: fd-table setup, `spawn_user_task("/bin/init")`, thermal worker start, the x86 late-selftest calls, the x86 idle loop |
| `kernel/sched/fork.c` | modify | Keep the user-process fork path; the *kernel-thread* path moves to `kthread.c` |
| `kernel/sched/exit.c` | modify | Keep user-process exit; the kthread exit branch delegates to `kthread_exit()` in `kthread.c` |
| `kernel/sched/balance.c` | modify | Placement must gate on `online && scheduler_ok` and honour `allowed_cpus`; the remote-notify path must not use x86 IPI encoding on aarch64 |
| `kernel/include/sched/boot.h` | create | Public scheduler-boot interface |
| `kernel/include/sched/internal.h` | modify | Shared internal declarations: `task_switch_complete()`, `sched_pick_cpu_mask()`, kthread-constructor internals |
| `kernel/Makefile` | modify | x86 gains common files; aarch64 compile-only check gains common objects, with product whitelist unchanged until P3 |

---

### Task 1: Extract the common kernel-thread constructor into `kernel/sched/kthread.c`

**Files:**
- Modify: `kernel/sched/kthread.c`, `kernel/sched/fork.c`, `kernel/sched/exit.c`
- Modify: `kernel/include/sched/internal.h`, `kernel/include/sched/task.h`

**Interfaces:**
- Consumes: `arch_task_switch` / `arch_current_task` / `arch_task_context_valid` from Phase 1; `task_switch_complete()` from Phase 1 Task 2.
- Produces: `int kthread_alloc_and_publish(uint64_t (*fn)(uint64_t), uint64_t arg, const char *name, unsigned target_cpu, bool pinned, task_t **out_task, int *out_pid);` in `kernel/include/sched/internal.h`, plus `void kthread_exit(uint64_t code);`.

- [ ] **Step 1: Enumerate the live `kernel_thread` / `create_kthread` / `kernel_thread_on_cpu` call sites and their `flags`**

```bash
rg -n "kernel_thread\(|kernel_thread_on_cpu\(|create_kthread\(" kernel/ user/
```

Record every call site and its `flags` argument in the phase report. The spec (§8.1) requires this list; the constructor must accept exactly these shapes.

- [ ] **Step 2: Write the failing host test for the constructor's rollback**

Create `hosttests/cases/test_kthread_ctor.c`. The test drives the *real* `kernel/sched/kthread.c` against a mock slab whose allocator fails on a chosen call index, and asserts for each failure point (raw task-union allocation, thread-struct allocation, arch resource prepare):

- the call returns a negative errno;
- the global task list length is unchanged;
- the target runqueue's `nr_running` is unchanged;
- the mock allocator reports zero live bytes.

Add window/placement host assertions: legal x86 boot fallback queues CPU0 while scheduler_ok remains 0; close is permanent; runtime no-target, empty affinity, ineligible pinned CPU and failed pin followed by normal create are checked. Real execution after boot publication is proved by the x86 UP focused QEMU case assigned in P5, not by mocks.

Follow the existing hosttest pattern for compiling a production `.c` against mocks (see `hosttests/Makefile`'s `accounting_production.o` / `identity_production.o` rules and `hosttests/mock/`), and register the binary in `TEST_BINS`.

- [ ] **Step 3: Run the host test to verify it fails**

Run: `make PROFILE=x86_64-clang test-host`.

Expected: FAIL — the constructor does not exist yet.

- [ ] **Step 4: Implement the arch-neutral constructor**

In `kernel/sched/kthread.c`:

```c
/* Allocates, initialises and publishes a PF_KTHREAD task.
 *
 * Order matters and is part of the contract: every fallible step
 * happens BEFORE the task becomes visible to another CPU, and the
 * publication order is task_list_lock then runqueue lock. */
int kthread_alloc_and_publish(uint64_t (*fn)(uint64_t), uint64_t arg,
                              const char *name, unsigned target_cpu,
                              bool pinned, task_t **out_task, int *out_pid)
```

The steps, in order:

1. Validate `fn` (non-NULL). Validate the target: `target_cpu < NR_CPUS` and <num_cpus; all validation precedes publication. The CPU must be `online` **and** `scheduler_ok`, unless the boot-phase exception applies (§8.1) — see Step 5.
2. Allocate the raw task-union space with alignment slack, and the `thread_t`. Align the task-union base to `STACK_SIZE` so the masked `current` accessor resolves it. Record the real allocation base in `stack_alloc_base`.
3. Zero the new task and thread. Check that the `task_t` and the stack reserve do not overlap: `sizeof(task_t)` plus the diagnostic reserve must fit below `usable_low`.
4. Set PID (`alloc_pid()`), identity (`task_identity_init()`), `comm` (the name, written before publication), parent/session metadata, empty blocker/wait/io nodes, `vruntime`/`deadline`, and the CPU mask.
5. Set `flags = PF_KTHREAD`. Do **not** inherit `PF_SELF_REAP`, `on_cpu`, `on_rq`, or `in_schedule`.
6. Call the frozen `int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg)`. Implement x86 in `kernel/arch/x86_64/cpu/task_arch.c`: preserve inherited mm/CR3, copied files (files_dup failure is -ENOMEM), initial FPU state and the existing pt_regs first-entry frame. aarch64 remains a declared extern until Phase 3. The x86 hook cleans/nulls its files/FPU on failure; common rollback frees thread/raw stack. `arch_kthread_drop_files(t)` detaches files under task_list_lock and unpins outside it on exit or any post-prepare rollback; common switch tail frees FPU. No VFS references enter common kthread.c.
   Preserve existing parent signal handlers/blocked mask, pgrp/session, controlling-terminal metadata and default affinity; plain kernel_thread with name=NULL inherits comm under task_list_lock, named create_kthread publishes the supplied name. Only flags 0 and PF_KTHREAD are accepted as existing supported call shapes; both produce PF_KTHREAD, invalid bits return -EINVAL.
7. Publish atomically: acquire task_list_lock then the destination runqueue lock while still holding the first; insert global-list identity, set state=TASK_RUNNING and enqueue exactly once; release in reverse order. No fallible operation follows insertion. Validate placement under the same protected eligibility snapshot before making the task visible.
8. Return 0. Initialise outputs NULL/0 on entry, then capture task pointer and PID before publication. Public PID APIs return out_pid and never dereference a potentially self-reaped task. create_kthread returns only the explicitly non-owning out_task pointer. Failure returns -EINVAL/-ENOMEM with empty outputs.

Rollback: every step after the first allocation must release what it took, in reverse order, on failure.

- [ ] **Step 5: Implement the boot-phase creation window**

Add to `kernel/sched/boot.c`:

```c
/* True only during the BSP boot phase: per-CPU state is up, the current
 * task stack is up, the CPU0 runqueue is up, and the BSP has not yet
 * published scheduler_ok. Closes permanently at the first BSP
 * scheduler_ok publish. */
bool sched_boot_creation_open(void);
```

It starts false. A new `void sched_boot_creation_begin(void)` is called once only on x86 BSP after `x86_64_boot_percpu()` has established percpu, the current task stack and CPU0 runqueue, before any boot-service kthread creation. That opens the window; aarch64 never calls it. The first BSP scheduler publication closes it permanently, even if scheduler_ok is later cleared. When it is true, an *unpinned* `kernel_thread`/`create_kthread` whose placement finds no eligible CPU falls back to CPU0 and queues there. When it is false, the same situation returns `-EINVAL`. `kernel_thread_on_cpu()` never gets the exception.

`sched_boot_start()` (Step 6) is where `scheduler_ok` is published, and it must clear the window *before* publishing, so there is no instant in which both are true.

- [ ] **Step 6: Route the public APIs through the constructor**

Rewrite `kernel_thread`, `kernel_thread_on_cpu`, and `create_kthread` (currently in `kernel/sched/fork.c`) to call `kthread_alloc_and_publish()`. Preserve:

- `kernel_thread` returns the PID, or a negative errno;
- `kernel_thread_on_cpu` returns the PID, or `-EINVAL` for a bad/ineligible CPU, or `-ENOMEM` on allocation failure;
- `create_kthread` returns a non-owning `task_t *` or NULL, and the returned pointer must not be dereferenced after the task self-reaps (the existing callers already respect this — check `kernel/selftest/test_pgrp_signal.c`).

Move these three functions out of `fork.c` into `kthread.c`. `fork.c` keeps `do_fork()` for user processes.

- [ ] **Step 7: Extract `kthread_exit()`**

In `kernel/sched/exit.c`, the `PF_KTHREAD` branch of `do_exit()` (lines ~153-169) does: remove from the task list under `task_list_lock` with IRQs off, set `PF_SELF_REAP` and `TASK_ZOMBIE`, keep `on_cpu == 1`, then `schedule()` to switch out permanently.

Extract that into `kernel/sched/kthread.c`:

```c
/* Terminates a kernel thread. Only kernel threads may call this;
 * idle and user tasks are rejected. */
void kthread_exit(uint64_t code);
```

It must **not** free VMAs or page tables of the shared `init_mm`, and must not destroy a shared address space. On x86, release the thread's copied `files` reference using the existing pin protocol before switching out. On aarch64 there is no such resource.

`do_exit()` tests PF_KTHREAD and delegates immediately on entry, BEFORE init-protection, reparent/SIGCHLD, VMA/MM or files cleanup. Remove the later old branch so it cannot double-clean. User-process exit stays in exit.c. The first-entry x86 trampoline may continue calling do_exit; aarch64 directly calls kthread_exit.

Note the ordering constraint from the spec (§8.3): all cleanup that reads `current` must complete while the old stack is still valid, i.e. before the final `schedule()`.

- [ ] **Step 8: Run the host test to verify it passes**

Run: `make PROFILE=x86_64-clang test-host`.

Expected: PASS, including the new rollback case.

- [ ] **Step 9: Commit**

```bash
git add kernel/sched/kthread.c kernel/sched/fork.c kernel/sched/exit.c \
        kernel/sched/boot.c kernel/include/sched/internal.h \
        kernel/include/sched/task.h hosttests/
git commit -m "refactor(sched): extract arch-neutral kthread constructor and exit path"
```

### Task 2: Split `kernel/sched/core.c` into common core and boot/system startup

**Files:**
- Modify: `kernel/sched/core.c`
- Create: `kernel/sched/boot.c`, `kernel/arch/x86_64/cpu/sched_boot.c`, `kernel/include/sched/boot.h`
- Modify: `kernel/core/main.c` (x86), `kernel/arch/aarch64/boot/main.c` (aarch64, later — Phase 3 calls it)

**Interfaces:**
- Consumes: `kthread_alloc_and_publish()`, `sched_boot_creation_open()` from Task 1.
- Produces: boot.h with publish_idle, returning start, creation_begin and creation_open as frozen in the index.

- [ ] **Step 1: Inventory the x86-only dependencies of `core.c`**

```bash
rg -n "#include" kernel/sched/core.c
```

Expect: `fs/file.h`, `fs/vfs.h`, `memory/vma.h`, `memory/vmm.h`, `selftest/result.h`, `core/printk.h`, plus x86-only usage in `task_init()` (`files_alloc`, `vfs_lookup`, `fd_alloc`, `spawn_user_task`, `debug_task`, `selftest_record_late`, `arch_thermal_start_worker`) and the fixed x86 address windows in the `SCHED-PREV-BAD` diagnostics.

Remove actual VFS/user-startup/arch dependencies from core.c; neutral declarations such as core/panic.h may remain when needed. Move hang_dump_all into `kernel/sched/diag.c` with its public header at `kernel/include/sched/diag.h`, and retain real ASSERT-failure handling without pulling framebuffer/TTY into aarch64.

- [ ] **Step 2: Create `kernel/sched/boot.c` with the common idle publication and start sequence**

`kernel/include/sched/boot.h`:

```c
#ifndef KERNEL_SCHED_BOOT_H
#define KERNEL_SCHED_BOOT_H
#include <stdbool.h>

/* Publish idle metadata without current; never reset an existing
 * task list/runqueue (x86 boot services may already be queued). */
void sched_boot_publish_idle(void);

/* Close boot creation window, publish BSP scheduler_ok, then RETURN.
 * Architecture startup owns post-publication workers/tests/idle. */
void sched_boot_start(void);

/* The BSP boot-phase creation window — see sched_boot_creation_open(). */
bool sched_boot_creation_open(void);
void sched_boot_creation_begin(void);
#endif
```

`sched_boot_publish_idle()` sets `percpu_data[0].idle`, the idle task's identity, `state = TASK_RUNNING`, `on_cpu = 1`, `on_rq = 0`, and its CPU mask (CPU0 only), per spec §6. It must not create a TTY or file table and must not load a user init — that is x86 system startup, a different concern.

`sched_boot_start()` clears the boot-creation window, publishes CPU0 scheduler_ok=1, then returns; it does not enable IRQ or enter idle. `sched_boot_publish_idle()` never reinitialises the already populated x86 global task list or runqueue. Common core retains init_mm/init_task_union/init_task definitions; move architecture-shaped init_thread initialisers into each real task_arch.c (x86 here, aarch64 P3), so common TUs never name rsp/rip/fs/cr3.

- [ ] **Step 3: Move the x86 system startup into `kernel/arch/x86_64/cpu/sched_boot.c`**

Move, unchanged, from `core.c`'s `task_init()`:

- the fd-table setup (files 0/1/2 on `/dev/tty`),
- `spawn_user_task("/bin/init", NULL)` and the `debug_task` line,
- `vfs_end_bootstrap()`,
- `arch_thermal_start_worker()` and its warning,
- all existing x86 late-selftest calls (`kernel_mutex`, `kthread_self_reap`, `fd_refcount`, `pgrp_signal`, `tty_vintr`, `x86_intr`) and the `selftest_end_run()` that follows them,
- the x86 idle loop (`hlt` / `schedule()` / `arch_local_irq_enable()`).

The x86 task_init in arch/x86_64/cpu/sched_boot.c calls sched_boot_publish_idle(), performs the old mm metadata/fd/init-spawn setup, calls returning sched_boot_start(), then vfs_end_bootstrap(), thermal worker, every existing late selftest, END, and the original idle loop. Keep all original late registrations/calls (including thermal_affinity where configured); do not replace them by an assumed count of six. The order of the existing side effects must be preserved exactly: the current code sets `percpu_data[0].idle` and `init_task_union.task.cpu = 0` early in `task_init()`, spawns the user init, sets `state = TASK_RUNNING`, publishes `scheduler_ok = 1`, then runs the thermal worker and the late selftests, then the idle loop. Keep that order.

`kernel/arch/x86_64/cpu/sched_boot.c` is x86-only: it is not in the aarch64 source whitelist, and the aarch64 build must not link it.

- [ ] **Step 4: Strip `core.c` down to the arch-neutral core**

What stays in `core.c`: the runqueue operations and globals, `task_wake`, `task_finish_switch`, the blocker framework (`blocker_wait`, `blocker_wake`, `sched_unblock_blocked`), `schedule()`, `alloc_pid()`, `task_list_add`/`task_list_next`, and the `task_switch_complete()` tail (or keep that in `kthread.c` as Phase 1 put it there).

What leaves: everything in Step 1's inventory.

`schedule()` also needs review for aarch64 compilation:

- it reads `current->thread->...` — replaced in Phase 1 by the arch facade;
- it calls `sched_balance()` (`balance.c`), `update_curr`/`pick_eevdf` (`fair.c`), `enqueue_task`/`dequeue_task` (`fair.c`) — all arch-neutral, but `balance.c` includes `intr/ipi.h` and calls `ipi_send`, which is x86-shaped. See Task 3.
- it calls `hang_dump_all()` — that lives in `kernel/core/panic.c`, which pulls in `driver/fb_state.h` and `tty/console.h` (x86-only). The hang dump must become callable from the aarch64 build: move the *common* part of `hang_dump_all()` into a sched TU and give the architecture-specific part the `arch_dump_task_context()` hook from Phase 1. The x86-only fb/console bits stay in `panic.c`.

- [ ] **Step 5: Verify `core.c` no longer references anything x86-only**

```bash
rg -n "vfs_|files_|spawn_user_task|vma_|vmm_|0xffff8000|selftest_record_late" kernel/sched/core.c
```

Expected: no hits (the `0xffff8000...` windows moved into the arch facade in Phase 1).

- [ ] **Step 6: Build x86**

```bash
make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang kernel.bin
```

Expected: links; no undefined symbols; the behaviour is unchanged.

- [ ] **Step 7: Commit**

```bash
git add kernel/sched/core.c kernel/sched/boot.c kernel/arch/x86_64/cpu/sched_boot.c \
        kernel/include/sched/boot.h kernel/core/main.c kernel/core/panic.c
git commit -m "refactor(sched): split scheduler core from boot and system startup"
```

### Task 3: Make the common scheduler TUs build and link on aarch64

**Files:**
- Modify: `kernel/Makefile`, `kernel/sched/balance.c`, `kernel/sched/fair.c`, `kernel/sched/identity.c`
- Modify: `mk/components/run.mk` (compile-only audit selector); accounting.c stays x86-only in this series

**Interfaces:**
- Consumes: the split from Task 2.
- Produces: common scheduler .o compile checks and an unchanged working aarch64 product; real integration is P3.

- [ ] **Step 1: Write the failing link check**

The aarch64 audit gate already rejects `vma_*` / `uaccess_*` / `fork_*` symbols. Extend it to the symbols this phase could leak: `spawn_user_task`, `files_alloc`, `files_dup`, `files_unpin`, `vfs_lookup`, `vma_free_all`, `vmm_free_user_map`, `do_fork`.

In `mk/components/run.mk`, the `test-aarch64-audit` target greps `$(AARCH64_NM) $(BUILD_DIR)/artifacts/kernel.elf` for forbidden symbols. Extend the regex and update the target's echo text to list the new names.

- [ ] **Step 2: Run the audit to verify it fails**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi-kernel
make PROFILE=aarch64-clang test-aarch64-audit
```

Expected: PASS today (the common TUs are not in the aarch64 build yet, so the leak cannot happen). This step establishes the baseline — the gate covers the common scheduler product only once Phase 3 adds the TUs. Record that reasoning in the report; the gate is not yet proven effective.

- [ ] **Step 3: Compile-check common TUs without linking them into the aarch64 product**

Add the focused `test-static CASE=sched-common-compile` selector in mk/components/run.mk and a kernel/Makefile compile-only target. It compiles sched/core.c, boot.c, kthread.c, fair.c, balance.c, identity.c and diag.c with the real aarch64 compiler, production defines and freestanding include paths into `$(KERNEL_BUILD_DIR)/sched-compile-check/`, stopping at .o. Use no test macro that removes production code; undefined real arch hooks are legal in objects, not fake stubs. The normal product whitelist remains unchanged. Place x86 init_thread and resource handlers in arch/x86_64/cpu/task_arch.c; common globals stay arch-neutral.

Frozen command: `make PROFILE=aarch64-clang test-static CASE=sched-common-compile`. This selector does not run unrelated x86-only audits. Phase 3 supplies all backends and changes the product whitelist atomically. Compiler warnings/errors fail the check; the report lists every compiled TU.

- [ ] **Step 4: Fix `balance.c` for aarch64**

`balance.c` currently:

- includes `<intr/ipi.h>` and calls `ipi_send(dst->arch_processor_id, IPI_VECTOR_RESCHED)` in `sched_notify_remote()` — an x86 IPI encoding;
- scans candidate CPUs checking only `online`, not `scheduler_ok`;
- honours `allowed_cpus` when stealing, which is right.

Required changes per spec (§7):

- The candidate scan must require `online && scheduler_ok`, and must honour `allowed_cpus` for the *target* CPU too, not just the source task's mask.
- Use `ARCH_HAS_REMOTE_RESCHED` (x86=1, aarch64=0) from the context facade. `#if ARCH_HAS_REMOTE_RESCHED` gates BOTH `<intr/ipi.h>` and the ipi_send call; a runtime extern bool does not remove the linker dependency. Aarch64 constructors reject target_cpu!=0 before publication. Its local notify returns; unexpected remote notify is an invariant diagnostic, not a late substitute for admission validation.
- Add balance.c to the compile-only TU list now; Phase 3 adds it to the product whitelist. x86 behavior stays intact.

- [ ] **Step 5: Compile-check common TUs and build the unchanged aarch64 product**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang aarch64-uefi-kernel
```

Also run `make PROFILE=aarch64-clang test-static CASE=sched-common-compile`. Expected: compile-only objects and the unchanged product build succeed; unresolved real backends stay only in unlinked objects until Phase 3. Do not add stubs to obtain a Phase-2 product link.

- [ ] **Step 6: Run the extended audit**

```bash
make PROFILE=aarch64-clang test-aarch64-audit
```

Expected: PASS on the original product. Separately inspect compile-only objects for forbidden subsystem references; permit only the explicit pending arch backend externs. Phase 3 is the first real common-scheduler product link gate.

- [ ] **Step 7: Run the existing aarch64 suites**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp
make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe
```

Expected: both PASS, unchanged from the pre-phase baseline. The aarch64 kernel still ends in its existing `wfi` idle loop — this phase compile-checks scheduler TUs without adding them to the product link or boot path.

- [ ] **Step 8: Commit**

```bash
git add kernel/Makefile kernel/sched/balance.c mk/components/run.mk
git commit -m "build(aarch64): link common scheduler TUs and gate placement on scheduler_ok"
```

### Task 4: Two-arch regression gate

**Files:** none (verification only)

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: the gate evidence Phase 3 needs.

- [ ] **Step 1: Run the x86 host and static gates**

```bash
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
```

Expected: green.

- [ ] **Step 2: Run the x86 kernel selftest suite**

```bash
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
```

Expected: the same cases PASS as the pre-phase baseline — in particular `kernel_mutex`, `kthread_self_reap`, `fd_refcount`, `thermal_affinity`, `pgrp_signal`, `tty_vintr`, `x86_intr`. The kthread constructor rewrite is the main risk here.

- [ ] **Step 3: Run the x86 QEMU suites**

```bash
make PROFILE=x86_64-clang test-qemu SUITE=phase-0
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest
```

Expected: both pass.

- [ ] **Step 4: Run the remaining aarch64 modes**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi
make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault
```

Expected: PASS, unchanged. Also run `make PROFILE=aarch64-clang test-aarch64-audit`.

- [ ] **Step 5: Run the test-framework regression**

```bash
make test-harness
```

Expected: PASS. The harness suite is slow (~10 minutes); do not kill it.

- [ ] **Step 6: Write the phase report**

Record per command: exact command line, exit code, CPU count, log path. Record the `kernel_thread` call-site/flags inventory from Task 1 Step 1. Explicitly list anything not run.

---

## Self-Review

- **Spec coverage**: §4 (file table — the `boot.c` / `kthread.c` / `internal.h` split is the deliverable), §4.1 (aarch64 whitelist and header rules — Task 3), §5.1 (context contract — inherited from Phase 1), §8.1 (creation contract, boot-phase exception, flags list, rollback — Tasks 1 and 3), §8.2 (blocker — `blocker_wait`/`blocker_wake` stay in `core.c`; the *semantics* are exercised in Phase 4), §8.3 (exit/reclaim — Task 1 Step 7). §6 (BSP handoff), §5.2 (aarch64 save set), §10 (acceptance) are later phases.
- **Step scan**: every step names one edit or one command with an expected outcome. Task 1 Step 4's step list is a contract (the spec's §8.1 order), not an implementation — the body is the implementer's, the order is not.
- **Type consistency**: `kthread_alloc_and_publish`, `kthread_exit`, `sched_boot_publish_idle`, `sched_boot_start`, `sched_boot_creation_open`, `ARCH_HAS_REMOTE_RESCHED` match the frozen index contracts. `task_switch_complete` keeps the Phase 1 name and signature.
- **Resource ownership fixed**: Task 1 Step 7 says "on x86, release the thread's copied `files` reference using the existing pin protocol" — the exact pin/unpin call sequence must be copied from the current `do_exit()` kthread branch verbatim, because the fd refcount protocol has a known race (`fd_refcount` is an existing selftest). The instruction to copy verbatim is deliberate; inventing a new sequence here would be a regression.
- **Proportion**: this plan implements §4, §4.1, §8.1, §8.3 and the x86 half of §8.2 — roughly the size of those spec sections. The aarch64 machine work (§5.2, §6, §7) is Phase 3.

## Execution Handoff

Phase 2 is complete when Task 4's gate is green and the report is written. The next plan is:

`docs/superpowers/plans/2026-10-30-aarch64-el1-sched-p3-aarch64-el1.md`
