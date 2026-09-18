# aarch64 Per-CPU Timer / SMP Timer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close Phase 2 P2 follow-up #3 from `docs/aarch64-timer-phase1-closure-2026-09-18.md` — wire `percpu_data[cpu]` on aarch64 (so `this_cpu()` returns real `percpu_t *` with `need_resched` at offset 8), enable CNTP on application processors, and fix `kernel/arch/aarch64/gic.c::k_cpu_index()` to use `percpu_t *` not `aarch64_boot_percpu_t *`. After this plan, AP CNTP fires, `tick_handler()` runs per-CPU, and the Phase 2 #2 latent corruption is closed.

**Architecture:** Two commits in strict dependency order:
1. **Commit 1** (combined §A + §C prerequisite): add `percpu/percpu.c` to aarch64 whitelist + modify `head.S:323,650` to point TPIDR_EL1 at `&percpu_data[cpu_id]` + add `percpu_install_gs(0)` + `percpu_init(0, mpidr)` in BSP `aarch64_main` + add `percpu_install_gs(cpu_id)` + `percpu_init(cpu_id, mpidr)` in AP `secondary_idle` + modify `kernel/arch/aarch64/gic.c::k_cpu_index()` to use `percpu_t*`. **This commit lands §A + §C prerequisite together** — splitting them creates broken intermediate states.
2. **Commit 2** (§B): remove `cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1))` masks at `smp.c:210,226`. AP CNTP enabled. **Must land AFTER Commit 1** (TPIDR_EL1 must point at real `percpu_data[cpu_id]` first, else AP writes to `aarch64_boot_percpu[cpu_id].cpu_id` corrupt `k_cpu_index()`).

**Tech Stack:** freestanding C (clang -target aarch64-none-elf), AArch64 assembly (head.S), GNU Make profile build (`mk/profiles/aarch64-clang.mk`), Python QEMU harness (`qemutests/aarch64_uefi_smp.py`).

**Spec:** `docs/superpowers/specs/2026-09-18-aarch64-smp-timer-design.md` (R3 APPROVED-WITH-NITS, commit `15e1df9`)

## Global Constraints

