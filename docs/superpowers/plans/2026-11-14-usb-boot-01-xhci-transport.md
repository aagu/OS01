# USB Boot M1a: xHCI Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide a synchronous, DMA-safe polling transport for one MSC interface per xHCI slot.

**Architecture:** The x86 controller owns the DMA window and serializes event/command submission. New endpoint operations are tested with synthetic rings before any block driver binds.

**Tech Stack:** Freestanding C, xHCI TRBs, hosttests

**Spec:** [USB boot design](../specs/2026-11-14-usb-boot-design.md). Requires M0 gate. §§5.1–5.3, 5.10–5.11 (xHCI portions).

## Global Constraints

- x86_64 / QEMU first, polling only; root-port direct attach, BOT/SCSI LUN 0, logical sectors exactly 512 bytes.
- GPT + UEFI; no xHCI IRQ, UAS, hubs, SET_INTERFACE, or hotplug. aarch64 shares the handoff ABI only.
- Kernel operational messages use `log_*`; new hardware uses optional subsystem registration, not generic `kernel_main`.
- Use profile artifacts and private test images. Leave unrelated suites and AHCI baseline behavior intact.
- At each commit stage only the task's explicit files, review `git diff --cached --check`, then use the indicated message.

## Review Focus

- Inactive alternate cannot supply an otherwise missing endpoint or overwrite an active DCI (Task 1).
- An EP0 error points to Setup; it must not be dropped while waiting for Status (Task 2).
- Link wrap when abandoning a Halted TD requires the *next* Normal TRB and its DCS, including cycle 0 (Task 3).
- An idle peer pipe still needs host toggle reset after device ClearFeature (Task 3).
- Both enumeration and newly published transports must handle unproven ownership without freeing or reusing DMA (Task 2).

**Files and ownership:** `kernel/include/driver/xhci.h` declares transport-facing operations and slot state; `kernel/arch/x86_64/driver/xhci.c` owns TRBs/locks/DMA. Reuse `hosttests/cases/test_xhci_phase2.c`, `test_xhci_phase3.c`, and their link fixtures in `hosttests/mk/drivers.mk`. The same `xhci_production.o` also links into `test_xhci_init.c` and `test_usb_devfs.c`; the shared boundary is `hosttests/mock/arch9/xhci_runtime.h`.

---

### Task 1: Bind an active interface to its endpoint rings

**Files:** xHCI header/source and phase3 fixture.

**Interfaces:** produce `struct xhci_msc_iface { bool active; uint8_t interface_num, in_addr, out_addr, in_dci, out_dci; }` as `slot->msc_iface`. Existing `slot->iface` remains interface 0 / alt 0.

- [ ] **Write red** `msc_alt_zero_pair`: fixtures with nonzero interface number, two complete interfaces (first complete wins), incomplete first and complete second, inactive alternate reusing an active DCI with different MPS, and IN/OUT split across interfaces. Assert `in_addr=0x81,in_dci=3,out_addr=0x02,out_dci=4` for the chosen pair, no cross-interface pair, and no ready MSC interface after failed SET_CONFIGURATION.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-xhci-phase3` fails the new interface assertions.
- [ ] **Implement:** track interface and alt in both descriptor passes, limit to actual returned configuration length, discard malformed/truncated candidates and invalid bulk DCI/MPS; select first complete class/subclass/protocol `08/06/50` pair with alt 0. Validate instantiated ring directions/types. Preserve the actual descriptor count independently of later return codes and bound the raw lsusb cache allocation, copy and reported length to it; inject a truncated reply that advertises a larger length. Initialize/clear state on setup/unwind, publish only after SET_CONFIGURATION succeeds. Seed companion parsing and Max Burst for SuperSpeed bulk; missing/invalid companion makes that MSC interface ineligible; streams disabled.
- [ ] **Run green:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-xhci-phase3`; existing descriptor/lsusb-compatible assertions still pass.
- [ ] **Commit:** `feat(xhci): identify active MSC bulk endpoints`.

### Task 2: Add synchronous polling primitives and safe timeout ownership

**Files:** xHCI header/source, phase2/phase3/init/USB devfs fixtures, `hosttests/mock/arch9/xhci_runtime.h`, `hosttests/mk/drivers.mk`. Share narrow host definitions for mutex and scheduler gate or link their required mocks in all four.

**Interfaces:** produce `int xhci_control_no_data(struct xhci_controller *c, uint8_t slot, uint8_t type, uint8_t req, uint16_t value, uint16_t index)`; `int xhci_bulk_transfer(struct xhci_controller *c, uint8_t slot, uint8_t dci, void *cpu_buf, size_t len, size_t *actual)`. CPU buffer is never DMA'd directly. Actual initialized to zero, successful IN may be short, STALL is `-EPIPE`, timed out TD is `-ETIMEDOUT` and quarantined. Other errors return `-EIO`/validation errno; if ownership/state cannot be established, reject further I/O.

