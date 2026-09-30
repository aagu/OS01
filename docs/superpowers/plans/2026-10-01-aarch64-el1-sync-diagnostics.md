# aarch64 EL1h 同步异常诊断实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 aarch64 高半区 VBAR 的 EL1h sync 入口输出可信的故障现场并停机，用隔离的 QEMU 故障注入验证；保留现有 IRQ 和 x86_64 行为。

**Architecture:** 入口汇编复用现有 `pt_regs_t` 布局保存现场，aarch64 `trap.c` 用已存在的跨架构 `arch_early_put*`、`arch_local_irq_disable`、`arch_cpu_halt`、`arch_atomic_cas` 完成无锁首报与终止。纯 ESR/FAR 判定放在 aarch64 对称头供内核与宿主测试共用；构建使用独立 `sync-fault` 变体，QEMU harness 复用现有固件/DTB 机制。

**Tech Stack:** AArch64 assembly、freestanding C、GNU Make、Python 3、QEMU `virt,gic-version=2`、现有 hosttests。

**Spec:** `docs/superpowers/specs/2026-09-30-aarch64-el1-sync-diagnostics-design.md`

## Global Constraints

- 仅接通 `exception_vectors + 0x200` 的 EL1h sync 槽；16 槽 × 128 字节、2 KiB 表对齐保持不变，EL1h IRQ 的保存/恢复/`eret` 语义保持不变。
- `pt_regs_t` 仍为 272 字节，x0–x30、SP_EL0、ELR_EL1、SPSR_EL1 的现有偏移不得改变；如意外改变结构，按 `AGENTS.md` 先 `make clean` 再构建。
- fatal 路径不使用普通日志、锁、分配器、VFS、调度器或 GIC；复用 `arch/early_print.h`、`arch/irq.h`、`arch/cpu.h`、`arch/atomic.h` 的现有接口。
- 诊断行格式、FnV/FAR 规则和故障变体隔离严格按 spec §4–5；正常构建不包含故障注入。
- 不实现 EL0、FIQ、SError、异常恢复、通用 `arch_intr_dispatch`；x86_64 的源文件和运行行为不变。

## Review Focus

- 异常到来时 x0–x2 已有活值：入口先保存全部原寄存器，再用 x0–x2 传参；Task 1 的汇编结构检查和 Task 3 的真实 data abort 覆盖此路径。
- `ESR_EL1.ISS.FnV=1` 的 instruction abort/data abort/watchpoint：必须输出 `far=n/a`；Task 1 宿主测试逐项覆盖。
- 第一个故障发生在 BSP 的 per-CPU 初始化之前：诊断只读 MPIDR，不依赖 TPIDR；Task 3 在 SMP/DTB 初始化之前注入并断言行完整。
- 故障构建污染普通 selftest 镜像：Task 2 检查 `KERNEL_VARIANT=sync-fault` 的 kernel/image 路径与 `selftest` 不同，随后重建正常变体。
- QEMU 只有 `armed`、缺少 fatal 行却一直运行：Task 3 harness 必须超时失败，不把停机/超时本身当成功。

---

### Task 1: EL1h sync 入口与 fatal 诊断

**Files:**
- Create: `kernel/include/arch/aarch64/sync_fault.h`
- Modify: `kernel/arch/aarch64/intr/entry.S`, `kernel/arch/aarch64/intr/trap.c`, `hosttests/Makefile`, `qemutests/aarch64_smp_test.py`
- Test: `hosttests/cases/test_aarch64_sync_fault.c`; the existing aarch64 ELF/source audit extended in `qemutests/aarch64_smp_test.py`

**Interfaces:**
- Produces: `static inline bool aarch64_sync_far_valid(uint64_t esr)` in the aarch64 header; `__attribute__((noreturn, no_stack_protector, cold)) void aarch64_el1_sync_fatal(struct pt_regs *regs, uint64_t esr, uint64_t far)`; global assembly symbol `el1_sync_entry`.
- Consumes: existing `PT_REGS_*`, `arch_early_putc/puts`, `arch_local_irq_disable`, `arch_cpu_halt`, `arch_atomic_cas`.

