# X86 Interrupt Dispatch Phase Two Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect the 20 installed x86 exceptions and 28 hardware interrupt vectors to the common dispatcher, preserving their original completion semantics.

**Architecture:** Expand pure exception decoding and thin C adapters; add IRQ family routing without moving ACK/EOI into the common layer. Move the x86 generic adapter into the architecture directory, retain a neutral registry helper, and give LAPIC spurious a full frame with restore-only return. Verify production objects, runtime probes, and isolated destructive cases.

**Tech Stack:** C, x86 assembly, Clang/LLD, profile Make builds, host C tests, protocol-v1 kernel selftests, Python harness and QEMU/QMP.

**Spec:** [2026-10-24-intr-dispatch-phase2-x86-design.md](../specs/2026-10-24-intr-dispatch-phase2-x86-design.md), committed as `33f69b91`. The user approved and authorized implementation after independent review.

## Global Constraints

- Exactly the installed synchronous-channel set `0..14,16..20` and IRQ set `0x20..0x37,0x38,0x40,0x41,0xff`; syscall 0x80 and new ISA exceptions excluded.
- Preserve existing gate type/DPL/IST, native error-code conventions, and exception return labels. NMI keeps explicit C error code 0; #DF IST3 and #PF IST0 stay unchanged.
- Preserve common event ABI, phase1 #PF single CR2 snapshot/uaccess/COW and #GP safe-MSR behavior; all aarch64 semantics unchanged.
- ACK stays in device controller logic; timer/IPI EOI stays in original handlers; 0xff has no EOI and no ret_from_intr work.
- New adapters/decoders/backend validation use no canary, allocator, locks, ordinary logging/current/percpu/readiness gate. Original runtime business handlers retain existing dependencies.
- No x86 private headers or routing whitelist in common intr/vector_dispatch.c. Public generic adapter moves to x86; common table/helper remains neutral and is not added to ARM whitelist.
- Use production components in tests, spies only at hardware/nonreturn boundaries; record expected RED before new behavior and GREEN afterward.
- All artifact paths/profile/variant values must pass existing controlled submake boundaries. Never mix KERNEL_SELFTEST and OS01_SYSTEST. No merge/push without user authorization.

## Review Focus

1. Missing/zero address versus valid #PF zero: Task 1 decode tests and Task 2 IRQ decode.
2. NMI's historical pseudo-error slot must never supply native_status: Task 1 wrapper/target checks.
3. Registered vectors, timer and device families cannot route to one another: Task 2 production adapters and ACK tests.
4. Spurious full frame must restore registers/flags without invoking scheduler/softirq: Tasks 2/4/6.
5. QMP and serial ordering must not manufacture NMI-return evidence; #BP RIP follows int3: Task 5 parser/process tests.

## File Responsibilities and Interfaces

| Files | Responsibility |
|---|---|
| kernel/include/arch/x86_64/intr_vectors.h; intr/intr_decode.c; intr_decode.h | Single native-vector vocabulary and expanded pure decoders |
| x86 intr/trap.c; intr/dispatch.c; intr_dispatch.h | Exception adapters/private handoffs and both strong backends |
| x86 intr/irq_entry.c; irq_hooks.c; apic/lapic_timer.c; apic/lapic.c | IRQ adapters and original business handoffs/spurious assembly |
| kernel/intr/vector_dispatch.c; kernel/include/intr/vector_dispatch.h | Neutral registry and intr_dispatch_registered_vector lookup |
| x86 intr/entry.S; gate.h | Restore-only spurious return, existing stub/gate ABI preserved |
| hosttests/cases/test_intr*.c; hosttests/mock/intr*; hosttests/Makefile | Production decode, route, ACK and controller-side tests |
| qemutests/intr_dispatch_audit.py and test_intr_dispatch_audit.py | Actual ELF/frame/installed-vector wiring checks |
| kernel/selftest/test_x86_intr.c; paired kernel/include/selftest/x86_intr.h; selftest.c; sched/core.c | Scheduled x86 live probes, test-only observations and v1 reporting |
| qemutests/x86_intr_fault.py; test_x86_intr_fault.py; existing run_test.py/harness; mk/project.mk, profiles, run.mk and kernel/Makefile | Isolated fault variant, QMP NMI, strict runner and root registration |
| docs/arch.md; roadmap.md; changelog.md; reports | Honest final coverage and evidence |

