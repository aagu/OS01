# USB Boot M2: Boot Identity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Use the loader's ESP GUID to select both mounts in dual-disk boots.

**Architecture:** The loader captures identity before the last memory map fetch. The kernel parses GPTs without side effects, resolves a unique pair, then publishes and mounts only the selected disk's partitions.

**Tech Stack:** C, UEFI ABI, GPT, QEMU, hosttests

**Spec:** [USB boot design](../specs/2026-11-14-usb-boot-design.md). Integrate after M1b; ABI/parser and GPT fixtures can be developed against AHCI independently. §§5.6–5.10 (M2).

## Global Constraints

- x86_64 / QEMU first, polling only; root-port direct attach, BOT/SCSI LUN 0, logical sectors exactly 512 bytes.
- GPT + UEFI; no xHCI IRQ, UAS, hubs, SET_INTERFACE, or hotplug. aarch64 shares the handoff ABI only.
- Kernel operational messages use `log_*`; new hardware uses optional subsystem registration, not generic `kernel_main`.
- Use profile artifacts and private test images. Leave unrelated suites and AHCI baseline behavior intact.
- At each commit stage only the task's explicit files, review `git diff --cached --check`, then use the indicated message.

## Review Focus

- Real UEFI `04 01 2a 00` HD bytes are unaligned; bad end markers or duplicate nodes never publish identity (Task 1).
- Corrupt GPT header lengths and overflowed entry spans must not read out of bounds or make the disk ambiguous (Task 2).
- Duplicate GUID after the legacy 16-partition cap still counts (Task 3).
- A known identity with unscannable media must fail without attempting to mount the first disk (Task 3).
- AHCI must be genuinely bootable and registered first, so the dual-disk test rejects accidental fallback (Task 4).

**Files and ownership:** `boot/uefi/boot_device.{c,h}` pure byte parser, `boot/uefi/main.c` protocol adapter; `kernel/include/core/bootinfo.h` wire layout; `kernel/{fs/gpt.c,include/fs/gpt.h}` GPT parsing/publication; `kernel/{fs/boot.c,include/fs/boot.h,core/main.c}` root selection. Test EFI parser in `hosttests/cases/test_boot_devpath.c`, GPT/selection in `test_gpt_bootdev.c` and existing boot fixtures. Keep producer and consumer wire version change in one commit; first commit must still boot AHCI.

---

### Task 1: Upgrade the handoff and capture the firmware boot identity

**Files:** ABI header, new parser, common EFI main, both `boot/uefi/arch/{x86_64,aarch64}/boot.c` handoff comments/assertions, `mk/components/uefi.mk`, `boot/uefi/Makefile`, `hosttests/cases/test_bootinfo_abi.c`, parser host case and `hosttests/mk/drivers.mk`.

**Interfaces:** `int os01_bootdev_from_devpath(const uint8_t *dp, size_t max_len, struct boot_device_info *out)` returns 0 only on valid GPT HD path, leaves out unchanged otherwise. `boot_context` appends the exact spec §5.6 struct, offset 136/size 176, record size 40, version 3, flag 1<<6.

- [ ] **Write red** `boot_devpath_wire`: `04 01 2a 00` node followed by `7f ff 04 00`; assert captured byte-exact GUID, number=3, start=0x123456789, size=0x23456789 at unaligned input. Reject type 03 masquerading as HD, MBR type/signature not 02, zero GUID, zero number/size, undersized/oversized/truncated nodes, duplicate HD, End Instance, missing/trailing End Entire. Assert untouched out on failure. Add compile-time v3/record assertions in both EFI and kernel builds and version/size reject assertions to boot fixtures.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-boot-devpath`.
- [ ] **Implement layout/parser:** raw LE fields at 4/8/16/24/40/41, check node lengths against remaining bytes, use exact terminator, reject multi-instance. Keep offsets of all v2 fields; assert handoff record fits its first page/reservation.
- [ ] **Implement capture:** static `capture_boot_device(struct boot_context *ctx)` runs once before GetMemoryMap loop; uses `LIP->DeviceHandle`, standard Device Path Utilities GetDevicePathSize with bounded trusted-firmware walk fallback, reject out-of-range size >4096. EFIAPI correct for new protocol typedef; log `UEFI: ESP GUID=<canonical lowercase UUID>` using EFI mixed-endian formatting from raw bytes, or reason absent; flag follows parser success. For x86 automation require this exact line on the headless COM1 transcript; use existing firmware console only if the profile OVMF boot proves it reaches COM1, otherwise add a bounded EFI Serial I/O route (avoid duplicate lines; no kernel-side echo as a substitute). Add source to both wrapper per-arch `SRCS` lists and the header to wrapper target prerequisites; likewise update outer `UEFI_BOOT_SRCS` / `UEFI_BOOT_INPUTS` for both profiles. Update hardcoded v2 tests in `test_bootinfo_abi.c`; no VFS consumption yet.
- [ ] **Run green:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-boot-devpath`; `make test-host` runs the updated `test_bootinfo_abi` suite; `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` (confirm UEFI marker); `make PROFILE=aarch64-clang test-aarch64 MODE=smp` (builds both EFI and kernel and checks boot).
- [ ] **Commit:** `feat(boot): capture firmware ESP identity in handoff v3`.