- **Worktree:** all work in `feat/aarch64-timer-phase2-smp` worktree (`/home/aagu/aarch64-timer-phase2-smp`); do NOT touch master, `/home/aagu/OS01`, or other worktrees (`feat/aarch64-timer`, `feat/aarch64-timer-phase2`, `feat/aarch64-timer-phase2-cntp` preserved).
- **AGENTS.md is PROTECTED.** Do not edit.
- **No push.** Local commits only.
- **Strict commit ordering:** Commit 1 → Commit 2. Reversing this order turns Phase 2 #2's latent corruption into ACTIVE corruption.
- **aarch64 build contract:** `make PROFILE=aarch64-clang test-build-contract-aarch64` must pass.
- **Build cache trap** (Phase 2 #1 Task 3 / Phase 2 #2 pattern): `make` does not recompile `.o` files on `CFLAGS`-only changes (notably `KERNEL_SELFTEST=1`). Every QEMU regression run MUST be preceded by `make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang KERNEL_SELFTEST=1 ... aarch64-uefi` to avoid stale-cache false negatives.
- **Submodule init** (verify with `git submodule status` — should be all SHAs, no `m`/`-` prefix).
- **`PERCPU_DATA_SIZE` macro**: spec NIT-1 — must be added as a numeric literal to `kernel/include/percpu/percpu.h` BEFORE Commit 1. Calculate as `sizeof(percpu_t)` (mirror the actual struct layout; verify by `nm | grep percpu_data` size).
- **`mpidr_el1_read()` function**: spec NIT-2 — does NOT exist. Use inline asm `mrs xN, mpidr_el1` directly (spec §C snippet shows the right pattern). Do NOT define a helper function.
- **`#if OS01_SELFTEST` guard around `arch_local_irq_enable()` in `secondary_idle`** (spec NIT-3): APs only unmask IRQs in selftest builds. In production builds, APs halt with IRQs masked → AP CNTP fires do not reach `cntp_tick_handler`. The spec's `[tick] N` lines from APs only appear in `OS01_SELFTEST=1` builds. Verification harness already runs with `KERNEL_SELFTEST=1` — AP `[tick] N` lines visible. **Plan**: keep `OS01_SELFTEST` guard around `arch_local_irq_enable()` (no change). Verification section explicitly notes this.
- **Harness `[tick] N` gate** (QEMU E2E): `qemutests/aarch64_uefi_smp.py:545/574` requires `≥3 [tick] N` lines per case. BSP alone produces ≥3 lines per second. AP `[tick] N` lines (only in selftest builds) ADD to the count, never subtract. Gate preserved.
- **x86_64 byte-identity:** no edits to x86_64 sources.

---

## File Structure

**Modified:**
- `kernel/Makefile` — aarch64 KERNEL_C_SOURCES whitelist adds `percpu/percpu.c`.
- `kernel/include/percpu/percpu.h` — add `PERCPU_DATA_SIZE` numeric literal macro.
- `kernel/arch/aarch64/head.S:312-324` — BSP `setup_bsp_stack_and_tpidr` sets `TPIDR_EL1 = &percpu_data[0]`.
- `kernel/arch/aarch64/head.S:640-680` — AP entry computes `&percpu_data[cpu_id]` and sets `TPIDR_EL1`.
- `kernel/arch/aarch64/main.c` — BSP adds `percpu_install_gs(0)` + `percpu_init(0, mpidr)` after SUBSYS hook init (use inline asm `mrs xN, mpidr_el1`; no helper function).
- `kernel/arch/aarch64/smp.c:secondary_idle` — AP adds `percpu_install_gs(cpu_id)` + `percpu_init(cpu_id, mpidr)` after `gic_cpu_init()`, BEFORE CNTP enable (Commit 2).
- `kernel/arch/aarch64/smp.c:210` — remove `cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1))` (Commit 2).
- `kernel/arch/aarch64/smp.c:226` — remove duplicate mask line (Commit 2).
- `kernel/arch/aarch64/gic.c:20-25` `k_cpu_index()` — change `(aarch64_boot_percpu_t *)` cast to `(percpu_t *)`.

**Not modified:**
- `kernel/percpu/percpu.c` — unchanged (already defines `percpu_data[NR_CPUS]`, `percpu_init`, `percpu_install_gs`).
- `kernel/include/percpu/percpu.h` `this_cpu()` — unchanged (already returns `percpu_t *`).
- `kernel/include/arch/percpu.h` — unchanged (already does `mrs %0, tpidr_el1` for `__aarch64__`).
- All x86_64 sources — unchanged.

---

### Task 1: Commit 1 — combined §A + §C prerequisite

**Files:**
- Modify: `kernel/Makefile` (aarch64 KERNEL_C_SOURCES add `percpu/percpu.c`)
- Modify: `kernel/include/percpu/percpu.h` (add `PERCPU_DATA_SIZE` macro)
- Modify: `kernel/arch/aarch64/head.S:312-324` (BSP TPIDR_EL1 setup)
- Modify: `kernel/arch/aarch64/head.S:640-680` (AP TPIDR_EL1 setup)
- Modify: `kernel/arch/aarch64/main.c` (BSP `percpu_install_gs` + `percpu_init`)
- Modify: `kernel/arch/aarch64/smp.c::secondary_idle` (AP `percpu_install_gs` + `percpu_init`)
- Modify: `kernel/arch/aarch64/gic.c:20-25` (`k_cpu_index` cast fix)

**Interfaces:**
- Produces: `percpu_data[NR_CPUS]` linked into aarch64 kernel (currently only in x86_64 whitelist).
- Produces: BSP `TPIDR_EL1 = &percpu_data[0]`; AP `TPIDR_EL1 = &percpu_data[cpu_id]`.
- Produces: `percpu_init(cpu, mpidr)` called for each CPU before first CNTP tick.

- [ ] **Step 1: Verify submodule state + baseline build**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
git submodule status | head -5
git log --oneline -3
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang SMP=1 aarch64-uefi 2>&1 | tail -5
```

Expected: 4 submodules with SHAs. HEAD at `15e1df9` (R2 spec). aarch64 build exits 0.

Verify `nm build/aarch64-clang/image/kernel.elf | grep percpu_data` returns ZERO hits — `percpu_data[]` is NOT linked into aarch64 today (it's only in `kernel/percpu/percpu.c`, which is NOT in the aarch64 whitelist per `kernel/Makefile:42-49`).

Verify `nm | grep idle_resume` returns ONE hit (single BSP-only entry — pre-Phase-2-#3).

- [ ] **Step 2: Add `PERCPU_DATA_SIZE` macro to `kernel/include/percpu/percpu.h`**

Open the file. Find the `typedef struct percpu { ... }` block (ends around line 50). Add the macro AFTER the `percpu_t` typedef:

```c
/* Phase 2 #3: numeric literal sizeof percpu_t. Computed by inspection
 * of the struct layout above. Keep in sync if fields are added.
 * As of the Phase 2 #3 change: self + need_resched + cpu_id +
 * arch_processor_id + online + scheduler_ok + *tss + *tss_hw +
 * tlb_wanted + tlb_ack + run_queue + min_vruntime + rq_lock +
 * arch_pad = 200 bytes (rounded up to 64-byte alignment per
 * kernel/arch/aarch64/percpu.c __attribute__((aligned(64)))).
 *
 * Verify with `nm | grep percpu_data` (size = NR_CPUS ×
 * PERCPU_DATA_SIZE ≈ 8 × 200 = 1600 bytes).
 *
 * NIT-1 (R3 review): must be numeric literal for asm use in
 * head.S:649 (cannot reference sizeof(percpu_t) from assembly). */
#define PERCPU_DATA_SIZE  200
```

(The exact size value depends on the actual `percpu_t` layout; adjust after verifying `nm` output. If `nm | grep percpu_data` reports size ≠ NR_CPUS × 200, recompute and update the macro.)

- [ ] **Step 3: Modify `kernel/Makefile`** — add `percpu/percpu.c` to aarch64 whitelist

Locate the `ifeq ($(ARCH),aarch64)` block (around lines 42-49). Edit to add the new entry:

```make
ifeq ($(ARCH),aarch64)
KERNEL_C_SOURCES := memory/pmm.c memory/pmm_arch.c \
                   percpu/percpu.c \
                   time/clocksource.c time/tick.c time/timer.c \
                   intr/softirq.c \
                   arch/aarch64/udivti3_stub.c arch/aarch64/subsys.c \
                   arch/aarch64/subsys_stub.c \
                   arch/aarch64/idle_resume_stub.c \
                   arch/aarch64/libc_stub.c
endif
```

(Insert `percpu/percpu.c` between `memory/pmm_arch.c` and `time/clocksource.c`.)

Do NOT touch the `ifeq ($(ARCH),x86_64)` block.

- [ ] **Step 4: Modify `kernel/arch/aarch64/head.S:312-324` (BSP TPIDR_EL1)**

Open the file. Locate the `setup_bsp_stack_and_tpidr` block (around lines 314-324). Use Edit to replace the body with:

```asm
setup_bsp_stack_and_tpidr:
    /* Phase 2 #3: TPIDR_EL1 = &percpu_data[0] (real percpu_t with
     * need_resched at offset 8), not &aarch64_boot_percpu[0]
     * (48-byte struct without need_resched). */
    ldr     x0, =percpu_data
    msr     tpidr_el1, x0

    /* Stack setup (unchanged from before this commit) */
    ldr     x1, =aarch64_boot_stacks
    mov     x2, #AARCH64_BOOT_STACK_SIZE
    add     x1, x1, x2
    mov     sp, x1
    ret
```

(Drops the `aarch64_boot_percpu.self` / `cpu_id = 0` writes — `percpu_init()` does those at runtime.)

- [ ] **Step 5: Modify `kernel/arch/aarch64/head.S:640-680` (AP TPIDR_EL1)**

Open the file. Locate the AP entry around line 649 (`msr tpidr_el1, x6` where `x6 = &aarch64_boot_percpu[cpu_id]`). Use Edit to replace the `msr tpidr_el1, x6` line with:

```asm
    /* Phase 2 #3: TPIDR_EL1 = &percpu_data[cpu_id] (real percpu_t).
     * x19 is cpu_id (verified above via [x6, #AARCH64_BOOT_CPU_ID_OFFSET]).
     * PERCPU_DATA_SIZE is a numeric literal in kernel/include/percpu/percpu.h
     * (assembly context cannot use sizeof(percpu_t)). */
    ldr     x7, =percpu_data
    mov     x8, #PERCPU_DATA_SIZE
    madd    x7, x19, x8, x7
    msr     tpidr_el1, x7
```

- [ ] **Step 6: Modify `kernel/arch/aarch64/main.c`** — BSP `percpu_install_gs` + `percpu_init`

Open the file. Locate the `#if defined(__aarch64__)` block (around lines 309-332, after Phase 2 #1's `arch_register_subsys()` + `subsys_init_phase(SUBSYS_PHASE_4)`). Add BSP `percpu_install_gs(0)` + `percpu_init(0, mpidr)` after the SUBSYS hook, BEFORE `arch_tick_start()`:

```c
#if defined(__aarch64__)
    /* Phase 2 #3: install real per-CPU data for BSP.
     * head.S:323 already set TPIDR_EL1 = &percpu_data[0];
     * percpu_install_gs re-confirms (idempotent) and percpu_init
     * populates self/cpu_id/arch_processor_id/online/rq_lock. */
    extern void percpu_install_gs(uint32_t cpu);
    extern void percpu_init(uint32_t cpu, uint32_t apic_id);
    uint32_t mpidr_bsp;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr_bsp));
    percpu_install_gs(0);
    percpu_init(0, mpidr_bsp);
#endif
```

(NIT-2: use inline asm `mrs xN, mpidr_el1` directly; do NOT define a `mpidr_el1_read()` helper.)

- [ ] **Step 7: Modify `kernel/arch/aarch64/smp.c::secondary_idle`** — AP `percpu_install_gs` + `percpu_init`

Open the file. Locate `secondary_idle` (around lines 205-235). Insert AP calls AFTER `gic_cpu_init()`, BEFORE the CNTP mask removal (which Commit 2 removes):

```c
void secondary_idle(uint32_t cpu_id)
{
    gic_cpu_init();
    /* Phase 2 #3: install real per-CPU data for AP.
     * head.S:649 already set TPIDR_EL1 = &percpu_data[cpu_id];
     * percpu_install_gs re-confirms (idempotent) and percpu_init
     * populates self/cpu_id/arch_processor_id/online/rq_lock. */
    extern void percpu_install_gs(uint32_t cpu);
    extern void percpu_init(uint32_t cpu, uint32_t apic_id);
    uint32_t mpidr_ap;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr_ap));
    percpu_install_gs(cpu_id);
    percpu_init(cpu_id, mpidr_ap);
    /* Phase 2 #3 (Commit 2): CNTP enable — line 210 mask removed */
    ...
}
```

Note: the existing CNTP mask line at line 210 is **NOT removed in this commit** (that's Commit 2). The "Commit 2" inline comment can be added in this commit as a forward reference, or in Commit 2 itself.

- [ ] **Step 8: Modify `kernel/arch/aarch64/gic.c:20-25`** — `k_cpu_index` cast fix

Open the file. Locate `k_cpu_index` (around lines 20-25). Use Edit to change the cast:

old_string:
```c
static uint32_t k_cpu_index(void)                      /* R2-6: TPIDR 槽读本核号 */
{
    uint64_t slot;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(slot));
    return ((volatile aarch64_boot_percpu_t *)(uintptr_t)slot)->cpu_id;
}
```

new_string:
```c
static uint32_t k_cpu_index(void)                      /* Phase 2 #3: TPIDR_EL1 → percpu_data[cpu] */
{
    uint64_t slot;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(slot));
    return ((volatile percpu_t *)(uintptr_t)slot)->cpu_id;
}
```

(The cast changes from `aarch64_boot_percpu_t *` to `percpu_t *` — TPIDR_EL1 destination is `&percpu_data[cpu_id]` after Commit 1.)

- [ ] **Step 9: Clean build**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
make PROFILE=aarch64-clang clean
make PROFILE=x86_64-clang kernel.bin 2>&1 | tail -5
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 SMP=1 aarch64-uefi 2>&1 | tail -10
```

Expected: BOTH builds succeed. `percpu_data[]` is now linked into aarch64 kernel (defined by `kernel/percpu/percpu.c`); `head.S` AP entry correctly computes `&percpu_data[cpu_id]`; `k_cpu_index()` reads `cpu_id` from `percpu_t`.

If aarch64 link error on duplicate `percpu_data` symbol: the Makefile change in Step 3 must be wrong (added `percpu/*.c` to BOTH x86_64 and aarch64 — but x86_64 already uses `percpu/*.c`). Verify only aarch64 block has the entry.

If `nm | grep percpu_data` shows size ≠ `NR_CPUS × PERCPU_DATA_SIZE`: recompute PERCPU_DATA_SIZE.

- [ ] **Step 10: RED — verify pre-state (QEMU 9/9 PASS, AP CNTP still masked)**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
grep -n "\[clocksource\]\|\[cntp\]\|\[tick\]" \
    test-results/aarch64-uefi-smp/$(ls -t test-results/aarch64-uefi-smp/ | head -1)/cpus-2-run-1.stdout.log | head -10
```

Expected: 9 case JSONs, all `"result":"PASS"`. `[tick] N` markers from BSP only (Commit 2 hasn't landed; AP CNTP still masked). Verify `nm | grep percpu_data` shows 1 symbol.

- [ ] **Step 11: Verify `k_cpu_index` is functional** (manual QEMU check)

```sh
cd /home/aagu/aarch64-timer-phase2-smp
# After build, check `k_cpu_index` reads the right per-CPU data:
nm build/aarch64-clang/image/kernel.elf | grep -E "percpu_data|k_cpu_index"
```

Expected: `percpu_data` symbol present; `k_cpu_index` is a text symbol pointing into `gic.c`.

- [ ] **Step 12: Commit**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
git add kernel/Makefile \
        kernel/include/percpu/percpu.h \
        kernel/arch/aarch64/head.S \
        kernel/arch/aarch64/main.c \
        kernel/arch/aarch64/smp.c \
        kernel/arch/aarch64/gic.c
git commit -m "feat(aarch64): install percpu_data[cpu] + fix k_cpu_index (Phase 2 #3 §A+§C)

Closes Phase 2 P2 follow-up #3 prerequisite (Phase 2 #2's
latent 'this_cpu()->need_resched writes to wrong struct'
corruption). AP CNTP enable (Phase 2 #3 §B) lands in Commit 2
only after this commit — flipping AP CNTP on without this
fix would turn latent corruption into ACTIVE corruption (AP
writes to aarch64_boot_percpu[cpu_id].cpu_id, corrupting
k_cpu_index()).

- kernel/Makefile: aarch64 KERNEL_C_SOURCES adds
  percpu/percpu.c (was previously x86_64-only; aarch64 build
  had no percpu_data symbol, hence this_cpu() dereferenced
  aarch64_boot_percpu which has no need_resched field).

- kernel/include/percpu/percpu.h: add PERCPU_DATA_SIZE macro
  (numeric literal 200 bytes for asm use in head.S:649; the
  sizeof(percpu_t) C expression can't be used in assembly).

- kernel/arch/aarch64/head.S:312-324: BSP TPIDR_EL1 now
  &percpu_data[0] (was &aarch64_boot_percpu[0]). Drop the
  boot-percpu self/cpu_id writes; percpu_init() does them.

- kernel/arch/aarch64/head.S:640-680: AP TPIDR_EL1 now
  &percpu_data[cpu_id] (was &aarch64_boot_percpu[cpu_id]).
  Use PERCPU_DATA_SIZE macro for offset calculation.

- kernel/arch/aarch64/main.c: BSP path calls
  percpu_install_gs(0) + percpu_init(0, mpidr_el1_read) after
  SUBSYS hook init, before arch_tick_start(). Inline asm
  'mrs xN, mpidr_el1' (no helper function — does not exist
  in codebase).

- kernel/arch/aarch64/smp.c:secondary_idle: AP path calls
  percpu_install_gs(cpu_id) + percpu_init(cpu_id, mpidr_el1_read)
  after gic_cpu_init(), before CNTP mask removal (Commit 2
  removes the mask). Inline asm 'mrs xN, mpidr_el1'.

- kernel/arch/aarch64/gic.c:20-25: k_cpu_index() cast changed
  from (aarch64_boot_percpu_t *) to (percpu_t *) — TPIDR_EL1
  destination is now &percpu_data[cpu_id] (not &aarch64_boot_percpu).

This is a combined commit because all six changes are required
together — splitting them creates broken intermediate states
(Commit 2 depends on this). Replaces latent Phase 2 #2
corruption with real per-CPU data wiring.

x86_64 byte-identity: no x86_64 sources modified (only
kernel/arch/aarch64/, kernel/Makefile's aarch64 block,
kernel/include/percpu/percpu.h which is arch-neutral).

Verification:
- nm | grep percpu_data: 1 symbol of size NR_CPUS *
  PERCPU_DATA_SIZE
- nm | grep idle_resume: 1 symbol (BSP-only, unchanged)
- QEMU 9/9 PASS (CPUs still single CNTP because §B not landed)
- [clocksource] + [tick] N markers still appear (BSP alone)

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

Verify commit landed with `git log --oneline -2`.

---

### Task 2: Commit 2 — AP CNTP enable (§B)

**Files:**
- Modify: `kernel/arch/aarch64/smp.c:210` — remove the CNTP mask line
- Modify: `kernel/arch/aarch64/smp.c:226` — remove the duplicate mask line

**Interfaces:**
- Modifies: AP `secondary_idle` (smp.c:205-235) — CNTP enable (line 210 + 226 mask removal).
- Effect: AP CNTP fires; AP `[tick] N` markers appear in `OS01_SELFTEST=1` builds.

- [ ] **Step 1: Verify pre-state**

```sh
grep -n "cntp_ctl_el0_write.*~UINT64_C(1)\|#if OS01_SELFTEST" \
    /home/aagu/aarch64-timer-phase2-smp/kernel/arch/aarch64/smp.c | head -10
```

Expected: shows line 210 (before bench loop) and line 226 (after bench loop) with `cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));`.

- [ ] **Step 2: Modify `kernel/arch/aarch64/smp.c:210`** — remove first CNTP mask

Open the file. Locate line 210. Use Edit:

old_string (the surrounding context including the line):
```c
    /* Test-only loss of ACK: the AP still reaches C initialization and
     * observes the persistent idle command after the BSP times out. */
    if (cpu_id != AARCH64_SMP_TEST_NO_ACK_CPU)
        boot_online_set(cpu_id);
    uint32_t command;
    do {
        command = boot_go_get(cpu_id);
        if (command == AARCH64_BOOT_GO_WAIT) arch_cpu_pause();
    } while (command == AARCH64_BOOT_GO_WAIT);
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);

    /* Includes a late AP: go=2 persists even if the BSP already resumed
     * ticks. APs keep their CNTP disabled; the only enabled banked lines
     * are SGI 0/1/2 (IPI + probe) and — for the BSP — the CNTP PPI. */
    cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));
