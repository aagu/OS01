# Unified SMP Boot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unify x86_64 and aarch64 SMP AP bring-up behind a single `arch_smp_boot_aps(cpu_count, entry, per_cpu_data)` facade, a shared `kernel/core/smp.c` BSP orchestrator + AP C lifecycle state machine, and a uniform Fail-Closed policy across both architectures.

**Architecture:** Introduce a zero-dependency arch-neutral facade `kernel/include/arch/smp.h` and a shared `kernel/core/smp.c` that owns `arch_boot_aps()` and `smp_ap_common_entry()`. Both architectures implement the strong overrides (`arch_smp_boot_aps`, `arch_smp_last_status`, and the four AP lifecycle hooks), pass `(cpu_id, &percpu_data[cpu_id])` from their assembly entries (`trampoline.S` / `secondary_entry.S`), and report any failure back to `kernel/core/smp.c` for uniform `[boot-1] FATAL` + `boot_failure_stop("smp", ...)` halt.

**Tech Stack:** C11 (freestanding kernel + hosttests), x86_64 GAS / AArch64 GAS assembly, GNU Make, Python unittest / QEMU acceptance harnesses.

**Spec:** `docs/superpowers/specs/2026-11-19-unified-smp-boot-design.md`

## Global Constraints

- `kernel/include/arch/smp.h` MUST only `#include <stdint.h>` and forward-declare `struct percpu;` so it compiles standalone under `-ffreestanding -Ikernel/include` for both `x86_64` and `aarch64`.
- `percpu_t` layout MUST NOT change (`PERCPU_DATA_SIZE == 144`); x86_64 `trampoline_data_t` MUST remain 40 bytes; aarch64 `aarch64_boot_percpu_t` MUST remain 48 bytes (reusing offset 40 as `entry`).
- All runtime logs added/modified in `kernel/core/smp.c` MUST use `log_info` / `log_err` with stack-formatted decimal strings passed via `%s` (AArch64 mini-formatter compatibility; no heap allocation, no `%u` dependency).
- `kernel/core/smp.c` MUST be explicitly listed in the aarch64 `KERNEL_C_SOURCES` whitelist in `kernel/Makefile` (x86_64 picks it up via `$(wildcard core/*.c)`).
- New host test `hosttests/cases/test_smp_boot_unified.c` MUST carry `@os01-test` metadata and regenerate catalog artifacts via `make regen-test-catalog`.
- Do NOT move aarch64 AP idle-task creation or `scheduler_ok` publication out of stage 9 (`ap_sched.c`).

## Review Focus

- **Single-CPU (`num_cpus == 1`) boot on both architectures:** `arch_smp_boot_aps()` must succeed with `online == 1, online_mask == 1` without touching PSCI or sending SIPI, and `arch_boot_aps()` must still emit `[boot-1] complete discovered=1 ready=1` and call `arch_smp_post_boot_hook(1)` + `subsys_prepare_cpus()` (pinned in Task 2, Task 3, Task 4, Task 6).
- **`arch_smp_last_status()` returning `NULL` or mismatched `online_mask` when `rc == 0`:** public orchestrator must fail-closed with `SMP_STAGE_INVARIANT` instead of dereferencing `NULL` or accepting a partial mask (pinned in Task 2).
- **Non-global `per_cpu_data` pointer passed to `arch_smp_boot_aps()`:** both x86_64 and aarch64 must reject `per_cpu_data != percpu_data` with `-EINVAL` and `SMP_STAGE_INVARIANT` because assembly/runtime paths index global `percpu_data[]` (pinned in Task 3 and Task 4).
- **AArch64 `secondary_entry.S` AAPCS64 preservation across `bl aarch64_boot_install_translation`:** entry pointer must be loaded and zero-checked into callee-saved `x20` *before* the `bl`, never reloaded through caller-saved `x6` afterward (pinned in Task 4).
- **Source-audit hosttests & harness tests (`test_double_state_publish`, `test_boot_contract_softirq`, `test_x86_ipi_ready_publish`, `test_boot_contract_cpu_prepare`, `test_x86_intr_fault`, `test_aarch64_ap_boot_contract`, `test_aarch64_harness`):** all 7 existing static/source-scanning tests must be updated alongside the refactor so `make test-host` and `make test-static` stay green (pinned in Task 5).

---

### Task 1: Arch-Neutral SMP Facade Header & Standalone Compile Gate

**Files:**
- Create: `kernel/include/arch/smp.h`
- Modify: `qemutests/test_boot_facade_headers.py`