- [ ] **Step 1: Write failing host and vector ABI checks.** In `hosttests/cases/test_aarch64_sync_fault.c`, assert `aarch64_sync_far_valid(esr)` is true for EC `0x20,0x21,0x22,0x24,0x25,0x34,0x35` with FnV clear; false for EC `0x20,0x21,0x24,0x25,0x34,0x35` with bit 10 set, BRK `0x3c`, and unrelated EC; PC alignment `0x22` stays true when bit 10 is set. Add `test_aarch64_sync_fault.elf` to `TEST_BINS` and a host-native rule using `$(KERNEL_INC)`. In `qemutests/aarch64_smp_test.py`, extend `check_elf()` to decode the 32-bit A64 unconditional `b` at `exception_vectors+0x200` and `+0x280`, assert they target symbols `el1_sync_entry` and `el1_irq_entry` respectively, and add a source-order check of `el1_sync_entry` requiring stores for x0–x30 at the existing `PT_REGS_*` offsets before the first instruction that reuses x0–x2 for ESR/FAR/C arguments. Use the ELF PT_LOAD file offset to read instructions, not an objdump string match.
- [ ] **Step 2: Run RED checks.** `make PROFILE=x86_64-clang test-host` fails because the FAR helper/header is absent; `python3 qemutests/aarch64_smp_test.py` fails because the EL1h sync slot still branches to itself or `el1_sync_entry` is absent. Keep existing hosttests/ELF checks passing aside from these new assertions.
- [ ] **Step 3: Add `sync_fault.h` and the fatal C path.** Implement the pure ESR helper there; declare the fatal entry with a forward `struct pt_regs`. In `trap.c`, retain `el1_irq()` and `arch_install_exception_vectors()` untouched. Define BSS `volatile uint64_t first_fault`, claim it with `arch_atomic_cas(&first_fault, 0, 1)` after IRQ disable and `DAIFSet #0xf`. Use only `arch_early_put*` for the spec's exact fixed-width lowercase line, read MPIDR_EL1, and loop on `arch_cpu_halt()`; all fatal-path local output helpers also avoid stack protector, locks, formatting libc and allocation. Losers halt without UART output.
- [ ] **Step 4: Connect offset `0x200` to `el1_sync_entry` in `entry.S`.** Save all GPRs to the existing `PT_REGS_*` layout before clobbering x0–x2, snapshot SP_EL0/ELR_EL1/SPSR_EL1 and ESR_EL1/FAR_EL1 before the C call, pass `(sp, esr, far)` using AAPCS64, and place a non-returning fallback loop after `bl`. Leave the IRQ slot and `el1_irq_entry` instruction sequence unchanged; keep the 16 vector offsets and table alignment.
- [ ] **Step 5: Run host, cross-build, and layout checks; verify GREEN.** `make PROFILE=x86_64-clang test-host`; `make PROFILE=aarch64-clang aarch64-uefi-kernel`; `python3 qemutests/aarch64_smp_test.py`. The last command must pass its new decoded-branch and source-order checks, plus existing ELF alignment/layout checks.
- [ ] **Step 6: Commit Task 1** with the six listed source/test files (`feat(aarch64): diagnose EL1 sync exceptions`).

### Task 2: 隔离故障注入构建与启动探针

**Files:**
- Modify: `mk/project.mk`, `mk/profiles/aarch64-clang.mk`, `mk/targets/aarch64.mk`, `mk/components/run.mk`, `kernel/Makefile`, `kernel/arch/aarch64/boot/main.c`, `qemutests/build_contract.sh`

**Interfaces:**
- Produces: `AARCH64_SYNC_FAULT_TEST=1` through `OS01_SUBMAKE_ALLOWED` to kernel `ALL_CFLAGS`; `KERNEL_VARIANT=sync-fault`, separate `AARCH64_UEFI_SYNC_FAULT_DISK/FIRMWARE` paths; private `_test-aarch64-prep-sync-fault` build target; `aarch64_main()` injection markers.
- Consumes: Task 1 `el1_sync_entry`/fatal path; current `aarch64_pt_query_4k`, `AARCH64_PT_SELFTEST_VA`, `AARCH64_PT_ENOENT`, `arch_get_page_table`, `AARCH64_TTBR_BASE_MASK`.