### Task 2: Parse first, publish second

**Files:** GPT header/source, GPT host case and `hosttests/mk/fs.mk`.

**Interfaces:** `struct gpt_table` opaque, allocate by `int gpt_read_table(block_device_t *disk, struct gpt_table **out)` (0 valid, `-ENOENT` no signature, `-EINVAL` proved invalid, `-E2BIG` unsupported implementation bound, `-EIO` unreadable, `-ENOMEM` allocation). A 512-byte-sector disk too small for a header counts as no GPT; any non-512 physical candidate is unsupported (-E2BIG before I/O), so it cannot silently drop out of a known-identity scan. `void gpt_free_table(struct gpt_table *t)`; `size_t gpt_table_count(const struct gpt_table *t)`; `bool gpt_table_entry(const struct gpt_table *t, size_t ordinal, gpt_partition_t *out)` (0-based compact ordinal over valid entries; `out->dev=NULL`, add `unique_guid[16]`, `uint32_t entry_number`); `gpt_info_t *gpt_publish_table(const struct gpt_table *t, bool strict)` preserves legacy device naming and 16-slot publication cap; strict mode treats any devfs registration failure (including -EEXIST) as a failed publication. `gpt_scan(block_device_t *)` remains existing compatibility entry point.

- [ ] **Write red** `gpt_metadata_has_no_side_effects` and `gpt_bounds`: valid sparse slot 17 has `entry_number == 18`, all entries visible, exact GUID at offset 16, no block or devfs partition registration until publish. Include the stock mkdisk shape (backing length 395264 sectors, root ends at 395263 despite reported last usable LBA 393182). Reject header sizes 91/513, zero count, entry size <128, overflow/off-disk entry area, bad CRC and invalid used entry range; bound an otherwise valid >1 MiB table as `-E2BIG`. Inject I/O and OOM distinctly, and reject a non-512 candidate without issuing reads. Preserve old wrapper outcomes for missing signature, valid 0/1 partition, and first-two order.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-gpt-bootdev test-device-boot`.
- [ ] **Implement:** parse 512-byte primary header, checked CRC and ceil division for entry array; preserve 1 MiB table cap. Avoid unaligned reads, raw allocation proportional to bounded table; enumerate all valid entries before publication. Store raw GPT data and decode on iteration rather than placing a large struct on kernel stack. The strict path validates used entries against physical backing `start <= end < disk->sector_count` (not reported usable bounds/backup GPT, for current mkdisk compatibility), rejects unsupported revisions as `-E2BIG`, and maps any attempted block read failure to `-EIO` regardless of lower-layer errno. Proven corrupt used ranges are invalid; keep legacy behavior of skipping bad entries in `gpt_scan` while retaining safe table checks. Build partition devices in existing naming order; callers compare required devices by `entry_number` and not compact name.
- [ ] **Run green:** focused commands; no ABI changes in public block ops.
- [ ] **Commit:** `refactor(gpt): separate read-only metadata from partition publication`.

### Task 3: Resolve and mount a unique ESP/root pair

**Files:** fs boot header/source, kernel main, `test_gpt_bootdev.c`, `test_device_boot.c`, `test_boot_failure_mounts.c`, `test_boot_contract_sequence.c`, `hosttests/mk/{drivers,fs}.mk` and other existing host fixture make rules as needed. Link real GPT into the selection-specific fixture; add fail-if-called stubs for the new API to legacy fixtures that intentionally mock GPT. Add every new host binary to both `TEST_BINS` and `register_host_test`.

**Interfaces:** `void fs_boot_mounts(const struct boot_context *bootctx)`; uses Task 2 table API, preserves existing registry and mount callbacks. `bootctx == NULL` or flag absent uses literal legacy path. GUID log string `fs: boot disk selected by GUID (<disk>), root=<part> boot=<part>`, plus actual `/boot` success/failure status.

- [ ] **Write red** `guid_mount_selection`: AHCI registered first but USB GUID selects exact ext2 and FAT nodes, root before ESP; ESP slot gaps and start/size mismatch only warn. Cases: Linux-data plus unrelated non-ESP selects Linux-data; unique non-ESP fallback; ambiguous Linux-data, ESP-only, duplicate ESP GUID after entry 16, no match, I/O/OOM/unsupported candidate, cap excludes required partition, block/devfs publication failure (including a colliding devfs name), and root mount failure all stop with no AHCI fallback. No flag preserves old first-disk NULL/<2/normal cases; `/boot` mount failure warns without remounting root. Assert no publication on unrelated disks and one table parse per disk.
- [ ] **Run red:** `make -C hosttests PROFILE=x86_64-clang OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" test-gpt-bootdev test-device-boot`.
- [ ] **Implement:** pass validated context from generic main. Snapshot physical BLOCK_DISK order before publication. Consume every valid entry; ignore only `-ENOENT/-EINVAL` from metadata scan, fail closed otherwise. Compare matching ESP type+GUID as raw bytes; retain only the uniquely matched table, free other metadata, then choose pair and publish selected table once with strict=true; strict attempts at most the first 16 valid slots, and a required publication missing for any reason is fatal before either mount. Legacy mode preserves existing cap and -EEXIST semantics; resolve by slot and require both devices to exist before mounting. Root via existing failure injection/stop, FAT via warning; preserve root-ready/procfs/tmpfs order. Implement the legacy branch using the existing wrapper and no new probes; log that missing boot identity is the reason for using the first disk. Log first physical disk in known-identity flow to prove registry order.
- [ ] **Run green:** focused host commands, `make test-host`, and `make test-kernel-layout`. Additionally run `make test-boot-failure-host CASE=mounts` and `make test-boot-contract-host CASE=sequence`.
- [ ] **Commit:** `feat(fs): select and mount the disk identified by the boot ESP`.

