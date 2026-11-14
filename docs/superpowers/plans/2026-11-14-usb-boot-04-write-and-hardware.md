# USB Boot M3: Writes and Hardware Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Verify persistent USB writes and make supported direct-attached hardware paths safe.

**Architecture:** The existing MSC ops gain writes and flush; xHCI gains bounded timeout recovery and hardware capability checks. QEMU verifies persistence and SuperSpeed; real hardware is conditional.

**Tech Stack:** C, BOT/SCSI, xHCI, QEMU, hosttests

**Spec:** [USB boot design](../specs/2026-11-14-usb-boot-design.md). Requires M1a, M1b and M2. §§5.2–5.4, 5.9–5.12 and 6 (M3); M4 is excluded.

## Global Constraints

- x86_64 / QEMU first, polling only; root-port direct attach, BOT/SCSI LUN 0, logical sectors exactly 512 bytes.
- GPT + UEFI; no xHCI IRQ, UAS, hubs, SET_INTERFACE, or hotplug. aarch64 shares the handoff ABI only.
- Kernel operational messages use `log_*`; new hardware uses optional subsystem registration, not generic `kernel_main`.
- Use profile artifacts and private test images. Leave unrelated suites and AHCI baseline behavior intact.
- At each commit stage only the task's explicit files, review `git diff --cached --check`, then use the indicated message.

## Review Focus

- Failed or uncertain WRITE can change media; overlapping read-ahead must be invalidated before sending it (Task 1).
- Empty-ring successful Stop still needs Force Stopped Event; a normal Transfer Event alone is inadequate (Task 2).
- A supported controller with 32-bit DMA must reject any span straddling 4 GiB, including late slot allocations (Task 3).
- Firmware ownership timeout must never proceed to HCRST; USB 3 port reset must use the correct link-state operation (Task 3).
- Guest cache alone cannot prove persistence or flush; verify two processes and the real SCSI command path (Task 4).

**Files and ownership:** `kernel/driver/usb_msc{,_bot}.c` owns write/flush and retry policy; `kernel/{arch/x86_64/driver/xhci.c,include/driver/xhci.h,arch/x86_64/driver/xhci_msc.c}` owns recovery and hardware checks; host fixtures from M1a/M1b plus private QEMU regression images from M2. No production write to a raw host device is part of this plan.

---

### Task 1: Enable write and flush through BOT

**Files:** `kernel/driver/usb_msc.c`, `kernel/driver/usb_msc_bot.c`, `hosttests/cases/test_usb_msc.c`.

**Interfaces:** set `ops.write = usb_msc_write` (`static int usb_msc_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)`) and `ops.flush = usb_msc_flush` (`static int usb_msc_flush(block_device_t *dev)`). Use M1b BOT interface with `retry_safe=false` for both.

