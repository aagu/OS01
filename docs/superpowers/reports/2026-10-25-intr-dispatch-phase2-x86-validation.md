# x86 installed interrupt dispatch phase 2 validation — 2026-10-25

All required matrix commands have run. The x86 installed-entry work passes its targeted acceptance; both full kernel-selftest aggregates remain **FAIL** solely on the retained, baseline-matched `fd_refcount` failure. Independent whole-branch review is the controller’s next gate; this report does not pre-approve it.

## Scope and verification identity

Source verification revision: `476eb6870e059ef04169bdb25505fd4cecb54ec0`, branch `codex/intr-dispatch-phase2`, worktree `/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01`. `005dc4a482a09f715710887af9c7e66c898ce854` adds only the LAPIC NMI host fixture's success summary; production is identical. Each matrix row must retain its actual recorded revision. A later documentation commit does not retroactively become an archive's tested SHA.

Pre-existing dirty BusyBox submodule `thirdpart/busybox-1.36.1` was preserved, never reset and never staged as a pointer. Dirty archive metadata must not be read as a pristine source claim.

The bounded scope is the installed 20 synchronous channels `0..14,16..20` plus 28 IRQ channels `0x20..0x37,0x38,0x40,0x41,0xff`. Syscall `0x80`, new ISA exceptions, ARM recovery/EL0/SVC/uaccess, user page tables/ASID/COW, context-switch abstraction, unified SMP and unified `kernel_main` remain outside completion. Spec A and M4 remain open.

## Production changes and preserved contracts

Native exception/IRQ entry decodes into the common event ABI and calls `arch_intr_dispatch`; x86 policy remains in its strong backend. `irq_entry.c` owns the x86 public adapters, `irq_hooks.c` retains controller/device business, and common `vector_dispatch.c` owns architecture-neutral registered lookup. The public IRQ adapters reject NULL frames and vectors outside their allowed family before decoding or frame access; adapters/decoders/backend validation have no allocator, locks, ordinary logging, per-CPU/current/readiness dependency or stack canary. Existing business dependencies remain.

Gate type/DPL/IST, native error codes, return labels, phase-1 single CR2 snapshot/uaccess/COW and #GP safe-MSR semantics are preserved. The `_set_gate` inline-assembly operand constraint fix prevents the compiler reusing a clobbered IST input; runtime gate evidence checks the installed result. Device controllers retain ACK; timer/IPI retain their original EOI; `0xff` has neither EOI nor return work.