Tasks execute sequentially in one isolated worktree. Task 1/2 temporarily outpace the phase1-only audit; Task 3 updates it before final static acceptance. Record this bounded intermediate failure rather than disabling the gate.

### Task 1: Complete the installed x86 exception path

**Files:** native vector header, decode/header, trap.c, dispatch.c, intr_dispatch.h; existing host decode/backend cases and Makefile.

**Interfaces:** Retain x86_intr_decode signature and phase1 four handoffs. Add `void x86_handle_<existing_do_suffix>(pt_regs_t *, uint64_t)` for the 16 remaining handlers, with declarations in the x86 private header. Preserve all public do_* symbols. Native reason table is exactly spec §5.

- [ ] Step 1: Extend independent literal-vector host cases to all 20 exceptions; assert reason/origin/raw-error and PF-only addresses, UNKNOWN CPL and unsupported 15/21/0x80 rejection. Route spies must distinguish each private handler and escape nonreturn endpoints.
- [ ] Step 2: Run root `make PROFILE=x86_64-clang test-host`, capture RED for old classification/whitelist. Focused root-supported host selection may be used during iteration; register every new binary in TEST_BINS.
- [ ] Step 3: Implement vector truth header, pure classification and the 16 thin adapters/private bodies. No new handler semantics; keep original logs/signals/fixup order, NMI's explicit argument and assembly unchanged. Disable canary on the new adapter/decode/validation chain.
- [ ] Step 4: GREEN host suite and `make PROFILE=x86_64-clang kernel.bin`; inspect NMI/error-code/frame callsite preservation. Run phase-0 and systest if builds ready; subsequent full guest gates remain mandatory.
- [ ] Step 5: Commit `feat(x86): complete installed exception event dispatch`; report exact commands/results and any pre-existing failures.

### Task 2: Complete IRQ families and spurious restore-only entry

**Files:** new x86 intr/irq_entry.c, decode/header/backend/private header; irq_hooks.c; common vector_dispatch.c and new paired header; apic/lapic_timer.c/lapic.c; entry.S/gate.h; host tests/mocks/Makefile.

**Interfaces:** Add `void x86_intr_decode_irq(struct intr_event *, const pt_regs_t *, uint64_t)`, `void x86_irq_entry(pt_regs_t *, uint64_t)`. Device and generic public adapter names unchanged. Add spec's `void x86_handle_device_irq/registered_irq/lapic_timer_irq/spurious_irq(pt_regs_t *, uint64_t)` handoffs; common helper `void intr_dispatch_registered_vector(pt_regs_t *, uint64_t)`; restore-only assembly symbol `x86_intr_restore_only`.

- [ ] Step 1: RED tests for all 28 IRQs/CPL0/3, bad fields/NULL/unknown vectors/wrong adapter families. Link real adapters/backend/neutral lookup and device body, spy only port IO/controller/nonreturn hardware. Assert handler then ACK, no-handler controller ACK, no-controller behavior, slave ACK writes, registered params, timer/IPI-owned EOI and spurious zero EOI.
- [ ] Step 2: Implement adapters and private routes. Move generic adapter into x86 irq_entry.c; retain neutral table/lookup helper in common source with paired neutral header. Keep ARM whitelist unchanged. Reject NULL before decode. No recursion/extra ACK; original runtime business logs/dependencies retained only after routing.
- [ ] Step 3: Repoint timer stub's C call to an adapter with actual vector 0x38 while preserving its saved frame and return; original timer business handler remains source of ticks/EOI. Replace 0xff bare iret with full INTR_SAVE_ALL-compatible frame, adapter call and restore-only synthetic return. Export minimal restore-only alias to RESTORE_ALL; never ret_from_intr for spurious. Preserve arbitrary interrupted register/flag state and C call stack requirements.
- [ ] Step 4: GREEN full host suite and x86 normal build; QEMU phase-0 SMP1/2 and systest. Record production object wiring for all four families and preserved gate/error-slot offsets.
- [ ] Step 5: Commit `feat(x86): route device and local interrupts through common dispatch`.