**Interfaces:**
- Produces (`kernel/include/arch/smp.h`):
  - `typedef void (*arch_smp_ap_entry_t)(uint32_t cpu_id, struct percpu *cpu) __attribute__((noreturn));`
  - `enum smp_boot_stage { SMP_STAGE_INVARIANT = 0, SMP_STAGE_PREPARE, SMP_STAGE_ACTIVATE, SMP_STAGE_ONLINE, SMP_STAGE_SCHEDULER };`
  - `struct arch_smp_boot_status { uint32_t requested; uint32_t online; uint64_t online_mask; uint32_t failed_cpu; enum smp_boot_stage failed_stage; int32_t arch_rc; };`
  - `int arch_smp_boot_aps(uint32_t cpu_count, arch_smp_ap_entry_t entry, struct percpu *per_cpu_data);`
  - `const struct arch_smp_boot_status *arch_smp_last_status(void);`
  - `void smp_ap_common_entry(uint32_t cpu_id, struct percpu *cpu) __attribute__((noreturn));`
  - `void arch_smp_ap_cpu_init(uint32_t cpu_id, struct percpu *cpu);`
  - `void arch_smp_ap_post_online(uint32_t cpu_id, struct percpu *cpu);`
  - `void arch_smp_ap_publish_ipi_ready(uint32_t cpu_id, struct percpu *cpu);`
  - `void arch_smp_ap_enter_idle(uint32_t cpu_id, struct percpu *cpu) __attribute__((noreturn));`
  - `void arch_smp_post_boot_hook(uint32_t online_cpus);`

- [ ] **Step 1: Add failing standalone header compile tests to `qemutests/test_boot_facade_headers.py`**