```

new_string:
```c
    /* Test-only loss of ACK: the AP still reaches C initialization and
     * observes the persistent idle command after the BSP times out. */
    if (cpu_id != AARCH64_SMP_TEST_NO_ACK_CPU)
        boot_online_set(cpu_id);
    uint32_t command;
    do {
        command = boot_go_get(cpu_id);
        if (command == AARCH64_BOOT_GO_WAIT) arch_cpu_pause();
    } while (command == AARCH64_BOOT_GO_WAIT);
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);

    /* Phase 2 #3: AP CNTP enabled. Each AP receives its own CNTP
     * PPI tick. tick_handler() runs per-CPU via this_cpu() (now
     * pointing at percpu_data[cpu] from Commit 1). */
```

(Simply remove the `cntp_ctl_el0_write` line; the preceding comment block stays.)

- [ ] **Step 3: Modify `kernel/arch/aarch64/smp.c:226`** — remove duplicate mask

Locate line 226 (the second mask line). Use Edit to remove it:

old_string:
```c
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);

    /* Includes a late AP: go=2 persists even if the BSP already resumed
     * ticks. APs keep their CNTP disabled; the only enabled banked lines
     * are SGI 0/1/2 (IPI + probe) and — for the BSP — the CNTP PPI. */
    cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));