### Task 3: Upgrade real-object audit for the complete installed surface

**Files:** qemutests/intr_dispatch_audit.py, test_intr_dispatch_audit.py; existing harness bucket registration as necessary.

**Interfaces:** Retain CLI/profile/tool arguments and `[intr-dispatch-audit] OK`. Replace phase1 four-only/IRQ-failclosed expectation with the explicit native exception/IRQ manifests, keeping ARM checks intact.

- [ ] Step 1: Add failing fixture tests for omitted installed vectors, wrong family/ACK ownership, bad NMI zero argument, malformed error-code frames, wrong gate/DPL/IST, wrong timer target, spurious incomplete restore or ret_from_intr/EOI. Derive expected vector counts and routes independently of production whitelist.
- [ ] Step 2: Run `python3 -m unittest qemutests.test_intr_dispatch_audit` RED. Support existing movabs→indirect call/jump, FUNC-slot indirect call, NMI direct call and all real stub shapes; include unresolved/wrong-target rejection, not source-name presence alone.
- [ ] Step 3: Extend audit with minimal actual ELF dataflow plus source/runtime gate evidence. Preserve PF-only CR2 count and GP diagnostic allowance. Runtime IDT snapshots belong Task 4. Validate early no-canary dependencies only before original business endpoints; dynamic tables use production behavioral tests.
- [ ] Step 4: GREEN fixture suite, root test-static and ARM test-aarch64-audit with actual profile ELF/tool paths; no unknown-format pass/skip.
- [ ] Step 5: Commit `test(intr): audit complete x86 exception and IRQ wiring`.

### Task 4: Add real scheduled x86 IRQ and restoration probes

**Files:** new kernel/selftest/test_x86_intr.c and paired header; selftest/selftest.c; sched/core.c; scoped selftest-only observations in x86 timer/IPI business paths if needed; kernel Makefile and host/harness fixtures as needed.

**Interfaces:** Add `int test_x86_intr(void)`, registered as required late v1 case `x86_intr`. Execute only after scheduler/online/ipi_ready are available. Test-only counters/observers compile out completely when OS01_SELFTEST is absent and must not gate production dispatch.

- [ ] Step 1: Write required-case/runner fixtures and controlled host tests for probe-state comparisons, timeout/failure cleanup and cross-CPU target/ack accounting; demonstrate RED before adding case/observers.
- [ ] Step 2: Implement real LAPIC tick progression on BSP/AP, real remote TLB flush/ack and real resched IPI receipt followed by task progress. Use bounded waits and existing TLB protocol; no direct-handler substitute, no forced global double tick. All acquired test resources cleaned on failure.
- [ ] Step 3: Add assembly sentinel probe executing int $0xff, recording GPR/RSP/RFLAGS preservation and showing no scheduler/softirq/signal return work. Mask unrelated IRQ while taking before/after observations, restore original state; do not disable the synthetic int. Verify runtime IDT target/DPL/IST for inventory using existing IDT table, without altering gates. Use expected descriptors from independent literals.
- [ ] Step 4: Run root kernel-selftest SMP2 and SMP4 with required x86_intr PASS, phase1 uaccess longjmp marker and msr_safe intact. Report fd_refcount baseline flakes honestly, never drop required cases. Production build has no observer symbols/markers.
- [ ] Step 5: Commit `test(x86): verify live IRQ dispatch and spurious restoration`.

### Task 5: Isolate destructive #BP and QMP NMI acceptance

**Files:** new qemutests/x86_intr_fault.py and test_x86_intr_fault.py; root runner dispatch; mk/project.mk, profiles/x86_64-clang.mk, aarch64-clang.mk, components/run.mk; kernel Makefile and x86 test probe source/header; build docs.