Add `test_arch_smp_header_x86_64` and `test_arch_smp_header_aarch64` compiling a snippet that includes `<arch/smp.h>`, declares a `static` function pointer of type `arch_smp_ap_entry_t` pointing to `smp_ap_common_entry`, and asserts `SMP_STAGE_INVARIANT == 0` and `SMP_STAGE_SCHEDULER == 4` using `self._run_clang("x86_64-none-elf", ..., ["-fsyntax-only", "-Werror"])` and `self._run_clang("aarch64-none-elf", ..., ["-fsyntax-only", "-Werror"])` (matching existing target triples in `test_boot_facade_headers.py`).

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m unittest qemutests.test_boot_facade_headers -v`
Expected: FAIL (`'arch/smp.h' file not found`).

- [ ] **Step 3: Create `kernel/include/arch/smp.h`**

Write `kernel/include/arch/smp.h` matching Spec §2.1 verbatim (only `#include <stdint.h>`, `struct percpu;` forward declaration, all types and prototypes listed in Interfaces above).

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m unittest qemutests.test_boot_facade_headers -v`
Expected: All tests in `BootFacadeHeadersTests` PASS.

- [ ] **Step 5: Commit**

```bash
git add kernel/include/arch/smp.h qemutests/test_boot_facade_headers.py
git commit -m "feat(smp): add arch-neutral arch/smp.h facade and standalone header gate"
```

---

### Task 2: Public SMP Orchestrator & AP State Machine (`kernel/core/smp.c`) + Host Unit Test

**Files:**
- Create: `kernel/core/smp.c`
- Create: `hosttests/mock/smp_boot_unified_runtime.h`
- Create: `hosttests/cases/test_smp_boot_unified.c`
- Modify: `hosttests/mk/sched.mk`
- Modify: `kernel/Makefile` (temporarily `filter-out core/smp.c` on x86_64 until Task 3 so every commit remains linkable; aarch64 whitelist addition happens in Task 4)
- Generated (via `make regen-test-catalog`): `config/test-catalog.json`, `mk/generated/*`

**Interfaces:**
- Consumes: `<arch/smp.h>`, `<arch/boot.h>`, `<arch/irq.h>`, `<arch/barrier.h>`, `<core/boot_failure.h>`, `<percpu/percpu.h>`, `<subsys/subsys.h>`, `<log/log.h>`, `<errno.h>`
- Produces:
  - `void arch_boot_aps(void)` (single public definition in `kernel/core/smp.c`)
  - `void smp_ap_common_entry(uint32_t cpu_id, struct percpu *cpu) __attribute__((noreturn))`
  - `__attribute__((weak)) void arch_smp_post_boot_hook(uint32_t online_cpus)` (default no-op)

- [ ] **Step 1: Create preinclude mock header, write failing host test `hosttests/cases/test_smp_boot_unified.c`, and wire `LINK_GROUP_smp-boot-unified` in `hosttests/mk/sched.mk`**

Create `hosttests/mock/smp_boot_unified_runtime.h` (preincluded via `-include` when compiling `smp_boot_unified_production.o`):
- Pre-defines `_ARCH_IRQ_H` and declares `void arch_local_irq_enable(void);` as a traceable extern function (avoiding the `static inline` `sti` / no-op in `kernel/include/arch/irq.h`).
- Pre-defines `_ARCH_BARRIER_H` and defines `void smp_test_arch_mb(void); #define arch_mb() smp_test_arch_mb()`.

Create `hosttests/cases/test_smp_boot_unified.c` with `@os01-test` header (`id: test_smp_boot_unified`, `layer: host`, `subsystem: boot`, `timing: strict`, `tier: pr`, `link: smp-boot-unified`, `scenarios: []`). Explicitly define `int g_log_level = 6;`, `percpu_t percpu_data[NR_CPUS];`, and `uint32_t num_cpus;`, plus mocks for `boot_failure_stop` (using `setjmp`/`longjmp` to capture stage, component, rc, cpu), `arch_smp_boot_aps`, `arch_smp_last_status`, `arch_smp_ap_cpu_init`, `arch_smp_ap_post_online`, `arch_smp_ap_publish_ipi_ready`, `arch_smp_ap_enter_idle` (via `longjmp`), `arch_smp_post_boot_hook`, `subsys_prepare_cpus`, `arch_local_irq_enable`, `smp_test_arch_mb`, and `_log_info_impl`/`_log_err_impl`.

Cover these exact test cases:
1. `test_precondition_rejects_invalid_num_cpus` (`num_cpus == 0` and `num_cpus == NR_CPUS + 1` → `[boot-1] FATAL cpu=0 stage=invariant` + `boot_failure_stop("smp", "invariant", -EINVAL, -1, 0)`; `arch_smp_boot_aps` not called).
2. `test_precondition_rejects_bsp_offline` (`percpu_data[0].online == 0` → `SMP_STAGE_INVARIANT`).
3. `test_precondition_rejects_dirty_ap` (`percpu_data[2].online == 1` or `percpu_data[2].scheduler_ok == 1` → `failed_cpu == 2`, `stage=invariant`).
4. `test_single_cpu_boot_succeeds` (`num_cpus == 1`, `online == 1`, `online_mask == 1` → emits `[boot-1] complete discovered=1 ready=1`, calls `arch_smp_post_boot_hook(1)` then `subsys_prepare_cpus()`).
5. `test_multicore_boot_order_and_args` (`num_cpus == 4` → verifies `arch_smp_boot_aps(4, smp_ap_common_entry, percpu_data)` arguments, emits `[boot-1] complete discovered=4 ready=4`, calls `arch_smp_post_boot_hook(4)` strictly before `subsys_prepare_cpus()`).
6. `test_postcondition_rejects_null_status_or_mask_mismatch` (`rc == 0` with `st == NULL`, `st->online != num_cpus`, or `st->online_mask != expected_mask` → Fail-Closed with `stage=invariant`, `-EIO`, no `post_boot_hook` / `subsys_prepare_cpus`).
7. `test_fail_closed_propagates_all_stages` (test `SMP_STAGE_PREPARE` / `-ENOMEM`, `SMP_STAGE_ACTIVATE` / `-ENODEV`, `SMP_STAGE_ONLINE` / `-ETIMEDOUT`, `SMP_STAGE_SCHEDULER` / `-ETIMEDOUT` on cpu 1 → verifies `[boot-1] FATAL cpu=1 stage=<name>` + `boot_failure_stop("smp", "<name>", rc, -1, 1)` and neither `post_boot_hook` nor `subsys_prepare_cpus` runs).
8. `test_ap_common_entry_strict_order` (drives `smp_ap_common_entry(1, &percpu_data[1])`; asserts trace order `cpu_init` (with `online==0`, `irq==0`) → `post_online` (with `online==1`, `irq==0`) → `irq_enable` → `arch_mb` → `publish_ipi_ready` (with `online==1`, `irq==1`) → `enter_idle`).

In `hosttests/mk/sched.mk`, follow the generated link-group convention (do NOT hand-write `test_smp_boot_unified.o`/`.elf` rules, which `scan_tests.py` generates into `mk/generated/test-link-rules.mk`):
- Compile `$(TEST_BLD)/smp_boot_unified_production.o` from `$(TESTS_DIR)/kernel/core/smp.c` with `-include $(TEST_MOCK)/smp_boot_unified_runtime.h` and `$(BOOT4_HOST_CFLAGS)`.
- Define `LINK_CFLAGS_smp-boot-unified = $(BOOT4_HOST_CFLAGS)`.
- Define `LINK_GROUP_smp-boot-unified := $(TEST_BLD)/smp_boot_unified_production.o`.
- Define `.PHONY: test_smp_boot_unified` depending on `$(TEST_BLD)/test_smp_boot_unified.elf` and running `$<`.

Run `make regen-test-catalog`.

- [ ] **Step 2: Run test to verify it fails**

Run: `OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test_smp_boot_unified`
Expected: FAIL (`kernel/core/smp.c` does not exist).

- [ ] **Step 3: Implement `kernel/core/smp.c` (with temporary x86_64 `filter-out` for bisectability)**

Implement `kernel/core/smp.c` per Spec §3:
- Stack-based `format_u32()` helper (no heap, `%s`-only formatting for `log_info` / `log_err`).
- `smp_stage_name()` mapping all 5 `enum smp_boot_stage` values.
- `smp_boot_fail(uint32_t cpu, enum smp_boot_stage stage, int rc)` (`noreturn`) emitting `[boot-1] FATAL cpu=%s stage=%s\n` and calling `boot_failure_stop("smp", smp_stage_name(stage), rc, -1, (int)cpu)`.
- `__attribute__((weak)) void arch_smp_post_boot_hook(uint32_t online_cpus) { (void)online_cpus; }`
- `void arch_boot_aps(void)` performing precondition checks, calling `arch_smp_boot_aps(num_cpus, smp_ap_common_entry, percpu_data)`, checking `rc != 0` and `st != NULL && st->requested == num_cpus && st->online == num_cpus && st->online_mask == expected_mask` (handling `num_cpus == 64u`), emitting `[boot-1] complete discovered=%s ready=%s\n`, then calling `arch_smp_post_boot_hook(num_cpus)` and `subsys_prepare_cpus()`.
- `__attribute__((noreturn)) void smp_ap_common_entry(uint32_t cpu_id, struct percpu *cpu)` executing the 5-step sequence in Spec §3.3 (`arch_smp_ap_cpu_init` → `__atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE)` → `arch_smp_ap_post_online` → `arch_local_irq_enable(); arch_mb();` → `arch_smp_ap_publish_ipi_ready` → `arch_smp_ap_enter_idle`).

Temporarily exclude `core/smp.c` via `$(filter-out core/smp.c, ...)` in the x86_64 `KERNEL_C_SOURCES` wildcard in `kernel/Makefile` (removed in Task 3 when x86_64 removes its legacy `arch_boot_aps()`).

- [ ] **Step 4: Run host unit test & catalog drift check**

Run: `OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test_smp_boot_unified && make check-test-catalog`
Expected: All 8 test cases in `test_smp_boot_unified` PASS; `check-test-catalog` exits 0.

- [ ] **Step 5: Commit**

```bash
git add kernel/core/smp.c kernel/Makefile hosttests/mock/smp_boot_unified_runtime.h hosttests/cases/test_smp_boot_unified.c hosttests/mk/sched.mk config/test-catalog.json mk/generated/
git commit -m "feat(smp): add unified core/smp.c orchestrator, AP state machine, and hosttests"
```

---

### Task 3: x86_64 SMP Backend Adaptation

**Files:**
- Modify: `kernel/arch/x86_64/trampoline.S:86-93`
- Modify: `kernel/arch/x86_64/smp/smp.c`
- Modify: `kernel/arch/x86_64/smp/boot.c`
- Modify: `kernel/include/arch/x86_64/smp_boot.h`
- Modify: `kernel/Makefile` (remove temporary x86_64 `filter-out core/smp.c` added in Task 2)
- Delete: `kernel/include/core/smp.h`

**Interfaces:**
- Consumes: `<arch/smp.h>` from Task 1; `kernel/core/smp.c` (`arch_boot_aps` / `smp_ap_common_entry`) from Task 2
- Produces (x86_64 strong overrides): `arch_smp_boot_aps`, `arch_smp_last_status`, `arch_smp_ap_cpu_init`, `arch_smp_ap_post_online`, `arch_smp_ap_publish_ipi_ready`, `arch_smp_ap_enter_idle`

- [ ] **Step 1: Update `kernel/arch/x86_64/trampoline.S` to pass `(cpu_id, gs_base)`**

Before `call *%rax` in `trampoline_long`, load:
```asm
    movl    trampoline_data + 36(%rip), %edi
    movq    trampoline_data + 8(%rip),  %rsi
    movq    trampoline_data + 24(%rip), %rax
    call    *%rax
```
Keep `trampoline_data` layout unchanged (40 bytes).

- [ ] **Step 2: Refactor `kernel/arch/x86_64/smp/smp.c` to implement `arch/smp.h` strong overrides**

- Include `<arch/smp.h>` and `<errno.h>`.
- Add `static struct arch_smp_boot_status x86_smp_status;` + helper `x86_smp_fail(uint32_t cpu, enum smp_boot_stage stage, int32_t arch_rc, int ret)` + `const struct arch_smp_boot_status *arch_smp_last_status(void)`.
- Implement `int arch_smp_boot_aps(uint32_t cpu_count, arch_smp_ap_entry_t entry, struct percpu *per_cpu_data)`:
  - Validate `!entry || per_cpu_data != percpu_data || cpu_count == 0 || cpu_count > NR_CPUS` → `x86_smp_fail(0, SMP_STAGE_INVARIANT, -EINVAL, -EINVAL)`.
  - Initialize `x86_smp_status = { .requested = cpu_count, .online = 1, .online_mask = 1ULL }`.
  - Keep `boot1_fault_variant_identity()`, `ipi_init()`, `ipi_ready_publish_bsp()`, trampoline copy, per-AP loop (`i = 1 .. cpu_count - 1`) with `tdata->entry = (uint64_t)(uintptr_t)entry`, INIT-SIPI-SIPI, bounded `online` wait (`-ETIMEDOUT` / `SMP_STAGE_ONLINE`), TSC sync (advisory timeout preserved), bounded `scheduler_ok` wait (`-ETIMEDOUT` / `SMP_STAGE_SCHEDULER`), post-wait `online && ipi_ready` check (`-EIO` / `SMP_STAGE_INVARIANT`), and `[boot-1] ready cpu=%u online=1 ipi_ready=1 scheduler_ok=1\n`.
  - Remove local `boot_fatal_cpu()` and `[boot-1] complete` print (now owned by `kernel/core/smp.c`).
- Split `ap_entry()` into the 4 strong-override hooks per Spec §4.1.2:
  - `arch_smp_ap_cpu_init(uint32_t cpu_id, struct percpu *cpu)` (ends with `boot1_fault_before_online(cpu_id)`).
  - `arch_smp_ap_post_online(uint32_t cpu_id, struct percpu *cpu)` (TSC handshake).
  - `arch_smp_ap_publish_ipi_ready(uint32_t cpu_id, struct percpu *cpu)` (`ipi_ready_publish_ap(cpu_id)` + `boot1_fault_after_ipi_ready(cpu_id)`).
  - `arch_smp_ap_enter_idle(uint32_t cpu_id, struct percpu *cpu)` (publishes `scheduler_ok = 1` release-store + `hlt`/`need_resched`/`schedule()` loop).

- [ ] **Step 3: Clean up `kernel/arch/x86_64/smp/boot.c`, `kernel/include/arch/x86_64/smp_boot.h`, `kernel/Makefile`, and remove `kernel/include/core/smp.h`**

- Remove `x86_64_boot_aps()` and `arch_boot_aps()` from `kernel/arch/x86_64/smp/boot.c` (and remove stale `#include <core/smp.h>` and `subsys_prepare_cpus` header comment reference).
- Remove `x86_64_boot_aps()` declaration from `kernel/include/arch/x86_64/smp_boot.h`.
- Remove the temporary x86_64 `filter-out core/smp.c` from `kernel/Makefile` so `kernel/core/smp.c` links into x86_64.
- Delete `kernel/include/core/smp.h`.

- [ ] **Step 4: Build x86_64 kernel to verify clean compile & link**

Run: `make kernel.bin`
Expected: Builds cleanly with zero warnings/errors and no duplicate `arch_boot_aps` symbol.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/trampoline.S kernel/arch/x86_64/smp/smp.c kernel/arch/x86_64/smp/boot.c kernel/include/arch/x86_64/smp_boot.h kernel/Makefile
git rm kernel/include/core/smp.h
git commit -m "feat(x86_64): adapt SMP trampoline and AP bring-up to unified arch/smp.h"
```

---

### Task 4: AArch64 SMP Backend Adaptation (Fail-Closed & AAPCS64-Safe Entry)

**Files:**
- Modify: `kernel/Makefile:85-92` (explicitly add `core/smp.c` to aarch64 `KERNEL_C_SOURCES`)
- Modify: `kernel/include/arch/aarch64/boot_offsets.h:15`
- Modify: `kernel/include/arch/aarch_percpu.h:30,44,67`
- Modify: `kernel/arch/aarch64/boot/secondary_entry.S:56-126`
- Modify: `kernel/arch/aarch64/smp/smp.c`
- Modify: `kernel/include/arch/aarch64/smp.h`
- Modify: `kernel/arch/aarch64/boot/main.c:641-685,724-729`

**Interfaces:**
- Consumes: `<arch/smp.h>` from Task 1; `kernel/core/smp.c` from Task 2
- Produces (aarch64 strong overrides): `arch_smp_boot_aps`, `arch_smp_last_status`, `arch_smp_ap_cpu_init`, `arch_smp_ap_post_online`, `arch_smp_ap_publish_ipi_ready`, `arch_smp_ap_enter_idle`, `arch_smp_post_boot_hook`

- [ ] **Step 1: Reuse offset 40 as `entry` in `boot_offsets.h` and `aarch_percpu.h`**

- In `kernel/include/arch/aarch64/boot_offsets.h`, rename `AARCH64_BOOT_RESERVED_OFFSET` to `AARCH64_BOOT_ENTRY_OFFSET` (`40`).
- In `kernel/include/arch/aarch_percpu.h`, rename `uint64_t reserved;` to `uint64_t entry;`, update `BOOT_OFFSET_ASSERT(entry, AARCH64_BOOT_ENTRY_OFFSET)`, and update `int aarch64_smp_publish_boot(const struct aarch64_topology *topology, uint64_t entry_va);`.

- [ ] **Step 2: Update `kernel/arch/aarch64/boot/secondary_entry.S` with AAPCS64 callee-saved `x20`**

- During slot validation (before `bl aarch64_boot_install_translation`), load and validate entry into `x20`:
  ```asm
      ldr     x20, [x6, #AARCH64_BOOT_ENTRY_OFFSET]
      cbz     x20, secondary_panic
  ```
- After MMU and `exception_vectors` are active, replace hardcoded `=secondary_idle` call with:
  ```asm
      mov     x0, x19
      mrs     x1, tpidr_el1
      blr     x20
  ```

- [ ] **Step 3: Refactor `kernel/arch/aarch64/smp/smp.c`, `kernel/include/arch/aarch64/smp.h`, and `kernel/arch/aarch64/boot/main.c`**

- In `kernel/arch/aarch64/smp/smp.c`:
  - Include `<arch/smp.h>`, `<percpu/percpu.h>`, and `<errno.h>`.
  - Update `aarch64_smp_publish_boot(topology, entry_va)` to reject `!entry_va` and populate `slots[i].entry = entry_va` before `clean_to_poc()`.
  - Add `static struct arch_smp_boot_status a64_smp_status;` + `arch_smp_last_status()`.
  - Implement `int arch_smp_boot_aps(uint32_t cpu_count, arch_smp_ap_entry_t entry, struct percpu *per_cpu_data)` per Spec §4.2.3:
    - Validate `!entry || per_cpu_data != percpu_data || cpu_count == 0 || cpu_count > NR_CPUS || boot_published || !topology || topology->cpu_count != cpu_count || !hz || hz > UINT64_MAX / 2` → record `SMP_STAGE_INVARIANT`, return `-EINVAL`.
    - Publish boot slots (failure → `SMP_STAGE_PREPARE`, `-EINVAL`).
    - Emit existing `[smp] boot el=... conduit=...` and `[smp-test] no_ack_cpu=...` lines.
    - If `cpu_count > 1 && psci_init(topology->conduit) != 0`: record `failed_cpu=1, failed_stage=SMP_STAGE_ACTIVATE, arch_rc=-ENODEV`, log `[smp] reason=psci-unavailable\n`, return `-ENODEV` (Fail-Closed).
    - Run `smp_boot_run(...)`; log per-CPU `[smp] cpu=...` lines; if `status < 0` record `SMP_STAGE_INVARIANT` return `-EINVAL`; if `status != 0` locate first failed CPU (`SMP_FAILURE_CPU_ON` → `SMP_STAGE_ACTIVATE` / `-EIO`, `SMP_FAILURE_TIMEOUT` → `SMP_STAGE_ONLINE` / `-ETIMEDOUT`), record `a64_smp_status` and return negative errno (Fail-Closed, no degraded fallback).
    - On `status == 0`: freeze `boot_result` and `a64_smp_status` (`online = result.online`, `online_mask = result.online_mask`), call `aarch64_page_table_probe_finish(true)`, emit `[smp] requested=N online=N status=PASS\n`, return `0`.
  - Split `secondary_idle()` into the 4 AP hooks (`arch_smp_ap_cpu_init`, `arch_smp_ap_post_online`, `arch_smp_ap_publish_ipi_ready` with `isb` + `ipi_ready_publish_and_count`, `arch_smp_ap_enter_idle`) and add strong override `void arch_smp_post_boot_hook(uint32_t online_cpus) { (void)test_spinlock_smp(online_cpus); }`.
- In `kernel/include/arch/aarch64/smp.h`: remove `smp_boot_aps()` and `secondary_idle()` declarations.
- In `kernel/arch/aarch64/boot/main.c`: remove `arch_boot_aps()` and `boot1_no_ack_observation()` (plus its call site in `arch_boot_runtime()`).
- In `kernel/Makefile`: explicitly append `core/smp.c` to the aarch64 `KERNEL_C_SOURCES` whitelist right after `core/main.c`.

- [ ] **Step 4: Build aarch64 kernel and run symbol/stack-frame audit**

Run: `make PROFILE=aarch64-clang aarch64-uefi-kernel && make PROFILE=aarch64-clang test-aarch64-audit`
Expected: Builds cleanly and `test-aarch64-audit` PASSES.

- [ ] **Step 5: Commit**

```bash
git add kernel/Makefile kernel/include/arch/aarch64/boot_offsets.h kernel/include/arch/aarch_percpu.h kernel/arch/aarch64/boot/secondary_entry.S kernel/arch/aarch64/smp/smp.c kernel/include/arch/aarch64/smp.h kernel/arch/aarch64/boot/main.c
git commit -m "feat(aarch64): adapt PSCI AP bring-up to unified arch/smp.h with Fail-Closed policy"
```

---

### Task 5: Update Source-Audit Hosttests & Harness Unit Tests

**Files:**
- Modify: `hosttests/cases/test_double_state_publish.c` & `hosttests/mk/sched.mk:367-369`
- Modify: `hosttests/cases/test_boot_contract_softirq.c:80-116`
- Modify: `hosttests/cases/test_x86_ipi_ready_publish.c:99-122` & `hosttests/mk/intr.mk:263-267`
- Modify: `hosttests/cases/test_boot_contract_cpu_prepare.c:414-422`
- Modify: `qemutests/test_x86_intr_fault.py:461-479`
- Modify: `qemutests/test_aarch64_ap_boot_contract.py`
- Modify: `qemutests/boot_lifecycle.py` & `qemutests/test_boot_lifecycle.py`
- Modify: `qemutests/aarch64_uefi_smp.py` & `qemutests/test_aarch64_harness.py:366-434`

**Interfaces:**
- Consumes: Final symbols and file locations from Tasks 2–4 (`kernel/core/smp.c`, `arch_smp_boot_aps`, `smp_ap_common_entry`, `arch_smp_ap_*`)

- [ ] **Step 1: Update the 4 source-audit hosttests & `.mk` dependency lists**

Update per Spec §5.2 items 3–6:
- `test_double_state_publish.c` (+ add `kernel/core/smp.c` to `hosttests/mk/sched.mk` deps): split `suite_aarch64_boot_order()` across `kernel/core/smp.c::arch_boot_aps()` (`arch_smp_boot_aps` before `subsys_prepare_cpus`) and `kernel/arch/aarch64/boot/main.c::arch_boot_runtime()` (`arch_local_irq_enable` → `ipi_ready_publish_and_count(0)` → `aarch64_shootdown_probe`); update `suite_ap_tail_order()` and `suite_x86_smp_order()` to check `smp_ap_common_entry()` + arch hooks + `[boot-1] complete` in `kernel/core/smp.c`.
- `test_boot_contract_softirq.c`: anchor `softirq_init` before `arch_boot_runtime` in `kernel/arch/aarch64/boot/main.c` and verify `kernel/core/smp.c::arch_boot_aps()` does not call `softirq_init`.
- `test_x86_ipi_ready_publish.c` (+ add `kernel/core/smp.c` to `hosttests/mk/intr.mk` deps): verify `arch_local_irq_enable()` precedes `arch_smp_ap_publish_ipi_ready()` in `kernel/core/smp.c::smp_ap_common_entry()`, and `ipi_ready_publish_ap(...)` / `ipi_init()` → `ipi_ready_publish_bsp()` in `kernel/arch/x86_64/smp/smp.c`.
- `test_boot_contract_cpu_prepare.c`: keep negative legacy check on `kernel/arch/x86_64/smp/boot.c` and move positive `subsys_prepare_cpus` check to `kernel/core/smp.c`.

- [ ] **Step 2: Update Python harness & QEMU acceptance runners**

Update per Spec §5.2 items 1, 2, 7, 8:
- `qemutests/boot_lifecycle.py` & `qemutests/test_boot_lifecycle.py`: expand `_FATAL_TEXT` to `stage=(prepare|activate|online|scheduler|invariant)`; expect `[boot-1] FATAL cpu=1 stage=<stage>` + `[boot-3] FATAL stage=smp component=<stage> ...` with zero `kernel panic` lines on fatal cases.
- `qemutests/aarch64_uefi_smp.py` & `qemutests/test_aarch64_harness.py`: update `no_ack_cpu=1` acceptance from DEGRADED/`a64-no-ack` to Fail-Closed (`[boot-1] FATAL cpu=1 stage=online` + `[boot-3] FATAL stage=smp component=online ...`, no `[spinlock]` / `SHOOTDOWN-PROBE` / `[tick]`), and migrate the 6 `test_no_ack_*` unit tests in `test_aarch64_harness.py`.
- `qemutests/test_x86_intr_fault.py`: update `ApNmiOrderingTests` to check `arch_smp_ap_cpu_init` in `x86_64/smp/smp.c` + `smp_ap_common_entry` in `kernel/core/smp.c`.
- `qemutests/test_aarch64_ap_boot_contract.py`: adapt release-gate & online-before-ACK scans to `arch_smp_ap_enter_idle` / `smp_ap_common_entry` / `arch_smp_ap_post_online`; replace obsolete `boot1_no_ack_observation` test with Fail-Closed `SMP_STAGE_ONLINE` contract test.

- [ ] **Step 3: Run full host tests, harness unit tests, static audit, and catalog check**

Run: `make check-test-catalog && make test-host && make test-harness && make test-static`
Expected: All host tests, Python harness unit tests, static audits, and catalog checks PASS.

- [ ] **Step 4: Commit**

```bash
git add hosttests/ qemutests/
git commit -m "test(smp): align source-audit hosttests and QEMU harnesses with unified Fail-Closed SMP boot"
```

---

### Task 6: Cross-Arch QEMU End-to-End Verification & Documentation Closure

**Files:**
- Modify: `docs/roadmap.md`
- Modify: `docs/arch.md`

**Interfaces:**
- Consumes: Complete unified SMP boot implementation and test suites from Tasks 1–5

- [ ] **Step 1: Run x86_64 QEMU boot-lifecycle & phase-0 acceptance suites**

Run: `make test-qemu SUITE=boot-lifecycle && make test-qemu SUITE=phase-0`
Expected: `normal` (1/2/4/8 CPUs), `delay-online`, `no-online`, `no-scheduler`, and `phase-0` all PASS.

- [ ] **Step 2: Run aarch64 QEMU SMP acceptance suites (normal 1/2/4 CPUs + `MODE=no-ack` Fail-Closed)**

Run:
```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64 MODE=no-ack
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
```
Expected: Normal 1/2/4-CPU runs PASS (`[smp] requested=N online=N status=PASS`, `[boot-1] complete`, `[spinlock] ... PASS`, `[tick]`), `MODE=no-ack` PASSES Fail-Closed verification, and normal aarch64 image is restored cleanly afterward.

- [ ] **Step 3: Update `docs/roadmap.md` and `docs/arch.md`**

- In `docs/roadmap.md`: mark「SMP 启动统一」as completed with closure summary (link to spec/plan) and add entry to「近期闭环索引」.
- In `docs/arch.md`: add `arch/smp.h` row to the Facade/override matrix and update the `arch_smp_boot_aps()` status note in §aarch64 用户态现状.

- [ ] **Step 4: Final verification & commit**

Run: `make check-test-catalog && git status --short`
Expected: Only `docs/roadmap.md` and `docs/arch.md` modified; catalog clean.

```bash
git add docs/roadmap.md docs/arch.md
git commit -m "docs: record unified SMP boot closure in roadmap and arch docs"
```