### Task 4: Make the optional dual-disk path the M2 acceptance gate

**Files:** M1b runner/unit tests, `mk/components/run.mk`, `docs/build/build.md`, `mk/components/image.mk` only if image prerequisites need a shared target. **Interfaces:** `--case dual|ahci`; `--ahci-disk FILE` is the pre-marker AHCI source shared by dual/AHCI-only; `--disk FILE` is the USB source; make defaults to a matrix of `readonly,dual,ahci` (explicit case still possible); same two private sentinel paths from M1b.

- [ ] **Write red** `test_dual_requires_identity_and_content`: reject equal GUIDs, missing/incorrect EFI GUID, missing first-disk proof, missing or wrong `fs: boot disk selected by GUID (usba), root=usba2 boot=usba1` after root mount, failed `/boot` status, out-of-order milestones, AHCI sentinel or echoed command; require USB root and ESP markers at separate expected positions. Validate dual argv adds AHCI bootindex 1, USB bootindex 0; AHCI case contains no USB disk and can read its own markers.
- [ ] **Run red:** `python3 -m unittest qemutests.test_usb_boot_runner`.
- [ ] **Implement:** have make build profile artifacts and run `$(HOST_MKDISK) --output <private AHCI source> --efi $(UEFI_EFI) --temp-dir <private dir> --rootfs-manifest $(ROOTFS_MANIFEST)` with the normal build inputs (kernel.bin comes from the manifest), producing an independent second GPT. Invoke the M1b image preparer twice and require distinct ESP GUIDs. Use two independently bootable images; expose only disposable prepared AHCI media writable (QEMU ide-hd rejects readonly=on), never its source. Keep USB images read-only; enforce register order and match captured EFI UUID against USB GUID; require the exact post-root GUID selection log plus explicit successful `/boot` before reading both installed USB markers. AHCI-only run reads both AHCI markers; retain ordinary phase-0 as separate baseline. Keep image writes private and runner outside recursive make recipe lines.
- [ ] **Run green:** unit suite, `make PROFILE=x86_64-clang test-qemu SUITE=usb-boot`, `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`, `make PROFILE=aarch64-clang test-aarch64 MODE=smp`. Use make dry-run to verify one intended suite dispatch.
- [ ] **Commit:** `test: verify firmware-selected USB mounts with AHCI registered first`.
