# USB Boot M1b: Read-only MSC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot the USB-only image to shell and read both partitions.

**Architecture:** A host-testable BOT/SCSI core accepts abstract transport callbacks. An x86 optional subsystem binds the transport to boot-time block publication; the existing first-disk path mounts the sole USB disk.

**Tech Stack:** Freestanding C, BOT/SCSI, block cache, QEMU

**Spec:** [USB boot design](../specs/2026-11-14-usb-boot-design.md). Requires M0 and M1a. §§5.1, 5.3–5.5, 5.9–5.11 (M1).

## Global Constraints

- x86_64 / QEMU first, polling only; root-port direct attach, BOT/SCSI LUN 0, logical sectors exactly 512 bytes.
- GPT + UEFI; no xHCI IRQ, UAS, hubs, SET_INTERFACE, or hotplug. aarch64 shares the handoff ABI only.
- Kernel operational messages use `log_*`; new hardware uses optional subsystem registration, not generic `kernel_main`.
- Use profile artifacts and private test images. Leave unrelated suites and AHCI baseline behavior intact.
- A new host test binary needs all four: source in `hosttests/cases/`, registration plus explicit `.o`/`.elf` rules and `TEST_BINS` in `hosttests/mk/<group>.mk`, and a `primary.selector` entry in `config/test-scenarios.json`. `qemutests/test_scenario_catalog.py::test_all_host_tests_mapped` fails otherwise.
- At each commit stage only the task's explicit files, review `git diff --cached --check`, then use the indicated message.

## Review Focus

- A valid CSW with status 1 fails a command without tearing down an otherwise synchronized pipe (Task 1).
- Stale tag, wrong length or repeated CSW STALL must not let the next CBW race the old command (Task 1).
- A failed probe must not register a disk or consume a name (Task 2).
- A later failing read or status DMA must not overwrite a previously valid window; concurrent readers see their own sectors (Task 3).
- A shell prompt or echoed command is insufficient evidence of either mount (Task 4).

**Files and ownership:** `kernel/include/driver/usb_msc.h` declares the transport and probe; `kernel/driver/usb_msc.c` owns BOT, disk registration and caching. `kernel/arch/x86_64/driver/xhci_msc.c` binds xHCI slots and owns arch-specific probe order. `hosttests/cases/test_usb_msc.c` drives production generic code; register the suite in `hosttests/mk/drivers.mk`.

---

### Task 1: Implement BOT framing and recovery

**Files:** MSC header, source and host suite. Use the transport struct verbatim from spec §5.1. For independent command-level testing put the internal contract in `kernel/include/driver/usb_msc_bot.h` and implementation in `kernel/driver/usb_msc_bot.c`; production and hosttest both compile that TU.

**Interfaces:** `struct usb_msc_bot { struct usb_msc_transport transport; uint8_t interface_num, in_addr, out_addr; uint32_t next_tag; bool dead; }`; `enum usb_msc_dir { USB_MSC_NONE, USB_MSC_IN, USB_MSC_OUT };`; `int usb_msc_bot_command(struct usb_msc_bot *b, const uint8_t *cdb, uint8_t cdb_len, enum usb_msc_dir dir, void *buf, uint32_t len, bool retry_safe, size_t *actual)`. Caller serializes the command. Returns 0 only on CSW pass, reports trusted bytes on pass, `-EIO` on command failure, `-ETIMEDOUT` on M1 timeout, rejects later commands if dead. Initialize `actual=0`; transport `-EPIPE` may report zero actual rather than claiming partial data.

- [ ] **Write red** `bot_wire_and_csw` and `bot_recovery`. Assert CBW length 31, `55 53 42 43`, tag echoed by 13-byte CSW with `55 53 42 53`, little-endian length, IN flag 0x80, LUN 0, CDB length and zero padding; bad tag/signature/length/illegal status and residue > len cause Reset Recovery. Check partial IN trustworthy length `min(actual,len-residue)`. Assert data STALL → clear data pipe → CSW; first CSW STALL → clear IN → read CSW; repeated STALL or phase error → reset 21/ff/value 0/index interface then clear IN/OUT and at most one fresh-tag retry. Status 1 does not reset. Short CBW (actual != 31), generic transport failure that loses synchronization, and a second failed replay each run full recovery; assert no data after a short CBW and no third CBW after the second desync. Recovery failure/timeout prevents any later CBW; no reset after M1 timeout.
- [ ] **Run red:** first register `test_usb_msc` in `hosttests/mk/drivers.mk` (explicit `.o`/`.elf` rules and `TEST_BINS`) and add its `primary.selector` entry to `config/test-scenarios.json`; then run `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-usb-msc`.
- [ ] **Implement:** serialize fields bytewise; validate CDB length 1–16 and direction/length agreement before issuing CBW. Test outcome ordering is the implementation contract; for forced phase error ignore residue and do not expose data. Status 1 returns error with no extra command. CBW success requires actual=31; no other CBW outcome proceeds to data. A non-timeout transport error that desynchronizes the command uses full recovery if the transport still permits it, even on the replay; once the one replay is spent return the original attempt's terminal error after restoring sync, no third CBW. M1 timeout is immediately terminal. Use bounded recovery and strictly increasing tag modulo uint32; read-only retry only after recovery. Keep transport-supplied callback state opaque.
- [ ] **Run green:** host command above; register the case in the normal host bucket.
- [ ] **Commit:** `feat(usb): implement BOT command framing and recovery`.

### Task 2: Probe and publish a read-only SCSI disk

**Files:** MSC header/source, xHCI adapter, host suite. `kernel/Makefile` only if needed to constrain the new source files; existing x86 wildcards and aarch64 source restrictions are the default.