- [ ] **Step 1: Extend build-contract RED assertions.** In the aarch64 `targets` mode, assert `make -n PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` selects `KERNEL_SELFTEST=1 AARCH64_SYNC_FAULT_TEST=1`, a `sync-fault` kernel/image path, and not `image/selftest/aarch64-uefi.img`; assert x86 profile rejects the mode and normal `MODE=smp` dry run does not include the fault flag. Run `sh qemutests/build_contract.sh aarch64-clang targets` and confirm the new checks fail (the root `test-contract` umbrella runs all modes and does not accept `MODE=targets` as a selector).
- [ ] **Step 2: Wire the dedicated variant.** Add `AARCH64_SYNC_FAULT_TEST` to `OS01_SUBMAKE_ALLOWED`; make aarch64 profile choose `sync-fault` before `selftest` when that flag is 1 (reject flag without `KERNEL_SELFTEST=1`); add the kernel Makefile define only for aarch64; expose explicit sync-fault image/firmware paths via `mk/targets/aarch64.mk`; add `_test-aarch64-prep-sync-fault` using the same private-target recipe pattern as `smp`, and recognize `MODE=sync-fault` in the umbrella capability gate. Preserve existing `smp/no-ack/gic-spi` recipes and Make `-n` behavior.
- [ ] **Step 3: Add the guarded BSP probe.** Under `#if AARCH64_SYNC_FAULT_TEST`, immediately after the existing PMM/page-table selftests and before DTB/GIC/SMP setup, derive and validate the active TTBR root as `aarch64_pt_smoke_test()` does. Query `AARCH64_PT_SELFTEST_VA`; if return is not `AARCH64_PT_ENOENT`, print `[aarch64-sync-test] precondition FAIL` and halt. Print `[aarch64-sync-test] armed`, perform a `volatile` inline-asm `ldr` from that VA with a register output, and put `[aarch64-sync-test] returned` directly after it. The flag is absent in production and ordinary selftests.
- [ ] **Step 4: Run contract and variant build; verify GREEN.** `sh qemutests/build_contract.sh aarch64-clang targets`. Capture the first fault-variant build's stdout: `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_SYNC_FAULT_TEST=1 aarch64-uefi > /tmp/os01-sync-fault-build.log 2>&1`. Require at least one actual `clang ... -c ...` line containing `-DAARCH64_SYNC_FAULT_TEST=1`, and the same define in `build/aarch64-clang/kernel/sync-fault/.cflags`; check new kernel and image land under `/sync-fault/`. Then `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi` and require `build/aarch64-clang/kernel/selftest/.cflags` lacks the define and `/selftest/` remains separate. Require `make -n PROFILE=x86_64-clang kernel.bin` contains no fault define. If no header/struct ABI changed, avoid unnecessary clean.
- [ ] **Step 5: Commit Task 2** with the seven listed files (`build(aarch64): isolate sync fault probe image`).

### Task 3: QEMU 故障验收和回归

**Files:**
- Create: `qemutests/aarch64_sync_fault.py`
- Modify: `mk/components/run.mk`, `docs/build-system-harness.md`

**Interfaces:**
- Produces: public `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` end-to-end target and harness parser `sync_fault_evidence(text: str) -> bool`.
- Consumes: Task 2 `AARCH64_UEFI_SYNC_FAULT_DISK/FIRMWARE`, `_test-aarch64-prep-sync-fault`, `armed`/`returned`/`precondition FAIL` markers; existing `qemutests/aarch64_uefi_smp.py` diagnostic DTB generator and QEMU argument pattern.

- [ ] **Step 1: Write parser RED cases in `aarch64_sync_fault.py --self-test`.** Valid synthetic log has one `armed`, one full fatal line with `ec=0x25`, `far=0xffff800000000000`, ESR EC bits matching 0x25, 16-digit lowercase MPIDR/ESR/ELR/SPSR, no later `returned`/tick/normal completion. Reject missing/duplicate/out-of-order markers, `precondition FAIL`, `far=n/a`, wrong EC/FAR and malformed width; accept PL011's CR after LF. Run `python3 qemutests/aarch64_sync_fault.py --self-test`; confirm RED before parser implementation.
- [ ] **Step 2: Implement harness and private run target.** `sync_fault_evidence` normalizes CR, checks all fields and ordering, and does not treat timeout as success. Harness uses one `cortex-a53`, one CPU, `virt,gic-version=2`, existing UEFI/DTB generation pattern, bounded serial capture, and a short post-fatal drain before it terminates QEMU; if fatal never appears, expiry fails. Keep stdout/stderr logs and command metadata under `test-results/aarch64-sync-fault/`. `_test-aarch64-run-sync-fault` passes the isolated image/firmware to it; update `MODE` help and `docs/build-system-harness.md`.
- [ ] **Step 3: Verify parser GREEN, then QEMU end-to-end.** `python3 qemutests/aarch64_sync_fault.py --self-test`; `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault`. Confirm a complete fatal line, ENOENT precondition path, no `returned`, and harness exit 0 because of evidence rather than timeout.
- [ ] **Step 4: Run focused regressions.** `make PROFILE=aarch64-clang test-aarch64 MODE=smp`; `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi`; `make PROFILE=x86_64-clang test-static`; `git diff --check`. Verify normal/selftest/sync-fault artifact paths remain distinct and x86_64 compile input has no `AARCH64_SYNC_FAULT_TEST` define.
- [ ] **Step 5: Commit Task 3** with the harness, `run.mk` and build-harness doc (`test(aarch64): verify EL1 sync diagnostics in QEMU`).

## Self-review before execution handoff

- Spec §1–4 are implemented and checked in Task 1; §5 build isolation/probe in Task 2; §5 QEMU evidence and regressions in Task 3.
- The host ESR test exercises real header logic, not a copied Python predicate; the QEMU case exercises the real vector-to-PL011 path.
- Each task ends with a checkable artifact and a commit; all paths, build flags, markers, function names and verdict rules are pinned above.
