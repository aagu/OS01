# AArch64 Per-CPU Timer / SMP Timer — Design (R2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.
>
> **R1 review**: sonnet ad1f4cdcb found 8 CRITICAL items. R2 rewrites the spec to address all of them. R3 (codex) will re-verify.

**Goal:** Close Phase 2 P2 follow-up item #3 from `docs/aarch64-timer-phase1-closure-2026-09-18.md` — wire `percpu_data[cpu]` so aarch64's `this_cpu()` returns the real `percpu_t *` (with `need_resched` at offset 8), enable CNTP on application processors (currently disabled by Phase 1 GIC `kernel/arch/aarch64/smp.c:210,226`), and dispatch per-CPU timer init. After this spec lands, aarch64 multi-core tick semantic is correct, AP CNTP fires, and `tick_handler()` runs per-CPU.

**Architecture — 4 changes in dependency order:**

1. **§A — Wire `percpu_data[cpu]` on aarch64**: aarch64's `this_cpu()` already returns `percpu_t *` correctly (verified — `kernel/include/percpu/percpu.h:66-69` + `kernel/include/arch/percpu.h:16-20` reading `TPIDR_EL1`). What's wrong is **what TPIDR_EL1 points at**: currently `head.S:319` (BSP) and `head.S:649` (AP) set `TPIDR_EL1 = &aarch64_boot_percpu[cpu_id]` (48-byte `aarch64_percpu_t` struct WITHOUT `need_resched`). The fix: switch to `&percpu_data[cpu_id]` (real `percpu_t` array from `kernel/percpu/percpu.c:5`).
2. **§B — AP CNTP enable** (HARD-DEPENDS on §A): remove `cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1))` masks at `smp.c:210,226`. AP CNTP fires. **`tick_handler()` writes to per-CPU `need_resched` correctly ONLY because §A made TPIDR_EL1 point at the real `percpu_data[cpu]`**. Without §A first, §B turns latent corruption into ACTIVE corruption (AP writes to `aarch64_boot_percpu[cpu_id].cpu_id`, corrupting `k_cpu_index()`).
3. **§C — Per-CPU timer dispatch (AP-side)**: each `secondary_idle` calls `percpu_install_gs(cpu_id)` (already exists per `kernel/percpu/percpu.c:8-11`; sets `TPIDR_EL1 = &percpu_data[cpu]`) and `percpu_init(cpu_id, mpidr)` (zeros `percpu_data[cpu]` + sets `cpu_id`/`self`/`rq_lock=1`). Then `subsys_init_percpu()` walks the (empty) `subsys_percpu_table[]` — no-op (framework is in place for future use; deferred registration).
4. **§D — per-CPU `init_task[]` array**: DEFERRED to future spec. Current `kernel/include/sched/task.h:241` `init_task[NR_CPUS] = {&init_task_union.task, 0}` has only BSP entry; APs halt in `for (;;) arch_cpu_halt();` (`smp.c:233`) without entering idle thread. This is acceptable for Phase 2 #3 — the main goal is per-CPU timer, not per-CPU scheduler integration.

## Context — why R2 rewrites §A

R1 finding CRITICAL-1: the spec referenced `kernel/include/arch/aarch64/percpu.h` — **this file does not exist**. Verified: `git -C /home/aagu/OS01 ls kernel/include/arch/aarch64/percpu.h` returns ENOENT. The actual file path is `kernel/include/arch/percpu.h` (arch-neutral, `#ifdef __aarch64__` branch already does `mrs %0, tpidr_el1`). The aarch64 `this_cpu()` already returns `percpu_t *` correctly. The latent corruption is NOT a `this_cpu()` type bug; it's a **TPIDR_EL1 destination** bug.