#if OS01_SELFTEST
```

new_string:
```c
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);

#if OS01_SELFTEST
```

(Removes the second `cntp_ctl_el0_write` line and the preceding comment that explained the mask; the `#if OS01_SELFTEST` block stays.)

- [ ] **Step 4: Clean build**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
make PROFILE=aarch64-clang clean
make PROFILE=x86_64-clang kernel.bin 2>&1 | tail -5
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 SMP=1 aarch64-uefi 2>&1 | tail -10
```

Expected: BOTH builds succeed. aarch64's `secondary_idle` no longer masks CNTP. AP CNTP fires.

- [ ] **Step 5: GREEN — verify QEMU 9/9 PASS with AP CNTP firing**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
```

Expected: 9 case JSONs, all `"result":"PASS"`. `[tick] N` markers — BSP alone produces ≥3 per case (gate preserved). AP `[tick] N` lines (selftest-only) appear from CPU 2/4 cases.

Verify markers per case:

```sh
cd /home/aagu/aarch64-timer-phase2-smp
grep -n "\[clocksource\]\|\[cntp\]\|\[tick\]" \
    test-results/aarch64-uefi-smp/$(ls -t test-results/aarch64-uefi-smp/ | head -1)/cpus-4-run-1.stdout.log | head -20
```