MADT type-4 records supply processor UID/all-CPU, LINT and polarity/trigger routing, following [ACPI 6.5 §5.2.12.7](https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html#local-apic-nmi-structure); unspecified pins remain masked. AP routing is enabled only after kernel GDT/IDT, selectors, TSS/IST and TR are ready. BSP destructive probes run after per-CPU/APIC initialization, before AP/task concurrency. No boot-context/UAPI ABI changed; the APIC-structure change required and received a clean x86 rebuild.

## Evidence taxonomy and limits

| Evidence | What it proves | Limit |
|---|---|---|
| Object/linked ELF audits | All 48 native paths and strong backend/call-chain structure; validation dependencies; preserved labels | Not execution of every physical fault |
| Runtime IDT 48/48 | Actual handler address, selector, type/DPL, IST and reserved gate bytes | Does not induce every exception |
| Real scheduled BSP/AP timer, TLB shootdown and resched IPI | Production IRQ delivery, remote flush observed through changed mapping, ack and scheduling progress | Restricted tested CPU/topology/window |
| Software `int 0xff` sentinel | GPR/RSP/RFLAGS/DS/ES restoration with no return-work/scheduling consumption | Software spurious path; not a physically generated spurious IRQ |
| Software `int3` isolated ELF probe | Original #BP diagnostic and saved RIP equals ELF resume label; no continuation | Software breakpoint; not a hardware fault |
| QMP `inject-nmi`, post-percpu | Real NMI delivered to original fatal path under kernel MADT routing | Single BSP; not injection at every AP startup instruction |
| Host production/controller synthesis | Decoder/backend policy, invalid inputs, ACK/EOI ordering and MADT routing decisions | Host substitution at hardware/nonreturn boundaries; no PIC guest claim |
| Existing user syscall fault/signal and COW cases | Executed user faults, parent survival and recovery on named cases | Does not prove arbitrary user exception coverage |

No hardware #DF/#MC reliability claim, no physical PIC guest proof, and no blanket physical-machine certification. ARM behavior is unchanged and independently revalidated. Source-order AP readiness regression is not real instruction-by-instruction NMI injection. SIGKILL cannot run the destructive runner's cleanup; ordinary exits reap QEMU and remove its private socket.

## x86 matrix

### Collector record

Evidence collector only; no production changes, clean, reset, staging or commits performed. All requested x86 commands ran. This is not an all-green aggregate claim: first host run failed, and both kernel-selftest aggregates failed as recorded below.

## Revision and environment

Worktree: `/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01`; branch `codex/intr-dispatch-phase2`.

Normal image and first host run started at `476eb6870e059ef04169bdb25505fd4cecb54ec0`. Initial dirt was only ` m thirdpart/busybox-1.36.1`. Harness started on that revision and spanned the fixture-only commit `005dc4a482a09f715710887af9c7e66c898ce854` at 2026-10-25T11:23:06+08:00. The single covering host run and all commands after harness ran at `005dc4a4`; production sources are identical between these commits. The fixture changed only stdio inclusion plus a PASS summary after all assertions. No matrix repeat was needed for this test-output change.

Tools: clang 23.1.3; LLD/LLVM 23.1.3; QEMU 11.1.2; GNU Make 4.4.1. Exact environment: `/tmp/os01-task6-x86/environment.json`. Commands ran with authorized escalation because installed QEMU snapshot uses `/var/tmp`; no TMPDIR workaround, sandbox-denied run, timeout or abort replaced acceptance. ARM collector used its separate profile; no shared source mutation was performed by this collector.

## Commands and exits

Logs are `/tmp/os01-task6-x86/<name>.log`; exact shell-escaped commands are adjacent `<name>.command`, except the covering command (shown explicitly). Timestamps and exits are preserved in `/tmp/os01-task6-x86/status`.

| Name | Command | Exit | Result |
|---|---|---:|---|
| normal | `make PROFILE=x86_64-clang -j4 image` | 0 | normal image built |
| host | `make PROFILE=x86_64-clang test-host` | 2 | 141 binaries; test_lapic_nmi empty suite evidence; rc=0 binary rejected |
| harness | `make PROFILE=x86_64-clang test-harness` | 0 | 769 tests in 559.949s, aggregate OK (skipped=1); deliberate negative fixtures retained |
| static | `make PROFILE=x86_64-clang test-static` | 0 | all static audits PASS, including intr-dispatch-audit |
| phase0-smp1 | `env QEMU_SMP=1 make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | 0 | PASS, actual QEMU -smp 1 |
| phase0-smp2 | `env QEMU_SMP=2 make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | 0 | PASS, actual QEMU -smp 2 |
| syscall | `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` | 0 | all 99 syscall tests PASS |
| network | `make PROFILE=x86_64-clang OS01_NETTEST=1 test-qemu SUITE=network` | 0 | DHCP, UDP, DNS, TCP, wget, socket_exit PASS |
| selftest-smp2 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` | 2 | FAIL aggregate; 39 PASS, fd_refcount FAIL; x86_intr/uaccess/msr_safe PASS |
| selftest-smp4 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=4 test-kernel-selftest` | 2 | FAIL aggregate; 39 PASS, fd_refcount FAIL; x86_intr/uaccess/msr_safe PASS |
| intr-x86 | `make PROFILE=x86_64-clang test-qemu SUITE=intr-x86` | 0 | BP and QMP NMI PASS, no timeout, evidence accepted |
| host-covering | `make PROFILE=x86_64-clang test-host` at `005dc4a4` | 0 | one full covering rerun; 141 binaries pass plus root follow-up checks |

First host RED is preserved in `host.log`; it was a real protocol failure (`ERROR: test_lapic_nmi: empty output: no suite evidence`), not reclassified as PASS. Controller-authorized fixture-only fix and one covering rerun are in `host-covering.log`/`.status`. Focused fixer RED/GREEN are `/tmp/intr5-host-output-red.log` and `/tmp/intr5-host-output-green.log`.

## Archives and actual guest configuration

Full per-result path/status/argv/CPU/revision/image/outcomes manifest: `/tmp/os01-task6-x86/archive-manifest.json`. Each archive contains immutable `result.json`, `stdout.log`, `stderr.log`; these preserve guest image, actual QEMU argv and case-level verdicts. The following are this matrix's real guest/static archive paths (not fake harness fixtures):

- `build/x86_64-clang/logs/tests/runtime-audit/20261025T033017Z-46371f7946fc4b08aa865437e54360db/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/stack-canary-audit/20261025T033017Z-d0d18ae3b0d04a4a80c9dea0831db9a1/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/validate-kernel/20261025T033018Z-89fcc1daa72748f3b4c5bc60fb564f8f/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/runtime-link-order/20261025T033020Z-7e6dda0c38054d67b937d18aca3f8c21/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/kernel-runtime-link/20261025T033021Z-7c27a4ba9bd3442aa70e2615c4361ca8/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/syscall-boundary/20261025T033030Z-a518dd1f40d243c89427dc1e173283d2/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/kernel-layout/20261025T033030Z-8f27bc99c28c4b3caddb0cefe357c286/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/kernel-canary-contract/20261025T033031Z-aa6762e6969748acb97160042e055a83/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/header-object-audit/20261025T033032Z-978e5226799a4947b9cf3a1cf6cc2b9f/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/stack-frame-audit/20261025T033035Z-1fd69f8bee3e40bd9e3ce451bdab4ceb/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/driver-model-boundary/20261025T033035Z-371966400b1d4ce4b3fb1a9888e8470f/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/gui-build-audit/20261025T033035Z-6d0f6657db3b4f5391a91b76ee67a89e/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/intr-dispatch-audit/20261025T033036Z-442ad4e7e10840f5b1f70315788ef905/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/user-canary/20261025T033039Z-bf8ec311634848b1bd579c864b227ba8/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/phase-0/20261025T033042Z-59b5c10d3cb04486a1f038d97cdd00fe/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/phase-0/20261025T033049Z-376e8a1070ca46e9869804161718a5bc/result.json` — PASS; CPU 2.
- `build/x86_64-clang/logs/tests/systest/20261025T033100Z-124e4c938e0843e2830df6df7bec7816/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/network/20261025T033137Z-57eb7269b0e741ed917820f8a55f8b92/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/kernel-selftest/20261025T033156Z-520c56d4eea941a3961418ef4df68e50/result.json` — FAIL; CPU 2.
- `build/x86_64-clang/logs/tests/kernel-selftest/20261025T033203Z-ec5e0b3119374812970c7d92fc8624c1/result.json` — FAIL; CPU 4.
- `build/x86_64-clang/logs/tests/intr-x86/20261025T033213Z-9a7aebc0db4e435faa4f8d24129d8e24/result.json` — PASS; CPU 1.
- `build/x86_64-clang/logs/tests/intr-x86/20261025T033222Z-c42f21f19ef74e239f80e144dd9af7fc/result.json` — PASS; CPU 1.

Actual QEMU argv (copied from acceptance result.json):

- `phase-0/20261025T033042Z-59b5c10d3cb04486a1f038d97cdd00fe`: `qemu-system-x86_64 -M q35 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -m 512 -smp 1 -serial file:/tmp/os01_serial_6r0687v3.log -display none -no-reboot -no-shutdown`
- `phase-0/20261025T033049Z-376e8a1070ca46e9869804161718a5bc`: `qemu-system-x86_64 -M q35 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -m 512 -smp 2 -serial file:/tmp/os01_serial_7micq__a.log -display none -no-reboot -no-shutdown`
- `systest/20261025T033100Z-124e4c938e0843e2830df6df7bec7816`: `qemu-system-x86_64 -M q35 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/systest/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -m 512 -smp 1 -serial stdio -display none -no-reboot -no-shutdown`
- `network/20261025T033137Z-57eb7269b0e741ed917820f8a55f8b92`: `qemu-system-x86_64 -M q35 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/nettest/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -m 512 -smp 1 -serial file:/tmp/os01_serial_rl1fb47g.log -display none -no-reboot -no-shutdown -netdev user,id=net0,dhcpstart=10.0.2.20 -device e1000,netdev=net0`
- `kernel-selftest/20261025T033156Z-520c56d4eea941a3961418ef4df68e50`: `qemu-system-x86_64 -M q35 -smp 2 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/selftest/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -m 512M -display none -serial stdio -no-reboot -no-shutdown`
- `kernel-selftest/20261025T033203Z-ec5e0b3119374812970c7d92fc8624c1`: `qemu-system-x86_64 -M q35 -smp 4 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/selftest/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -m 512M -display none -serial stdio -no-reboot -no-shutdown`
- `intr-x86/20261025T033213Z-9a7aebc0db4e435faa4f8d24129d8e24`: `qemu-system-x86_64 -M q35 -m 512 -smp 1 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/intr-x86-fault-bp/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -snapshot -display none -serial stdio -no-reboot -no-shutdown -qmp unix:/tmp/os01-qmp-z29vgbr7/qmp.sock,server,nowait`
- `intr-x86/20261025T033222Z-c42f21f19ef74e239f80e144dd9af7fc`: `qemu-system-x86_64 -M q35 -m 512 -smp 1 -drive if=pflash,format=raw,readonly=on,file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/firmware/OVMF.fd -drive file=/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/intr-x86-fault-nmi/disk.img,format=raw,if=none,id=disk -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 -snapshot -display none -serial stdio -no-reboot -no-shutdown -qmp unix:/tmp/os01-qmp-90jv9flq/qmp.sock,server,nowait`

## 48-vector and recovery evidence

`intr-dispatch-audit` archive stdout proves the installed `20 exceptions + 28 IRQs` surface, native entry chains, gates/source evidence, exact one-read CR2 #PF chain and production backend edges. This is object/source proof, not a claim that every physical exception was injected.

Both selftest guest logs independently contain:

```
[selftest] uaccess: dispatch longjmp restore OK
[TEST] PASS uaccess
[TEST] PASS msr_safe
[selftest] x86_intr: runtime IDT 48/48 PASS
[selftest] x86_intr: int ff GPR/RSP/RFLAGS/DS/ES and return-work PASS
[selftest] x86_intr: live BSP/AP timer, remote TLB and resched PASS
[TEST] PASS x86_intr
```

The remote CPU probe reports `cpu=1 flush=2222 gen=2 ack=3 AP ticks=1 restored=1` in both configs; post-IPI progress is 38105 at SMP2 and 17401 at SMP4. This is real BSP/AP timer and remote TLB/resched IRQ execution. `int ff` is software spurious proof. BP is software #BP with saved-RIP/resume-label equality evidence; NMI is QMP-injected NMI with accepted native evidence. Device networking passes exercise integrated real NIC IRQs. Host production-chain tests cover synthetic decode/routing/ACK cases; they do not prove physical MCE/double-fault reliability. No physical MC/DF reliability claim is made.

## Selftest first failures and same-configuration baseline

Both new aggregate runs fail only `fd_refcount` (`get-vs-detach: FAIL (no cross-CPU overlap reader=1 writer=3)`); pin-vs-detach passes. These first runs were retained and not rerun. Baseline evidence is from pre-Task4 `441fdd2e`, as documented in task-4-report.md; the leftover baseline copy has no Git identity (`git_revision=unknown` in result.json), so its revision attribution relies on the Task4 recorded source comparison, not a fresh checkout verification by this collector. This collector inspected both archived baseline result.json and serial evidence:

| Baseline CPUs | Archived result.json | Aggregate | Same diagnostic found |
|---:|---|---|---|
| 2 | `/tmp/os01-task4-baseline/build/x86_64-clang/logs/tests/kernel-selftest/20261024T144209Z-b3fca2f759494e57b2828ab03cca14ca/result.json` | FAIL, 38 PASS / 1 FAIL | True |
| 4 | `/tmp/os01-task4-baseline/build/x86_64-clang/logs/tests/kernel-selftest/20261024T135106Z-7110ca8170004172b2a7ec6d87baba87/result.json` | FAIL, 38 PASS / 1 FAIL | True |

New runs are 39 PASS / 1 FAIL because x86_intr adds one passing case. Aggregate failures remain FAIL; the baseline comparison supports identifying this unchanged fixture failure without weakening or deleting it. Required phase1 uaccess and safe-MSR and new x86_intr cases pass despite aggregate failures.

## Normal-build absence and image guard

Before variants, fresh normal `llvm-nm` exit 0 found zero `selftest_x86`, `test_x86_intr`, `fault_probe`, `x86_intr_fault` symbols. Normal ipi.o/lapic_timer.o/sched/core.o/entry.o have no selftest_x86 observer refs (`/tmp/os01-task6-x86/normal-cleanliness.log`). Intr-x86 normal-image hash sandwich is recorded by the suite in `build/x86_64-clang/image/normal.before` and `normal.after`; both digests were verified identical below.

## Limits

All x86 §9.4 commands ran; ARM evidence is owned by the separate collector. The harness's one SKIP remains a SKIP (see aggregate output), not a guest IRQ pass. No source fix was attempted for selftest baseline failures. No physical MCE/DF injection, exhaustive device hardware coverage or all-green selftest aggregate is claimed.

`normal.before`: `87856eef8a7670f95a416c52cb55db7c41d9ad0ba237300ccc82000b069699c3  /home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/disk.img`

`normal.after`: `87856eef8a7670f95a416c52cb55db7c41d9ad0ba237300ccc82000b069699c3  /home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/x86_64-clang/image/disk.img`

Final observed git status:

```
 m thirdpart/busybox-1.36.1
```


## Historical failures and corrections

Task 4's full kernel-selftest runs at SMP2/SMP4 reported `total=40 passed=39 failed=1`, with `x86_intr` PASS and `fd_refcount get-vs-detach` failing for lack of cross-CPU overlap (`reader=1 writer=3`); `pin-vs-detach` passed. Same-configuration pre-Task-4 baseline `441fdd2e` at SMP2/SMP4 reported `total=39 passed=38 failed=1` with that identical failure. This establishes a pre-existing failure; it does not turn the full suite into PASS. Final matrix results must retain their own exit/status.

The first Task 6 host run genuinely failed: `test_lapic_nmi` returned zero but emitted no suite evidence. `005dc4a4` adds a success line after existing assertions; strict runner acceptance and assertions are unchanged. Original failure `/tmp/os01-task6-x86/host.log`, focused RED `/tmp/intr5-host-output-red.log`, GREEN `/tmp/intr5-host-output-green.log` remain preserved. The covering full-host rerun at `005dc4a4` passed all 141 binaries and root follow-up checks; exact logs and archive manifest are recorded above.

Task 5's early-boot-only/QEMU explanation was false: OS01 masked LINT1. Firmware-described routing and post-percpu evidence supersede it. Earlier phase-0 `SMP=2` alone actually ran one CPU; only explicit `QEMU_SMP=2` plus `-smp 2` and observed online CPUs count as SMP2 evidence. Initial sandbox `/var/tmp/vl.*` errors had empty guest logs and are environment failures, never guest PASS.

## ARM final source-revision evidence

### Collector record

Revision: `476eb6870e059ef04169bdb25505fd4cecb54ec0`. Branch: `codex/intr-dispatch-phase2`.
Worktree: `/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01`. Profile: `aarch64-clang`. Collected 2026-10-25.

Before and after the commands, `git status --porcelain` was exactly ` M thirdpart/busybox-1.36.1`. This pre-existing submodule dirt was preserved. No source changes, global clean, commits, or overlapping ARM profile runs were performed. Historical task-6-arm-evidence.md remains unchanged.

Tools: clang 23.1.3; LLD 23.1.3; QEMU aarch64 11.1.2; Python 3.14.8; GNU Make 4.4.1; Linux 7.2.9-arch1-1 x86_64.

## Exact commands and exit status

| Command | Exit | Log |
|---|---:|---|
| `make PROFILE=aarch64-clang aarch64-uefi` | 0 | `/tmp/os01-intr2-task6-arm-final/build.log` |
| `make PROFILE=aarch64-clang test-aarch64-audit` | 0 | `/tmp/os01-intr2-task6-arm-final/audit.log` |
| `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | 0 | `/tmp/os01-intr2-task6-arm-final/sync-fault.log` |
| `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | 0 | `/tmp/os01-intr2-task6-arm-final/smp.log` |
| `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | 0 | `/tmp/os01-intr2-task6-arm-final/gic-spi.log` |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | 0 | `/tmp/os01-intr2-task6-arm-final/m3-probe.log` |

All commands ran once and exited 0. No command was skipped, timed out, or reclassified. Normal build reused unchanged runtime inputs. Audit PASS; sync-fault PASS with fatal observed; SMP 9/9 PASS with CPU counts 1, 2, 4 repeated three times; GIC-SPI PASS with one injected/handled interrupt; m3-probe PASS with two CPUs and SHOOTDOWN-PROBE: OK.

## Archives

Paths below are relative to the worktree; each contains result.json plus raw logs. All result.json records pin the revision above, git_dirty=true, status=PASS and runner_exit_code=0.

| Archive | CPU count | Status | Child exit | Outcomes |
|---|---:|---|---:|---|
| `build/aarch64-clang/logs/tests/aarch64-gic-spi/20261025T032047Z-8b1c84524f8044f2bdad4f65674281dd` | 1 | PASS | 0 | `[{"armed_count": 1, "died_before_bind": false, "errors": [], "handled_count": 1, "injected": true}]` |
| `build/aarch64-clang/logs/tests/aarch64-m3-probe/20261025T032048Z-3f6211251dea46968291ccac397257ce` | 2 | PASS | 0 | `[{"errors": [], "ok_observed": true, "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-sync-fault/20261025T031951Z-4603c77bafee4f0cbbfe43adbd0b79a2` | 1 | PASS | 0 | `[{"errors": [], "fatal_observed": true, "qemu_self_exited": false, "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032000Z-8b84958caffc41998a375c3194eef660` | 1 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032005Z-5d84e9559f674189bdbaf28783adc1fd` | 1 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032009Z-bf073ee082ea40928c06aa850bf4ae45` | 1 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032014Z-eec7a189a1324b889468f6d58594af9e` | 2 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032019Z-cdbddd8ea92b4ec3affd7aaf9b990071` | 2 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032023Z-2ea6856ba0064c16b9eefd54596d2d95` | 2 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032028Z-b6318838d2974d9c9a96263d94cac71e` | 4 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032033Z-2268390132554ebd8b43414e98baa4d3` | 4 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032039Z-e5a2f558367a491cbdfab95bcbe767d4` | 4 | PASS | 0 | `[{"complete": true, "errors": [], "timeout": false}]` |
| `build/aarch64-clang/logs/tests/intr-dispatch-audit/20261025T031950Z-5e56be1dfa384f1c98395221e049b709` | 1 | PASS | 0 | `[{"argv": ["python3", "qemutests/intr_dispatch_audit.py", "--profile", "aarch64-clang", "--elf", "/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01/build/aarch64-clang/artifacts/kernel.elf", "--llvm-nm", "llvm-nm", "--llvm-objdump", "llvm-objdump", "--repo-root", "/home/aagu/.codex/worktrees/intr-dispatch-phase2/OS01"], "audit": "intr-dispatch-audit", "child_exit_code": 0, "errors": []}]` |

## Runtime evidence

`build/aarch64-clang/logs/tests/aarch64-gic-spi/20261025T032047Z-8b1c84524f8044f2bdad4f65674281dd`:

```
[smp] topology source=uefi-dtb cpus=1
[smp] requested=1 online=1 status=PASS
[gic] spi-test armed intid=33
[gic-spi] intid=33 handled count=1
```

`build/aarch64-clang/logs/tests/aarch64-m3-probe/20261025T032048Z-3f6211251dea46968291ccac397257ce`:

```
[smp] topology source=uefi-dtb cpus=2
[smp] requested=2 online=2 status=PASS
SHOOTDOWN-PROBE: START
SHOOTDOWN-PROBE: OK
```

`build/aarch64-clang/logs/tests/aarch64-sync-fault/20261025T031951Z-4603c77bafee4f0cbbfe43adbd0b79a2`:

```
[aarch64-sync] FATAL mpidr=0x0000000080000000 ec=0x25 esr=0x0000000096000004 elr=0xffff000040099844 spsr=0x00000000600003c5 far=0xffff800000000000
```

`build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032028Z-b6318838d2974d9c9a96263d94cac71e`:

```
[smp] topology source=uefi-dtb cpus=4
[smp] requested=4 online=4 status=PASS
[gic] spi-test armed intid=33
SHOOTDOWN-PROBE: START
SHOOTDOWN-PROBE: OK
[selftest] m3mc: 5/5 PASS
[gic-probe] save-restore OK
[gic-probe] unexpected intid=40 survived
[ipi] summary targets=3 received=3 status=PASS
```

`build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032033Z-2268390132554ebd8b43414e98baa4d3`:

```
[smp] topology source=uefi-dtb cpus=4
[smp] requested=4 online=4 status=PASS
[gic] spi-test armed intid=33
SHOOTDOWN-PROBE: START
SHOOTDOWN-PROBE: OK
[selftest] m3mc: 5/5 PASS
[gic-probe] save-restore OK
[gic-probe] unexpected intid=40 survived
[ipi] summary targets=3 received=3 status=PASS
```

`build/aarch64-clang/logs/tests/aarch64-uefi-smp/20261025T032039Z-e5a2f558367a491cbdfab95bcbe767d4`:

```
[smp] topology source=uefi-dtb cpus=4
[smp] requested=4 online=4 status=PASS
[gic] spi-test armed intid=33
SHOOTDOWN-PROBE: START
SHOOTDOWN-PROBE: OK
[selftest] m3mc: 5/5 PASS
[gic-probe] save-restore OK
[gic-probe] unexpected intid=40 survived
[ipi] summary targets=3 received=3 status=PASS
```

## Anomalies and limits

SMP command log contains 1311 transient `FAIL:` lines. As documented in the historical report, these are per-poll missing-marker diagnostics while serial output is incomplete, a harness output-hygiene issue. They are disclosed, not suppressed. All nine final case events and archives are PASS, complete=true, timeout=false, errors=[]. Pattern counts:

```
1045 FAIL: gic evidence '^\\[gic\\] GICv2 driver: intids=\\d+$' found 0, want 1
233 FAIL: gic evidence '^\\[gic\\] dispatch ready$' found 0, want 1
10 FAIL: gic evidence '^\\[gic-probe\\] save-restore OK$' found 0, want 1
9 FAIL: gic evidence '^\\[ipi\\] summary targets=(\\d+) received=(\\d+) status=PASS$' found 0, want 1
8 FAIL: gic evidence '^\\[gic-probe\\] unexpected intid=40 survived$' found 0, want 1
5 FAIL: gic evidence '^\\[ipi\\] send sgi=0 filter=others$' found 0, want 1
1 FAIL: ipi bsp raw_iar lines 0, cpus=2
```

QEMU-backed archives report stopped_by_runner=true: the runner intentionally stops QEMU after evidence appears; sync-fault qemu_self_exited=false. The single-CPU SMP m3mc selftest emits its expected single-CPU SKIP; the SMP matrix command and all nine cases completed and passed. This does not constitute multicore evidence for the one-CPU topology.

All 13 archive tool_versions fields are empty; direct host versions are recorded above. GIC-SPI has no metadata.json; result.json retains argv. Evidence supports the assigned ARM acceptance commands only; x86 validation is collected independently. No product fixes were made.

Runtime CPU topology cross-check: every SMP serial archive contains `requested=N online=N status=PASS`, matching its recorded cpu_count (N=1,2,4, three archives each). m3-probe records and observes two online CPUs; sync-fault and GIC-SPI run one CPU. Audit cpu_count=1 is archive metadata, not a QEMU boot.

## Decision audit

Current decisions: firmware-described MADT NMI routing; post-percpu probe; AP descriptor/IST readiness before NMI unmasking; explicit runtime CPU proof; actual per-command revision; explicit `gpt-6.1-sol` implementation/integration and `gpt-6-astra` architecture/final review. The historical Claude alias/quota rulings are superseded and do not describe current model selection.

The following verbatim scratch-ledger rulings are an audit trail, not all current policy. Entries about MADT diagnostic-only/all-CPU unconditional LINT1 and Claude alias/model policy are explicitly superseded by later rulings. Historical early-only QEMU and command-only SMP2 claims are corrected above.

**Decision record:**

> Ruling: native managed worktree created from exact spec commit; current main workspace left unchanged except requested spec commit — isolation is reversible and necessary for execution.

**Decision record:**

> Ruling: use six sequential task implementers and independent task reviews under subagent-driven-development; standard gpt-6.1-sol for implementation/reviews, gpt-6-astra for final architecture review — skill model-selection requirement.

**Decision record:**

> Ruling: no .codegraph in this fresh worktree; per AGENTS skip CodeGraph here, use rg. Original main checkout still indexed.

**Decision record:**

> Ruling: existing phase1 audit may pass despite checking only4 stubs rather than fail; Task3 still mandatory to prove expanded coverage. No assertion that a phase1 audit PASS certifies all20.

**Decision record:**

> Ruling: Task3 includes minimal _set_gate inline-asm constraint repair with failing actual-descriptor regression: current RCX input-only operand is written and reused by compiler, so consecutive IST1 gates install actual IST0. Controller independently confirmed second gate lacks RCX reload in normal ELF. Preserve all declared policy values; Task4 runtime confirms. Cost if wrong: small gate-encoding change requires clean rebuild and revalidation, reversible on branch.

**Decision record:**

> Ruling: Task4 uses gpt-6-astra implementer due real cross-CPU mapping/ack cleanup and assembly sentinel design judgment; standard model remains appropriate for fixture/runner integration tasks. Cost if wrong: extra model cost, no product semantic change.

**Decision record:**

> Ruling: keep the WIP in the tree (it encodes real design work: 48-gate runtime IDT snapshot, spurious sentinel, cross-CPU TLB/resched/timer probe) and hand it to a fresh implementer as an untrusted draft to audit, fix, complete, test and commit — rather than discarding it and re-deriving the design. Cost if wrong: one extra round of review on a design that has to be reworked anyway.

**SUPERSEDED historical claim:**

> Ruling: Task4 implementer model = opus (multi-file kernel + inline asm sentinel + cross-CPU/CR3/TLB judgment; prior ledger already ruled opus-class model for this task's risk profile). Cost if wrong: extra model cost, no product semantic change.

**SUPERSEDED historical claim:**

> Ruling: model alias mapping discovered in ~/.claude/settings.json — opus => glm-5.3[1m] (the account-level 5-hour cap target, reset 2026-10-25 03:18), sonnet => minimax-m3[1m] (this session's own model, healthy), haiku => glm-5.3-flash[1m] (the same upstream that serves the safety classifier). The earlier Task4 dispatch on opus and its reviewer therefore ran on glm-5.3 and worked until that tier's quota ran out. Task5 re-dispatched on sonnet.

**SUPERSEDED historical claim:**

> Ruling: Task5 implementer model = sonnet/minimax-m3 (the standard tier). The ledger's own earlier ruling already classed fixture/runner integration work as standard-model territory, and Task 5 is exactly that (harness + build-system wiring + a guest probe), so this is a risk-tier match, not only a quota workaround. Cost if wrong: the QMP/evidence semantics may need one extra review round.

**SUPERSEDED historical claim:**

> Ruling: all Task5/Task6 implementer and reviewer dispatches in this session use sonnet until the glm-5.3 tier quota resets; the final whole-branch review also goes to sonnet, which is a downgrade from the plan's most-capable-model rule — recorded here so the final report can state it honestly.

**Decision record:**

> Ruling: fix the LINT1 gap in production and restore the brief's mandated probe placement. Rationale: spec 9.3 requires a REAL NMI delivery (QMP inject-nmi -> do_nmi(2)), which is only reachable when LINT1 is programmed for NMI delivery; with LINT1 masked the current evidence proves the IDT path under a pre-kernel default rather than the product's own configuration. Masking LINT1 is itself a latent product defect (the kernel cannot take a hardware NMI after APIC init, e.g. NMI button/watchdog) and the installed vector-2 gate + IST1 + do_nmi fatal path are otherwise dead. The change is small, does not touch gate type/DPL/IST, error-code conventions or return labels, and restores conformance to the brief's own wording rather than expanding scope. Cost if wrong: a spurious NMI would now be delivered and fatal the guest; bounded by MADT/hardware reality and covered by the phase-0 boot re-run I am requiring.

**SUPERSEDED historical claim:**

> Ruling: MADT NMI entries stay diagnostic (the kernel parses them but cannot route NMI sources); LINT1 is programmed to NMI delivery on every CPU at LAPIC init, matching the standard x86 behaviour where MADT type-4 entries describe NMI SOURCES, not a per-CPU delivery enable. The implementer must justify this choice against the MADT data it already parses.

**Decision record:**

> Ruling: fold the reviewer's Minors 2,3,4,5,6 into the fix round. Minor 5 (archive-write failure swallowed while the run still exits 0) is upgraded to Important — it is a swallowed error and directly contradicts Step 4's 'archives complete'. Minor 4 (the 'QMP response arrives after the diagnostic' fixture never actually stages that ordering) is upgraded because Step 1 names that exact ordering as a required fixture. Minor 3 is a verbatim duplicate fixture. Minors 1,7,8 are deferred to the final review (Minor 8 becomes moot once the probe moves back to the after-boot_percpu placement).

**Decision record:**

> Ruling: supersede Claude controller claim that MADT type4 is only diagnostic. ACPI6.5 section5.2.12.7 explicitly requires appropriate per-processor LINT programming, including UID/all-CPU and flags. Complete smallest correct NMI-delivery fix using firmware-described routing (or preserve validated firmware NMI state with explicit evidence), not unconditional all-CPU LINT1 assumption. Source https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html#local-apic-nmi-structure . Cost if wrong: additional bounded APIC configuration work and clean rebuild; avoids configuring unrelated pins on physical machines.

**Decision record:**

> Ruling: Claude model-alias/quota workarounds are superseded on Codex takeover; use explicit available gpt-6.1-sol integration implementers and gpt-6-astra architecture/final reviewers per SDD. Prior completed approvals stand; Task5 resumed fix needs fresh review because old Claude reviewer unavailable. Cost if wrong: one extra review context.

**Decision record:**

> Ruling: record exactper-commandrevision rather than redundantlyrerun unchanged ARM/guestmatrix after host-fixture-onlycommit. Supersedes Claude sameallarchiveSHA objective; production revision476eb687 remains identical, changedhostfixture gets focused+fullhostcoveringvalidation. Cost if wrong: reportmust clearlydistinguish hostfixtureSHA from unchangedproductSHA; avoids duplicating unchangedtests.

## Fresh baseline provenance verification

On 2026-10-25 the controller independently compared `git archive 441fdd2e -- kernel mk qemutests hosttests Makefile runtime libc config` with `/tmp/os01-task4-baseline`: **1,015 files checked, zero missing, zero mismatches**. This strengthens the recorded source attribution for the same-configuration baseline. The archived baseline `result.json` still truthfully says `git_revision=unknown`; the fresh source comparison supplies provenance rather than rewriting archive metadata.

## Named user-mode and recovery cross-check

The final systest serial archive above contains `[TEST] PASS` for `54b_user_fault_divzero` (SIGFPE, status `0x8`), `54c_user_fault_write_ro` (SIGSEGV, status `0xb`, eight children waited successfully), `54d_user_fault_ud2` (SIGILL, status `0x4`), and `54e_user_fault_gp` (SIGSEGV, status `0xb`), plus `54e_parent_continues` (next child waited with status `0x700`). The 99-case aggregate passed. This checks the named user-mode signal paths and parent survival; it is not coverage of every ISA exception from user mode. The same serial archive records COW kernel-output child privatization and unchanged parent buffers, including `[TEST] PASS 53_cow_kernel_outputs`. Both selftest configurations retain real #PF dispatch longjmp and safe-MSR evidence.

The isolated BP archive records ELF resume address `0xffff80000015ab1f`, accepted evidence, `timeout=false` and `qemu_self_exited=false`; runner termination after the fatal observation is intentional. Fatal #BP does not return or execute the resume label. NMI is injected only after the required `stage=post-percpu` marker and is judged by native fatal diagnostic and absence of continuation, independently of QMP response timing.

## Final independent review and delivery

The controller performs the independent task/whole-branch review after this documentation commit. Targeted acceptance and complete command execution do not erase the two full-suite failures or the baseline attribution limit (`git_revision=unknown` in the baseline archives). No required case was skipped to obtain approval. A docs-only commit follows the tested source revisions; archives pin their actual source/test-fixture revisions, not that future documentation SHA. No merge or push is performed.