**Interfaces:** New `test-qemu SUITE=intr-x86`; selector `X86_INTR_FAULT_TEST=bp|nmi` (empty normal), variant `intr-x86-fault-bp|intr-x86-fault-nmi` to prevent object/image collisions. Root suite builds/runs each sequentially using controlled submake variables, never ordinary kernel-selftest. Reject selector on ARM and conflicts with selftest/syscall/nettest/canary/weak/resolution/driver faults. Explicit profile/firmware/image/ELF/QEMU paths, standard ProcessSession/RunArchive ownership and finite deadlines.

- [ ] Step 1: RED parser/process tests: valid bp/nmi, missing/duplicate/malformed/late fatal, returned/heartbeat after completed diagnostic, QMP error, no response, response arriving after diagnostic, buffered pre-fatal heartbeat, split chunks, timeout, process exit/config error. No normal completion or timeout counts as success. Add make -n and variant/conflict/propagation tests to existing harness/contract buckets.
- [ ] Step 2: Build isolated probe after IDT/serial/percpu usable but before SMP/task concurrency. BP emits armed then single-byte int3, expose exact immediately following resume label for ELF-derived RIP check, never returned. NMI emits armed and lock-free heartbeat/wait loop, no ordinary logger lock during injection.
- [ ] Step 3: Implement runner with private QMP socket, handshake/inject-nmi success, original fatal diagnostics and minimum 1s post-complete observation; guest serial diagnostic defines ordering rather than host QMP response timestamp. Enforce errors/nonzero on buffered false positives or any later continue/panic/fatal. Always close/reap resources. Match current logger line format without changing fatal production output.
- [ ] Step 4: GREEN all fixtures and dry-run/config gates. Run actual root suite both single-CPU boots, verify original-image hash unchanged, ELF resume RIP valid and archives complete; normal build contains no fault probe. Do not claim hardware MC/DF E2E.
- [ ] Step 5: Commit `test(x86): add isolated breakpoint and NMI dispatch acceptance`.

### Task 6: Full matrix, final review and evidence report

**Files:** docs/superpowers/reports/2026-10-24-intr-dispatch-phase2-x86-validation.md (use actual date if different); docs/arch.md, roadmap.md, changelog.md, build docs if necessary.

**Interfaces:** Full spec §9.4 acceptance, coverage/evidence levels; no new production functionality unless validation discovers a reproducible defect (then RED/fix/covering rerun).

- [ ] Step 1: Build both normal profiles, run full host/harness/static suites. Preserve exact revision/dirty state/toolchain/profile/variant and archived command results; inspect 48 native vector paths and runtime IDT observations.
- [ ] Step 2: Run all spec §9.4 commands, including phase-0 SMP1/2, separate systest/network, kernel-selftest SMP2/4, new destructive suite, and ARM audit/sync-fault/SMP/GIC-SPI/m3-probe. Capture each exit status; fail/skip/timeout not silently reclassified. Meaningful existing flakes require same-configuration baseline evidence.
- [ ] Step 3: Write coverage by actual evidence: true IRQ/NMI, software spurious/#BP, host synthesis and object checks; keep MC/DF physical reliability excluded. Record any unresolved blocker honestly, do not mark complete while acceptance is missing.
- [ ] Step 4: Request final independent whole-branch review and fix real findings with covering verification. Update roadmap only for this x86 completion; ARM recovery/M4, contexts and kernel_main remain pending. Commit validation/docs; leave isolated branch reviewable, no automatic merge or push.

## Self-review coverage

Spec §§1–3 → Tasks1–3/6; §§4–6 → Tasks1–3; §7 → Tasks2/4; §8 → Tasks2–4; §9.1 → Tasks1–4; §9.2 → Task4/6; §9.3 → Task5; §9.4/§10 → Task6. This plan is execution-authorized by the user's instruction; it does not need another permission checkpoint before Task1.