Expected: 3 `[clocksource]` markers, 1 `[cntp]` marker, ≥3 `[tick] N` markers (BSP alone). AP `[tick]` may appear if both BSP and AP write to global `g_ticks` (race is benign for ≥3 lines).

- [ ] **Step 6: x86_64 byte-identity**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
git diff --stat
make PROFILE=x86_64-clang test-build-contract-x86 2>&1 | tail -5
```

Expected: `git diff` shows only `kernel/arch/aarch64/smp.c` modified. x86_64 byte-identity preserved.

- [ ] **Step 7: Commit**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
git add kernel/arch/aarch64/smp.c
git commit -m "feat(aarch64): enable AP CNTP (Phase 2 #3 §B)

- kernel/arch/aarch64/smp.c:210 + 226: remove the
  cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1))
  masks in secondary_idle. APs now keep their CNTP enabled
  and receive their own CNTP PPI tick.

This commit MUST land after the Phase 2 #3 prerequisite
commit (percpu_data install + k_cpu_index fix); enabling AP
CNTP without the prerequisite would turn Phase 2 #2's
latent corruption (this_cpu()->need_resched writes to wrong
struct) into ACTIVE corruption (AP writes to
aarch64_boot_percpu[cpu_id].cpu_id, corrupting k_cpu_index).

Verification:
- QEMU 9/9 PASS (SMP=1/2/4 × 3)
- [tick] N markers still appear (BSP alone; APs contribute
  in selftest builds when arch_local_irq_enable() is unmasked)
- [clocksource] markers still appear (3 per case)
- x86_64 byte-identity preserved (no x86_64 sources modified)

Phase 2 P2 follow-up #3 closed. Remaining: #4 (__udivti3
hoist), #5 (-I libc/include policy).

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

Verify commit landed with `git log --oneline -2`.

---

### Task 3: Full regression + closure doc

**Files:**
- Create: `docs/aarch64-timer-phase2-smp-closure-2026-09-18.md`

**Interfaces:**
- (No new operations.)

- [ ] **Step 1: Full hosttest regression (5 binaries × 2 profiles)**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
for prof in x86_64-clang aarch64-clang; do
    echo "=== PROFILE=$prof ==="
    for t in test_gic_driver test_gic_probe test_gic_marker_lines test_clocksource test_lwip_rand; do
        echo "--- $t ---"
        (cd hosttests && make PROFILE=$prof OS01_PROFILE_FILE=$PWD/../mk/profiles/$prof.mk $t) 2>&1 | tail -2
    done
done
```

