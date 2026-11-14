# USB Boot M0: Firmware Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove profile OVMF loads the stock kernel from USB only.

**Architecture:** A dedicated runner launches a private xHCI-only storage topology and captures serial evidence. Later plans extend the same suite and leave the standard AHCI runner untouched.

**Tech Stack:** Python 3, GNU Make, QEMU q35, OVMF

**Spec:** [USB boot design](../specs/2026-11-14-usb-boot-design.md). Start here. §§5.9–5.10, 6 (M0).

## Global Constraints

- x86_64 / QEMU first, polling only; root-port direct attach, BOT/SCSI LUN 0, logical sectors exactly 512 bytes.
- GPT + UEFI; no xHCI IRQ, UAS, hubs, SET_INTERFACE, or hotplug. aarch64 shares the handoff ABI only.
- Kernel operational messages use `log_*`; new hardware uses optional subsystem registration, not generic `kernel_main`.
- Use profile artifacts and private test images. Leave unrelated suites and AHCI baseline behavior intact.
- At each commit stage only the task's explicit files, review `git diff --cached --check`, then use the indicated message.

## Review Focus

- Missing/wrong firmware or image must fail, never silently use a system OVMF.
- Reaching only OVMF or an unrelated boot failure must not pass M0.
- The M0 FATAL evidence is a stable prefix, not a whole line: `boot_failure_stop()` appends `phase=`/`cpu=` fields, so the matcher must not require exact-line equality.
- No AHCI/virtio disk, implicit bootable disk, or `-boot order` contaminates the USB-only argv.
- QEMU output spam cannot extend the deadline; timeout and early exit keep serial evidence and reap QEMU.
- Unsupported profile and dry-run must not launch QEMU or overwrite the profile image.

---

### Task 1: Add the runner and profile entry points

**Files:** Create `qemutests/usb_boot.py` (executable runner), `qemutests/test_usb_boot_runner.py` (unit tests). Modify `qemutests/harness/qemu_cmd.py` (add `storage="usb"`), `qemutests/test_harness_qemu_cmd.py`, `mk/components/run.mk`, `docs/build/build.md`; add a `TEST_MANIFEST` (`layer: harness`) to `test_usb_boot_runner.py`. Keep the expected-failure M0 suite out of automated buckets; add the suite to the generated catalog in M1b when it becomes a success gate.

**Interfaces:** CLI `python3 qemutests/usb_boot.py --case firmware --qemu PATH --firmware FILE --disk FILE --smp N --log-dir DIR [--timeout 120]`; cases added by successor plans. Python `usb_argv(qemu: str, firmware: Path, disk: Path, smp: int) -> list[str]` delegates to the shared `build_qemu_x86(storage="usb")`, and `firmware_evidence(serial: str) -> bool`. Make: `run-usb`, `test-qemu-usb-boot USB_BOOT_CASE=firmware` (dedicated target; the suite is not a `test-qemu SUITE=` value until M1b registers it in the generated catalog).

The implementation must follow the merged 2026-11-14 test-framework rules: metadata lives in the test file, the QEMU command line comes only from `qemutests/harness/qemu_cmd.py`, and any metadata change is committed together with the `make regen-test-catalog` output (`config/test-catalog.json`, `mk/generated/*`, `config/test-ci-schedule.json`).

- [ ] **Write runner contract tests** in `test_usb_boot_runner.py`: `assert firmware_evidence(usb_enumeration + configured_msc + missing_root_fatal)`; assert false for OVMF-only text, fatal rc other than -19, or markers out of order. Feed the real line with trailing `phase=... cpu=...` fields and require the stable prefix to match; reject a whole-line-only comparison. Assert argv contains `p2=4,p3=0`, `bus=xhci.0,port=1,bootindex=0`, serial stdio, read-only pflash and rng, and exactly one non-pflash drive. Fake process tests assert pre-spawn failure on absent input, timeout with a noisy child, early exit without success, serial log retained and a reaped child.
- [ ] **Run red:** `python3 -m unittest qemutests.test_usb_boot_runner` fails on absent runner/contracts.
- [ ] **Implement runner:** use q35, 512 MiB, supplied SMP and QEMU binary, `-display none -serial stdio -no-reboot -no-shutdown`, `-object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0`. Add only `-drive file=...,format=raw,if=none,id=usbdisk,readonly=on` and the specified USB storage device. Accept in order xHCI connected at `speed=3`, USB configuration marker `slot N: configured with config X (iface class 08:06, ep_count=2)`, then a line beginning `[boot-3] FATAL stage=fs component=root rc=-19` (the real line from `boot_failure_stop()` continues with `phase=... cpu=...`; match the prefix); retain full serial and argv in a private run dir. Use the existing process-session helper with an absolute monotonic deadline and bounded process cleanup.
- [ ] **Wire make/docs:** apply `rootfs` capability to both entry points, explicit `NORMAL_IMAGE` and `OVMF_FIRMWARE` prerequisites only for supported profile. Use a dedicated recursive `test-qemu-usb-boot` branch and exclude it from generic Python dispatch; keep recursion and process launch on different recipe lines. Runner reads normal image only; exclude this branch from the generic pre/post normal-image guard; inside the dedicated target hash the source image after its build and before/after running. Use job-private log paths. Add the suite in both dispatcher allowlist and usage text. `run-usb` is interactive with existing display/SMP/network settings but uses the USB disk topology, not `RUN_QEMU_DISK`. Document opt-in firmware-only checkpoint and profile override.
- [ ] **Run green and gate M1:** unit command above, `make -n PROFILE=x86_64-clang test-qemu-usb-boot USB_BOOT_CASE=firmware` prints exactly one intended runner; `make PROFILE=aarch64-clang -n run-usb` fails capability. Run `make PROFILE=x86_64-clang test-qemu-usb-boot USB_BOOT_CASE=firmware`: require exact M0 evidence and untouched image. If profile OVMF does not reach the signature, stop M1 and investigate firmware/loader. Use a fresh profile firmware artifact for this gate if it may contain a previous override; record the source and SHA-256 of the artifact actually booted. A local OVMF override is diagnostic, not evidence for the default edk2-nightly source.
- [ ] **Commit:** `test: gate USB boot on profile firmware loading the kernel`.