- [ ] **Write red** `write10_payload_and_flush`, `write_uncertain_invalidates_window`: assert 129 sectors become 128+1 WRITE(10) CDBs `0x2a` with BE LBA/count and correct OUT payload; zero/overflow/out-of-range produces no command. Check SYNCHRONIZE CACHE(10) `0x35` with zero payload and no IMMED. Reject short OUT or successful CSW with nonzero WRITE residue; on lost CSW or transport error confirm no second WRITE/FLUSH, no stale overlap after possible media update, and no unrelated-window invalidation.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-usb-msc`.
- [ ] **Implement:** validate ranges, split at 128 blocks and invalidate any shared read-ahead range before the corresponding WRITE. An error means partial progress may have occurred; propagate failure without replay. Send synchronous FLUSH under the device lock. M1b's BOT framing continues to validate CBW and CSW, including exact no-data residue and full OUT success. Task 4 uses QEMU command trace as independent evidence; do not put test logs in the production driver.
- [ ] **Run green:** focused test plus `make PROFILE=x86_64-clang kernel.bin`.
- [ ] **Commit:** `feat(usb): persist SCSI writes and synchronize cache`.

### Task 2: Recover a timeout only after confirmed DMA retirement

**Files:** xHCI header/source and slot adapter, generic BOT header/source, phase2/3 xHCI fixtures and fake MSC transport tests.

**Interfaces:** extend `struct usb_msc_transport` with `int (*recover_timeout)(void *ctx, uint8_t ep_addr)` (optional; 0 guarantees retired timed-out TD and safe ring, negative means leave disabled). Export `int xhci_recover_bulk_timeout(struct xhci_controller *c, uint8_t slot, uint8_t dci)`. Existing bulk API returns `-ETIMEDOUT` while quarantining the TD, and exposes this one explicit recovery entry; ordinary I/O cannot run against pending DMA. Driver retains ownership until recovery or confirmed disable.

- [ ] **Write red** `stop_endpoint_recovery`: inject Stopped then matching command success, expect Stopped → Set TR Dequeue without Reset; inject old normal completion then FSE pointing to next dequeue and success, expect same result. Inject Stop Context State Error + Halted output context and assert halt recovery uses Reset(TSP=0) → ClearFeature → Set Dequeue. Missing FSE, unrelated DCI, failing/late command or Error state: no buffer reuse and no new doorbell. Fake BOT: after recovered timeout, full reset and two cleared pipes precede one safe retry; WRITE/FLUSH never replay; EP0/command timeouts remain terminal. A second timeout after a safe replay does not attempt another Stop/replay; keep the slot disabled even if a late completion arrives.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-xhci-phase2 test-xhci-phase3 test-usb-msc`.
- [ ] **Implement:** keep a per-slot pending DCI/TRB and frozen staging, acquire controller lock to run Stop Endpoint (type 15, SP=0) and consume the ring in order. Recognize Stopped/FSE by slot/DCI and stop event codes, not original pointer alone; require it preceding matching command success. On confirmed Stopped install normalized next dequeue/DCS. For provable Halted race use M1a halt primitive. Failure leaves DMA quarantined; command timeout disables controller operations. After host-side success MSC performs full BOT Reset Recovery, then can retry a safe command once, with a fresh tag; if that replay also times out keep the slot quarantined and return the timeout. Distinguish transport recoverable bulk timeout from EP0/command timeout via adapter pending entry, and do not perform reset unless transport recovery succeeds.
- [ ] **Run green:** all three focused suites, then the M2 QEMU matrix. QEMU omission of the required empty-ring FSE is not a success case; that transition is a synthetic test.
- [ ] **Commit:** `fix(xhci): retire timed-out bulk DMA before restarting BOT`.

### Task 3: Qualify controller ownership, address width and USB 3 ports

**Files:** xHCI header/source, phase2/3 fixtures and `docs/build/build.md` hardware scope notes.

**Interfaces:** internal `static bool xhci_dma_range_ok(const struct xhci_controller *c, uint64_t phys, size_t bytes)`, `static int xhci_claim_legacy(struct xhci_controller *c)`; `xhci_port_reset()` retains public signature. Scope decision: controllers reporting nonzero scratchpads remain explicitly unsupported, as spec §5.12 permits. Support default USB speed encoding only (PSIC=0 and known raw IDs); reject custom PSI and unknown speed IDs explicitly instead of guessing endpoint speed.