R1 finding CRITICAL-3: the spec said "NEW `kernel/arch/aarch64/percpu.c`" with `percpu_t percpu_data[NR_CPUS]` — but `kernel/percpu/percpu.c:5` ALREADY defines this symbol. Duplicate definition → link error. The fix: **add `kernel/percpu/percpu.c` to the aarch64 Makefile whitelist** (it's not in there today per `kernel/Makefile:42-49`).

R1 finding CRITICAL-4: missing `percpu_init(cpu, mpidr)` calls. R2 §C adds them in `secondary_idle` BEFORE the CNTP mask removal.

R1 finding CRITICAL-2: §B without §A turns latent corruption active. R2 makes §A a HARD PREREQUISITE of §B in the commit order (single combined commit, §A lands first, then §B).

R1 finding CRITICAL-5: line numbers wrong. R2 corrects: `smp.c:210` (before bench loop) and `smp.c:226` (after bench loop, before `arch_local_irq_enable()`).

R1 finding CRITICAL-6/7: §C's `subsys_init_percpu()` walk is empty on aarch64. R2 §C scopes down: the call is for framework readiness (deferred registration), not real per-CPU timer init. AP CNTP fires (from §B); each AP's `cntp_tick_handler` runs (registered once by BSP via `time.c:104`); `tick_handler()` runs per-CPU via `this_cpu()` (now correctly pointing at `percpu_data[cpu]`).

## Design

### A. Wire `percpu_data[cpu]` on aarch64

**Files:**
- Modify: `kernel/Makefile` (lines 42-49 aarch64 KERNEL_C_SOURCES whitelist) — ADD `percpu/percpu.c`.
- Modify: `kernel/arch/aarch64/head.S:319` (BSP `setup_bsp_stack_and_tpidr`) — change `ldr x0, =aarch64_boot_percpu` to `ldr x0, =percpu_data` + `msr tpidr_el1, x0` (after BSP `self` setup).
- Modify: `kernel/arch/aarch64/head.S:649` (AP entry) — change `msr tpidr_el1, x6` (currently `x6 = &aarch64_boot_percpu[cpu_id]`) to compute `x6 = &percpu_data[cpu_id]` BEFORE line 649.
- Modify: `kernel/arch/aarch64/main.c` — add `percpu_init(0, mpidr_el1_read())` call AFTER `percpu_install_gs(0)` (BSP path).
- Modify: `kernel/arch/aarch64/gic.c:20-25` `k_cpu_index()` — use `percpu_t *` not `aarch64_boot_percpu_t *` (TPIDR_EL1 destination changed).

**Detailed changes:**

#### kernel/Makefile (aarch64 KERNEL_C_SOURCES around lines 42-49)

Add `percpu/percpu.c` to the existing whitelist:

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

#### kernel/arch/aarch64/head.S:312-324 (BSP TPIDR_EL1 setup)

Replace:
```asm
setup_bsp_stack_and_tpidr:
    ldr     x0, =aarch64_boot_percpu
    str     x0, [x0, #AARCH64_BOOT_SELF_OFFSET]
    mov     x1, #0
    str     w1, [x0, #AARCH64_BOOT_CPU_ID_OFFSET]
    ...
    msr     tpidr_el1, x0
    ret
```

with:
```asm
setup_bsp_stack_and_tpidr:
    /* Phase 2 #3: TPIDR_EL1 = &percpu_data[0] (real percpu_t with
     * need_resched at offset 8), not &aarch64_boot_percpu[0] (48-byte
     * struct without need_resched). */
    ldr     x0, =percpu_data
    msr     tpidr_el1, x0

    /* Stack setup (unchanged from before this commit) */
    ldr     x1, =aarch64_boot_stacks
    mov     x2, #AARCH64_BOOT_STACK_SIZE
    add     x1, x1, x2
    mov     sp, x1
    ret
```

(Drop the `aarch64_boot_percpu` self-write and `cpu_id = 0` write — `percpu_init()` does those at runtime.)

#### kernel/arch/aarch64/head.S:649 (AP TPIDR_EL1)

Currently:
```asm
    ldr     x7, [x6, #AARCH64_BOOT_STACK_OFFSET]
    ...
    mov     sp, x7
    msr     tpidr_el1, x6
```

After (replace `msr tpidr_el1, x6` with computation from `x19` = `cpu_id`):

```asm
    /* Phase 2 #3: compute &percpu_data[cpu_id] BEFORE setting TPIDR_EL1.
     * x19 is cpu_id (verified above via [x6, #AARCH64_BOOT_CPU_ID_OFFSET]). */
    ldr     x7, =percpu_data
    mov     x8, #PERCPU_DATA_SIZE   /* sizeof(percpu_t) — defined in percpu.c */
    madd    x7, x19, x8, x7
    msr     tpidr_el1, x7
```

(`PERCPU_DATA_SIZE` may need to be a constant in `kernel/include/percpu/percpu.h`; if not, use `madd x7, x19, sizeof, ldr x7, =percpu_data_size` via literal pool.)

### B. AP CNTP enable

**Files:**
- Modify: `kernel/arch/aarch64/smp.c:210` and `:226` — remove the CNTP mask lines.

Currently:
```c
/* Includes a late AP: go=2 persists even if the BSP already resumed
 * ticks. APs keep their CNTP disabled; the only enabled banked lines
 * are SGI 0/1/2 (IPI + probe) and — for the BSP — the CNTP PPI. */
cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));   /* line 210 - REMOVE */
```

Replace with:
```c
/* Phase 2 #3: AP CNTP enabled. Each AP receives its own CNTP PPI
 * interrupt on each tick. tick_handler() runs per-CPU (per-CPU
 * jiffies counters are future work; current jiffies is a single
 * global — race is benign for ≥3 [tick] N harness gate). */
```

OR simply delete the line entirely.

Same fix at `smp.c:226` (the duplicate mask line after `smp_bench_iter`).

### C. Per-CPU timer dispatch (AP-side)

**Files:**
- Modify: `kernel/arch/aarch64/smp.c:secondary_idle` — add `percpu_install_gs(cpu_id)` + `percpu_init(cpu_id, mpidr_el1_read())` calls BEFORE the CNTP mask removal (if any) and BEFORE `arch_local_irq_enable()`.

Currently `secondary_idle` (smp.c:205-235):
```c
void secondary_idle(uint32_t cpu_id)
{
    gic_cpu_init();
    cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));   /* line 210 - REMOVE */
    ...
    if (command == AARCH64_BOOT_GO_TEST)
        smp_bench_iter(cpu_id, 1000000);
    cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));   /* line 226 - REMOVE */
    ...
    arch_local_irq_enable();
    ...
    for (;;) arch_cpu_halt();
}
```

Modify to:
```c
void secondary_idle(uint32_t cpu_id)
{
    gic_cpu_init();
    /* Phase 2 #3: install real per-CPU data (percpu_data[cpu]) */
    extern void percpu_install_gs(uint32_t cpu);
    extern void percpu_init(uint32_t cpu, uint32_t apic_id);
    uint32_t mpidr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
    percpu_install_gs(cpu_id);
    percpu_init(cpu_id, mpidr);
    /* AP CNTP now enabled (line 210 removed) */
    ...
    arch_local_irq_enable();
    ...
}
```

### D. per-CPU `init_task[]` array — DEFERRED

Document as Phase 2 #3 follow-up scope-creep. Out of this spec.

## Non-goals (out of scope)

1. **Per-CPU jiffies counters** — current `jiffies` is a single global (with `__atomic` issues from multiple CPUs writing non-atomically). Phase 2 #4 if needed. Latent for now; ≥3 `[tick] N` lines still pass (BSP alone produces enough).
2. **Per-CPU scheduler integration** — `init_task[]` array expansion, per-CPU idle threads.
3. **`__udivti3` hoist to `compiler_rt/`** — Phase 2 follow-up #4.
4. **`-I libc/include` policy cleanup** — Phase 2 follow-up #5.
5. **Replacing `subsys_stub.c` with real `kernel/subsys/subsys.c`** — depends on `serial_printk` etc.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| BSP TPIDR_EL1 set BEFORE `percpu_init(0, mpidr)` → `percpu_data[0].self = 0`, `cpu_id = 0`, `online = 0` | High | §C BSP path explicitly calls `percpu_init(0, mpidr_el1_read())` AFTER `arch_set_percpu_base(&percpu_data[0])` (via `percpu_install_gs(0)`) |
| §B without §A turns latent corruption into ACTIVE corruption | Critical | §A HARD PREREQUISITE — single combined commit or §A first commit |
| `PERCPU_DATA_SIZE` constant missing (head.S assembly can't multiply without it) | Medium | Add `PERCPU_DATA_SIZE` macro to `kernel/include/percpu/percpu.h` (mirror `sizeof(percpu_t)`); or use `madd x7, x19, #(sizeof(.L_percpu_size)), x6` via assembly macro |
| BSP `head.S:319` no longer writes `aarch64_boot_percpu.self = &aarch64_boot_percpu[0]` (which was needed by `k_cpu_index()` in `gic.c:20-25`) | Medium | `k_cpu_index()` uses `__asm__ mrs %0, tpidr_el1` then reads `(aarch64_boot_percpu_t *)tpidr → cpu_id` (offset 8). After §A, TPIDR_EL1 = `&percpu_data[0]` which is `percpu_t *` not `aarch64_boot_percpu_t *` — `cpu_id` field is at different offset. **§C must update `kernel/arch/aarch64/gic.c:20-25` `k_cpu_index()`** to use `percpu_t *` not `aarch64_boot_percpu_t *` |
| AP CNTP race vs BSP CNTP | Low | Each AP has its own CNTP; tick rates independent. No shared state |
| Cross-CPU IPI tick broadcast | N/A | Out of scope |
| `[tick] N` harness gate (≥3 lines per case) | Low | BSP alone produces ≥3 `[tick] N` per second; AP ticks add to the count not subtract |

## Verification

### Unit / host tests

- Existing 5 hosttests (x86_64 + aarch64) must continue to pass byte-for-byte.
- `nm | grep percpu_data` shows 1 symbol (`percpu_data`) of size `NR_CPUS × sizeof(percpu_t)` ≈ 8 × ~150 bytes ≈ 1200 bytes.

### QEMU end-to-end

- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3): **9/9 PASS**.
- 3 `[clocksource]` markers + ≥3 `[tick] N` markers per case (harness gate preserved).
- NEW for Phase 2 #3: SMP=2/4 cases should now show `[tick] N` lines from BOTH BSP AND APs (multiple CPUs contributing to the same global `g_ticks` counter; race is benign for ≥3 lines).
- x86_64 byte-identity preserved (10 hosttests + 4 x86_64 regression targets).

### Build verification

- aarch64 build: clean (no `percpu_data` duplicate symbol).
- x86_64 build: clean (no `percpu_data` duplicate — `kernel/percpu/percpu.c` is unchanged).

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #3** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 3. Timer per-CPU affinity / SMP timer

After this spec lands, AP CNTP fires, `tick_handler()` runs per-CPU with `this_cpu()` pointing at real `percpu_data[cpu]`, and the Phase 2 #2 latent corruption is closed.

## Subagent-driven plan (to be written via writing-plans skill)

Estimated commit count: 2-3 functional + 1 docs.

1. **Commit 1** (combined §A + §B + §C prerequisite): add `percpu/percpu.c` to aarch64 whitelist + modify `head.S:319,649` + add `percpu_install_gs`/`percpu_init` calls in `smp.c::secondary_idle` + modify `kernel/arch/aarch64/gic.c::k_cpu_index()` to use `percpu_t*`. Single commit because all parts are required together — splitting them creates intermediate broken states.
2. **Commit 2** (B's CNTP enable): remove the CNTP mask lines at `smp.c:210,226`. Must land AFTER Commit 1.
3. **Commit 3** (optional fixup): any issues discovered during QEMU E2E (e.g., additional percpu_install_gs calls, or `arch_register_subsys_percpu` stub).

Each commit RED→GREEN→QEMU 9/9→next.

This is Phase 2 P2 follow-up #3; #4 (`__udivti3` hoist) and #5 (`-I libc/include` policy) are separate specs.