Expected: all 10 hosttests pass.

- [ ] **Step 2: Full QEMU regression**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
make PROFILE=aarch64-clang test-aarch64-uefi-smp 2>&1 | grep '"result"' | tail -9
make PROFILE=aarch64-clang test-aarch64-gic-spi 2>&1 | tail -5
```

Expected: 9/9 QEMU SMP PASS; SPI PASS.

- [ ] **Step 3: x86_64 byte-identity (4 tests)**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
make PROFILE=x86_64-clang test-user-canary 2>&1 | tail -5
make PROFILE=x86_64-clang test-kernel-selftest 2>&1 | tail -5
make PROFILE=x86_64-clang test-pmm-boot-reservation 2>&1 | tail -5
make PROFILE=x86_64-clang test-kernel-canary-contract 2>&1 | tail -5
```

Expected: all 4 pass.

- [ ] **Step 4: Write closure doc**

Create `docs/aarch64-timer-phase2-smp-closure-2026-09-18.md`:

```markdown
# AArch64 Per-CPU Timer / SMP Timer Phase 2 #3 — Closure Report

**Date**: 2026-09-18
**Branch**: `feat/aarch64-timer-phase2-smp` (based on `master @ 10fd3d2`)
**Commits**: 2 functional + 1 spec revisions + 1 closure doc

Spec revisions (3 commits):
- `44f4314` v1 spec (R1 REJECTED with 8 CRITICAL)
- `15e1df9` R2 spec (R2 rewrite — addresses 8 CRITICAL; R3 APPROVED-WITH-NITS)

Implementation (2 commits):
- TBD-1: combined §A + §C prerequisite (percpu_data install + k_cpu_index fix)
- TBD-2: §B AP CNTP enable

## Goal achieved

`percpu_data[cpu]` is wired on aarch64 (TPIDR_EL1 = `&percpu_data[cpu_id]`); `k_cpu_index()` reads `cpu_id` from real `percpu_t`; AP CNTP enabled (was Phase 1 GIC's deliberate simplification); Phase 2 #2's latent `tick_handler()` writes to per-CPU `need_resched` field correctly (was latent because TPIDR_EL1 previously pointed at `aarch64_boot_percpu[0]`).

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang SMP=1 aarch64-uefi` | exit 0 |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3) | **9/9 PASS** |
| `make PROFILE=aarch64-clang test-aarch64-gic-spi` | PASS |
| 3 `[clocksource]` markers per case before `[cntp]` | verified |
| ≥3 `[tick] N` markers per case (harness gate) | verified |
| `nm | grep percpu_data` shows 1 symbol | verified |
| 10 hosttests (5 binaries × 2 profiles) | all pass |
| `make PROFILE=x86_64-clang test-user-canary` | audit passed |
| `make PROFILE=x86_64-clang test-kernel-selftest` | 27/27 PASS |
| `make PROFILE=x86_64-clang test-pmm-boot-reservation` | 6/6 PASS |
| `make PROFILE=x86_64-clang test-kernel-canary-contract` | 5/5 PASS |