**Interfaces:** `int usb_msc_probe(const struct usb_msc_transport *transport, uint8_t interface_num, uint8_t in_addr, uint8_t out_addr, const char *name, block_device_t **out)`; copies callbacks, takes no ownership of `transport->ctx`, publishes only after successful probe. `ctx` has static/slot lifetime. `static int usb_msc_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)` matches block ops. x86 wrappers dispatch to M1a `xhci_control_no_data`, `xhci_bulk_transfer`, `xhci_clear_halt` and map USB address only to the chosen slot's matching DCI.

- [ ] **Write red** `probe_publish_and_geometry`: TUR fail then pass; INQUIRY bounded printable vendor/product/rev; RC10 8 BE bytes for last LBA 255/block 512 → count 256; reject truncated RC10, 4096-byte logical block or 0xffffffff last LBA. Assert failed cases do not publish, `ops->write == NULL`, `parent == NULL`, and a 129-sector read is split 128+1 with BE LBA/count. Verify a short direct READ returns `-EIO`. Assert optional subsystem dependencies in the host fixture or x86 boot contract evidence.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-usb-msc`.
- [ ] **Implement:** support TUR (bounded retries for command failure, never retry poisoned transport), INQUIRY, READ CAPACITY(10), READ(10). Check block/LBA range using subtraction or checked addition, handle count zero, chunk at 128 blocks. Initialize device mutex, serialize commands using bootstrap gate; publish physical BLOCK_DISK with ops, capacity, name and private state. On probe failure free only CPU state; xHCI retains quarantined DMA. Add `SUBSYS_INITCALL` to register optional `usb-msc` in `SUBSYS_PHASE_STORAGE` with dependency `device-boot`; traverse configured controllers/slots in order, allocate names usba…usbz on success only. Log `usb_msc: usba: N sectors, 512-byte blocks`; let fs boot add devfs nodes.
- [ ] **Run green:** focused host suite and `make PROFILE=x86_64-clang kernel.bin`; verify existing AHCI-only boot still finds hda first.
- [ ] **Commit:** `feat(usb): publish read-only SCSI disks from xHCI`.

### Task 3: Add device-private read-ahead

**Files:** MSC source and host suite. **Interfaces:** block read callback unchanged; cache is internal. MSC owns separate 64 KiB CPU cache and scratch; adapter owns its DMA staging.

- [ ] **Write red** `read_ahead_window` and `read_ahead_concurrent`: 200 one-sector calls from zero produce exactly two READ CDBs, a crossing request copies the cached prefix then fills the remaining range from a new window (short refill succeeds only if its full sectors cover the requested suffix), LBA 255 of a 256-sector disk sends one block, partial prefetch of 1023 bytes exposes at most one sector, 511 bytes returns `-EIO`, and failed refill leaves previous window valid. Gate runtime locks with the host scheduler-ready fixture, deliberately interleave readers at a fake transport barrier and assert data and max simultaneous BOT commands per device = 1.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-usb-msc`.
- [ ] **Implement:** on single-sector miss, read `min(128, capacity-lba)` into scratch, validate complete sectors, publish window only on success. A multi-sector cache miss with no cached prefix uses exact bounded CDB chunks; direct short reads always fail. A crossing request with a cached prefix copies that prefix and prefetches from the first uncached LBA; for each refill only hand the caller complete validated sectors covering the requested part. Allocate and check CPU buffers before publication; hold the device mutex through cache handling and all BOT I/O. No bcache changes.
- [ ] **Run green:** focused host suite and both xHCI fixtures; no failed operation exposes unvalidated sectors as success.
- [ ] **Commit:** `perf(usb): coalesce single-sector reads with a private window`.

### Task 4: Prove USB-only boots and mounts both filesystems

**Files:** M0 runner and unit tests, `mk/components/run.mk`,  `docs/build/build.md`, `mk/tests/catalog.mk`, `qemutests/test_suite_catalog.py`. Use tools already required for image creation; leave `tools/mkdisk.c` unchanged.

**Interfaces:** add CLI `--case readonly`, make default `USB_BOOT_CASE=readonly`. Extract Python helper `prepare_image(source: Path, destination: Path, root_marker: str, esp_marker: str) -> uuid.UUID` returning ESP GUID; M2 uses it on two independently generated images. Marker paths `/usb-root.txt` and `/boot/USBESP.TXT`.

- [ ] **Write red** `test_readonly_requires_partition_content`: prompt without both standalone marker lines fails; input echo or duplicate disk echo fails. Unit-check partition offset calculation for a synthetic GPT; harness checks the immutable source hash before/after.
- [ ] **Run red:** `python3 -m unittest qemutests.test_usb_boot_runner`; `make test-qemu SUITE=usb-boot USB_BOOT_CASE=readonly` shows missing acceptance path before wiring it.
- [ ] **Implement:** copy normal image inside a job-private dir, read GPT extents and ESP GUID; use `mcopy -i <copy>@@<ESP byte offset>` for ESP and bounded extract/edit/reinsert of the ext2 partition via `debugfs -w` for root. Validate the bytes written by reading files back from their filesystem images; reject noncanonical input or partition overrun. Boot only that private image read-only, require MSC publication and existing shell prompt, inject `cat /usb-root.txt /boot/USBESP.TXT` after prompt, require expected whole lines and fresh prompt within global deadline. Archive serial evidence. Register `usb-boot` as x86_64-clang/rootfs/nightly, isolated/direct dispatch (no prepare/run worker), normal flavor and 512 MiB, and assert catalog coverage.
- [ ] **Run green:** unit suite, `make PROFILE=x86_64-clang test-qemu SUITE=usb-boot`, `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`, `make test-host`. M0 case is only a pre-M1 gate; do not expect missing root with the new driver.
- [ ] **Commit:** `test: verify USB-only shell and both filesystem mounts`.