- [ ] **Write red** `bulk_event_identity`, `bulk_wrap_and_boundary`, `ep0_no_data_error`, `timeout_owns_dma`: inject wrong slot/DCI/TRB then true event; len 65536 and residual 512 → 65024, invalid residual and short OUT rejected, 65537/unaligned DMA window rejected; index 254 → Link/C=1 → index 0/C=0; EP0 Setup STALL recognized; descriptor Data success/short does not complete the call before its Status, residual comes from matching Data, stale pointer and wrong DCI are discarded, Status failure is not reported as a successful read, and stage boundary at Link preserves next cycle; timeout + late event never frees/reuses slot DMA, Address Device timeout inside its own error path keeps DCBAA/context/pages and prevents later doorbells; post-address non-timeout failure with failed Disable Slot also retains them; controller start unwind/remove with HCH not established keeps controller/slot DMA alive.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-xhci-phase2 test-xhci-phase3`.
- [ ] **Implement:** lazy per-slot 64 KiB staging, explicit physical alignment/range verification (read AC64 for the staging check), separate from EP0 and CPU cache. Publish single Normal TRB with TD Size=0, IOC, cycle ordering and a DMA barrier before doorbell; match ED=0 pointer + slot/DCI, copy only confirmed IN bytes. For no-data SETUP use IDT, TRT none, Status IN + IOC. Migrate existing descriptor and SET_CONFIGURATION paths to the same slot/DCI=1/stage-address event handling: descriptor Data uses IOC to supply its actual/residual and Status uses IOC to retire the request; no descriptor copy or next request until matching successful Status. Handle error on any recorded stage; keep Link CH=0 between these one-TRB stage TDs. Existing enumeration timeouts must mark ownership unsafe before cleanup. Audit `xhci_address_device()` internal err_cmd, `xhci_probe_ports()` post-address failure, `xhci_probe()` start unwind, and `xhci_remove()`. Once xHC may know a slot/context, require successful Disable Slot or confirmed controller halt before clearing DCBAA or freeing any reachable allocation, including on non-timeout failures. Before publication of a context, unwind unexposed CPU allocations normally. Keep slot/command objects and context/staging live if stop/disable unproven; no later use on poisoned slot; poison controller on unretired command. An unsuccessful HCH wait in remove/unwind cannot authorize free. One controller mutex spans transfer + event polling + copies, with locked internal helpers for Task 3; initialize before use and gate lock by `vfs_sched_ready()`, sampling once. No nested reacquisition.
- [ ] **Run green:** phase2/3 commands above; `make PROFILE=x86_64-clang kernel.bin` links; live regressions are checked using AHCI `make test-qemu SUITE=phase-0` (M0 fatal assertion can still run before MSC binding).
- [ ] **Commit:** `feat(xhci): submit polling transfers with explicit DMA ownership`.

### Task 3: Resynchronize the two sides of a bulk pipe

**Files:** xHCI header/source, phase3 fixture.

**Interfaces:** produce `int xhci_clear_halt(struct xhci_controller *c, uint8_t slot, uint8_t ep_addr)`; caller serializes the whole BOT transaction. Consumes Task 2's locked control helper and command-completion address filtering.

- [ ] **Write red** `clear_halt_state_drives_command_sequence`: assert Halted → type 14/TSP=0, EP0 02/01/value 0/index 0x81, type 16 targeting actual next physical TRB with DCS=0 for a wrapped producer; idle or Stopped → EP0 clear then Configure Endpoint with A0=1, A1/D0/D1=0, one target DCI A/D and retained Slot Context Entries, no Reset Endpoint. Inject EP0 STALL and command failure/timeouts, assert no restart or later submit on unsafe slot.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-xhci-phase3`.
- [ ] **Implement:** read synchronized Output Endpoint Context state while holding controller lock. Use spec §5.2 sequences with no healthy pipe reset command; construct a fresh input context preserving configuration, dequeue and DCS; exclude outstanding TDs from idle reconfigure. Validate command completion and EP0 result after each step. On failure quarantine the device or controller per Task 2. Export no independent public command primitives until needed.
- [ ] **Run green:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-xhci test-xhci-phase2 test-xhci-phase3 test-usb-devfs` and `make PROFILE=x86_64-clang kernel.bin` pass. Rerun the M0 USB firmware-only gate with this kernel and `test-qemu SUITE=phase-0` for AHCI. This transport milestone does not publish disks.
- [ ] **Commit:** `feat(xhci): resynchronize halted and idle bulk pipes`.