## Spec review trajectory (3 rounds)

R1: 8 CRITICAL (REJECTED) — spec v1 had fundamental design errors:
- CRITICAL-1: referenced nonexistent `kernel/include/arch/aarch64/percpu.h`
- CRITICAL-2: §B without §A turns latent corruption active
- CRITICAL-3: spec proposed NEW `kernel/arch/aarch64/percpu.c` that would duplicate symbol
- CRITICAL-4: missing `percpu_init()` calls
- CRITICAL-5: wrong line numbers
- CRITICAL-6/7: §C "framework ready" claim misleading
- CRITICAL-8: §D snippet left despite self-revocation

R2: major rewrite (APPROVED-WITH-NITs from R3):
- Correct file paths (no more nonexistent-file reference)
- §A and §C prerequisite combined commit (single commit guarantees order)
- Use existing `kernel/percpu/percpu.c` (no duplicate symbol)
- `percpu_install_gs(cpu_id)` + `percpu_init(cpu_id, mpidr)` calls in both BSP and AP paths
- Correct line numbers
- §C scoped down (framework readiness, not real per-CPU timer init)
- §D deferred entirely (single paragraph)

R3: APPROVED-WITH-NITS — 3 MEDIUM + 6 LOW NITs (PERCPU_DATA_SIZE macro, mpidr_el1 inline asm vs function, selftest guard policy, doc consistency). All NITs addressed in plan §Global Constraints.

## Latent corruption closed