- [ ] **Write red** `xhci_ownership_and_dma`, `xhci_port_protocol_reset`: simulate HCCPARAMS1.AC64 clear; accept aligned span ending exactly at 4 GiB, reject overflow and any byte at or above 4 GiB. Assert no controller setup or slot/address command uses high DMA after rejected allocation. xECP points outside mapped aperture, successor truncates capability, custom PSI, unknown speed ID, BIOS ownership release before HCRST, stuck semaphore fails without reset, no legacy cap proceeds. USB2 PR, enabled USB3 U0 no reset, recovery from inactive USB3 uses WPR, unsupported or timed-out ports skipped; companion bMaxBurst programmed for SS bulk with no streams.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-xhci-phase2 test-xhci-phase3`.
- [ ] **Implement:** walk xECP inside the baseline mapped controller aperture (ctrl->mmio_len=0x1000), using checked dword offsets, nonzero forward Next and fixed payload bounds (Legacy ≥8 bytes, Supported Protocol ≥16, including declared PSI table before checking PSIC). Reject malformed/outside aperture or port registers beyond it; do not infer BAR size or map more MMIO in this task. Claim USBLEG support (set OS Owned, wait BIOS Owned clear, disable legacy SMI with correct RW1C writes) before HCRST; fail within 1 s using wrap-safe elapsed time from arch_clocksource_read_ns(), with a finite iteration safeguard if the early clock does not advance. Validate all DMA ranges before dereference by xHC: controller DCBAA/ERST/command/event rings, slot input/output contexts, EP0 DMA and endpoint rings and MSC staging. Existing allocator may provide high memory; refuse affected allocation cleanly, do not add a PMM allocator in this phase; do not publish invalid addresses or free unretired DMA. For ports parse Supported Protocol compatible port ranges; for USB 2 use PR and PRC, for USB 3 handle U0/PED and warm reset for inactive state (WPR/WRC), use hot reset only when allowed by reported link. Keep elapsed-time bounded waits and safe PORTSC masked writes; use a host clock fixture. Log nonzero scratchpads as unsupported; document zero-scratchpad, sufficient controller aperture and default known speed IDs as requirements. The Slot Context still takes the raw port speed ID.
- [ ] **Run green:** focused host tests, `make test-host`, x86 AHCI baseline. Verify no hard dependency on MSC for a platform without supported USB controller.
- [ ] **Commit:** `feat(xhci): validate firmware ownership and DMA capabilities`.

### Task 4: Verify persistent USB storage and supported SuperSpeed

**Files:** USB runner/unit tests, `mk/components/run.mk`, `docs/build/build.md`, `user/usb_boot_probe/{app.mk,main.c}`, `config/rootfs.mk`. Build `usb_boot_probe.elf` with the existing user app manifest (`APP_SOURCES := main.c`) and profile artifact publisher; filter `usb_boot_probe` from normal `ROOTFS_USER_PROGRAMS`. Inject it via debugfs with mode 0100755 at `/bin/usb-boot-probe` into the private persistence image only. Keep ordinary operations quiet.

**Interfaces:** runner cases `--case persistence|superspeed`; `--probe-elf FILE` points at `$(USER_ARTIFACT_DIR)/usb_boot_probe.elf` for persistence; `make test-qemu SUITE=usb-boot USB_BOOT_CASE=persistence` and `USB_BOOT_CASE=superspeed`. Add both to complete M3 matrix. Preserve previous default cases as part of the matrix.

- [ ] **Write red** `test_persistence_evidence`, `test_superspeed_argv`: fake evidence rejects missing write/flush acknowledgement, echoed persisted string, reused live session, overlay/discard, and speed=3 in SS test. Verify both persistence boots point to same private backing file, first writable and second read-only, while original image stays immutable.
- [ ] **Run red:** `python3 -m unittest qemutests.test_usb_boot_runner`.
- [ ] **Implement persistence:** on a job-private writable clone of the M2 USB image, boot, run `/bin/usb-boot-probe <random-marker>` via serial. The probe opens `/usb-persist.txt` with O_CREAT|O_EXCL|O_WRONLY, checks total write length for the marker plus newline, and emits exactly `USB_WRITE_SYNC_OK` only when close and raw `sync()` through `<sys/syscall.h>` both return 0 (BusyBox discards the sync result). Require status plus the normal prompt. For first boot enable `scsi_disk_new_request` and `usb_msd_send_status` via QEMU `-trace events=<private events file>,file=<private trace>`; assert genuine WRITE(10) opcode 0x2a followed by 0x35, both LUN 0 and with tag-correlated success CSW. Missing requisite trace events fail setup explicitly. A random marker appearing in first-boot command echo is never evidence for second boot. Close/reap QEMU cleanly after success. Relaunch using the same backing image without snapshot/ephemeral overlay and read the distinct marker through a fresh shell. Match standalone output lines; verify absence before creation or use a fresh random marker not present in image. Correlate QEMU transcripts with two invocations and source hash check. Make the persistence artifact prerequisite available only for the matrix/persistence case; document the test-only helper.
- [ ] **Implement SuperSpeed:** use `qemu-xhci,id=xhci,p2=0,p3=4`, slot port 1, USB storage with bootindex 0; require speed=4 and the M1b two mount sentinels. If physical hardware is available, use a separately authorized disposable pre-imaged 512-byte BOT USB drive on a supported zero-scratchpad root port; collect USB-only and USB + distinct internal disk logs. Missing hardware is recorded as not run, not emulated success.
- [ ] **Run green:** unit commands; `make PROFILE=x86_64-clang test-qemu SUITE=usb-boot` (all cases) and `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`. Persist summary/log paths for both processes and SuperSpeed output. If hardware is absent, state that explicitly.
- [ ] **Commit:** `test: prove USB writes survive reboot and SuperSpeed boots`.