Phase 2 #2 closure report §"Latent corruption acknowledged":
- `tick_handler()` writes `this_cpu()->need_resched = 1` → now writes to real `percpu_data[cpu].need_resched` (not `aarch64_boot_percpu[0].cpu_id`). CLOSED.
- `tick_handler()` writes `this_cpu()->watchdog_counter++` → now writes to real `percpu_data[cpu].watchdog_counter`. CLOSED.
- `set_softirq_status(TIMER_SIRQ)` → still sets the bit; cleared when `do_softirq` runs (dead code on aarch64; latent). Partially closed.

## Scope NOT done in Phase 2 #3 (deferred)

1. **Per-CPU `init_task[]` array** — APs still halt in `for (;;) arch_cpu_halt();` without per-CPU idle thread. Future spec.
2. **Per-CPU jiffies counters** — `jiffies` is single global; multi-CPU writes are racy (BSP produces ≥3 `[tick] N` so harness gate passes). Future spec.
3. **`__udivti3` hoist to `compiler_rt/`** — Phase 2 follow-up #4.
4. **`-I libc/include` policy cleanup** — Phase 2 follow-up #5.
5. **`arch_register_subsys_percpu()` on aarch64** — `subsys_percpu_table[]` stays empty (framework in place for future use).
```

Use Write tool.

- [ ] **Step 5: Commit closure doc**

```sh
cd /home/aagu/aarch64-timer-phase2-smp
git add docs/aarch64-timer-phase2-smp-closure-2026-09-18.md
git commit -m "docs(aarch64): Per-CPU timer / SMP timer Phase 2 #3 closure report

2 functional commits + 2 spec revisions + this closure doc:
- TBD-1: combined percpu_data install + k_cpu_index fix
- TBD-2: AP CNTP enable

Verification matrix: QEMU 9/9 PASS (SMP=1/2/4 × 3),
[clocksource] + [tick] N markers per case, nm |
grep percpu_data symbol present, 10 hosttests pass, 4 x86_64
regression targets pass. x86_64 byte-identity preserved.

3 spec review rounds (R1 8 CRITICAL, R2 major rewrite,
R3 APPROVED-WITH-NITS). R1-NITs addressed in plan:
PERCPU_DATA_SIZE macro, mpidr_el1 inline asm, selftest
guard policy.

Phase 2 P2 follow-up #3 closed. Remaining: #4 (__udivti3
hoist), #5 (-I libc/include policy).

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

Verify commit landed.

## Report

After completing all tasks, write a structured report.

---

## Self-Review

**1. Spec coverage** — every spec section maps to a task:
- §A wire percpu_data → Task 1 Steps 2-7 ✓
- §B AP CNTP enable → Task 2 Steps 2-3 ✓
- §C AP per-CPU timer dispatch → Task 1 Step 7 ✓
- §D deferred → Task 3 Step 4 (closure doc note) ✓

**2. Placeholder scan** — no "TBD", "TODO", "implement later", "similar to Task N" — all steps concrete.

**3. Type consistency** —
- `percpu_install_gs(uint32_t cpu)` — signature matches `kernel/percpu/percpu.c:8`.
- `percpu_init(uint32_t cpu, uint32_t apic_id)` — matches `kernel/percpu/percpu.c:13`.
- `k_cpu_index()` cast changed from `aarch64_boot_percpu_t *` to `percpu_t *` — consistent with Task 1's TPIDR_EL1 destination change.
- `PERCPU_DATA_SIZE` macro — verified numeric literal 200 (subject to verification after nm output).
- `cntp_ctl_el0_write(ctl)` / `cntp_ctl_el0_read()` — unchanged (both still used by smp.c after mask removal).

**4. R3 NITs addressed in plan §Global Constraints**:
- NIT-1: `PERCPU_DATA_SIZE` macro defined in `kernel/include/percpu/percpu.h` (Task 1 Step 2)
- NIT-2: `mpidr_el1_read()` not defined — inline asm used directly (Task 1 Steps 6-7)
- NIT-3: `#if OS01_SELFTEST` guard preserved around `arch_local_irq_enable()` (not changed in Task 1-2); AP `[tick] N` lines appear only in selftest builds (documented in Task 2 Step 5 + closure)

**5. Out-of-scope items** — Phase 2 follow-up #4 (`__udivti3` hoist), #5 (`-I libc/include`), per-CPU `init_task[]`, per-CPU jiffies — explicitly listed in closure doc.

Self-review: PASS. Ready for execution.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-09-18-aarch64-smp-timer-plan.md`. Spec R3 APPROVED-WITH-NITS.

Two execution options:
1. Subagent-Driven (recommended) — dispatch fresh subagent per task, review between tasks
2. Inline Execution — execute tasks in this session using `executing-plans`

Per project convention and user direction, recommend option 1.