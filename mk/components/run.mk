# ── Run / Debug / Test / Validation / Clean entry points ─────────
# (spec: mk/components/run.mk — the root Makefile's user-facing aliases).
# Every QEMU, debug, test, validation, clean and compat-copy recipe lives
# here so the root Makefile stays a pure include + default-goal file.
#
# Capability gates use the same pattern as lib/user (root Makefile): the
# artifact prereqs exist only for capable profiles, and the recipe's first
# line expands to $(error ...) on incapable profiles — so both real runs and
# dry runs (-n) fail cleanly at the gate with
#   PROFILE='<p>' lacks capability '<cap>'
# instead of "No rule to make target".

# ── Firmware (x86 run targets) ─────────────────────────────────
# Root-owned, profile-private real-file rule. Same acquisition contract as
# the aarch64 QEMU_EFI rule (mk/components/image.mk) — both expand the
# firmware_fetch_cmd canned recipe defined there. Capability-gated on
# rootfs: the x86-only OVMF_FIRMWARE variable is undefined for
# aarch64-clang, so without the guard this rule expands to an empty-target
# rule with a recipe — a make-version-sensitive parse hazard.
ifeq ($(filter rootfs,$(PROFILE_CAPABILITIES)),rootfs)
$(OVMF_FIRMWARE):
	@$(call firmware_fetch_cmd,OVMF_FIRMWARE_SOURCE)

# $(OVMF_FIRMWARE) is absolute (BUILD_DIR is profile-absolute), so the same
# on-disk file is also reachable through its relative spelling. This alias
# makes `make build/<profile>/firmware/OVMF.fd` work too; the absolute rule
# above does the actual work.
build/$(PROFILE)/firmware/OVMF.fd: $(OVMF_FIRMWARE)
endif

# ── Kernel compat copy ─────────────────────────────────────────
# Project-root kernel.bin is a one-way copy of the profile's kernel artifact
# (mk/components/kernel.mk). Only defined when the profile declares a kernel
# artifact (x86_64-clang); other profiles simply fail on `make kernel.bin`.
ifdef KERNEL_ARTIFACT
kernel.bin: $(KERNEL_ARTIFACT)
	@cmp -s $(KERNEL_ARTIFACT) $@ || cp $(KERNEL_ARTIFACT) $@
endif

# ── Run (x86, rootfs capability) ───────────────────────────────
# All x86 QEMU entry points consume the profile's NORMAL_IMAGE and
# OVMF_FIRMWARE directly — never the project-root disk.img compat copy and
# never the source-tree boot/uefi/OVMF.fd.

# Shared QEMU argument groups. Variant flags are inserted between these
# groups so the final argument order matches each original recipe.
RUN_QEMU_BASE = $(QEMU_BIN) -M q35 -smp $(SMP) \
  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FIRMWARE)
QEMU_IMAGE ?= $(NORMAL_IMAGE)
RUN_QEMU_DISK = -drive file=$(QEMU_IMAGE),format=raw,if=none,id=disk \
  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -device qemu-xhci,id=xhci \
  -device usb-tablet,bus=xhci.0 \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0 \
  -m $(MEMORY) -display $(DISPLAY) -serial stdio
# USB-only storage topology (USB boot design §5.9): one usb-storage
# drive on qemu-xhci and nothing else (no AHCI/IDE/virtio, no
# `-boot order` — it cannot mix with bootindex).  Consumed by `run-usb`.
# DRIFT WARNING: hand-maintained mirror of build_qemu_x86(storage="usb")
# (display/SMP/network flags are inserted by the recipe);
# test_usb_boot_runner.py::MakeTopologyDriftTests compares the tokens —
# update both sides together.
QEMU_USB_DISK = -drive file=$(QEMU_IMAGE),format=raw,if=none,id=usbdisk,readonly=on \
  -device qemu-xhci,id=xhci,p2=4,p3=0 \
  -device usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0 \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0

.PHONY: run run-kvm run-virtio run-usb debug
# Boot storage selection for `run` / `run-kvm` / `run-virtio`:
#   BOOT_METHOD=hdd (default) — AHCI ide-hd
#   BOOT_METHOD=usb — USB-only (usb-storage on qemu-xhci, same as run-usb)
BOOT_METHOD ?= hdd
ifeq ($(filter $(BOOT_METHOD),hdd usb),)
$(error ERROR: invalid BOOT_METHOD='$(BOOT_METHOD)' (expected 'hdd' or 'usb'))
endif
ifeq ($(BOOT_METHOD),usb)
RUN_SELECTED_DISK = $(QEMU_USB_DISK) -m $(MEMORY) -display $(DISPLAY) -serial stdio
else
RUN_SELECTED_DISK = $(RUN_QEMU_DISK)
endif

# Capability-gated prerequisites reproduce the original
# `$(if $(filter rootfs,...),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))` pattern
# so a clean workspace still gets the disk image + firmware built first.
run:        $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -netdev user,id=net0 -device e1000,netdev=net0 $(RUN_SELECTED_DISK) -no-reboot
run-kvm:    $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -accel kvm -netdev user,id=net0 -device e1000,netdev=net0 $(RUN_SELECTED_DISK) -no-reboot
run-virtio: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -netdev user,id=net0 -device virtio-net-pci,disable-modern=on,netdev=net0 $(RUN_SELECTED_DISK)
# M0 firmware-only checkpoint (USB boot plan Task 1, design §5.9): the
# same interactive display/SMP/network settings as `run`, but the USB
# disk topology instead of RUN_QEMU_DISK.  The stock kernel has no USB
# MSC driver, so the expected outcome is
#   `[boot-3] FATAL stage=fs component=root rc=-19 phase=-1 cpu=-1`.
# The automated gate is `make test-qemu-usb-boot`; this target is
# for manual diagnosis (e.g. the OVMF boot menu).  The drive is
# read-only so an M0 run can never modify the profile image.
run-usb:    $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_USB_DISK) -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot
debug: QEMU_IMAGE = $(DISK_IMG)
debug:      $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(DISK_IMG) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -S -s -netdev user,id=net0 -device e1000,netdev=net0 $(RUN_QEMU_DISK)

# ── aarch64 UEFI bring-up (uefi capability) ────────────
# Targets are always defined so `make aarch64-uefi` under the default x86
# profile fails at the capability gate (not "no rule to make target");
# the aarch64-only AARCH64_UEFI_* variables are empty under x86, so the
# guard inside `require_capability aarch64-only` errors out cleanly. The
# kernel artifact + image rules live in mk/components/image.mk; there is
# no `lib` dependency — aarch64 does not consume the sysroot.
define require_aarch64_uefi
$(if $(AARCH64_UEFI_DISK),,$(error ERROR: aarch64-uefi targets require the aarch64-clang profile (AARCH64_UEFI_DISK is empty)))
endef

.PHONY: aarch64-uefi
aarch64-uefi: $(AARCH64_UEFI_DISK) $(AARCH64_UEFI_FIRMWARE)
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)

.PHONY: aarch64-uefi-kernel
aarch64-uefi-kernel: $(BUILD_DIR)/artifacts/kernel.elf
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)

# M3.1 audit gate (Task 13): the compiled aarch64 kernel must carry NO
# x86-only VMM symbols (vma_*/fork_* and the x86 user-VM families;
# vmm_*/arch_vmm_* are deliberately excluded — aarch64 has its own
# page_table/vmm_gate surface).  2026-11-19: uaccess_*, spawn_user_task,
# files_* and vfs_lookup* were REMOVED from the gate — the aarch64
# product userspace chain (81930db8: OS01_HAS_USERSPACE=1, /bin/init ->
# serial shell) ships those subsystems on aarch64 by design, and the
# gate failed on master for exactly that reason.  Source-scan twin:
# hosttests/cases/test_vmm_caller_audit.c; chain table:
# docs/memory/memory.md ("vmm 变更调用链审计（M3.1 验收）").
# llvm-nm host tool (the aarch64 profile defines no LLVM_NM — that
# belongs to the x86 toolchain discovery).
AARCH64_NM     ?= llvm-nm
# The aarch64 profile defines no x86 toolchain variables (no LLVM_NM /
# LLVM_OBJDUMP): the intr-dispatch wiring audit needs a disassembler for
# the aarch64 ELF, so it gets its own overridable host tool here.  No
# x86 toolchain include is pulled into the aarch64 path.
AARCH64_OBJDUMP ?= llvm-objdump
AARCH64_READELF ?= llvm-readelf
.PHONY: test-aarch64-audit
test-aarch64-audit: $(BUILD_DIR)/artifacts/kernel.elf
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@echo "  [audit] aarch64 kernel.elf: entry symbol contract"
	@if ! command -v $(AARCH64_READELF) >/dev/null 2>&1; then \
	    echo "AUDIT GATE FAIL: $(AARCH64_READELF) tool not found" >&2; exit 1; \
	fi
	@syms="$$($(AARCH64_READELF) -Ws $(BUILD_DIR)/artifacts/kernel.elf)" || { \
	    echo "AUDIT GATE FAIL: $(AARCH64_READELF) exited with failure on $(BUILD_DIR)/artifacts/kernel.elf" >&2; exit 1; \
	}; \
	if ! echo "$$syms" | grep -qE 'FUNC[[:space:]]+GLOBAL[[:space:]]+DEFAULT[[:space:]]+[0-9]+[[:space:]]+kernel_main[[:space:]]*$$'; then \
	    echo "AUDIT GATE FAIL: kernel_main must be defined as GLOBAL FUNC symbol" >&2; exit 1; \
	fi; \
	if echo "$$syms" | grep -qE '[[:space:]]+aarch64_main[[:space:]]*$$'; then \
	    echo "AUDIT GATE FAIL: obsolete symbol aarch64_main found in ELF" >&2; exit 1; \
	fi
	@echo "  [audit] aarch64 kernel.elf: no vma_*/fork_* or sched-split x86-subsystem T/t symbols"
	@bad="$$($(AARCH64_NM) $(BUILD_DIR)/artifacts/kernel.elf | awk '$$2 == "T" || $$2 == "t" { print $$3 }' | grep -E '^(vma_|fork_|x86_vma_destroy_node|x86_vmm_map_(2m|4k)|x86_aslr_|mm_(init_user_heap|set_brk|user_range_protected)|do_(mmap|munmap|mprotect)|vmm_free_user_map|do_fork)')"; \
	  if [ -n "$$bad" ]; then \
	    echo "AUDIT GATE FAIL: forbidden symbols in aarch64 kernel.elf:" $$bad >&2; exit 1; \
	  fi
	@echo "  [audit] OK (vma_*/fork_*/x86 user-VM + vmm_free_user_map/do_fork symbol-exclusion gate)"
	$(RUN_STATIC_AUDIT) intr-dispatch-audit -- \
	  python3 qemutests/intr_dispatch_audit.py \
	    --profile "$(PROFILE)" \
	    --elf "$(BUILD_DIR)/artifacts/kernel.elf" \
	    --llvm-nm "$(AARCH64_NM)" \
	    --llvm-objdump "$(AARCH64_OBJDUMP)" \
	    --repo-root "$(OS01_ROOT)"
	$(RUN_STATIC_AUDIT) stack-frame-audit -- \
	  python3 qemutests/stack_frame_audit.py \
	  --elf "$(BUILD_DIR)/artifacts/kernel.elf" \
	  --llvm-objdump "$(AARCH64_OBJDUMP)" \
	  --limit 512
# Manual negative control (Fix round 1): the same pipeline run against the
# x86 kernel.elf must hit symbols (proves the filter works):
#   make PROFILE=x86_64-clang kernel.bin
#   llvm-nm build/x86_64-clang/kernel/kernel.elf | awk '$2=="T"||$2=="t"{print $3}' \
#     | grep -E '^(vma_|uaccess_|fork_|x86_vma_destroy_node)'  # expect: fork_vma_copy, x86_vma_destroy_node, ...

# The default product run is SINGLE-CORE: the aarch64 user-mm runtime
# gate is single-core v1, and a multi-core boot comes up kernel-only
# by design (init spawn refused with -EOPNOTSUPP, boot-5 falls back).
# Bare `make PROFILE=aarch64-clang run-aarch64-uefi` therefore shows
# the product userspace chain; an explicit SMP=<N> on the command line
# (or environment) still wins for SMP bring-up work.
# origin SMP: 'file' = the top-level SMP ?= 2 default (not requested);
# command line / environment / override = the user asked for N CPUs.
run-aarch64-uefi_SMP = $(if $(filter-out file undefined,$(origin SMP)),$(SMP),1)

.PHONY: run-aarch64-uefi
run-aarch64-uefi: aarch64-uefi
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@set -e; \
	case "$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)" in \
	0) extra_dtb= ;; \
	*) \
	  dtb_dir="$(BUILD_DIR)/logs/aarch64-uefi-dtb"; \
	  mkdir -p "$$dtb_dir"; \
	  sparse="$$dtb_dir/qemu-virt.dtb.sparse"; \
	  packed="$$dtb_dir/qemu-virt.dtb"; \
	  $(AARCH64_QEMU) -M virt,gic-version=2 -cpu cortex-a53 -smp $(run-aarch64-uefi_SMP) \
	    -machine "dumpdtb=$$sparse" -display $(DISPLAY) -m $(MEMORY); \
	  dtc -I dtb -O dtb -o "$$packed" "$$sparse"; \
	  rm -f "$$sparse"; \
	  extra_dtb="-dtb $$packed" ;; \
	esac; \
	$(AARCH64_QEMU) -M virt,gic-version=2$${extra_dtb:+,acpi=off} -cpu cortex-a53 -smp $(run-aarch64-uefi_SMP) -m $(MEMORY) \
	  -drive if=pflash,format=raw,readonly=on,file=$(AARCH64_UEFI_FIRMWARE) \
	  -drive if=none,file=$(AARCH64_UEFI_DISK),format=raw,readonly=on,id=disk \
	  -device virtio-blk-device,drive=disk \
	  -device virtio-rng-device \
	  -device ramfb \
	  $$extra_dtb \
	  -serial stdio -display $(DISPLAY) -no-reboot

# The AArch64 PSCI SMP acceptance suite runs the production firmware/image
# with multiple vCPU counts; the host-only python parser asserts structured
# kernel diagnostics (never a UEFI banner / firmware FAIL text).
#
# DTB constraint: the prebuilt edk2 firmware does not expose the device
# tree through the EFI configuration table, so the suites default to
# --diagnostic-dtb=auto (packed QEMU-generated DTB via `-dtb`, acpi=off);
# AARCH64_UEFI_SMP_DIAGNOSTIC_DTB=0 requires the production firmware path.
#
# no-ack mode only consumes an already injected image; switching the
# compiled value needs the profile-clean sequence:
#   make PROFILE=aarch64-clang clean
#   make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
#   make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64 MODE=no-ack
#   make PROFILE=aarch64-clang clean
#   make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
# gic-spi (spec §7.4, Task 2.3b) reuses the SMP suite's firmware/image/DTB
# mechanism and asserts the PL011 "handled count=1" marker.
.PHONY: test-aarch64
# The contract harness expects invalid no-ack injection to fail even under
# `make -n`. Keep this as a parse-time check.
ifneq ($(filter test-aarch64,$(MAKECMDGOALS)),)
ifeq ($(MODE),no-ack)
ifneq ($(AARCH64_SMP_TEST_NO_ACK_CPU),1)
$(error AARCH64_SMP_TEST_NO_ACK_CPU must be 1; clean and build the injected image first)
endif
endif
endif
# MODE=sched (spec §10.2, P5 Task 1): parse-time conflict rejection. The
# sched acceptance mode builds its own KERNEL_SELFTEST sched variants
# through the controlled sub-make boundary, so it cannot share an
# invocation with any other compile-affecting test variant. This applies
# even when the outer invocation has no AARCH64_SCHED_TEST selector yet;
# a nonempty selector additionally gets the same rejection inside the
# aarch64 profile (which direct variant sub-makes also parse).
# MODE=sched-ap (P1 spec §7): the AP cooperative-scheduler acceptance
# mode owns its sched-ap variant trees through the controlled sub-make
# boundary and refuses every other compile-affecting selector — the
# outer make rejects, never just the profile's sub-make parse.
# sched-tick/sched-wake/sched-ap share one parse-time conflict gate: the
# mode owns its compile selector(s) and rejects every other
# compile-affecting override (sched has extra NO_ACK=0 leniency and keeps
# its own gate below).  tick owns AARCH64_SCHED_TICK_TEST and rejects
# both sched selectors; wake owns AARCH64_AP_SCHED_TEST=wake; ap owns
# AARCH64_AP_SCHED_TEST — so each rejects the others' selectors.
empty_for_gates :=
space_for_gates := $(empty_for_gates) $(empty_for_gates)
define _aarch64_mode_gate
$(if $(filter test-aarch64,$(MAKECMDGOALS)),
$(if $(filter $(MODE),$(1)),
$(if $(filter 1,$(AARCH64_SYNC_FAULT_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST)),$(error MODE=$(1) cannot be combined with sync-fault/weak-selftest/canary/OS01_SYSTEST),)
$(if $(strip $(foreach v,$(2),$($(v))) $(KERNEL_SELFTEST)),$(error MODE=$(1) cannot be combined with $(subst $(space_for_gates),/,$(2))/KERNEL_SELFTEST overrides — it sets its own),)
,),)
endef
$(eval $(call _aarch64_mode_gate,sched-tick,AARCH64_M1_TEST AARCH64_SCHED_TEST AARCH64_AP_SCHED_TEST AARCH64_USER_RUNTIME_TEST AARCH64_SMP_TEST_NO_ACK_CPU))
$(eval $(call _aarch64_mode_gate,user-runtime,AARCH64_EL0_TEST AARCH64_M1_TEST AARCH64_SCHED_TEST AARCH64_AP_SCHED_TEST AARCH64_SCHED_TICK_TEST AARCH64_SMP_TEST_NO_ACK_CPU))
$(eval $(call _aarch64_mode_gate,sched-wake,AARCH64_M1_TEST AARCH64_SCHED_TEST AARCH64_SMP_TEST_NO_ACK_CPU))
$(eval $(call _aarch64_mode_gate,sched-ap,AARCH64_M1_TEST AARCH64_SCHED_TEST AARCH64_SMP_TEST_NO_ACK_CPU))
$(eval $(call _aarch64_mode_gate,lma-layout,AARCH64_M1_TEST AARCH64_SCHED_TEST AARCH64_AP_SCHED_TEST AARCH64_SCHED_TICK_TEST AARCH64_SMP_TEST_NO_ACK_CPU))
$(eval $(call _aarch64_mode_gate,serror,AARCH64_SERROR_TEST AARCH64_M1_TEST AARCH64_SCHED_TEST AARCH64_AP_SCHED_TEST AARCH64_SCHED_TICK_TEST AARCH64_USER_RUNTIME_TEST AARCH64_EL0_TEST AARCH64_SMP_TEST_NO_ACK_CPU))
ifneq ($(filter test-aarch64,$(MAKECMDGOALS)),)
ifeq ($(MODE),sched)
ifneq ($(filter 1,$(AARCH64_SYNC_FAULT_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST)),)
$(error MODE=sched cannot be combined with sync-fault/weak-selftest/canary/OS01_SYSTEST)
endif
ifneq ($(strip $(AARCH64_M1_TEST)),)
$(error MODE=sched cannot be combined with AARCH64_M1_TEST='$(AARCH64_M1_TEST)')
endif
ifneq ($(strip $(AARCH64_SMP_TEST_NO_ACK_CPU)),)
ifeq ($(filter 0,$(strip $(AARCH64_SMP_TEST_NO_ACK_CPU))),)
$(error MODE=sched cannot be combined with AARCH64_SMP_TEST_NO_ACK_CPU=$(AARCH64_SMP_TEST_NO_ACK_CPU))
endif
endif
endif
endif
# SCHED_CASE selector (MODE=sched prep/run only): the positive strict
# evidence matrix, one fault variant, or all of them. Default all. The
# resolution must also fire inside the per-case helper sub-makes the
# MODE=sched dispatch invokes (their MAKECMDGOALS are the helper
# targets, not test-aarch64), so the guard covers both spellings.
# AP_SCHED_CASE selector (MODE=sched-ap prep/run only): the AP
# production case set (`positive`, P1 Task 4) or the fatal-owner fault
# boot. Default positive. The resolution must also fire inside the
# helper sub-makes, so the guard covers the helper target spellings.
ifneq ($(filter test-aarch64,$(MAKECMDGOALS))$(findstring _test-aarch64-prep-sched-ap,$(MAKECMDGOALS))$(findstring _test-aarch64-run-sched-ap,$(MAKECMDGOALS)),)
AP_SCHED_CASES_VALID := positive fatal-owner
ifeq ($(AP_SCHED_CASE),)
AP_SCHED_CASE := positive
endif
ifeq ($(filter $(AP_SCHED_CASE),$(AP_SCHED_CASES_VALID)),)
$(error invalid AP_SCHED_CASE='$(AP_SCHED_CASE)'; valid: $(AP_SCHED_CASES_VALID))
endif
endif
ifneq ($(filter test-aarch64,$(MAKECMDGOALS))$(findstring _test-aarch64-prep-sched,$(MAKECMDGOALS))$(findstring _test-aarch64-run-sched,$(MAKECMDGOALS)),)
SCHED_CASES_VALID := positive sp-top sp-low pc-low pc-data all
ifeq ($(SCHED_CASE),)
SCHED_CASE := all
endif
ifeq ($(filter $(SCHED_CASE),$(SCHED_CASES_VALID)),)
$(error invalid SCHED_CASE='$(SCHED_CASE)'; valid: $(SCHED_CASES_VALID))
endif
endif
# MODE=sched-wake: the P2 cross-CPU wakeup acceptance — the wake
# variant owns kernel/image sched-wake trees via the controlled
# sub-make (AARCH64_AP_SCHED_TEST=wake); no per-case selector (the
# phase carries the whole envelope).
# MODE=sched-tick (2026-11-06, Task 4): the per-CPU tick acceptance —
# the tick variant owns kernel/image sched-tick trees via the controlled
# sub-make (AARCH64_SCHED_TICK_TEST=1); the runner drives the frozen
# 1/2/4/8 x 2 matrix.
.PHONY: _test-aarch64-prep-sched-wake _test-aarch64-run-sched-wake
_test-aarch64-run-sched-wake:
	python3 qemutests/aarch64_ap_scheduler.py \
	  --phase wake --case positive \
	  --image "$(call aarch64_variant_disk,sched-wake)" \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --elf "$(call aarch64_variant_kernel,sched-wake)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --llvm-nm "$(AARCH64_NM)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-sched-wake/$$(date -u +%Y%m%dT%H%M%S)-$$$$"
.PHONY: _test-aarch64-prep-sched-tick _test-aarch64-run-sched-tick
_test-aarch64-run-sched-tick:
	python3 qemutests/aarch64_sched_tick.py \
	  --image "$(call aarch64_variant_disk,sched-tick)" \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --elf "$(call aarch64_variant_kernel,sched-tick)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --llvm-nm "$(AARCH64_NM)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-sched-tick/$$(date -u +%Y%m%dT%H%M%S)-$$$$"

ifneq ($(filter test-aarch64,$(MAKECMDGOALS))$(findstring _test-aarch64-prep-user-runtime,$(MAKECMDGOALS))$(findstring _test-aarch64-run-user-runtime,$(MAKECMDGOALS)),)
USER_RUNTIME_CASES_VALID := irq asid smp migration product fp
ifeq ($(USER_RUNTIME_CASE),)
USER_RUNTIME_CASE := irq
endif
ifeq ($(filter $(USER_RUNTIME_CASE),$(USER_RUNTIME_CASES_VALID)),)
$(error invalid USER_RUNTIME_CASE='$(USER_RUNTIME_CASE)'; valid: $(USER_RUNTIME_CASES_VALID))
endif
endif
.PHONY: _test-aarch64-prep-user-runtime _test-aarch64-run-user-runtime
_test-aarch64-prep-user-runtime:
ifeq ($(USER_RUNTIME_CASE),$(filter $(USER_RUNTIME_CASE),product fp))
	$(MAKE) aarch64-uefi
else
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_USER_RUNTIME_TEST=$(USER_RUNTIME_CASE) aarch64-uefi
endif
_test-aarch64-run-user-runtime:
	python3 qemutests/aarch64_user_runtime.py \
	  --case "$(USER_RUNTIME_CASE)" \
	  --image "$(if $(filter product fp,$(USER_RUNTIME_CASE)),$(AARCH64_UEFI_DISK),$(call aarch64_variant_disk,user-runtime-$(USER_RUNTIME_CASE)))" \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --elf "$(if $(filter product fp,$(USER_RUNTIME_CASE)),$(BUILD_DIR)/artifacts/kernel.elf,$(call aarch64_variant_kernel,user-runtime-$(USER_RUNTIME_CASE)))" \
	  $(call _aarch64_run_tail_nm,$(OS01_ROOT)/test-results/aarch64-user-runtime-$(USER_RUNTIME_CASE)/$$(date -u +%Y%m%dT%H%M%S)-$$$$)

# Per-MODE lookups. The "extra" DTB flag is a Make variable (NOT a shell
# variable) so it survives across recipe lines — a shell variable set
# in one `@`-prefixed recipe line is empty in the next (each `@` line is
# a fresh /bin/sh invocation, verified).
TEST_AARCH64_EXTRA_smp        := $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto)
TEST_AARCH64_EXTRA_no-ack     := $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto)
TEST_AARCH64_EXTRA_gic-spi    := --diagnostic-dtb=auto
# sync-fault has no DTB-mode knob — it runs the EL1h sync fault probe on
# the existing UEFI/DTB mechanism but with an isolated image/firmware tree.

# Shared aarch64 runner argument tails (the plumbing every python runner
# takes, expanded at recipe time via $(call) — arguments keep the same
# single-expansion semantics as direct recipe text, so $$-containing
# log-dir timestamps pass through unchanged).
#   _aarch64_run_tail    $(1)=image  $(2)=log-dir — the standard order
#   (firmware, image, qemu, build-dir/profile, log-dir) shared by the
#   smp/no-ack/gic-spi/sync-fault/m3-probe runners.
#   _aarch64_run_tail_nm $(1)=log-dir — for runners whose image/ELF paths
#   are passed earlier on the command line (user-runtime, sched,
#   sched-ap): qemu, llvm-nm, build-dir/profile, log-dir.
# test-aarch64-boot-logo keeps its explicit form (no --log-dir by design)
# and the m1/boot-contract generators their eval-escaped forms.
define _aarch64_run_tail
--firmware "$(AARCH64_UEFI_FIRMWARE)" \
--image "$(1)" \
--qemu "$(AARCH64_QEMU)" \
--build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
--log-dir "$(2)"
endef
define _aarch64_run_tail_nm
--qemu "$(AARCH64_QEMU)" \
--llvm-nm "$(AARCH64_NM)" \
--build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
--log-dir "$(1)"
endef

# prep helpers (one per MODE) put the $(MAKE) sub-invocation on its own
# recipe line so `make -n` honors the dry-run contract.  The uniform
# selftest-variant preps (one sub-make, optional compile selector) are
# generated; no-ack/ordinary/boot-contract/sched-ap keep explicit rules.
define _aarch64_prep
.PHONY: _test-aarch64-prep-$(1)
_test-aarch64-prep-$(1):
	$$(MAKE) KERNEL_SELFTEST=$(2) $(3) aarch64-uefi
endef
$(eval $(call _aarch64_prep,smp,1,))
$(eval $(call _aarch64_prep,gic-spi,1,))
$(eval $(call _aarch64_prep,sync-fault,1,AARCH64_SYNC_FAULT_TEST=1))
$(eval $(call _aarch64_prep,serror-el1t,1,AARCH64_SERROR_TEST=el1t))
$(eval $(call _aarch64_prep,serror-el1h,1,AARCH64_SERROR_TEST=el1h))
$(eval $(call _aarch64_prep,serror-el0t,1,AARCH64_SERROR_TEST=el0t))
$(eval $(call _aarch64_prep,exception-trace-read,1,AARCH64_EXCEPTION_TRACE_TEST=read))
$(eval $(call _aarch64_prep,exception-trace-bad-fp,1,AARCH64_EXCEPTION_TRACE_TEST=bad-fp))
$(eval $(call _aarch64_prep,exception-trace-task,1,AARCH64_EXCEPTION_TRACE_TEST=task))
EXCEPTION_TRACE_CASE ?= all
EXCEPTION_TRACE_CASES_VALID := all read bad-fp task
ifeq ($(filter $(EXCEPTION_TRACE_CASE),$(EXCEPTION_TRACE_CASES_VALID)),)
$(error invalid EXCEPTION_TRACE_CASE='$(EXCEPTION_TRACE_CASE)'; valid: $(EXCEPTION_TRACE_CASES_VALID))
endif
.PHONY: _test-aarch64-prep-exception-trace
_test-aarch64-prep-exception-trace:
ifeq ($(EXCEPTION_TRACE_CASE),all)
	$(MAKE) --no-print-directory _test-aarch64-prep-exception-trace-read
	$(MAKE) --no-print-directory _test-aarch64-prep-exception-trace-bad-fp
	$(MAKE) --no-print-directory _test-aarch64-prep-exception-trace-task
else
	$(MAKE) --no-print-directory _test-aarch64-prep-exception-trace-$(EXCEPTION_TRACE_CASE)
endif
.PHONY: _test-aarch64-prep-serror
_test-aarch64-prep-serror:
	$(MAKE) --no-print-directory _test-aarch64-prep-serror-el1t
	$(MAKE) --no-print-directory _test-aarch64-prep-serror-el1h
	$(MAKE) --no-print-directory _test-aarch64-prep-serror-el0t
$(eval $(call _aarch64_prep,sched-wake,1,AARCH64_AP_SCHED_TEST=wake))
$(eval $(call _aarch64_prep,sched-tick,1,AARCH64_SCHED_TICK_TEST=1))
$(eval $(call _aarch64_prep,m3-probe,,))

_test-aarch64-prep-no-ack:
	@test "$(AARCH64_SMP_TEST_NO_ACK_CPU)" = 1 \
	  || { echo "AARCH64_SMP_TEST_NO_ACK_CPU must be 1; clean and rebuild" >&2; exit 1; }
	@test -f "$(AARCH64_UEFI_DISK)" -a -f "$(AARCH64_UEFI_FIRMWARE)" \
	  || { echo "Build the injected aarch64-uefi image first" >&2; exit 1; }
# Run helpers (one per MODE) keep the python harness on its own recipe
# line, after the pre-build. The python call therefore is NOT executed
# under `-n` (per the dry-run contract).
.PHONY: _test-aarch64-run-smp _test-aarch64-run-no-ack _test-aarch64-run-gic-spi _test-aarch64-run-sync-fault
_test-aarch64-run-smp:
	python3 qemutests/aarch64_uefi_smp.py \
	  --cpus 1 2 4 --repeat 3 --timeout 90 --expect-selftest --expect-gic --expect-clk \
	  --expect-slab-selftest --expect-m3-selftest --expect-m3mc-selftest \
	  --expect-scheduler-selftest \
	  $(TEST_AARCH64_EXTRA_smp) \
	  $(call _aarch64_run_tail,$(call aarch64_variant_disk,selftest),$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-normal-$$$$)
# MODE=lma-layout (LMA Task 5): the paired-boot acceptance. The runner
# manages its OWN private build root (default → relocated → default via
# AARCH64_BUILD_ROOT, fixed KERNEL_SELFTEST=1) and every QEMU boot; the
# outer make builds nothing and must not carry compile-affecting
# overrides (the shared mode gate below rejects them).
.PHONY: _test-aarch64-prep-lma-layout
_test-aarch64-prep-lma-layout:
	@true
.PHONY: _test-aarch64-run-lma-layout
_test-aarch64-run-lma-layout:
	python3 qemutests/aarch64_lma_layout.py \
	  --qemu "$(AARCH64_QEMU)" \
	  --timeout 120
_test-aarch64-run-no-ack:
	python3 qemutests/aarch64_uefi_smp.py \
	  --cpus 2 --repeat 1 --timeout 90 --expect-no-ack 1 \
	  $(TEST_AARCH64_EXTRA_no-ack) \
	  $(call _aarch64_run_tail,$(AARCH64_UEFI_DISK),$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-no-ack-$$$$)
_test-aarch64-run-gic-spi:
	python3 qemutests/aarch64_gic_spi.py \
	  --diagnostic-dtb=auto \
	  --expect-scheduler-selftest \
	  $(call _aarch64_run_tail,$(call aarch64_variant_disk,selftest),$(OS01_ROOT)/test-results/aarch64-gic-spi/$$(date -u +%Y%m%dT%H%M%S)-$$$$)
# AAGU-EL1-sync Task 3: drives the dedicated sync-fault QEMU image
# (KERNEL_VARIANT=sync-fault) in 1-CPU mode and asserts the EL1h sync fatal
# diagnostic via qemutests/aarch64_sync_fault.py. The harness parser is the
# only success criterion: timeouts do NOT count, and the run target only
# consumes the dedicated sync-fault paths (never the selftest image).
_test-aarch64-run-sync-fault:
	python3 qemutests/aarch64_sync_fault.py \
	  --timeout 60 \
	  --elf "$(call aarch64_variant_kernel,sync-fault)" \
	  --nm "$(LLVM_NM)" \
	  $(call _aarch64_run_tail,$(call aarch64_variant_disk,sync-fault),$(OS01_ROOT)/test-results/aarch64-sync-fault/$$(date -u +%Y%m%dT%H%M%S)-$$$$)

.PHONY: _test-aarch64-run-serror
_test-aarch64-run-serror:
	python3 qemutests/aarch64_serror.py \
	  --timeout 60 \
	  --el1t-image "$(call aarch64_variant_disk,serror-el1t)" \
	  --el1t-elf "$(call aarch64_variant_kernel,serror-el1t)" \
	  --el1h-image "$(call aarch64_variant_disk,serror-el1h)" \
	  --el1h-elf "$(call aarch64_variant_kernel,serror-el1h)" \
	  --el0t-image "$(call aarch64_variant_disk,serror-el0t)" \
	  --el0t-elf "$(call aarch64_variant_kernel,serror-el0t)" \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-serror/$$(date -u +%Y%m%dT%H%M%S)-$$$$"

.PHONY: _test-aarch64-run-exception-trace
_test-aarch64-run-exception-trace:
ifeq ($(EXCEPTION_TRACE_CASE),all)
	@for c in read bad-fp task; do \
	  python3 qemutests/exception_trace.py --profile "$(PROFILE)" --case "$$c" \
	    --timeout 60 \
	    --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	    --image "$(BUILD_DIR)/image/exception-trace-$$c/aarch64-uefi.img" \
	    --elf "$(BUILD_DIR)/kernel/exception-trace-$$c/kernel.elf" \
	    --nm "$(LLVM_NM)" \
	    --qemu "$(AARCH64_QEMU)" \
	    --build-dir "$(BUILD_DIR)" \
	    --log-dir "$(OS01_ROOT)/test-results/aarch64-exception-trace/$$c-$$(date -u +%Y%m%dT%H%M%S)-$$$$" || exit $$?; \
	done
else
	python3 qemutests/exception_trace.py --profile "$(PROFILE)" --case "$(EXCEPTION_TRACE_CASE)" \
	  --timeout 60 \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(call aarch64_variant_disk,exception-trace-$(EXCEPTION_TRACE_CASE))" \
	  --elf "$(call aarch64_variant_kernel,exception-trace-$(EXCEPTION_TRACE_CASE))" \
	  --nm "$(LLVM_NM)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-exception-trace/$(EXCEPTION_TRACE_CASE)-$$(date -u +%Y%m%dT%H%M%S)-$$$$"
endif

# MODE=sched (spec §10.2, P5 Tasks 1-3): the strict scheduler acceptance
# runner. Prep builds the exact KERNEL_SELFTEST=1 AARCH64_SCHED_TEST=<v>
# variants through SEQUENTIAL controlled recursive makes (each on its own
# recipe line, mirroring prep-smp's dry-run contract); `all` prepares the
# positive plus the four fault images. The run helper then consumes ONLY
# the explicit per-variant paths from mk/targets/aarch64.mk — the python
# runner never rebuilds and never guesses a variant. Every sched boot
# uses the per-CPU diagnostic DTB (the prebuilt edk2 firmware does not
# expose FDT through the EFI configuration table on current QEMU; see
# docs/build/build.md "MODE=sched").
AARCH64_SCHED_CASES := positive sp-top sp-low pc-low pc-data
define _aarch64_sched_prep
.PHONY: _test-aarch64-prep-sched-$(1)
_test-aarch64-prep-sched-$(1):
	$$(MAKE) KERNEL_SELFTEST=1 AARCH64_SCHED_TEST=$(1) aarch64-uefi
endef
$(foreach c,$(AARCH64_SCHED_CASES),$(eval $(call _aarch64_sched_prep,$(c))))
.PHONY: _test-aarch64-prep-sched _test-aarch64-prep-sched-all
_test-aarch64-prep-sched: _test-aarch64-prep-sched-$(SCHED_CASE)
# The `all` runner also audits the ordinary normal + ordinary selftest
# ELFs (`label=ordinary-*`), so `SCHED_CASE=all` must build them too —
# a clean tree otherwise fails the audit with "ELF not found".
_test-aarch64-prep-sched-all: $(foreach c,$(AARCH64_SCHED_CASES),_test-aarch64-prep-sched-$(c)) _test-aarch64-prep-sched-ordinary
.PHONY: _test-aarch64-prep-sched-ordinary
_test-aarch64-prep-sched-ordinary:
	$(MAKE) AARCH64_SCHED_TEST= AARCH64_M1_TEST= AARCH64_SYNC_FAULT_TEST= \
	  AARCH64_SMP_TEST_NO_ACK_CPU=0 KERNEL_SELFTEST= aarch64-uefi
	$(MAKE) AARCH64_SCHED_TEST= AARCH64_M1_TEST= AARCH64_SYNC_FAULT_TEST= \
	  AARCH64_SMP_TEST_NO_ACK_CPU=0 KERNEL_SELFTEST=1 aarch64-uefi

# Per-variant runner args: the runner's --variant schema is fixed —
# variant name + image + firmware + ELF; the positive case uses the
# plain sched variant, every fault case its sched-fault-<case> variant.
# `SCHED_CASE=all` passes --case all + every variant map.
define _aarch64_sched_variant_args
--variant $(1) --image "$(call aarch64_variant_disk,$(2))" \
  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
  --elf "$(call aarch64_variant_kernel,$(2))"
endef
AARCH64_SCHED_VARIANT = $(if $(filter $(1),positive),sched,sched-fault-$(1))
.PHONY: _test-aarch64-run-sched
_test-aarch64-run-sched:
	python3 qemutests/aarch64_scheduler.py \
	  $(if $(filter all,$(SCHED_CASE)),--case all $(foreach c,$(AARCH64_SCHED_CASES),$(call _aarch64_sched_variant_args,$(c),$(call AARCH64_SCHED_VARIANT,$(c)))),--case $(SCHED_CASE) $(call _aarch64_sched_variant_args,$(SCHED_CASE),$(call AARCH64_SCHED_VARIANT,$(SCHED_CASE)))) \
	  $(call _aarch64_run_tail_nm,$(OS01_ROOT)/test-results/aarch64-sched/$$(date -u +%Y%m%dT%H%M%S)-$$$$)

# P1 MODE=sched-ap: the AP cooperative-scheduler acceptance variants.
# prep drives the controlled sub-make with KERNEL_SELFTEST=1 and
# AARCH64_AP_SCHED_TEST=$(AP_SCHED_CASE); run invokes the dedicated
# runner with the explicit per-case artifact paths.
.PHONY: _test-aarch64-prep-sched-ap _test-aarch64-run-sched-ap
# AP_SCHED_CASE (positive|fatal-owner) maps onto the compile selector
# AARCH64_AP_SCHED_TEST (ap|fatal-owner): the positive case set is the
# `ap` variant; the fatal-owner case owns its own fault variant.
AP_SCHED_SELECTOR_positive = ap
AP_SCHED_SELECTOR_fatal-owner = fatal-owner
_test-aarch64-prep-sched-ap:
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_AP_SCHED_TEST=$(AP_SCHED_SELECTOR_$(AP_SCHED_CASE)) aarch64-uefi
_test-aarch64-run-sched-ap:
ifeq ($(AP_SCHED_CASE),fatal-owner)
	python3 qemutests/aarch64_ap_scheduler.py \
	  --phase ap --case fatal-owner \
	  --image "$(call aarch64_variant_disk,sched-ap-fatal-owner)" \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --elf "$(call aarch64_variant_kernel,sched-ap-fatal-owner)" \
	  --normal-elf "$(call aarch64_variant_kernel,sched-ap)" \
	  $(call _aarch64_run_tail_nm,$(OS01_ROOT)/test-results/aarch64-sched-ap/$$(date -u +%Y%m%dT%H%M%S)-$$$$)
else
	python3 qemutests/aarch64_ap_scheduler.py \
	  --phase ap --case positive \
	  --image "$(call aarch64_variant_disk,sched-ap)" \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --elf "$(call aarch64_variant_kernel,sched-ap)" \
	  $(call _aarch64_run_tail_nm,$(OS01_ROOT)/test-results/aarch64-sched-ap/$$(date -u +%Y%m%dT%H%M%S)-$$$$)
endif

# M3.5 Task 25: production shootdown probe (spec §7.3). Builds the
# PRODUCTION image (no KERNEL_SELFTEST=1) and runs it at -smp 2; the
# harness greps for the exact `SHOOTDOWN-PROBE: START` → `OK`
# sequence and fails on any FAIL / SKIP line or timeout. -smp 1 is
# intentionally NOT covered here: main.c gates the probe call on
# dtb_cpu_count() >= 2 so single-CPU boots skip it (the 0-AP FAIL
# contract is pinned by the hosttest instead).
.PHONY: _test-aarch64-prep-m3-probe _test-aarch64-run-m3-probe
_test-aarch64-run-m3-probe:
	python3 qemutests/aarch64_m3_probe.py \
	  --diagnostic-dtb=auto \
	  --timeout 90 \
	  $(call _aarch64_run_tail,$(AARCH64_UEFI_DISK),$(OS01_ROOT)/test-results/aarch64-m3-probe/$$(date -u +%Y%m%dT%H%M%S)-$$$$)

.PHONY: _test-aarch64-prep-boot-contract _test-aarch64-run-boot-contract
_test-aarch64-prep-boot-contract:
	@true
# The aarch64 boot-contract RUN target is generated by boot_matrix_suite
# (see the boot-contract-aarch64 eval after the generator definition — an
# eval placed before the define would expand $(call ...) to nothing).

# test-aarch64: the umbrella. Dispatches to the per-MODE prep + run helpers.
test-aarch64: MODE ?= smp
test-aarch64: MODE := $(MODE)
# No image prerequisites: smp/gic-spi build the selftest variant in their
# prep helper, while no-ack must only consume an already injected image.
# sync-fault is added to the gate in line with the dedicated _test-aarch64-
# prep-sync-fault / _test-aarch64-run-sync-fault pair (Task 3 wires the
# run target and harness parser; this umbrella already accepts the mode).
test-aarch64:
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@case "$(MODE)" in \
	  smp|no-ack|gic-spi|sync-fault|serror|exception-trace|m3-probe|sched|sched-ap|sched-wake|sched-tick|user-runtime|m1-ram|m1-sparse|m1-arena-exhaust|m1-table-exhaust|m1-ap-bad-root|boot-contract|lma-layout) ;; \
	  *) echo "MODE must be smp|no-ack|gic-spi|sync-fault|serror|exception-trace|m3-probe|sched|sched-ap|sched-wake|sched-tick|user-runtime|m1-ram|m1-sparse|m1-arena-exhaust|m1-table-exhaust|m1-ap-bad-root|boot-contract|lma-layout, got '$(MODE)'" >&2; exit 1;; \
	esac
	@echo "  [test-aarch64] MODE=$(MODE) extra=$(TEST_AARCH64_EXTRA_$(MODE))"
	$(MAKE) --no-print-directory _test-aarch64-prep-$(MODE)
	$(MAKE) --no-print-directory _test-aarch64-run-$(MODE)


# ── aarch64 boot logo (framebuffer wordmark) ─────────────────
# Boots the aarch64 image with `-device ramfb` (edk2's RamfbDxe turns it into
# a GOP framebuffer), lets the kernel draw the Craton wordmark and asserts the
# result on a QMP screendump: exact ink/accent pixel counts, the arch caption
# band, and the no-framebuffer negative case.  Headless: `-display none`.
#
# NO --log-dir: the suite archives through RunArchive whenever --build-dir
# is given, so a --log-dir next to it would be inert.  The evidence (the
# screendump, the serial log and result.json) lands in
# $(BUILD_DIR)/logs/tests/aarch64-boot-logo-<check>/<UTC>-<uuid>/.
.PHONY: test-aarch64-boot-logo
test-aarch64-boot-logo:
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	$(MAKE) --no-print-directory aarch64-uefi
	python3 qemutests/aarch64_boot_logo.py \
	  --check all \
	  --diagnostic-dtb=auto \
	  --timeout 120 \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)"

# ── Validation ─────────────────────────────────────────────
# validate keeps the x86 kernel + UEFI artifact checks (kernel ELF has no
# undefined symbols / INTERP / DYNAMIC, is EM_X86_64, exports _start /
# kernel_main / _text; the EFI app has a parseable COFF export table) and
# prints the selected profile's identity. x86-only: gated on the `rootfs`
# capability (the recipes inspect the x86 kernel.bin artifact and x86 kernel
# symbols, which aarch64-clang does not produce even though it has `uefi`)
# so an incapable profile gets the clean capability error instead of cryptic
# empty-LLVM_* failures.
.PHONY: validate validate-kernel validate-uefi validate-profile
validate: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),validate-kernel validate-uefi validate-profile)
	$(call require_capability,rootfs)
validate-kernel: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),kernel.bin)
	$(call require_capability,rootfs)
	@echo "  [validate] kernel.elf has no undefined symbols"
	@test -z "$$($(LLVM_NM) --undefined-only $(KERNEL_ELF))"
	@echo "  [validate] kernel.elf has no INTERP/DYNAMIC program headers"
	@! $(LLVM_READELF) -Wl $(KERNEL_ELF) | grep -E 'INTERP|DYNAMIC'
	@echo "  [validate] kernel.elf machine is $(RUNTIME_MACHINE_kernel)"
	@$(LLVM_READOBJ) --file-headers $(KERNEL_ELF) | grep -F '$(RUNTIME_MACHINE_kernel)'
	@echo "  [validate] GLOBAL _start present"
	@$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\b_start\b'
	@echo "  [validate] GLOBAL kernel_main present"
	@$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\bkernel_main\b'
	@echo "  [validate] GLOBAL _text present"
	@$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\b_text\b'
validate-uefi: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(UEFI_EFI))
	$(call require_capability,rootfs)
	@echo "  [validate] $(notdir $(UEFI_EFI)) coff-exports"
	@$(LLVM_READOBJ) --coff-exports $(UEFI_EFI) >/dev/null
validate-profile:
	@printf 'profile=%s triple=%s sysroot=%s capabilities=%s\n' "$(PROFILE)" "$(TARGET_TRIPLE)" "$(SYSROOT)" "$(PROFILE_CAPABILITIES)"

# ── Test ────────────────────────────────────────────────────
# Each x86 E2E test builds its image VARIANT in an isolated dir and runs
# qemutests/run_test.py against that exact image.  Variant builds never
# write the normal image: its sha256 is recorded before/after the variant
# build (image/normal.before / normal.after) and compared — skipped, not
# faked, when the normal image does not exist.  Recipe lines containing
# $(MAKE) execute even under -n: the sha256 sandwich and the python run
# stay on their own recipe lines (the dry-run contract).

# Explicit variant image paths (mirror the IMAGE_DIR computation the variant
# sub-make performs, so the suite runs against the exact image just built).
TEST_SYSTEST_IMAGE  := $(BUILD_DIR)/image/systest/disk.img
TEST_NETTEST_IMAGE  := $(BUILD_DIR)/image/nettest/disk.img
TEST_INITTAB_IMAGE  := $(BUILD_DIR)/image/inittab-test/disk.img
TEST_SELFTEST_IMAGE := $(BUILD_DIR)/image/selftest/disk.img
# terminal-present needs the per-second [term-present] serial records
# (USER_DEBUG=1) — it builds its own image variant so the normal image
# keeps the serial port clean.
TEST_TERMINAL_PRESENT_IMAGE := $(BUILD_DIR)/image/terminal-present/disk.img
TEST_ARCH10_CAPACITY_IMAGE := $(BUILD_DIR)/image/arch10-capacity/disk.img
TEST_ARCH6_FSSTRESS_IMAGE := $(BUILD_DIR)/image/arch6-fsstress/disk.img
# The fd_refcount selftest asserts actual cross-CPU coverage.  Four vCPUs
# avoid a scheduler-migration false negative seen with the interactive
# default of two, while keeping this test-only setting independently
# overridable.
KERNEL_SELFTEST_SMP ?= 4
HOST_TEST_JOBS ?= 1

.PHONY: test
test: check-test-catalog test-host test-framework-contract test-assertion-policy

.PHONY: test-host test-pmm-boot-reservation test-gfx-file-lifecycle test-gfx-fault-bridge test-gfx-device test-gfx-client test-gfx-primitives test-m1-host test-arch9-host test-resolution-host test-gui-host
test-host: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(SYSROOT_STAMP))
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,run HOST_TEST_JOBS=$(HOST_TEST_JOBS) $(OS01_SUBMAKE_ARGS))
	python3 qemutests/pmm_boot_reservation_test.py
	python3 qemutests/test_kernel_selftest_result.py
	python3 tools/tests/test_user_build.py
	python3 tools/tests/test_kernel_headers.py
	python3 tools/tests/test_cursor_icon.py
test-pmm-boot-reservation:
	python3 qemutests/pmm_boot_reservation_test.py

.PHONY: print-host-test-inventory check-registry-unique
print-host-test-inventory:
	@$(call os01_submake,hosttests,print-inventory $(OS01_SUBMAKE_ARGS))
check-registry-unique:
	@$(call os01_submake,hosttests,check-registry-unique $(OS01_SUBMAKE_ARGS))

.PHONY: regen-test-catalog check-test-catalog check-ci-schedule test-flake-report test-doctor
regen-test-catalog:
	python3 tools/tests/scan_tests.py --root $(OS01_ROOT) --bridge $(OS01_ROOT)/config/test-bridge.json --emit $(OS01_ROOT)
check-test-catalog:
	python3 tools/tests/scan_tests.py --root $(OS01_ROOT) --bridge $(OS01_ROOT)/config/test-bridge.json --check-drift
check-ci-schedule:
	python3 tools/tests/check_ci_schedule.py --root $(OS01_ROOT)
# Flake ledger report (report mode always exits 0; see qemutests/flake_report.py).
# The ledger baseline is config/flake-ledger.json; derived statistics are
# recomputed from its run history on every read.
#   FROM_CI=N       inspect the last N scheduled CI runs via `gh`
#   LEDGER=<path>   ledger file (default config/flake-ledger.json)
#   MERGE="a b"     merge fragment/ledger JSON files (space separated, repeatable)
#   UPDATE_LEDGER=1 write the merged ledger back to LEDGER
#   EMIT_FRAGMENT=<path> write the newly discovered runs as a fragment JSON
#   FORMAT=json     report format (table|json)
test-flake-report:
	python3 qemutests/flake_report.py --root $(OS01_ROOT) $(if $(FROM_CI),--from-ci $(FROM_CI)) $(if $(LEDGER),--ledger $(LEDGER)) $(if $(MERGE),$(foreach f,$(MERGE),--merge $(f))) $(if $(UPDATE_LEDGER),--update-ledger) $(if $(EMIT_FRAGMENT),--emit-fragment $(EMIT_FRAGMENT)) $(if $(FORMAT),--format $(FORMAT))
# Environment doctor: compare local toolchain versions against ci/Dockerfile pins.
test-doctor:
	python3 tools/tests/test_doctor.py --root $(OS01_ROOT)
# Local CI container parity entry (docker/podman; no auto-pull).
.PHONY: ci-local
ci-local:
	$(if $(SUITE),bash tools/tests/ci_local.sh $(PROFILE) test-qemu SUITE=$(SUITE),$(if $(MAKE_ARGS),bash tools/tests/ci_local.sh $(PROFILE) $(MAKE_ARGS),$(error ERROR: ci-local requires SUITE=<name> or MAKE_ARGS='<targets>' (e.g. make ci-local SUITE=phase-0))))

.PHONY: print-harness-modules print-test-qemu-flavors print-test-qemu-images
print-harness-modules:
	@printf '%s\n' $(TEST_HARNESS_MODULES)
print-test-qemu-flavors:
	@$(foreach v,$(sort $(filter TEST_QEMU_FLAVOR_%,$(.VARIABLES))),printf '%s\t%s\n' '$(patsubst TEST_QEMU_FLAVOR_%,%,$(v))' '$(value $(v))';)
print-test-qemu-images:
	@$(foreach v,$(sort $(filter TEST_QEMU_IMG_%,$(.VARIABLES))),printf '%s\t%s\n' '$(patsubst TEST_QEMU_IMG_%,%,$(v))' '$(value $(v))';)

# ── Test framework regression (no QEMU, always) ──────────────
# The module list is GENERATED (test-harness-modules.mk from @os01-test /
# TEST_MANIFEST, layer: harness) and append-only across tasks (Ruling 3):
# a new qemutests/test_*.py unittest module joins in the same commit, so
# `make test-harness` validates the framework before any QEMU run.  The
# recipe is one unittest call against the explicit list — never
# auto-discovery; fixtures use fakes / host-only probes, no QEMU.
# Deliberately NOT in the list: test_lvgl_runner (executable QEMU driver,
# not a unittest) and test_arch9_build_contract (drives real
# `make -n kernel.bin`, needs a prepared sysroot — owned by the
# test-contract bucket instead, which builds one first).
TEST_HARNESS_MODULES_GEN := $(OS01_ROOT)/mk/generated/test-harness-modules.mk
ifeq ($(wildcard $(TEST_HARNESS_MODULES_GEN)),)
$(error test-harness-modules.mk missing; run 'make regen-test-catalog')
endif
include $(TEST_HARNESS_MODULES_GEN)
.PHONY: test-harness test-framework-contract
test-harness: test-framework-contract check-ci-schedule
	@echo "  [test-harness] running $(words $(TEST_HARNESS_MODULES)) unittest module(s): $(TEST_HARNESS_MODULES)"
	python3 qemutests/run_framework_tests.py $(if $(HARNESS_MODULE_TIMEOUT),--module-timeout $(HARNESS_MODULE_TIMEOUT)) $(if $(HARNESS_TOTAL_TIMEOUT),--total-timeout $(HARNESS_TOTAL_TIMEOUT)) $(TEST_HARNESS_MODULES)

# Negative-contract hosttest: builds test_framework_contract.elf and
# asserts exit code exactly 1 (assertion exit-code propagation, macro
# single-evaluation, TEST_RESULTS() not resetting stats — see the
# hosttests/cases/test_framework_contract.c header).  Runs standalone
# (no QEMU, single-file compile); wired into `make test-harness`.
test-framework-contract:
	$(call os01_submake,hosttests,test-framework-contract $(OS01_SUBMAKE_ARGS))

# ── Focused host-test dispatch groups (shared generator) ────────
# Every "focused host tests" umbrella shares one skeleton: per-case runner
# rules (echo + controlled hosttests sub-make), an _run-all rule, a
# catch-all that rejects unknown cases with the valid set, and a
# capability-gated umbrella dispatching on the group's case variable.
# They are all generated here — new groups/cases append one eval call.
#
# NOTE: each case's runner rule is a concrete rule, so a case name that is
# a prefix of another (e.g. theme / theme-apply) is fine; explicit rules
# placed AFTER an eval call override the catch-all for special cases
# (resolution hooks, doom build).
#
# $(call host_case_group,target,cases,capability,case-var,all-targets,mapping)
#   target       umbrella target (e.g. test-gui-host); echo prefix too
#   cases        case list — drives _run-all, the catch-all message and
#                (with all-targets empty) the _run-all prerequisites
#   capability   require_capability argument (prereqs are only wired for
#                capable profiles so an incapable profile fails at the gate)
#   case-var     the per-invocation selector (CASE / RES_CASE / DOOM_CASE /
#                LIBC_CASE); empty selector runs _run-all
#   all-targets  _run-all recipe = one os01_submake with these hosttests
#                targets (empty = depend on the per-case runner rules)
#   mapping      "case:hosttests-target ..." — a case missing here needs
#                an explicit rule after the eval call
define _host_case_rule
.PHONY: _$(1)-run-$(2)
_$(1)-run-$(2):
	@echo "  [$(3)] $(2)"
	$$(call os01_submake,hosttests,$(4) $$(OS01_SUBMAKE_ARGS))
endef

define host_case_group
$(foreach kv,$(6),$(eval $(call _host_case_rule,$(1),$(word 1,$(subst :, ,$(kv))),$(1),$(word 2,$(subst :, ,$(kv))))))
.PHONY: $(1) _$(1)-run-all
$(if $(5),_$(1)-run-all:
	@echo "  [$(1)] all"
	$$(call os01_submake,hosttests,$(5) $$(OS01_SUBMAKE_ARGS))
,_$(1)-run-all: $(foreach c,$(2),_$(1)-run-$(c))
	@echo "  [$(1)] full group: $(2)")
_$(1)-run-%:
	@echo "ERROR: unknown $(4)='$$*'; valid: $(2), all" >&2
	@exit 1
$(1): $(if $(filter $(3),$(PROFILE_CAPABILITIES)),$(if $($(4)),_$(1)-run-$($(4)),_$(1)-run-all))
	$$(call require_capability,$(3))
	@echo "  [$(1)] $$(if $$($(4)),$(4)=$$($(4)),full group: $(2))"
endef

$(eval $(call host_case_group,test-boot-failure-host,subsys log mounts terminal stdio spawn task-init,rootfs,CASE,test_boot_failure_subsys test_boot_failure_log test_boot_failure_mounts test_boot_failure_terminal test_boot_stdio test_boot_spawn_failure test_boot_task_init,subsys:test_boot_failure_subsys log:test_boot_failure_log mounts:test_boot_failure_mounts terminal:test_boot_failure_terminal stdio:test_boot_stdio spawn:test_boot_spawn_failure task-init:test_boot_task_init))

$(eval $(call host_case_group,test-boot-contract-host,subsys cpu-prepare softirq sequence checks,rootfs,CASE,test_boot_contract_subsys test_boot_contract_cpu_prepare test_boot_contract_softirq test_boot_contract_sequence test_boot_contract_checks,subsys:test_boot_contract_subsys cpu-prepare:test_boot_contract_cpu_prepare softirq:test_boot_contract_softirq sequence:test_boot_contract_sequence checks:test_boot_contract_checks))

$(eval $(call host_case_group,test-boot-services-host,vfs spawn sequence services late mechanisms,rootfs,CASE,test_boot_services_vfs test_boot_services_spawn test_boot_services_sequence test_boot_services_services test_boot_services_late test_boot_services_mechanisms,vfs:test_boot_services_vfs spawn:test_boot_services_spawn sequence:test_boot_services_sequence services:test_boot_services_services late:test_boot_services_late mechanisms:test_boot_services_mechanisms))

$(eval $(call host_case_group,test-boot-irq-time-host,registration context rtc handoff,rootfs,CASE,test_boot_irq_time_registration test_boot_irq_time_context test_boot_irq_time_rtc test_boot_irq_time_handoff,registration:test_boot_irq_time_registration context:test_boot_irq_time_context rtc:test_boot_irq_time_rtc handoff:test_boot_irq_time_handoff))

$(eval $(call host_case_group,test-boot-lifecycle-host,construction wait,rootfs,CASE,test_boot_lifecycle_all,construction:test_boot_lifecycle_construction wait:test_boot_lifecycle_wait))

# Focused aarch64 per-CPU tick accounting hosttest (Task 1).  The
# binary is the REAL kernel/arch/aarch64/platform/time.c host-compiled
# against the machine-boundary mock: the frozen CNTP configuration and
# the local timer start are the production algorithm.  No QEMU, no
# kernel build — the root target forwards to the SAME-named hosttests
# target through the controlled sub-make boundary.
.PHONY: test-aarch64-sched-tick-host
test-aarch64-sched-tick-host:
	$(call os01_submake,hosttests,test-aarch64-sched-tick-host $(OS01_SUBMAKE_ARGS))

# Focused M1 host tests (aarch64 M1 plan).  The M1 host tests are
# arch-neutral C; both profiles route through hosttests.  Each case is a
# focused TEST_BINS entry, so `make test-m1-host CASE=...` is cheap when
# iterating on a single calculator; CASE omitted runs the full group.
$(eval $(call host_case_group,test-m1-host,m1-layout m1-reservation m1-arena m1-tree m1-contract-x86 m1-install m1-publish,rootfs,CASE,,m1-layout:test-m1-layout m1-reservation:test-m1-reservation m1-arena:test-m1-arena m1-tree:test-m1-tree m1-contract-x86:test-m1-contract-x86 m1-install:test-m1-install m1-publish:test-m1-publish))

# ARCH-9 driver model host tests.
$(eval $(call host_case_group,test-arch9-host,block pci backend ahci device-boot net lwip e1000 virtio socket,rootfs,CASE,,block:test-block pci:test-pci backend:test-backend ahci:test-ahci device-boot:test-device-boot net:test-net lwip:test-lwip e1000:test-e1000 virtio:test-virtio socket:test-socket))

# ── Resolution Switcher host tests ───────────────────────────
# RES_CASE accepts uapi, state, writers, bga, ioctl, pty, terminal,
# clients, hooks, or all (default).  Unregistered/unknown cases abort
# non-zero via the generated catch-all.  hooks is an explicit rule: it
# also runs the python resolution-profile harness.
$(eval $(call host_case_group,test-resolution-host,uapi state writers bga ioctl pty terminal clients hooks,rootfs,RES_CASE,,uapi:test-fb-uapi state:test-fb-state writers:test-fb-writers bga:test-bga ioctl:test-fb-ioctl pty:test-pty-winsize terminal:test-terminal-display clients:test-clients))

.PHONY: _test-resolution-host-run-hooks
_test-resolution-host-run-hooks:
	@echo "  [test-resolution-host] hooks"
	$(call os01_submake,hosttests,test-fb-hooks $(OS01_SUBMAKE_ARGS))
	python3 qemutests/fb_resolution_profile_test.py

# ── GUI host tests ───────────────────────────────────────────
# Focused host tests for the GUI plans (GUI-1 LVGL platform backend,
# GUI-2 input chain, GUI-2b widget system, GUI-3 Material design).
# CASE accepts any registered case or all (default); unknown cases abort
# non-zero before invoking submake.  The per-case spec contracts live in
# the matching hosttests/cases/test_gui_* sources and plan docs, not here.
$(eval $(call host_case_group,test-gui-host,platform lvgl devfs-input input-frame input-route input-decode input-nonce input-platform input-lvgl theme theme-apply desktop-state desktop-shell desktop-audit widget widget-layout widget-appearance widget-event focus,rootfs,CASE,,platform:test-gui-platform lvgl:test-gui-lvgl devfs-input:test-gui-devfs-input input-frame:test-gui-input-frame input-route:test-gui-input-route input-decode:test-gui-input-decode input-nonce:test-gui-input-nonce input-platform:test-gui-input-platform input-lvgl:test-gui-input-lvgl theme:test-gui-theme theme-apply:test-gui-theme-apply desktop-state:test-gui-desktop-state desktop-shell:test-gui-desktop-shell desktop-audit:test-gui-desktop-audit widget:test-gui-widget widget-layout:test-gui-widget-layout widget-appearance:test-gui-widget-appearance widget-event:test-gui-widget-event focus:test-gui-focus))

# ── D03: the Doom engine work package ───────────────────────────
# make test-doom-host DOOM_CASE=build|config|video|input|runtime|diag|all.
# build is an explicit rule (it drives the python build-contract unittest
# AND the engine-exit hosttest binary); unknown cases abort non-zero.
$(eval $(call host_case_group,test-doom-host,build config video input runtime diag,rootfs,DOOM_CASE,,config:test-doom-config video:test-doom-video input:test-doom-input runtime:test-doom-runtime diag:test-doom-diag))

.PHONY: _test-doom-host-run-build
_test-doom-host-run-build:
	@echo "  [test-doom-host] build"
	@cd $(OS01_ROOT) && PROFILE=$(PROFILE) python3 -m unittest qemutests.test_doom_build_contract -v
	$(call os01_submake,hosttests,test-doom-engine-exit $(OS01_SUBMAKE_ARGS))

# D05: Doom asset validation and packaging contract
.PHONY: test-doom-assets
test-doom-assets:
	$(call require_capability,rootfs)
	@cd $(OS01_ROOT) && PROFILE=$(PROFILE) python3 -m unittest qemutests.test_doom_assets -v

# Sanitizer instrumentation contract + live probe (roadmap P1 UBSan+KASan).
# Static contract always runs; the live QEMU probe builds both instrumented
# images and boots each probe — opt-in work guarded by OS01_SANITIZER_LIVE
# inside the module itself.
.PHONY: test-kernel-sanitizers
test-kernel-sanitizers:
	@cd $(OS01_ROOT) && OS01_SANITIZER_LIVE=1 PROFILE=$(PROFILE) python3 -m unittest qemutests.test_kernel_sanitizers -v

.PHONY: test-kernel-sanitizers-contract
test-kernel-sanitizers-contract:
	@cd $(OS01_ROOT) && PROFILE=$(PROFILE) python3 -m unittest qemutests.test_kernel_sanitizers -v

# gfx 2D API plan Task 1: per-file device ioctl/release lifecycle
# (spec §3; full contract in hosttests/cases/test_gfx_file_lifecycle.c).
test-gfx-file-lifecycle:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-file-lifecycle $(OS01_SUBMAKE_ARGS))
# gfx 2D API plan Task 2: bounded /dev/gfx0 present device (spec §3+§4;
# fb_get_info/fb_write_row mocked — the VMA/VMM chain is covered by the
# QEMU integration suites).  Full contract: hosttests/cases/test_gfx_device.c.
test-gfx-device:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-device $(OS01_SUBMAKE_ARGS))
# Partial-present Task 1 Step 2: host SIGSEGV bridge self-test (bridge +
# test TU only).  Must pass before test-gfx-device depends on the bridge.
test-gfx-fault-bridge:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-fault-bridge $(OS01_SUBMAKE_ARGS))
# gfx 2D API plan Task 3: libgfx client lifecycle (spec §3+§4+§5; the
# REAL libgfx/gfx.c wrapped at libc boundaries — no kernel deps).
# Full contract: hosttests/cases/test_gfx_client.c.
test-gfx-client:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-client $(OS01_SUBMAKE_ARGS))
# gfx 2D API plan Task 4: libgfx 2D primitives (pixel/line/rect/sprite,
# spec §4 — in-memory drawing only, no fd/no syscall).
# Full contract: hosttests/cases/test_gfx_primitives.c.
test-gfx-primitives:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-primitives $(OS01_SUBMAKE_ARGS))

# Focused hosttest for the display ownership plan Task 1 — exclusive
# display coordinator and writer admission.
.PHONY: test-display-owner
test-display-owner:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-display-owner $(OS01_SUBMAKE_ARGS))

# ── Terminal monitor host tests (2026-10-11 monitor plan) ────
# Focused host tests, one per monitor-plan task.  Strict dispatch: an
# unknown / multi-word / "all" CASE is rejected at parse time (fails even
# under -n/-j) BEFORE any submake; CASE omitted runs the whole group.
# Prerequisites run before recipes, even before require_capability, so
# the graph is only populated for a capable profile + one valid CASE.
MONITOR_CASES := file accounting pmm procfs collect model render
MONITOR_MAPPING := file:test-monitor-file accounting:test-monitor-accounting pmm:test-monitor-pmm procfs:test-monitor-procfs collect:test-monitor-collect model:test-monitor-model render:test-monitor-render
$(foreach kv,$(MONITOR_MAPPING),$(eval $(call _host_case_rule,test-monitor-host,$(word 1,$(subst :, ,$(kv))),test-monitor-host,$(word 2,$(subst :, ,$(kv))))))
.PHONY: test-monitor-host _test-monitor-host-run-%
_test-monitor-host-run-%:
	@echo "ERROR: unknown CASE='$*'; valid: $(MONITOR_CASES)" >&2
	@exit 1
MONITOR_CASE := $(strip $(CASE))
MONITOR_CASE_VALID := $(if $(MONITOR_CASE),$(if $(filter 1,$(words $(MONITOR_CASE))),$(filter $(MONITOR_CASES),$(MONITOR_CASE))),yes)
test-monitor-host: $(if $(and $(filter rootfs,$(PROFILE_CAPABILITIES)),$(MONITOR_CASE_VALID)),$(foreach c,$(if $(MONITOR_CASE),$(MONITOR_CASE),$(MONITOR_CASES)),_test-monitor-host-run-$(c)))
	$(call require_capability,rootfs)
	$(if $(MONITOR_CASE_VALID),,$(error unknown CASE='$(MONITOR_CASE)'; valid: $(MONITOR_CASES)))
	@echo "  [test-monitor-host] $(if $(MONITOR_CASE),CASE=$(MONITOR_CASE),full monitor group: $(MONITOR_CASES))"

# ── User runtime consumer gate (monitor plan Task 5) ─────────
# Target-artifact contract (NOT a host fixture; separate from
# test-monitor-host on purpose): runs monitor_runtime_test.py against
# the REAL consumer=user archive/receipt + link probe — no unresolved
# runtime symbols, kernel/user variant paths differ, unsupported
# providers rejected.
# ── monitor thermal plan, Task 1 umbrellas ──
# Each test-thermal-host case corresponds to a per-fixture runner in
# hosttests/Makefile.
$(eval $(call host_case_group,test-thermal-host,fixup affinity decode,rootfs,CASE,,fixup:test-thermal-fixup affinity:test-thermal-affinity decode:test-thermal-decode))

.PHONY: test-monitor-runtime
# $(KERNEL_RUNTIME_PREREQ) guarantees the kernel variant receipt exists even
# on a clean tree, so the kernel/user path-separation assertion always has
# its counterpart; the user side builds the consumer=user archive + probe.
test-monitor-runtime: $(KERNEL_RUNTIME_PREREQ) $(USER_ARTIFACTS) $(USER_RUNTIME_PROBE)
	$(call require_capability,rootfs)
	python3 qemutests/monitor_runtime_test.py \
	  --probe "$(USER_RUNTIME_PROBE)" \
	  --user-receipt "$(USER_RUNTIME_RECEIPT)" \
	  --kernel-receipt "$(KERNEL_RUNTIME_RECEIPT)" \
	  --llvm-nm "$(LLVM_NM)" \
	  --llvm-readobj "$(LLVM_READOBJ)" \
	  --profile "$(PROFILE)" \
	  --repo-root "$(OS01_ROOT)"

# driver-model matrix support — see Task 11.  The python harness
# (qemutests/driver_matrix_run.py) drives each (case, SMP) pair; the
# build contract test (qemutests/test_arch9_build_contract.py) only
# needs the Makefile as the build contract.  Both run from the repository
# root via subprocess; the recipes below are entry points.
TEST_DRIVER_MATRIX_CASES := all e1000 virtio mixed two-e1000 two-virtio no-nic no-ahci empty-ahci poll-busy bad-nic adapter-fail unsupported modern-only net-block-smp
DRIVER_MATRIX_DRIVER_CASE ?=
DRIVER_MATRIX_DRIVER_SMP ?=
DRIVER_MODEL_LOG_DIR := $(OS01_ROOT)/test-results/driver-model
.PHONY: _test-driver-model-prep _test-driver-model-run
# _test-driver-model-prep builds the canonical (non-fault) disk image
# which the matrix harness needs for all the no-fault cases.  Fault
# cases build their own variant images via `make DRIVER_MODEL_FAULT=<fault>`.
_test-driver-model-prep:
	@mkdir -p $(DRIVER_MODEL_LOG_DIR)
	$(MAKE) --no-print-directory disk.img
_test-driver-model-run:
	@mkdir -p $(DRIVER_MODEL_LOG_DIR)
	DRIVER_MODEL_LOG_DIR="$(DRIVER_MODEL_LOG_DIR)" \
	  OS01_BUILD_DIR="$(abspath $(BUILD_DIR))" \
	  python3 qemutests/driver_matrix_run.py \
	  $(if $(DRIVER_MODEL_DRIVER_CASE),--case $(DRIVER_MODEL_DRIVER_CASE),--case all) \
	  $(if $(DRIVER_MATRIX_DRIVER_SMP),--smp $(DRIVER_MATRIX_DRIVER_SMP),)

# driver-model matrix dispatch target.  `make test-qemu SUITE=driver-model`
# builds the canonical image then runs every matrix case; the
# optional DRIVER_MODEL_DRIVER_CASE narrows to a single case (mirrors
# the per-MODE pattern used by test-aarch64). DRIVER_MATRIX_DRIVER_SMP
# restricts the SMP count(s) passed to the python harness (Task 11
# parked follow-up resolved in Task 12):
#   make test-qemu SUITE=driver-model DRIVER_MATRIX_DRIVER_SMP=1
#   make test-qemu SUITE=driver-model DRIVER_MATRIX_DRIVER_SMP="1 2 4"
test-qemu-driver-model: _test-driver-model-prep _test-driver-model-run
	@true

# ── x86 boot-* QEMU fault matrices (shared generator) ───────────
# The five SUITE=boot-* matrices share one skeleton: build the normal
# image and hash it (<slug>.normal.before), run the normal CPU matrix,
# build each fault variant through the controlled sub-make and run it
# against its isolated per-case image paths, then re-hash the normal
# image and cmp against the before-hash (each matrix proves it never
# wrote the normal image).  The per-suite python runner is the only
# success criterion — a timeout is never a pass.
#
# $(call boot_matrix_suite,slug,runner.py,timeout-var,variant-flag,
#                           normal-flag,log-root,log-tmpl,extra-flags,cases)
#   slug          catalog/echo id (e.g. boot-lifecycle)
#   timeout-var   per-suite ?= timeout variable (defined next to the call)
#   variant-flag  $(MAKE) <flag>=<case> image for the variant builds
#   normal-flag   extra flags on the normal build (empty, or
#                 "BOOT_*_TEST=none KERNEL_SELFTEST=")
#   log-root/-tmpl --log-dir root + suffix template with @CASE@/@CPUS@
#                 (empty tmpl = runner takes no --log-dir; the runners
#                 create the directory with exist_ok, so reruns of one
#                 matrix reuse the same per-case directory)
#   extra-flags   fixed runner flags (e.g. --build-dir/--profile)
#   cases         "kind:case:cpus" (plus-separated cpus, e.g. 1+2+4); kind:
#                 normal | variant (variant build + run) | plain (reuses
#                 the normal image without a variant build)
#
# Per-suite arch-flavor knobs (BM_<knob>_<slug>, unset = x86 defaults) let
# a non-x86 suite reuse the same generator — the aarch64 boot-contract
# matrix (MODE=boot-contract) does exactly that:
#   BM_RUNNER_PRE_<slug>    args right after the runner (default empty;
#                           --arch <a> is required by boot_contract.py)
#   BM_FIRMWARE_<slug>      --firmware (default $(OVMF_FIRMWARE))
#   BM_QEMU_<slug>          --qemu (default $(QEMU_BIN))
#   BM_NORMAL_IMAGE_<slug>  the normal-case image + hash subject
#                           (default $(NORMAL_IMAGE); hash files land
#                           beside it as <slug>.normal.{before,after})
#   BM_VDIR_<slug>          variant image/kernel dir prefix (default
#                           <slug>; aarch64 must match project.mk's
#                           selector slug boot-contract-<case>)
#   BM_IMAGE_FILE_<slug>    variant image basename (default disk.img)
#   BM_BUILD_TARGET_<slug>  the sub-make build target (default image)
#   BM_UMBRELLA_<slug>      generated umbrella target (default
#                           test-qemu-<slug>)
#
# Generator pitfalls (each was a real bug during the 2026-11-15 slim-down):
#   - GNU make filter patterns support only ONE '%' — case specs are
#     therefore kind-first so "normal:%" etc. work;
#   - a define body stores backslash-newlines verbatim and eval
#     mis-parses them — every RHS/foreach here is ONE line;
#   - a recipe fragment containing real newlines cannot be materialized
#     into an assignment at eval time ("recipe before first target") —
#     the *_LINES values store ESCAPED $(call ...) text (@NL@ markers)
#     and only expand at recipe-execution time.
empty :=
space := $(empty) $(empty)

# One newline (the join separator between generated recipe fragments).
define _boot_matrix_nl


endef

# One (case, cpus) run — echo + python in one shell command.  Expanded
# at recipe-execution time, so the per-suite timeout keeps honoring
# command-line overrides.
# $(1)=slug $(2)=case $(3)=cpus $(4)=runner.py $(5)=timeout-var
# $(6)=log-root $(7)=log-tmpl (@CASE@/@CPUS@) $(8)=extra flags
define _boot_matrix_run_line
	@echo "  [test-qemu] SUITE=$(1): case=$(2) cpus=$(3)" && python3 $(4) $(BM_RUNNER_PRE_$(1)) --case $(2) --cpus $(3) --timeout $($(5)) --firmware "$(or $(BM_FIRMWARE_$(1)),$(OVMF_FIRMWARE))" --image "$(TEST_BOOT_$(1)_IMAGE_$(2))" --kernel-elf "$(TEST_BOOT_$(1)_ELF_$(2))" --qemu "$(or $(BM_QEMU_$(1)),$(QEMU_BIN))" $(8) $(if $(7),--log-dir "$(6)/$(subst @CASE@,$(2),$(subst @CPUS@,$(3),$(7)))")
endef

# $(1)=slug $(2)=case $(3)=kind
define _boot_matrix_case_vars
TEST_BOOT_$(1)_IMAGE_$(2) := $(if $(filter variant,$(3)),$(BUILD_DIR)/image/$(or $(BM_VDIR_$(1)),$(1))-$(2)/$(or $(BM_IMAGE_FILE_$(1)),disk.img),$(or $(BM_NORMAL_IMAGE_$(1)),$(NORMAL_IMAGE)))
TEST_BOOT_$(1)_ELF_$(2)   := $(if $(filter variant,$(3)),$(BUILD_DIR)/kernel/$(or $(BM_VDIR_$(1)),$(1))-$(2)/kernel.elf,$(BUILD_DIR)/kernel/kernel.elf)
endef

# Variant case: the deferred run-line list plus the build-then-run rule.
# $(1)=slug $(2)=case $(3)=cpus $(4)=runner.py $(5)=timeout-var
# $(6)=variant-flag $(7)=log-root $(8)=log-tmpl $(9)=extra flags
define _boot_matrix_variant
TEST_$(1)_VARIANT_$(2)_LINES = $$(subst @NL@,$$(_boot_matrix_nl),$$(foreach cpu,$(subst +,$(space),$(3)),@NL@$$(call _boot_matrix_run_line,$(1),$(2),$$(cpu),$(4),$(5),$(7),$(8),$(9))))
.PHONY: _test-$(1)-variant-$(2)
_test-$(1)-variant-$(2):
	@echo "  [test-qemu] SUITE=$(1): building the $(2) variant"
	$$(MAKE) --no-print-directory $(6)=$(2) KERNEL_SELFTEST= $(or $(BM_BUILD_TARGET_$(1)),image)
	$$(TEST_$(1)_VARIANT_$(2)_LINES)
endef

# $(1)=slug $(2)=runner.py $(3)=timeout-var $(4)=variant-flag
# $(5)=normal-flag $(6)=log-root $(7)=log-tmpl $(8)=extra flags $(9)=cases
define boot_matrix_suite
$(foreach spec,$(9),$(eval $(call _boot_matrix_case_vars,$(1),$(word 2,$(subst :, ,$(spec))),$(word 1,$(subst :, ,$(spec))))))
TEST_$(1)_NORMAL_LINES = $$(subst @NL@,$$(_boot_matrix_nl),$$(foreach spec,$(filter normal:% plain:%,$(9)),$$(foreach cpu,$$(subst +,$$(space),$$(word 3,$$(subst :, ,$$(spec)))),@NL@$$(call _boot_matrix_run_line,$(1),$$(word 2,$$(subst :, ,$$(spec))),$$(cpu),$(2),$(3),$(6),$(7),$(8)))))
# 2026-11-20: each variant body MUST be eval'd per iteration. The body
# ends in a TAB recipe line and the next body starts with an assignment;
# concatenated raw inside this foreach, the boundary newline is lost and
# the next variant's TEST_..._LINES assignment merges onto the previous
# variant's last run line (make 4.4.1 verified; broke every multi-variant
# boot matrix at runtime).
$(foreach spec,$(filter variant:%,$(9)),$(eval $(call _boot_matrix_variant,$(1),$(word 2,$(subst :, ,$(spec))),$(word 3,$(subst :, ,$(spec))),$(2),$(3),$(4),$(6),$(7),$(8))))
.PHONY: _test-$(1)-normal-image _test-$(1)-normal-cases _test-$(1)-normal-hash-check
_test-$(1)-normal-image:
	@echo "  [test-qemu] SUITE=$(1): building the normal image"
	$$(MAKE) --no-print-directory $(5) $(or $(BM_BUILD_TARGET_$(1)),image)
	sha256sum "$(or $(BM_NORMAL_IMAGE_$(1)),$(NORMAL_IMAGE))" > "$(dir $(or $(BM_NORMAL_IMAGE_$(1)),$(NORMAL_IMAGE)))$(1).normal.before"
_test-$(1)-normal-hash-check:
	sha256sum "$(or $(BM_NORMAL_IMAGE_$(1)),$(NORMAL_IMAGE))" > "$(dir $(or $(BM_NORMAL_IMAGE_$(1)),$(NORMAL_IMAGE)))$(1).normal.after"
	cmp "$(dir $(or $(BM_NORMAL_IMAGE_$(1)),$(NORMAL_IMAGE)))$(1).normal.before" "$(dir $(or $(BM_NORMAL_IMAGE_$(1)),$(NORMAL_IMAGE)))$(1).normal.after"
.PHONY: _test-$(1)-normal-cases
_test-$(1)-normal-cases:
	$$(TEST_$(1)_NORMAL_LINES)
# Prereq order is deliberate: normal image + hash FIRST, then the normal
# matrix, then each fault variant (build + run), the hash re-check last.
# Serial make only — the matrix depends on this strict ordering.
.PHONY: $(or $(BM_UMBRELLA_$(1)),test-qemu-$(1))
$(or $(BM_UMBRELLA_$(1)),test-qemu-$(1)): _test-$(1)-normal-image _test-$(1)-normal-cases $(foreach spec,$(filter variant:%,$(9)),_test-$(1)-variant-$(word 2,$(subst :, ,$(spec)))) _test-$(1)-normal-hash-check
	@true
endef

BOOT_LIFECYCLE_TIMEOUT ?= 90
$(eval $(call boot_matrix_suite,boot-lifecycle,qemutests/boot_lifecycle.py,BOOT_LIFECYCLE_TIMEOUT,X86_SMP_BOOT_TEST,,$(abspath $(BUILD_DIR))/test-results/boot-lifecycle,@CASE@-@CPUS@,--build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)",normal:normal:1+2+4+8 variant:delay-online:2+4 variant:no-online:2 variant:no-scheduler:2))

BOOT_IRQ_TIME_TIMEOUT ?= 90
$(eval $(call boot_matrix_suite,boot-irq-time,qemutests/boot_irq_time.py,BOOT_IRQ_TIME_TIMEOUT,X86_BOOT_IRQ_TIME_TEST,,$(abspath $(BUILD_DIR))/test-results/boot-irq-time,@CASE@-@CPUS@,--build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)",normal:normal:1+2+4+8 variant:force-rtc:2 variant:rtc-timeout:1 variant:bsp-lapic-fail:1+2+4 variant:unsafe-open:1))

BOOT_FAILURE_TIMEOUT ?= 90
$(eval $(call boot_matrix_suite,boot-failure,qemutests/boot_failure.py,BOOT_FAILURE_TIMEOUT,BOOT_FAILURE_TEST,,,,--build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)",normal:normal:1+2+4 variant:required-subsys:1+2 variant:optional-subsys:1+2 variant:registration:1 variant:root:1 variant:tty:1 variant:pty:1 variant:stdio:1+2 variant:init:1+2 variant:child-fds:1+2))

BOOT_CONTRACT_TIMEOUT ?= 120
# d10cc2d2 made boot_contract.py's --arch required; the x86 suite's run
# lines were never given it (every real run died in argparse before
# booting QEMU). Sourced through the generator's per-slug knob.
BM_RUNNER_PRE_boot-contract := --arch x86_64
$(eval $(call boot_matrix_suite,boot-contract,qemutests/boot_contract.py,BOOT_CONTRACT_TIMEOUT,BOOT_CONTRACT_TEST,BOOT_CONTRACT_TEST=none KERNEL_SELFTEST=,$(abspath $(BUILD_DIR))/test-results/boot-contract,x86_64-@CASE@-@CPUS@cpu,,normal:normal:1+2+4 variant:reverse:1+2 variant:missing:1 variant:cycle:1))

BOOT_SERVICES_TIMEOUT ?= 120
$(eval $(call boot_matrix_suite,boot-services,qemutests/boot_services.py,BOOT_SERVICES_TIMEOUT,BOOT_SERVICES_TEST,BOOT_SERVICES_TEST=none KERNEL_SELFTEST=,$(abspath $(BUILD_DIR))/test-results/boot-services,@CASE@-@CPUS@cpu,--build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)",normal:normal:1+2+4+8 plain:no-net:1+2 variant:overlap:2+4))

# ── aarch64 boot-contract (MODE=boot-contract) via the same generator ──
# The aarch64 boot-contract matrix reuses the x86 boot_matrix_suite
# generator (same runner, same case set, same sequence) with the aarch64
# image/ELF/QEMU/firmware plumbing through the BM_*_<slug> knobs. The
# generated umbrella IS the MODE run target (test-aarch64 MODE=boot-
# contract -> $(MAKE) _test-aarch64-run-boot-contract), so no extra
# dispatch layer exists. The variant image/kernel dirs follow project.mk's
# selector slug (boot-contract-<case>), NOT the suite slug — hence
# BM_VDIR. The hash files are <suite-slug>.normal.{before,after} beside
# the normal image (boot-contract-aarch64.normal.* — renamed from the
# old aarch64-boot-contract.normal.*; transient build artifacts).
BM_RUNNER_PRE_boot-contract-aarch64   := --arch aarch64
BM_FIRMWARE_boot-contract-aarch64     := $(AARCH64_UEFI_FIRMWARE)
BM_QEMU_boot-contract-aarch64         := $(AARCH64_QEMU)
BM_NORMAL_IMAGE_boot-contract-aarch64 := $(BUILD_DIR)/image/aarch64-uefi.img
BM_VDIR_boot-contract-aarch64         := boot-contract
BM_IMAGE_FILE_boot-contract-aarch64   := aarch64-uefi.img
BM_BUILD_TARGET_boot-contract-aarch64 := aarch64-uefi
BM_UMBRELLA_boot-contract-aarch64     := _test-aarch64-run-boot-contract
$(eval $(call boot_matrix_suite,boot-contract-aarch64,qemutests/boot_contract.py,BOOT_CONTRACT_TIMEOUT,BOOT_CONTRACT_TEST,BOOT_CONTRACT_TEST=none KERNEL_SELFTEST=,$(abspath $(BUILD_DIR))/test-results/boot-contract,aarch64-@CASE@-@CPUS@cpu,,normal:normal:1+2+4 variant:reverse:1+2 variant:missing:1 variant:cycle:1))





# ── intr-x86: isolated destructive #BP / NMI acceptance ─────────
# Task 5 (spec §9.3): the two fatal entries that cannot be exercised
# through the ordinary boot get their OWN kernel/image variant and
# single-CPU boot: bp (kernel #BP; saved RIP must equal the resume label
# resolved from the VARIANT ELF) and nmi (REAL hardware NMI over QMP
# `inject-nmi`).  Built via the controlled sub-make (probe-selection
# macro in KERNEL_EXTRA_CFLAGS); qemutests/x86_intr_fault.py's parser is
# the only success criterion.  Pre-build and run stay on their OWN
# recipe lines ($(MAKE) executes under -n; see test-qemu's note).
INTR_X86_TIMEOUT ?= 120
# The isolated variant paths.  The slug must match the one the profile
# builds (mk/profiles/x86_64-clang.mk) — the build-contract bucket
# pins the spellings together.
TEST_INTR_X86_IMAGE_bp := $(BUILD_DIR)/image/intr-x86-fault-bp/disk.img
TEST_INTR_X86_IMAGE_nmi := $(BUILD_DIR)/image/intr-x86-fault-nmi/disk.img
TEST_INTR_X86_IMAGE_trace_read := $(BUILD_DIR)/image/intr-x86-fault-trace-read/disk.img
TEST_INTR_X86_IMAGE_trace_bad_fp := $(BUILD_DIR)/image/intr-x86-fault-trace-bad-fp/disk.img
TEST_INTR_X86_ELF_bp   := $(BUILD_DIR)/kernel/intr-x86-fault-bp/kernel.elf
TEST_INTR_X86_ELF_nmi   := $(BUILD_DIR)/kernel/intr-x86-fault-nmi/kernel.elf
TEST_INTR_X86_ELF_trace_read := $(BUILD_DIR)/kernel/intr-x86-fault-trace-read/kernel.elf
TEST_INTR_X86_ELF_trace_bad_fp := $(BUILD_DIR)/kernel/intr-x86-fault-trace-bad-fp/kernel.elf
.PHONY: test-qemu-intr-x86
test-qemu-intr-x86:
	@echo "  [test-qemu] SUITE=intr-x86: building the isolated #BP variant"
	$(MAKE) --no-print-directory X86_INTR_FAULT_TEST=bp image
	@echo "  [test-qemu] SUITE=intr-x86: running the #BP case"
	python3 qemutests/x86_intr_fault.py --case bp \
	  --timeout $(INTR_X86_TIMEOUT) \
	  --firmware "$(OVMF_FIRMWARE)" \
	  --image "$(TEST_INTR_X86_IMAGE_bp)" \
	  --elf "$(TEST_INTR_X86_ELF_bp)" \
	  --nm "$(LLVM_NM)" \
	  --qemu "$(QEMU_BIN)" \
	  --build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)" \
	  --log-dir "$(abspath $(BUILD_DIR))/test-results/intr-x86/bp-$$(date -u +%Y%m%dT%H%M%S)-$$$$"
	@echo "  [test-qemu] SUITE=intr-x86: building the isolated NMI variant"
	$(MAKE) --no-print-directory X86_INTR_FAULT_TEST=nmi image
	@echo "  [test-qemu] SUITE=intr-x86: running the NMI case"
	python3 qemutests/x86_intr_fault.py --case nmi \
	  --timeout $(INTR_X86_TIMEOUT) \
	  --firmware "$(OVMF_FIRMWARE)" \
	  --image "$(TEST_INTR_X86_IMAGE_nmi)" \
	  --elf "$(TEST_INTR_X86_ELF_nmi)" \
	  --nm "$(LLVM_NM)" \
	  --qemu "$(QEMU_BIN)" \
	  --build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)" \
	  --log-dir "$(abspath $(BUILD_DIR))/test-results/intr-x86/nmi-$$(date -u +%Y%m%dT%H%M%S)-$$$$"
	@echo "  [test-qemu] SUITE=intr-x86: building the isolated trace-read variant"
	$(MAKE) --no-print-directory X86_INTR_FAULT_TEST=trace-read image
	@echo "  [test-qemu] SUITE=intr-x86: running the trace-read case"
	python3 qemutests/exception_trace.py --profile "$(PROFILE)" --case read \
	  --timeout $(INTR_X86_TIMEOUT) \
	  --firmware "$(OVMF_FIRMWARE)" \
	  --image "$(TEST_INTR_X86_IMAGE_trace_read)" \
	  --elf "$(TEST_INTR_X86_ELF_trace_read)" \
	  --nm "$(LLVM_NM)" \
	  --qemu "$(QEMU_BIN)" \
	  --build-dir "$(abspath $(BUILD_DIR))" \
	  --log-dir "$(abspath $(BUILD_DIR))/test-results/intr-x86/trace-read-$$(date -u +%Y%m%dT%H%M%S)-$$$$"
	@echo "  [test-qemu] SUITE=intr-x86: building the isolated trace-bad-fp variant"
	$(MAKE) --no-print-directory X86_INTR_FAULT_TEST=trace-bad-fp image
	@echo "  [test-qemu] SUITE=intr-x86: running the trace-bad-fp case"
	python3 qemutests/exception_trace.py --profile "$(PROFILE)" --case bad-fp \
	  --timeout $(INTR_X86_TIMEOUT) \
	  --firmware "$(OVMF_FIRMWARE)" \
	  --image "$(TEST_INTR_X86_IMAGE_trace_bad_fp)" \
	  --elf "$(TEST_INTR_X86_ELF_trace_bad_fp)" \
	  --nm "$(LLVM_NM)" \
	  --qemu "$(QEMU_BIN)" \
	  --build-dir "$(abspath $(BUILD_DIR))" \
	  --log-dir "$(abspath $(BUILD_DIR))/test-results/intr-x86/trace-bad-fp-$$(date -u +%Y%m%dT%H%M%S)-$$$$"

# Per-SUITE lookups (TEST_QEMU_SUITE_IDS, TEST_QEMU_FLAVOR_*, TEST_QEMU_IMG_*)
# and TEST_SUITE_CATALOG_DATA are generated into mk/generated/test-dispatch.mk.
TERMINAL_PRESENT_BASELINE_LOG ?= $(OS01_ROOT)/qemutests/baselines/terminal-present/serial.log
TEST_DOOM_IMAGE                = $(BUILD_DIR)/image/doom-test/disk.img
TEST_DISPATCH_GEN := $(OS01_ROOT)/mk/generated/test-dispatch.mk
ifeq ($(wildcard $(TEST_DISPATCH_GEN)),)
$(error test-dispatch.mk missing; run 'make regen-test-catalog')
endif
include $(TEST_DISPATCH_GEN)

# USB boot M0 firmware gate.  This is a dedicated target, not a catalog
# suite until M1b: the expected-failure M0 checkpoint stays out of the
# automated bucket, so `test-qemu SUITE=usb-boot` is intentionally
# invalid until the bridge entry lands in M1b.
USB_BOOT_CASE ?= readonly
USB_BOOT_TIMEOUT ?= 120
USB_BOOT_AHCI_IMAGE := $(abspath $(BUILD_DIR))/image/disk-ahci-usb-test.img
# One job-private log directory per invocation.  A Make variable (not a
# shell variable) so the same path is visible to every recipe line; the
# PID of the parse-time shell keeps concurrent jobs from colliding.
USB_BOOT_JOB_DIR := $(abspath $(BUILD_DIR))/test-results/usb-boot/$(shell date -u +%Y%m%dT%H%M%S)-$(shell echo $$$$)
USB_HID_CASE ?= direct
USB_HID_TIMEOUT ?= 180
USB_HID_JOB_DIR := $(abspath $(BUILD_DIR))/test-results/usb-hid/$(USB_HID_CASE)-smp$(SMP)-$(shell date -u +%Y%m%dT%H%M%S)-$(shell echo $$$$)

.PHONY: test-qemu
# Use the per-SUITE Make variables from Step 1. The image path is
# informational — it is NOT used as a prerequisite, because the
# variant image path has no direct build rule (the chain is `image` (phony)
# → `$(DISK_IMG)` (variable) → rule in mk/components/image.mk). Listing
# the raw image path as a prereq fails with "No rule to make target"
# (verified: `make -n test-qemu SUITE=systest` errors with the systest
# variant path). The variant build is triggered by a `$(MAKE) ... image`
# sub-make inside the recipe.
# Reject syscall E2E combined with kernel-selftest at parse time so it
# aborts before any build. This check must run before any prerequisites
# are built.
ifneq ($(filter test-qemu,$(MAKECMDGOALS)),)
ifeq ($(SUITE),systest)
ifeq ($(KERNEL_SELFTEST),1)
$(error ERROR: syscall E2E must not be combined with KERNEL_SELFTEST=1)
endif
endif
ifeq ($(SUITE),arch10-capacity)
ifneq ($(filter 1,$(CAPACITY_TEST)),1)
$(error ERROR: test-qemu SUITE=arch10-capacity requires CAPACITY_TEST=1 on the top-level make invocation)
endif
endif
ifeq ($(SUITE),arch6-fsstress)
ifneq ($(filter 1,$(ARCH6_FSSTRESS_TEST)),1)
$(error ERROR: test-qemu SUITE=arch6-fsstress requires ARCH6_FSSTRESS_TEST=1 on the top-level make invocation)
endif
ifneq ($(SMP),4)
$(error ERROR: test-qemu SUITE=arch6-fsstress requires SMP=4 on the top-level make invocation)
endif
ifneq ($(QEMU_SMP),)
ifneq ($(QEMU_SMP),4)
$(error ERROR: test-qemu SUITE=arch6-fsstress requires QEMU_SMP=4 (got QEMU_SMP=$(QEMU_SMP)))
endif
endif
endif
endif

SYSTEST_TIMEOUT ?= 120

test-qemu: SUITE ?= phase-0
test-qemu: SUITE := $(SUITE)
ifneq ($(filter test-qemu,$(MAKECMDGOALS)),)
ifeq ($(filter $(SUITE),$(TEST_QEMU_SUITE_IDS)),)
$(error ERROR: SUITE must be one of: $(TEST_QEMU_SUITE_IDS), got '$(SUITE)')
endif
ifneq ($(strip $(QEMU_SMP_MATRIX)),)
ifeq ($(filter phase-0 systest,$(SUITE)),)
$(error ERROR: QEMU_SMP_MATRIX is only supported for SUITE=phase-0 or systest)
endif
ifneq ($(filter-out 1 2 4 8,$(QEMU_SMP_MATRIX)),)
$(error ERROR: QEMU_SMP_MATRIX must contain only CPU counts 1 2 4 8)
endif
ifneq ($(words $(QEMU_SMP_MATRIX)),$(words $(sort $(QEMU_SMP_MATRIX))))
$(error ERROR: QEMU_SMP_MATRIX must not repeat CPU counts)
endif
ifneq ($(strip $(QEMU_SMP)),)
$(error ERROR: QEMU_SMP_MATRIX cannot be combined with QEMU_SMP)
endif
endif
endif
# Firmware remains a prerequisite for every suite. No image-path prereq:
# the variant image is built by the recursive make below.
test-qemu: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@echo "  [test-qemu] SUITE=$(SUITE) flavor=$(TEST_QEMU_FLAVOR_$(SUITE)) img=$(TEST_QEMU_IMG_$(SUITE))"
	# Normal-image hash guard.  Skipped for suites that never write the
	# normal image: phase-0 (no-variant path), gfx/gui/gui-desktop (run it
	# read-only via -snapshot), resolution (copies into a private per-run
	# dir and asserts the source sha256 itself), driver-model (canonical
	# no-fault image; fault variants land under image/driver-model-<fault>/),
	# monitor and terminal-present (own image variant).  intr-x86 is NOT
	# skipped: it builds isolated fault variants and the guard proves it.
	@if [ "$(SUITE)" != "phase-0" ] && [ "$(SUITE)" != "gfx" ] && [ "$(SUITE)" != "resolution" ] && [ "$(SUITE)" != "driver-model" ] && [ "$(SUITE)" != "monitor" ] && [ "$(SUITE)" != "terminal-present" ] && [ "$(SUITE)" != "gui" ] && [ "$(SUITE)" != "gui-desktop" ] && [ -f "$(NORMAL_IMAGE)" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.before"; \
	fi
	# Build step. THIS RECIPE LINE CONTAINS ``$(MAKE)`` AND IS DELIBERATELY
	# KEPT ON ITS OWN LOGICAL LINE: GNU make executes such lines even under
	# ``make -n``, while ``python3 qemutests/run_test.py`` (a later line)
	# is only printed — that split is what keeps the dry-run contract of
	# build_contract.sh ``targets`` and MakeFailureDryRunTests intact.
	# Keep the two on SEPARATE recipe lines; see fix/ci-test-qemu-dryrun.
	@if [ "$(SUITE)" = "driver-model" ]; then \
	  DRIVER_MODEL_DRIVER_CASE="$(DRIVER_MODEL_DRIVER_CASE)" \
	  DRIVER_MATRIX_DRIVER_SMP="$(DRIVER_MATRIX_DRIVER_SMP)" \
	  $(MAKE) --no-print-directory test-qemu-driver-model || exit 1; \
	elif [ "$(SUITE)" = "intr-x86" ]; then \
	  $(MAKE) --no-print-directory test-qemu-intr-x86 || exit 1; \
	elif [ "$(SUITE)" = "boot-lifecycle" ]; then \
	  $(MAKE) --no-print-directory test-qemu-boot-lifecycle || exit 1; \
	elif [ "$(SUITE)" = "boot-irq-time" ]; then \
	  $(MAKE) --no-print-directory test-qemu-boot-irq-time || exit 1; \
	elif [ "$(SUITE)" = "boot-failure" ]; then \
	  $(MAKE) --no-print-directory test-qemu-boot-failure || exit 1; \
	elif [ "$(SUITE)" = "boot-contract" ]; then \
	  $(MAKE) --no-print-directory test-qemu-boot-contract || exit 1; \
	elif [ "$(SUITE)" = "boot-services" ]; then \
	  $(MAKE) --no-print-directory test-qemu-boot-services || exit 1; \
	elif [ "$(SUITE)" = "usb-boot" ]; then \
	  for c in readonly dual ahci persistence superspeed hub; do \
	    $(MAKE) --no-print-directory test-qemu-usb-boot USB_BOOT_CASE=$$c || exit 1; \
	  done; \
	elif [ "$(SUITE)" = "usb-hid" ]; then \
	  for c in direct hub; do \
	    for s in 1 2; do \
	      $(MAKE) --no-print-directory test-qemu-usb-hid USB_HID_CASE=$$c SMP=$$s || exit 1; \
	    done; \
	  done; \
	elif [ "$(SUITE)" = "doom" ]; then \
	  if [ "$(DOOM_ASSET_COUNT)" != "4" ]; then \
	    echo "ERROR: SUITE=doom requires all four DOOM_IWAD variables to be provided" >&2; exit 1; \
	  fi; \
	  $(MAKE) MAKEOVERRIDES= DOOM_TEST=1 $(TEST_DOOM_IMAGE) || exit 1; \
	else \
	  if [ "$(SUITE)" = "systest" ] && [ -n "$(CASE)" ]; then \
	    case "$(CASE)" in *[!A-Za-z0-9_.-]*) echo "ERROR: CASE='$(CASE)' must match [A-Za-z0-9_.-]+ (see user/systest.c --list)" >&2; exit 2;; esac; \
	    mkdir -p "$(abspath $(BUILD_DIR))/config" || exit 1; \
	    printf 'tty1:once:/bin/systest --case %s\n' "$(CASE)" > "$(abspath $(BUILD_DIR))/config/inittab.systest.case" || exit 1; \
	    $(MAKE) OS01_SYSTEST=1 INITTAB_FILE="$(abspath $(BUILD_DIR))/config/inittab.systest.case" image || exit 1; \
	  else \
	    $(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image || exit 1; \
	  fi; \
	fi
	# Hash guard AFTER the build (own recipe line, no ``$(MAKE)``).
	@if [ "$(SUITE)" != "phase-0" ] && [ "$(SUITE)" != "gfx" ] && [ "$(SUITE)" != "resolution" ] && [ "$(SUITE)" != "driver-model" ] && [ "$(SUITE)" != "monitor" ] && [ "$(SUITE)" != "terminal-present" ] && [ "$(SUITE)" != "gui" ] && [ "$(SUITE)" != "gui-desktop" ] && [ "$(SUITE)" != "usb-boot" ] && [ "$(SUITE)" != "usb-hid" ] && [ -f "$(NORMAL_IMAGE_DIR)/normal.before" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.after" || exit 1; \
	  cmp "$(NORMAL_IMAGE_DIR)/normal.before" "$(NORMAL_IMAGE_DIR)/normal.after" || exit 1; \
	fi
	# Run the harness (own recipe line, no ``$(MAKE)`` — therefore NOT
	# executed under ``make -n``; this is the line that broke the
	# ``build_contract.sh targets`` dry-run when it was inlined into the
	# same ``if/else/fi`` block as ``$(MAKE)``). driver-model runs the
	# python harness inside ``test-qemu-driver-model`` and must NOT
	# launch it again here; intr-x86 runs its own per-case harness
	# inside ``test-qemu-intr-x86`` for the same reason.
	@if [ "$(SUITE)" != "driver-model" ] && [ "$(SUITE)" != "intr-x86" ] && [ "$(SUITE)" != "boot-lifecycle" ] && [ "$(SUITE)" != "boot-irq-time" ] && [ "$(SUITE)" != "boot-failure" ] && [ "$(SUITE)" != "boot-contract" ] && [ "$(SUITE)" != "boot-services" ] && [ "$(SUITE)" != "usb-boot" ] && [ "$(SUITE)" != "usb-hid" ]; then \
	  set -e; \
	  image="$(TEST_QEMU_IMG_$(SUITE))"; \
	  matrix="$(strip $(QEMU_SMP_MATRIX))"; \
	  if [ -n "$$matrix" ]; then \
	    run_dir=$$(mktemp -d "$$(dirname -- "$$image")/.qemu-smp.XXXXXX"); \
	    trap 'rm -f -- "$$run_dir"/smp-*.img; rmdir -- "$$run_dir"' EXIT; \
	    trap 'exit 130' INT; \
	    trap 'exit 143' TERM; \
	  fi; \
	  for cpus in $${matrix:-default}; do \
	    disk="$$image"; \
	    if [ -n "$$matrix" ]; then \
	      disk="$$run_dir/smp-$$cpus.img"; \
	      cp --reflink=auto --sparse=always -- "$$image" "$$disk"; \
	      export QEMU_SMP="$$cpus"; \
	      echo "  [test-qemu] $(SUITE) @ $$cpus CPU(s)"; \
	    fi; \
	    DISK_IMG="$$disk" \
	    $(if $(filter arch6-fsstress,$(SUITE)),QEMU_SMP="$(SMP)") \
	    OVMF_FIRMWARE="$(OVMF_FIRMWARE)" \
	    NETWORK_NIC="$(NETWORK_NIC)" \
	    FB_RESOLUTION_TEST="$(FB_RESOLUTION_TEST)" \
	    TERMINAL_PRESENT_BASELINE_LOG="$(TERMINAL_PRESENT_BASELINE_LOG)" \
	    OS01_BUILD_DIR="$(abspath $(BUILD_DIR))" \
	    OS01_RESOLUTION_RESULT_DIR="$(abspath $(BUILD_DIR)/test-results/resolution)" \
	    SYSTEST_CASE="$(CASE)" \
	    SYSTEST_TIMEOUT="$(SYSTEST_TIMEOUT)" \
	    python3 qemutests/run_test.py $(SUITE) || exit $$?; \
	    if [ -n "$$matrix" ]; then rm -f -- "$$disk"; fi; \
	  done; \
	fi

# ── USB boot M0 firmware gate (dedicated target) ──────────────
# Direct invocation only (deliberately NOT in the generated catalog
# until M1b): the runner owns its private USB-only QEMU topology, proves
# OVMF loaded BOOTX64.EFI + kernel.bin from USB, and expects the stock
# kernel (no USB MSC driver) to stop at fs_boot_mounts rc=-19.  The
# source image is read-only input — the hash sandwich is one shell line
# so the after-hash and cmp run even when the runner reports FAIL, and
# `$status` preserves the runner verdict as the exit status.  No
# $(MAKE) in the recipe — the -n dry-run prints exactly one
# `python3 qemutests/usb_boot.py` invocation.
ifneq ($(filter rootfs,$(PROFILE_CAPABILITIES)),)
$(USB_BOOT_AHCI_IMAGE): $(ROOTFS_MANIFEST) $(UEFI_EFI) $(HOST_MKDISK)
	@$(MAKE) -C tools check-deps HOST_TOOLS_DIR=$(BUILD_DIR)/host-tools
	@mkdir -p $(dir $@) $(dir $@)tmp-ahci
	@$(HOST_MKDISK) --output $@ --efi $(UEFI_EFI) \
	  --temp-dir $(dir $@)tmp-ahci --rootfs-manifest $(ROOTFS_MANIFEST)
endif

# Hash-sandwich halves shared by test-qemu-usb-boot / test-qemu-usb-hid:
# record the normal image sha before the run, and after the run compare it
# again — any mismatch means the runner modified the source image and fails.
# The python runner stays between the two expansions with its own
# multi-line command (kept out of the $(call) arguments: case-specific
# $(if) flags would make the argument commas fragile).
#   usb_hash_before $(1)=job dir
#   usb_hash_after  $(1)=job dir  $(2)=target label for the error message
define usb_hash_before
mkdir -p "$(1)"; \
sha256sum "$(NORMAL_IMAGE)" > "$(1)/image.before";
endef
define usb_hash_after
status=$$?; \
sha256sum "$(NORMAL_IMAGE)" > "$(1)/image.after"; \
if ! cmp -s "$(1)/image.before" "$(1)/image.after"; then \
  echo "ERROR: $(2) modified the source image $(NORMAL_IMAGE)" >&2; \
  exit 1; \
fi; \
exit $$status
endef

.PHONY: test-qemu-usb-boot
test-qemu-usb-boot: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE) $(if $(filter dual ahci,$(USB_BOOT_CASE)),$(USB_BOOT_AHCI_IMAGE)) $(if $(filter persistence,$(USB_BOOT_CASE)),$(USER_ARTIFACT_DIR)/usb_boot_probe.elf))
	$(call require_capability,rootfs)
	@case "$(USB_BOOT_CASE)" in \
	  firmware|readonly|dual|ahci|persistence|superspeed|hub) ;; \
	  *) echo "USB_BOOT_CASE must be firmware, readonly, dual, ahci, persistence, superspeed, or hub, got '$(USB_BOOT_CASE)'" >&2; exit 2;; \
	esac
	@set +e; \
	$(call usb_hash_before,$(USB_BOOT_JOB_DIR)) \
	python3 qemutests/usb_boot.py --case "$(USB_BOOT_CASE)" \
	  --qemu "$(QEMU_BIN)" --firmware "$(OVMF_FIRMWARE)" \
	  --disk "$(NORMAL_IMAGE)" \
	  $(if $(filter dual ahci,$(USB_BOOT_CASE)),--ahci-disk "$(USB_BOOT_AHCI_IMAGE)") \
	  $(if $(filter persistence,$(USB_BOOT_CASE)),--probe-elf "$(USER_ARTIFACT_DIR)/usb_boot_probe.elf") \
	  --smp "$(SMP)" \
	  --timeout "$(USB_BOOT_TIMEOUT)" \
	  --log-dir "$(USB_BOOT_JOB_DIR)"; \
	$(call usb_hash_after,$(USB_BOOT_JOB_DIR),test-qemu-usb-boot)

.PHONY: test-qemu-usb-hid
test-qemu-usb-hid: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE) $(USER_ARTIFACT_DIR)/usb_hid_probe.elf)
	$(call require_capability,rootfs)
	@case "$(USB_HID_CASE)" in \
	  direct|hub) ;; \
	  *) echo "USB_HID_CASE must be direct or hub, got '$(USB_HID_CASE)'" >&2; exit 2;; \
	esac
	@set +e; \
	$(call usb_hash_before,$(USB_HID_JOB_DIR)) \
	python3 qemutests/usb_hid.py --case "$(USB_HID_CASE)" \
	  --qemu "$(QEMU_BIN)" --firmware "$(OVMF_FIRMWARE)" \
	  --disk "$(NORMAL_IMAGE)" \
	  --probe-elf "$(USER_ARTIFACT_DIR)/usb_hid_probe.elf" \
	  --smp "$(SMP)" \
	  --timeout "$(USB_HID_TIMEOUT)" \
	  --log-dir "$(USB_HID_JOB_DIR)"; \
	$(call usb_hash_after,$(USB_HID_JOB_DIR),test-qemu-usb-hid)

# Exercise repeated exec/exit through the normal terminal and ash path.
.PHONY: test-syscall-repeat
ifneq ($(filter test-syscall-repeat,$(MAKECMDGOALS)),)
ifneq ($(filter 1,$(OS01_SYSTEST) $(KERNEL_SELFTEST)),)
$(error ERROR: test-syscall-repeat requires normal init and KERNEL_SELFTEST=0)
endif
endif
test-syscall-repeat: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	python3 qemutests/x86_64_systest_repeat.py --disk "$(NORMAL_IMAGE)" \
	  --firmware "$(OVMF_FIRMWARE)" --qemu "$(QEMU_BIN)" --memory "$(MEMORY)" \
	  --smp "$(SMP)" --timeout 180 \
	  --build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)"

# test-syscall — retained AGENTS.md-required alias exception.
#
# AGENTS.md (lines 60, 70) requires the literal invocation
#   `make OS01_SYSTEST=1 test-syscall`
# even though the canonical bucket target is `test-qemu SUITE=systest`.
# All other forwarding aliases were removed in the 2026-09-26 cleanup
# (see docs/build/build.md §alias policy); this is the single
# permitted exception. The recipe is a one-liner that delegates to
# the canonical bucket target with the same flag set so the alias
# never drifts from the underlying harness.
#
# Two parse-time gates enforce AGENTS.md's contract:
#  - OS01_SYSTEST must be 1 (systest is the only init mode supported
#    by this alias — running it with the normal inittab would not
#    load /bin/systest as PID 1).
#  - KERNEL_SELFTEST must NOT be 1 (in-kernel selftests spawn kthreads
#    at boot which interfere with systest's fork+exec+waitpid test).
.PHONY: test-syscall
ifneq ($(filter test-syscall,$(MAKECMDGOALS)),)
ifneq ($(OS01_SYSTEST),1)
$(error ERROR: test-syscall requires OS01_SYSTEST=1 on the top-level make invocation (AGENTS.md))
endif
ifeq ($(KERNEL_SELFTEST),1)
$(error ERROR: test-syscall must NOT be combined with KERNEL_SELFTEST=1 (AGENTS.md))
endif
endif
test-syscall:
	$(call require_capability,rootfs)
	$(MAKE) OS01_SYSTEST=1 test-qemu SUITE=systest

# ── Matrix Parallel Preparation & Isolated Execution (Task C5) ────────
.PHONY: _test-suite-prepare _test-suite-run

_test-suite-prepare:
	$(call require_capability,rootfs)
	@if [ -z "$(SUITE_ID)" ] || [ -z "$(PREPARED_MANIFEST)" ]; then \
	  echo "ERROR: _test-suite-prepare requires SUITE_ID=<id> and PREPARED_MANIFEST=<path>" >&2; exit 2; \
	fi
	@if [ "$(SUITE_ID)" = "doom" ]; then \
	  if [ "$(DOOM_ASSET_COUNT)" != "4" ]; then \
	    echo "ERROR: SUITE=doom requires all four DOOM_IWAD variables to be provided" >&2; exit 2; \
	  fi; \
	  $(MAKE) MAKEOVERRIDES= DOOM_TEST=1 $(TEST_DOOM_IMAGE) || exit 1; \
	else \
	  $(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE_ID)) image || exit 1; \
	fi
	python3 qemutests/run_suite_matrix.py prepare \
	  --suite-id "$(SUITE_ID)" \
	  --manifest "$(PREPARED_MANIFEST)" \
	  --build-dir "$(abspath $(BUILD_DIR))" \
	  --profile "$(PROFILE)" \
	  --image "$(TEST_QEMU_IMG_$(SUITE_ID))" \
	  --firmware "$(OVMF_FIRMWARE)" \
	  --variant "$(if $(filter doom,$(SUITE_ID)),doom-test,$(if $(TEST_QEMU_FLAVOR_$(SUITE_ID)),$(SUITE_ID),none))"

# _test-suite-run has NO build prerequisites (no compiler, no image, no sysroot)
_test-suite-run:
	@if [ -z "$(PREPARED_MANIFEST)" ] || [ -z "$(RUN_CONFIG)" ]; then \
	  echo "ERROR: _test-suite-run requires PREPARED_MANIFEST=<path> and RUN_CONFIG=<id>" >&2; exit 2; \
	fi
	python3 qemutests/run_suite_matrix.py run \
	  --manifest "$(PREPARED_MANIFEST)" \
	  --config "$(RUN_CONFIG)" \
	  --build-dir "$(abspath $(BUILD_DIR))" \
	  --profile "$(PROFILE)"

# test-static = the umbrella: runs every static audit in one shot.
# Delegates the 4 runtime-audit checks + validate-kernel to test-runtime
# (its prerequisite) so the recipe never drifts from the runtime target.
# The remaining audits (layout, canary-contract, user-canary, syscall
# boundary) are distinct in harness/prereqs and stay in this recipe.
.PHONY: test-static test-runtime test-kernel-layout test-kernel-canary-contract test-user-canary

# ── Static-audit evidence adapter (Task 12) ─────────────────────
# Every static audit below runs through qemutests/run_static_audit.py, which
# owns the process boundary (ProcessSession) and archives exactly ONE
# `audit`-unit result per executed audit command under
# build/<profile>/logs/tests/<suite>/<UTC>-<unique-id>/.  Make still prepares
# the inputs and selects the command (capability gates, prerequisites and the
# per-audit suite id live here); the adapter runs no build steps and invents
# no case records.  The audit program owns all artifact and diagnostic
# assertions: a PASS means it actually started and exited 0 (a missing
# executable, a signal, a nonzero exit or a timeout is archived as
# ERROR/FAIL/TIMEOUT).  The `<suite>` id is the audit's own name so every
# report names its real argv.
STATIC_AUDIT_TIMEOUT ?= 120
RUN_STATIC_AUDIT = python3 qemutests/run_static_audit.py --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" --timeout $(STATIC_AUDIT_TIMEOUT) --suite
# validate-kernel is not a pure-Python audit: it is a build-y sub-make
# (it may rebuild/relink the kernel), so it must NOT inherit the 120 s
# audit default — a legitimate, slow validate would be a spurious
# TIMEOUT/1.  It gets an explicit, larger budget of its own; every other
# audit keeps STATIC_AUDIT_TIMEOUT.
VALIDATE_KERNEL_AUDIT_TIMEOUT ?= 600
RUN_STATIC_AUDIT_VALIDATE = python3 qemutests/run_static_audit.py --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" --timeout $(VALIDATE_KERNEL_AUDIT_TIMEOUT) --suite

# The seven user-canary checks live in scripts/user_canary_audit.sh
# (verbatim from the pre-Task-12 recipe body; the adapter runs it as one
# audit command so every check still runs exactly as before).
# The focused scheduler compile selector must bypass rootfs-only audits.
ifeq ($(strip $(CASE)),)

# ── test-static: 5 runtime audits via test-runtime + 7 standalone + user-canary ──
STATIC_AUDIT_TARGETS = \
  test-runtime \
  test-syscall-boundary \
  test-kernel-layout \
  test-kernel-canary-contract \
  test-header-object-audit \
  test-stack-frame-audit \
  test-driver-model-boundary \
  test-gui-build-audit \
  test-intr-dispatch-audit \
  test-arch10-capacity-audit \
  test-assertion-policy \
  test-user-canary

.PHONY: $(STATIC_AUDIT_TARGETS) test-static test-static-prereqs

# The eight standalone audits without their own retained rules are
# table-driven: one STATIC_AUDIT_ARG_<name>
# command line + one _static_audit_rule eval per audit (capability gate
# and prereqs vary per audit; test-runtime and test-user-canary stay
# explicit below — five audits / the user-canary script respectively).
# The stored rule text keeps the "$(RUN_STATIC_AUDIT) <name> --" shape
# that test_harness_static derives the audit list from.
define _static_audit_rule
.PHONY: test-$(1)
test-$(1): $(3)
	$$(call require_capability_opt,$(2))
	$$(RUN_STATIC_AUDIT) $(1) -- $$(STATIC_AUDIT_ARG_$(1))
endef

STATIC_AUDIT_ARG_syscall-boundary = python3 qemutests/syscall_boundary_audit.py
STATIC_AUDIT_ARG_header-object-audit = python3 qemutests/header_object_audit.py --include-dir "kernel/include" --sysroot "$(SYSROOT)" --runtime-inc "runtime/include" --llvm-nm "$(LLVM_NM)" --clang "$(CLANG)"
STATIC_AUDIT_ARG_stack-frame-audit = python3 qemutests/stack_frame_audit.py --elf "$(KERNEL_BUILD_DIR)/kernel.elf" --llvm-objdump "$(or $(LLVM_OBJDUMP),$(AARCH64_OBJDUMP),llvm-objdump)" --limit 512
STATIC_AUDIT_ARG_driver-model-boundary = python3 qemutests/driver_model_boundary_audit.py
STATIC_AUDIT_ARG_gui-build-audit = python3 qemutests/gui_build_audit.py --sysroot "$(SYSROOT)" --build-dir "$(BUILD_DIR)" --repo-root "$(OS01_ROOT)" --profile "$(PROFILE)"
STATIC_AUDIT_ARG_intr-dispatch-audit = python3 qemutests/intr_dispatch_audit.py --profile "$(PROFILE)" --elf "$(KERNEL_BUILD_DIR)/kernel.elf" --llvm-nm "$(LLVM_NM)" --llvm-objdump "$(LLVM_OBJDUMP)" --repo-root "$(OS01_ROOT)"
STATIC_AUDIT_ARG_arch10-capacity-audit = python3 qemutests/arch10_capacity_audit.py --repo-root "$(OS01_ROOT)"
STATIC_AUDIT_ARG_assertion-policy = python3 qemutests/assertion_policy_audit.py --repo-root "$(OS01_ROOT)" --exemptions "$(OS01_ROOT)/config/assertion-exemptions.json"
$(eval $(call _static_audit_rule,syscall-boundary,rootfs,))
$(eval $(call _static_audit_rule,header-object-audit,rootfs,))
$(eval $(call _static_audit_rule,stack-frame-audit,kernel,))
$(eval $(call _static_audit_rule,driver-model-boundary,rootfs,))
$(eval $(call _static_audit_rule,gui-build-audit,rootfs,))
$(eval $(call _static_audit_rule,intr-dispatch-audit,rootfs,))
$(eval $(call _static_audit_rule,arch10-capacity-audit,rootfs,))
$(eval $(call _static_audit_rule,assertion-policy,,))
test-static-prereqs:
	$(call require_capability,rootfs)
	$(MAKE) --no-print-directory $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(KERNEL_ARTIFACT)) $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf $(ROOTFS_MANIFEST))

test-static: test-static-prereqs
	+$(MAKE) --no-print-directory $(STATIC_AUDIT_TARGETS)

else

# Focused compile-only selectors.  sched-common-compile (frozen command:
# `make PROFILE=aarch64-clang test-static CASE=sched-common-compile`)
# compiles the arch-neutral scheduler TUs with the real aarch64
# compiler, production defines and freestanding include paths into
# $(KERNEL_BUILD_DIR)/sched-compile-check/, stopping at .o — no test
# macro removes production code, and unresolved real arch externs are
# legal in unlinked objects.  The aarch64 product whitelist is unchanged
# until Phase 3.
SCHED_STATIC_CASES := sched-common-compile
MM_HEADER_BOUNDARY_STATIC_CASES := mm-header-boundary
STATIC_CASES_VALID := $(SCHED_STATIC_CASES) $(MM_HEADER_BOUNDARY_STATIC_CASES)

# Per-case runner targets (pattern of _test-m1-host-run-*): each owns a
# full recipe line so os01_submake's leading '+' stays a make prefix,
# not a shell token.
.PHONY: _test-static-case-sched-common-compile
_test-static-case-sched-common-compile:
	@echo "  [test-static] CASE=sched-common-compile: aarch64 common scheduler TU compile check"
	$(call os01_submake,kernel,sched-compile-check ARCH=aarch64 $(OS01_SUBMAKE_ARGS))

# mm-header-boundary (mm-architecture-boundaries Task 2): runs on BOTH
# profiles.  The controlled sub-make passes ARCH explicitly (the root
# make does not define ARCH); the x86 profile additionally needs the
# sysroot published (the target libc stdint.h is force-included).
.PHONY: _test-static-case-mm-header-boundary
_test-static-case-mm-header-boundary: $(if $(filter x86_64-clang,$(PROFILE)),$(SYSROOT_STAMP))
	@echo "  [test-static] CASE=mm-header-boundary: public memory header boundary ($(PROFILE))"
	$(call os01_submake,kernel,mm-header-boundary ARCH=$(if $(filter aarch64-clang,$(PROFILE)),aarch64,x86_64) $(OS01_SUBMAKE_ARGS))

test-static: _test-static-case-$(CASE)
	@case "$(CASE)" in \
	  sched-common-compile|mm-header-boundary) echo "  [test-static] CASE=$(CASE) done" ;; \
	  *) echo "ERROR: unknown CASE='$(CASE)'; valid: $(STATIC_CASES_VALID)" >&2; exit 1 ;; \
	esac

endif # CASE

# ── test-gui-incremental: the GUI library's incremental contract ──
# The INCREMENTAL audit modes never spawn make: this recipe owns every
# build step; the audit only records/compares snapshots.  Phases:
# noop (rebuild without recompile) / touch (public-header edits rebuild
# dependents) / install (staging orphans cleared) / publish (new
# generation clean, old preserved) / ps2rekey (phase 5 below).
# Deliberately NOT in test-static: publish allocates a new sysroot
# generation and would contend with the publisher's lock.
GUI_INCREMENTAL_AUDIT = python3 qemutests/gui_build_audit.py \
	--build-dir "$(BUILD_DIR)" --repo-root "$(OS01_ROOT)" --profile "$(PROFILE)"

define _touch_check_hdr
	@echo "    [incr-touch] testing $(1) (consumer: $(2))"
	@stat -c %y libgui/include/gui/$(1) > $(BUILD_DIR)/incr-header-time
	$(GUI_INCREMENTAL_AUDIT) --header $(1) --incremental-snapshot "$(BUILD_DIR)/incr-touch.json" --incremental-phase touch --libgui-compile "$(BUILD_DIR)/incr-libgui-compile.txt"
	@touch libgui/include/gui/$(1)
	$(call os01_submake,libgui,all)
	$(MAKE) --no-print-directory test-gui-host CASE=$(2)
	@touch -d "$$(cat $(BUILD_DIR)/incr-header-time)" libgui/include/gui/$(1)
	$(GUI_INCREMENTAL_AUDIT) --header $(1) --incremental-compare "$(BUILD_DIR)/incr-touch.json" --incremental-phase touch --libgui-compile "$(BUILD_DIR)/incr-libgui-compile.txt"
endef

.PHONY: test-gui-incremental
# CI runs this on a fresh workspace BEFORE "Build disk.img", so the sysroot
# (staging headers + libc.a the libgui compile/link consumes) must be built
# here rather than inherited from an earlier step.
test-gui-incremental: $(SYSROOT_STAMP)
	$(call require_capability,rootfs)
	@echo "  [test-gui-incremental] phase 1/4: a repeated build must not recompile"
	$(call os01_submake,libgui,all)
	$(GUI_INCREMENTAL_AUDIT) --incremental-snapshot "$(BUILD_DIR)/incr-noop.json" --incremental-phase noop
	$(call os01_submake,libgui,all)
	$(GUI_INCREMENTAL_AUDIT) --incremental-compare "$(BUILD_DIR)/incr-noop.json" --incremental-phase noop
	@echo "  [test-gui-incremental] phase 2/4: touching public headers must rebuild their dependents"
	@$(call os01_submake,libgui,--no-print-directory print-compile) > $(BUILD_DIR)/incr-libgui-compile.txt
	$(call _touch_check_hdr,gui_input.h,input-decode)
	$(call _touch_check_hdr,gui_widget.h,widget)
	$(call _touch_check_hdr,gui_layout.h,widget-layout)
	$(call _touch_check_hdr,gui_event.h,widget-event)
	$(call _touch_check_hdr,gui_focus.h,focus)
	@echo "  [test-gui-incremental] phase 3/4: install must clear orphaned staging files"
	@mkdir -p $(STAGING_DIR)/gui/usr/include/gui
	@printf '/* orphaned staging file */\n' > $(STAGING_DIR)/gui/usr/include/gui/gui_input_private.h
	@printf '/* orphaned staging file */\n' > $(STAGING_DIR)/gui/usr/include/gui/gui_widget_private.h
	$(GUI_INCREMENTAL_AUDIT) --incremental-snapshot "$(BUILD_DIR)/incr-install.json" --incremental-phase install
	$(call os01_submake,libgui,install INSTALL_ROOT=$(STAGING_DIR)/gui)
	$(GUI_INCREMENTAL_AUDIT) --incremental-compare "$(BUILD_DIR)/incr-install.json" --incremental-phase install
	@echo "  [test-gui-incremental] phase 4/4: a new generation must be clean, the old ones preserved"
	$(GUI_INCREMENTAL_AUDIT) --incremental-snapshot "$(BUILD_DIR)/incr-publish.json" --incremental-phase publish
	@rm -f $(BUILD_DIR)/stamps/sysroot.stamp
	$(MAKE) --no-print-directory sysroot
	$(GUI_INCREMENTAL_AUDIT) --incremental-compare "$(BUILD_DIR)/incr-publish.json" --incremental-phase publish
	# Phase 5 edits the PUBLISHED decoder header, republishes and checks
	# that the new generation carries the edit.  Repeatability contract:
	# (1) the probe edit carries a per-run nonce — a constant marker
	# collides with earlier generations' content from the second run on;
	# (2) the EXIT trap restores the source header AND republishes the
	# sysroot, then verifies source/staging/current-sysroot agree — a
	# mismatch fails the phase and prints the republish log (the probe
	# content must never survive in the active build state; historical
	# generations keep it as immutable evidence); (3) SERIAL only — the
	# phase mutates the shared source header via a fixed backup path, and
	# a missing /proc/sys/kernel/random/uuid fails explicitly.
	@echo "  [test-gui-incremental] phase 5/5: modifying the published PS2 header must rekey the generation and rebuild the consumers"
	@+$(SHELL) -ec '\
	  hdr=libgui/include/gui/gui_input_ps2.h; \
	  bak=$(BUILD_DIR)/incr-ps2-header.bak; \
	  cp "$$hdr" "$$bak"; \
	  restore() { main=$$?; st=$$main; \
	    cp "$$bak" "$$hdr" || st=1; \
	    rm -f $(BUILD_DIR)/stamps/sysroot.stamp || st=1; \
	    if $(MAKE) --no-print-directory sysroot >$(BUILD_DIR)/incr-ps2-republish.log 2>&1; then repub=0; else repub=1; st=1; fi; \
	    src=$$(sha256sum "$$hdr" 2>/dev/null | cut -d" " -f1); \
	    stg=$$(sha256sum $(STAGING_DIR)/gui/usr/include/gui/gui_input_ps2.h 2>/dev/null | cut -d" " -f1); \
	    pub=$$(sha256sum $(SYSROOT)/usr/include/gui/gui_input_ps2.h 2>/dev/null | cut -d" " -f1); \
	    if [ -z "$$src" ] || [ "$$src" != "$$stg" ] || [ "$$src" != "$$pub" ]; then st=1; fi; \
	    if [ $$st -ne 0 ]; then \
	      echo "  [test-gui-incremental] ERROR: phase 5 did not complete cleanly (main=$$main republish=$$repub); see $(BUILD_DIR)/incr-ps2-republish.log" >&2; \
	      echo "  [test-gui-incremental] source=$$src staging=$$stg published=$$pub" >&2; \
	    fi; \
	    exit $$st; }; \
	  trap restore EXIT; \
	  $(GUI_INCREMENTAL_AUDIT) --header gui_input_ps2.h --incremental-snapshot "$(BUILD_DIR)/incr-ps2rekey.json" --incremental-phase ps2rekey; \
	  nonce=$$(cat /proc/sys/kernel/random/uuid 2>/dev/null || true); \
	  if [ -z "$$nonce" ]; then \
	    echo "  [test-gui-incremental] ERROR: no unique nonce source (/proc/sys/kernel/random/uuid unavailable)" >&2; \
	    exit 1; \
	  fi; \
	  printf "\n/* D02 incremental probe: temporary marker %s */\n" "$$nonce" >> "$$hdr"; \
	  $(call os01_submake_cmd,libgui,all); \
	  $(MAKE) --no-print-directory test-gui-host CASE=input-decode; \
	  rm -f $(BUILD_DIR)/stamps/sysroot.stamp; \
	  $(MAKE) --no-print-directory sysroot; \
	  $(GUI_INCREMENTAL_AUDIT) --header gui_input_ps2.h --incremental-compare "$(BUILD_DIR)/incr-ps2rekey.json" --incremental-phase ps2rekey'
	@echo "  [test-gui-incremental] all five incremental assertions PASS"

# ── test-runtime: original recipe (lines 364-385 of run.mk) ──
test-runtime: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(KERNEL_ARTIFACT))
	$(call require_capability,rootfs)
	$(RUN_STATIC_AUDIT) runtime-audit -- \
	  python3 qemutests/runtime_audit.py \
	  --stage1 "$(KERNEL_BUILD_DIR)/kernel.elf.stage1" \
	  --final "$(KERNEL_ELF)" \
	  --link-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --runtime-input "$(KERNEL_RUNTIME_INPUTS)" \
	  --llvm-nm "$(LLVM_NM)" \
	  --llvm-readobj "$(LLVM_READOBJ)"
	$(RUN_STATIC_AUDIT) stack-canary-audit -- \
	  python3 qemutests/stack_canary_audit.py \
	  --object "$(KERNEL_BUILD_DIR)/sched/core.o" \
	  --elf "$(KERNEL_ELF)" \
	  --llvm-readelf "$(LLVM_READELF)" \
	  --llvm-objdump "$(LLVM_OBJDUMP)"
	$(RUN_STATIC_AUDIT_VALIDATE) validate-kernel -- \
	  $(MAKE) --no-print-directory validate-kernel
	$(RUN_STATIC_AUDIT) runtime-link-order -- \
	  python3 qemutests/runtime_link_order_test.py
	$(RUN_STATIC_AUDIT) kernel-runtime-link -- \
	  python3 qemutests/kernel_runtime_link_test.py \
	  --source-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --sysroot "$(SYSROOT)" \
	  --profile-file "$(OS01_PROFILE_FILE)" \
	  --profile "$(PROFILE)"

# ── test-kernel-layout: original recipe (line 485-488) ──────
test-kernel-layout: kernel.bin
	$(RUN_STATIC_AUDIT) kernel-layout -- \
	  python3 qemutests/x86_64_kernel_layout_test.py "$(KERNEL_BUILD_DIR)/kernel.elf" \
	  --llvm-nm "$(LLVM_NM)" --llvm-readelf "$(LLVM_READELF)"

# ── test-kernel-canary-contract: original recipe (line 490-492) ─
test-kernel-canary-contract:
	$(RUN_STATIC_AUDIT) kernel-canary-contract -- \
	  python3 qemutests/kernel_canary_contract_test.py

# ── test-user-canary: original recipe (lines 393-431) ───────
test-user-canary: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf $(ROOTFS_MANIFEST))
	$(call require_capability,rootfs)
	@$(RUN_STATIC_AUDIT) user-canary -- \
	  env LLVM_NM="$(LLVM_NM)" SYSROOT="$(SYSROOT)" \
	      USER_ARTIFACT_DIR="$(USER_ARTIFACT_DIR)" ROOTFS_MANIFEST="$(ROOTFS_MANIFEST)" \
	    bash $(OS01_ROOT)/scripts/user_canary_audit.sh

# This target builds and boots only the selftest-scoped image.  In particular
# it never uses the ordinary image, and it refuses a combined syscall/selftest
# request because those suites are intentionally run independently.
#
# The QEMU process boundary, the semantic gate (check_kernel_selftest.failures)
# and the run evidence now live in qemutests/run_kernel_selftest.py (Task 6).
# The runner receives explicit values — it does NOT infer profile paths — and
# stops QEMU controlled once the boot summary and scheduled task markers are
# complete, so the historical external `timeout 75` wrapper is gone.
.PHONY: test-kernel-selftest
test-kernel-selftest: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@if [ "$(OS01_SYSTEST)" = "1" ]; then \
	  echo "ERROR: test-kernel-selftest must not be combined with OS01_SYSTEST=1; run make OS01_SYSTEST=1 test-qemu SUITE=systest separately" >&2; \
	  exit 1; \
	fi
	$(MAKE) KERNEL_SELFTEST=1 image
	python3 qemutests/run_kernel_selftest.py \
	  --firmware "$(OVMF_FIRMWARE)" \
	  --image "$(TEST_SELFTEST_IMAGE)" \
	  --qemu "$(QEMU_BIN)" \
	  --cpu "$(KERNEL_SELFTEST_SMP)" \
	  --memory "$(MEMORY)" \
	  --timeout 75 \
	  --build-dir "$(abspath $(BUILD_DIR))" \
	  --profile "$(PROFILE)"

# ── Run paths ────────────────────────────────────────────────
# Prints the current profile's absolute firmware and active image path so a
# manual QEMU invocation can reuse exactly the selected variant. Gated on
# rootfs like every other x86 run entry point.
.PHONY: print-run-paths
print-run-paths:
	$(call require_capability,rootfs)
	@echo firmware=$(abspath $(OVMF_FIRMWARE))
	@echo image=$(abspath $(DISK_IMG))

.PHONY: print-user-paths
print-user-paths:
	@echo user_build_dir=$(abspath $(USER_BUILD_DIR))
	@echo user_artifact_dir=$(abspath $(USER_ARTIFACT_DIR))

# ── Help ─────────────────────────────────────────────────────
# Lists the root Makefile's user-facing targets, grouped by category, with the
# capability each one needs under the active profile. Works for any profile
# (no capability gate): the categories shown are the same, and the per-target
# capability badge is evaluated against the active profile's
# PROFILE_CAPABILITIES, so a user on aarch64-clang sees (uefi) badges
# on aarch64 targets and "n/a (active profile lacks <cap>)" on x86-only ones
# instead of a hard error. AGENTS.md Quick start and the docs are the canonical
# recipes; this is the discoverability surface for `make` itself.
.PHONY: help
# help is table-driven: one HELP_SEC_* variable per section, each entry a
# pre-quoted 'target' 'capability' 'purpose' triple (printf repeats the row
# format per triple). Keep make help in sync with new targets.
HELP_SEC_BUILD := \
	'disk.img' '(rootfs)' 'Build the normal disk image (default goal)' \
	'kernel.bin' '(rootfs)' 'Project-root kernel copy (cmp-guarded)' \
	'lib' '(userland)' 'Build the sysroot (libc, mbedtls, etc.)' \
	'user' '(userland)' 'Build userland ELFs (incl. busybox.elf)' \
	'image' '(rootfs)' 'Active profile disk image (variant-resolved)' \
	'sysroot' '(userland)' 'Project-root sysroot/ symlink for clangd (editor)'
HELP_SEC_RUN := \
	'run' '(rootfs)' 'QEMU q35 + e1000 + serial stdio (BOOT_METHOD=hdd|usb, default hdd)' \
	'run-kvm' '(rootfs)' 'QEMU with KVM acceleration' \
	'run-virtio' '(rootfs)' 'QEMU with virtio-net-pci (instead of e1000)' \
	'run-usb' '(rootfs)' 'QEMU USB-only topology (usb-storage on qemu-xhci; alias for run BOOT_METHOD=usb)' \
	'debug' '(rootfs)' 'QEMU paused, GDB :1234 (-S -s)'
HELP_SEC_AARCH64 := \
	'aarch64-uefi' '(uefi)' 'Build aarch64 disk + firmware' \
	'aarch64-uefi-kernel' '(uefi)' 'Build aarch64 kernel.elf only' \
	'run-aarch64-uefi' '(uefi)' 'QEMU virt + cortex-a53 + virtio-blk (default SMP=1 = the single-core product userspace run; explicit SMP=N wins, multi-core boots come up kernel-only; AARCH64_UEFI_SMP_DIAGNOSTIC_DTB=0 to skip auto DTB)'
HELP_SEC_VALIDATE := \
	'validate' '(rootfs)' 'kernel ELF + UEFI COFF + profile info' \
	'validate-kernel' '(rootfs)' 'kernel ELF sanity (no undef, EM_X86_64, exports)' \
	'validate-uefi' '(rootfs)' 'EFI app COFF exports parseable' \
	'validate-profile' '(always)' 'Print profile / triple / sysroot / capabilities' \
	'print-run-paths' '(rootfs)' 'Print absolute firmware + active image path'
HELP_SEC_TEST := \
	'test' '(rootfs)' 'Quick local checks: host tests, catalog drift, framework contract, and assertion policy (no QEMU or guest image)' \
	'test-qemu' '(rootfs)' 'QEMU E2E suite (SUITE=<phase-0|systest|inittab-phase|network|gfx|resolution|driver-model|monitor|terminal-present|gui|gui-desktop|intr-x86|arch10-capacity|arch6-fsstress>, CASE=<id> selects one systest case)' \
	'test-qemu-usb-boot' '(rootfs)' 'M0 USB-only firmware gate (USB_BOOT_CASE=firmware; expects fs/root rc=-19)' \
	'test-host' '(rootfs)' 'os01_submake hosttests + pmm_boot_reservation_test.py' \
	'test-static' '(rootfs)' 'All static audits (runtime, stack-canary, validate-kernel, runtime-link-order, kernel-runtime-link, syscall-boundary, kernel-layout, kernel-canary-contract, header-object, stack-frame, driver-model-boundary, gui-build, intr-dispatch, arch10-capacity, user-canary)' \
	'test-gui-incremental' '(rootfs)' 'GUI library incremental contract (no-op rebuild, header-touch rebuild, staging orphan, generation publish)' \
	'test-kernel-selftest' '(rootfs)' 'QEMU built-in selftests (isolated selftest image, KERNEL_SELFTEST=1)' \
	'test-aarch64' '(uefi)' 'aarch64 UEFI test (MODE=<smp|no-ack|gic-spi|sync-fault|m3-probe|sched|sched-ap|sched-wake|m1-*>)' \
	'test-contract' '(rootfs|uefi)' 'Full build contract (PROFILE=<x86_64-clang|aarch64-clang>)'
HELP_SEC_FRAMEWORK := \
	'test-harness' '(always)' 'Python unittest framework regression (TEST_HARNESS_MODULES list — append-only across tasks; never discovers QEMU scripts); also runs test-framework-contract as a prereq' \
	'test-framework-contract' '(always)' 'Hostside negative contract: builds test_framework_contract.elf and asserts exit code 1 (assertion exit-code propagation / single-evaluation / TEST_RESULTS snapshot)' \
	'test-flake-report' '(always)' 'Flake ledger report from the in-repo ledger (config/flake-ledger.json) + local build/*/logs/tests/**/result.json (or FROM_CI=N via gh); MERGE=<fragment> merges a CI fragment, UPDATE_LEDGER=1 writes the ledger back; always exits 0' \
	'test-doctor' '(always)' 'Compare local clang/ld.lld/qemu versions against ci/Dockerfile pins (exits 2 only if a tool cannot execute)' \
	'ci-local' '(always)' 'Run test suite inside ghcr.io/aagu/os01-ci:latest via docker/podman (SUITE=<name> or MAKE_ARGS=...)'
HELP_SEC_STANDALONE := \
	'test-syscall' '(rootfs)' 'RETAINED ALIAS — see docs/build/build.md section 4 (AGENTS.md exact-target requirement; use OS01_SYSTEST=1 test-qemu SUITE=systest in new code)' \
	'test-syscall-repeat' '(rootfs)' 'QEMU exec/exit stability through normal terminal (x86_64_systest_repeat.py)' \
	'test-user-canary' '(rootfs)' '6-step SSP / user-stack canary audit (spec 2026-09-17)' \
	'test-pmm-boot-reservation' '(rootfs)' 'PMM boot-time memory reservation guard (host-only)'
HELP_SEC_FOCUSED := \
	'test-kernel-layout' '(rootfs)' 'x86_64 kernel.elf layout audit (post-_end reserved)' \
	'test-kernel-canary-contract' '(rootfs)' 'Kernel canary compile-flag contract' \
	'test-resolution-host' '(rootfs)' 'Resolution switcher host tests (RES_CASE=<uapi|...|all>)'
HELP_SEC_MAINT := \
	'clean' '(always)' 'Remove build/<profile> + root compat copies (lock-checked)' \
	'unlock-profile' '(always)' 'Diagnose stale publish lock (FORCE_UNLOCK=1 to remove)'

help:
	@echo 'OS01 build — profile=$(PROFILE) capabilities=$(PROFILE_CAPABILITIES)'
	@printf '  %-22s %-13s %s\n' 'TARGET' 'CAPABILITY' 'PURPOSE'
	@echo '  --------------------- ------------- ------------------------------'
	@echo ''
	@echo 'Build / aliases:'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_BUILD)
	@echo ''
	@echo 'Run / Debug (x86, rootfs):'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_RUN)
	@echo ''
	@echo 'aarch64 UEFI bring-up (uefi):'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_AARCH64)
	@echo ''
	@echo 'Validation (x86 rootfs):'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_VALIDATE)
	@echo ''
	@echo 'Test (6 canonical buckets; varied capability):'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_TEST)
	@echo '  Phase-0 and syscall E2E can reuse one prepared image with QEMU_SMP_MATRIX="1 2".'
	@echo ''
	@echo 'Framework regression (no QEMU; always):'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_FRAMEWORK)
	@echo ''
	@echo 'Standalone test targets (distinct harness / image variant):'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_STANDALONE)
	@echo ''
	@echo 'Focused compatibility checks (retained; see docs/build/build.md alias policy):'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_FOCUSED)
	@echo ''
	@echo 'Maintenance:'
	@printf '  %-22s %-13s %s\n' $(HELP_SEC_MAINT)
	@echo ''
	@echo 'Common flags: PROFILE=<name>, DEBUG_CHANNELS=<a,b>, OS01_SYSTEST=1,'
	@echo '              OS01_NETTEST=1, INITTAB_FILE=<path>, KERNEL_SELFTEST=1,'
	@echo '              NDEBUG=1, LOG_TARGET=serial|both.'
	@echo 'See AGENTS.md Quick start and docs/build/build.md for recipes.'
	@echo 'See docs/build/build.md alias policy for the retained focused checks (test-runtime, test-kernel-layout, test-kernel-canary-contract, test-user-canary, test-pmm-boot-reservation).'
# ── Image alias ─────────────────────────────────────────────
# `make image` builds the current profile's disk image — variant-resolved
# (OS01_SYSTEST=1 / OS01_NETTEST=1 / INITTAB_FILE=config/inittab.test select
# the isolated variant image; otherwise the normal image).
.PHONY: image
image: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(DISK_IMG))
	$(call require_capability,rootfs)

# ── Build contract checks ───────────────────────────────────
.PHONY: test-contract
# CI's contract job runs against a clean workspace; the bucket must still
# pre-build the artifacts it inspects. x86 contract needs disk.img;
# aarch64 contract needs aarch64-uefi. (Both were dropped in the v1
# plan; this restored version matches the original line 610 / 631.)
X86_CONTRACT_MODES := legacy-components legacy x86 sysroot firmware targets sysroot-headers flags-cache host-test
AARCH64_CONTRACT_MODES := aarch64 targets

# Pre-build helpers are split per PROFILE so the `+env` recipe prefix
# sits at Make's recipe-line position (not inside a shell `case` branch
# where `+env` would become a command name and fail with "+env: not
# found"). The cross-profile sub-make also gets `$(MAKE)` on its own
# recipe line so `-n` honors the standard recursive-make contract.
.PHONY: _test-contract-prep-x86 _test-contract-prep-aarch64
_test-contract-prep-x86:
	@echo "  [test-contract] x86: build hosttests via sub-make"
	$(call os01_submake,hosttests,all)
	@echo "  [test-contract] x86: pre-build aarch64-uefi in its own workspace"
	+env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	  $(MAKE) MAKEOVERRIDES= PROFILE=aarch64-clang aarch64-uefi
_test-contract-prep-aarch64:
	@true

# test-contract: the umbrella. Each PROFILE gets its own prereq and
# capability gate. The pre-build dispatch and the per-mode shell loop
# are on separate recipe lines.
test-contract: PROFILE ?= $(DEFAULT_PROFILE)
test-contract: PROFILE := $(PROFILE)
test-contract: $(if $(filter x86_64-clang,$(PROFILE)),disk.img,aarch64-uefi)
	$(call require_capability,$(if $(filter x86_64-clang,$(PROFILE)),rootfs,uefi))
	$(MAKE) --no-print-directory _test-contract-prep-$(if $(filter x86_64-clang,$(PROFILE)),x86,aarch64)
	# arch9-build-contract runs FIRST: it dry-runs ``make -n
	# DRIVER_MODEL_FAULT=<fault> kernel.bin``, whose ``$(KERNEL_ARTIFACT)``
	# recipe carries an ``@+$(SHELL) -ec '...'`` line that forces
	# execution under ``-n`` (so it sees the real publish-lock /
	# sysroot-symlink state). The ``host-test`` mode at the END of
	# ``X86_CONTRACT_MODES`` does ``make clean`` (deletes the sysroot
	# symlink); running arch9 BEFORE the modes loop sees a still-
	# intact build tree. (Earlier ordering masked this: the
	# ``targets`` mode used to fail first with ``OVMF_FIRMWARE must
	# name a readable firmware file`` and short-circuited the loop
	# before ``host-test`` could run.)
	set -e; \
	if [ "$(PROFILE)" = "x86_64-clang" ]; then \
	  echo "  [test-contract] $(PROFILE)/arch9-build-contract"; \
	  PROFILE=$(PROFILE) python3 -m unittest qemutests.test_arch9_build_contract; \
	  echo "  [test-contract] $(PROFILE)/build-incremental"; \
	  PROFILE=$(PROFILE) python3 -m unittest qemutests.test_build_incremental; \
	  echo "  [test-contract] $(PROFILE)/dry-run-contract"; \
	  PROFILE=$(PROFILE) python3 -m unittest qemutests.test_dry_run_contract; \
	fi; \
	case "$(PROFILE)" in \
	  x86_64-clang)    modes="$(X86_CONTRACT_MODES)";; \
	  aarch64-clang)   modes="$(AARCH64_CONTRACT_MODES)";; \
	  *) echo "PROFILE must be x86_64-clang or aarch64-clang, got '$(PROFILE)'" >&2; exit 1;; \
	esac; \
	for m in $$modes; do \
	  echo "  [test-contract] $(PROFILE)/$$m"; \
	  sh qemutests/build_contract.sh $(PROFILE) $$m; \
	done

# ── Clean ───────────────────────────────────────────────────
# Only the default profile owns the project-root kernel.bin / disk.img compat
# copies; other profiles remove just build/<profile>. The obsolete source-tree
# boot/uefi/OVMF.fd is removed at ROOT level (never by recursing into a
# component) and only when it passes both guards: git-ignored AND untracked.
# A tracked or user-owned file is preserved with an error.
CLEAN_COMPAT := $(if $(filter $(DEFAULT_PROFILE),$(PROFILE)),rm -f disk.img kernel.bin;)

# clean takes the publish lock (60 s retry), fails without deleting anything
# if a generation read lease exists, then removes build/<profile> (NOT the
# whole build/ tree — build/.locks/ and other profiles survive) plus, for the
# default profile, the project-root compat artifacts. Component Makefiles are
# never recursed into: they are profile-only now, and clean already removes
# every component's profile output wholesale. All of it runs in ONE shell line
# so the trap releases the lock in every path (success or failure), and the
# lock is held for the whole clean.
.PHONY: clean unlock-profile
clean:
	@if [ -n "$(DRY_RUN)" ]; then \
	  echo "  [clean] dry-run: not deleting the $(PROFILE) build"; \
	  exit 0; \
	fi; \
	set -e; \
	$(call os01_acquire_publish_lock) \
	if [ -d "$(LEASES_DIR)" ] && [ -n "$$(ls -A "$(LEASES_DIR)" 2>/dev/null)" ]; then \
	  echo "ERROR: cannot clean while sysroot generations are leased:"; \
	  ls -A "$(LEASES_DIR)"; \
	  echo "lock owner: $$(cat "$(LOCK_DIR)/owner")"; \
	  exit 1; \
	fi; \
	rm -rf build/$(PROFILE); \
	$(CLEAN_COMPAT) \
	if [ -f boot/uefi/OVMF.fd ]; then \
	  if git check-ignore -q boot/uefi/OVMF.fd && ! git ls-files --error-unmatch boot/uefi/OVMF.fd >/dev/null 2>&1; then \
	    rm -f boot/uefi/OVMF.fd; \
	  else \
	    echo "ERROR: refusing to remove boot/uefi/OVMF.fd (not a gitignored, untracked generated artifact); preserving it" >&2; \
	  fi; \
	fi; \
	rm -rf sysroot

# Diagnose and remove a stale publish lock. Prints the owner data and only
# removes the lock when FORCE_UNLOCK=1 is set.
unlock-profile:
	@if [ -d "$(LOCK_DIR)" ]; then \
	  echo "publish lock $(LOCK_DIR):"; \
	  cat "$(LOCK_DIR)/owner" 2>/dev/null || true; \
	  if [ "$$FORCE_UNLOCK" = "1" ]; then \
	    rm -rf "$(LOCK_DIR)"; \
	    echo "lock removed"; \
	  else \
	    echo "set FORCE_UNLOCK=1 to remove the stale lock"; \
	    exit 1; \
	  fi; \
	else \
	  echo "no lock held at $(LOCK_DIR)"; \
	fi

# Driver model matrix fault-fixture cleanup.
# Removes ONLY the derived kernel/image artifacts for the given fault
# (kernel/driver-model-<fault>/ + artifacts/kernel/driver-model-<fault>/
# + image/driver-model-<fault>/); never touches libc, user, firmware,
# or normal artifacts.  Used between fault runs so the matrix harness
# can switch faults without `make clean` of the whole profile.
.PHONY: clean-driver-model-fixture
# Parse-time validation: DRIVER_MODEL_FAULT must be a known non-none slug.
# project.mk's enum gate covers all values; this target refuses empty,
# "none", or any non-driver-model variant (defence in depth).
ifneq ($(filter $(DRIVER_MODEL_FAULT),none),)
clean-driver-model-fixture:
	@echo "ERROR: clean-driver-model-fixture requires DRIVER_MODEL_FAULT=<non-none slug>" >&2; \
	exit 1
else ifeq ($(filter $(DRIVER_MODEL_FAULT),observe bad-nic-bar adapter-fail ahci-empty irq-conflict),)
clean-driver-model-fixture:
	@echo "ERROR: DRIVER_MODEL_FAULT='$(DRIVER_MODEL_FAULT)' is not a known driver-model fault slug" >&2; \
	exit 1
else
clean-driver-model-fixture:
	@echo "  [clean-driver-model-fixture] DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT)"
	rm -rf "$(BUILD_DIR)/kernel/driver-model-$(DRIVER_MODEL_FAULT)"
	rm -rf "$(BUILD_DIR)/artifacts/kernel/driver-model-$(DRIVER_MODEL_FAULT)"
	rm -rf "$(BUILD_DIR)/image/driver-model-$(DRIVER_MODEL_FAULT)"
endif

# M1 uses distinct immutable build/image variants; outer flags never choose
# the matrix's normal/selftest artifact implicitly.
.PHONY: _test-aarch64-prep-m1-ram _test-aarch64-run-m1-ram
_test-aarch64-prep-m1-ram:
	$(MAKE) KERNEL_SELFTEST= AARCH64_M1_TEST= AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_M1_TEST= AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
_test-aarch64-run-m1-ram:
	python3 qemutests/aarch64_m1_ap_tlbi.py --self-test \
	  --llvm-objdump "$(or $(LLVM_OBJDUMP),llvm-objdump)" \
	  --elf "$(BUILD_DIR)/kernel/kernel.elf" --elf "$(BUILD_DIR)/kernel/selftest/kernel.elf"
	python3 qemutests/aarch64_m1_matrix.py \
	  --normal-image "$(BUILD_DIR)/image/aarch64-uefi.img" \
	  --selftest-image "$(call aarch64_variant_disk,selftest)" \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/m1-ram"

# M1 fault cases share one prep/run pair shape: prep drives the
# controlled sub-make with KERNEL_SELFTEST=1 + AARCH64_M1_TEST=<variant>;
# run consumes the m1-<variant> disk through aarch64_m1_matrix.py.
# m1-ram above is deliberately explicit (it needs the normal AND the
# selftest image).
AARCH64_M1_FAULT_CASES := m1-sparse:sparse m1-arena-exhaust:arena-exhaust m1-table-exhaust:table-exhaust m1-ap-bad-root:ap-bad-root
define _aarch64_m1_pair
.PHONY: _test-aarch64-prep-$(1) _test-aarch64-run-$(1)
_test-aarch64-prep-$(1):
	$$(MAKE) KERNEL_SELFTEST=1 AARCH64_M1_TEST=$(2) AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
_test-aarch64-run-$(1):
	python3 qemutests/aarch64_m1_matrix.py --variant $(2) \
	  --image "$$(call aarch64_variant_disk,$(1))" \
	  --firmware "$$(AARCH64_UEFI_FIRMWARE)" --qemu "$$(AARCH64_QEMU)" \
	  --build-dir "$$(BUILD_DIR)" --profile "$$(PROFILE)" \
	  --log-dir "$$(OS01_ROOT)/test-results/$(1)"
endef
$(foreach p,$(AARCH64_M1_FAULT_CASES),$(eval $(call _aarch64_m1_pair,$(word 1,$(subst :, ,$(p))),$(word 2,$(subst :, ,$(p))))))

.PHONY: test-intr-irq-x86
test-intr-irq-x86:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-intr-irq-x86 $(OS01_SUBMAKE_ARGS))

.PHONY: test-irq-capacity
test-irq-capacity:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-irq-capacity $(OS01_SUBMAKE_ARGS))

.PHONY: test-usb-hid-report
test-usb-hid-report:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-usb-hid-report $(OS01_SUBMAKE_ARGS))

.PHONY: test-xhci-hid-desc
test-xhci-hid-desc:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-xhci-hid-desc $(OS01_SUBMAKE_ARGS))

.PHONY: test-xhci-intr-in
test-xhci-intr-in:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-xhci-intr-in $(OS01_SUBMAKE_ARGS))

.PHONY: test-xhci-hid-service
test-xhci-hid-service:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-xhci-hid-service $(OS01_SUBMAKE_ARGS))

# ── libc completion focused host tests (T1) ──────────────────────
# make PROFILE=x86_64-clang test-libc-host LIBC_CASE=<case>
#
# LIBC_HOST_MAPPING is the delivered case set; LIBC_HOST_CASES is the full
# registered set (a value outside it is invalid and fails non-zero at
# parse time, before any submake).  A registered-but-undelivered case
# falls to the catch-all — it must never be reported as a pass.  Each
# completion task appends its mapping entry; `all` (default) runs the
# delivered groups.
LIBC_HOST_MAPPING := api:test-libc-api alloc:test-libc-alloc env:test-libc-environ shutdown:test-libc-shutdown stream:test-libc-stream-state append:test-vfs-append io:test-libc-stdio-io line:test-libc-line format:test-libc-stdio-format integer:test-libc-integer float:test-libc-float-parse scan:test-libc-scan time:test-libc-time dir:test-libc-dir path:test-libc-path
LIBC_HOST_DELIVERED_CASES := $(foreach kv,$(LIBC_HOST_MAPPING),$(word 1,$(subst :, ,$(kv))))
LIBC_HOST_CASES := $(LIBC_HOST_DELIVERED_CASES) regex

$(foreach kv,$(LIBC_HOST_MAPPING),$(eval $(call _host_case_rule,test-libc-host,$(word 1,$(subst :, ,$(kv))),test-libc-host,$(word 2,$(subst :, ,$(kv))))))
.PHONY: test-libc-host _test-libc-host-run-all _test-libc-host-run-%
_test-libc-host-run-all: $(foreach c,$(LIBC_HOST_DELIVERED_CASES),_test-libc-host-run-$(c))
	@echo "  [test-libc-host] all delivered groups: $(LIBC_HOST_DELIVERED_CASES)"
_test-libc-host-run-%:
	@echo "ERROR: LIBC_CASE='$*' is registered but not delivered in this stage." >&2
	@echo "       Delivered groups: $(LIBC_HOST_DELIVERED_CASES)" >&2
	@exit 1
LIBC_CASE_RAW := $(strip $(LIBC_CASE))
LIBC_CASE_EFFECTIVE := $(if $(LIBC_CASE_RAW),$(LIBC_CASE_RAW),all)
LIBC_CASE_VALID := $(if $(filter 1,$(words $(LIBC_CASE_EFFECTIVE))),$(filter all $(LIBC_HOST_CASES),$(LIBC_CASE_EFFECTIVE)))
test-libc-host: $(if $(and $(filter rootfs,$(PROFILE_CAPABILITIES)),$(LIBC_CASE_VALID)),_test-libc-host-run-$(LIBC_CASE_EFFECTIVE))
	$(call require_capability,rootfs)
	$(if $(LIBC_CASE_VALID),,$(error unknown LIBC_CASE='$(LIBC_CASE_RAW)'; valid: $(LIBC_HOST_CASES), all))
	@echo "  [test-libc-host] LIBC_CASE=$(LIBC_CASE_EFFECTIVE)"

# RED builds of the libc completion groups (T14/T15 evidence): expected
# to fail — that failure is the transcript.  RED-1 compiles the group
# against the pre-task source; RED-2 reproduces the pre-task link hole
# through the shared scripts/libc_red_link.sh pipeline (delete the task's
# archive members, CONTROL link against the pristine archive, RED link
# against the hole).  Full narrative:
# docs/superpowers/plans/2026-11-08-libc-completion.md.
.PHONY: test-libc-path-red
test-libc-path-red:
	@echo "=== RED-1 (behaviour): realpath was a stub that answered NULL ==="
	@$(call os01_submake_cmd,hosttests,test-libc-path-red $(OS01_SUBMAKE_ARGS)); rc_run=$$?; \
	  echo "RED-1 exit code: $$rc_run"; \
	  echo "=== RED-2 (link): execl/execlp/execle had NO definition before T14 ==="; \
	  env SYSROOT="$(SYSROOT)" OS01_ROOT="$(OS01_ROOT)" \
	      LLVM_AR="$(LLVM_AR)" LLVM_NM="$(LLVM_NM)" \
	      EFFECTIVE_CC="$(EFFECTIVE_CC)" TARGET_LD="$(TARGET_LD)" \
	      USER_BUILD_DIR="$(USER_BUILD_DIR)" \
	    bash $(OS01_ROOT)/scripts/libc_red_link.sh \
	      "$$rc_run" b7c33ba6 libc/unistd execl.c \
	      "execl.o" 'execl|execlp|execle' \
	      '#include <unistd.h>' \
	      'int main(void) { execl("/bin/sh", "sh", (char *)0); return 0; }'

.PHONY: test-libc-dir-red
test-libc-dir-red:
	@echo "=== RED-1 (behaviour): the pre-T15 dirent.c has no type check, no errno and no d_off ==="
	@$(call os01_submake_cmd,hosttests,test-libc-dir-red $(OS01_SUBMAKE_ARGS)); rc_run=$$?; \
	  echo "RED-1 (sub-make) exit code: $$rc_run"; \
	  echo "=== RED-2 (link): dirfd/fdopendir/scandir/alphasort had NO definition before T15 ==="; \
	  env SYSROOT="$(SYSROOT)" OS01_ROOT="$(OS01_ROOT)" \
	      LLVM_AR="$(LLVM_AR)" LLVM_NM="$(LLVM_NM)" \
	      EFFECTIVE_CC="$(EFFECTIVE_CC)" TARGET_LD="$(TARGET_LD)" \
	      USER_BUILD_DIR="$(USER_BUILD_DIR)" \
	    bash $(OS01_ROOT)/scripts/libc_red_link.sh \
	      "$$rc_run" 7aeb732a libc/stdlib scandir.c \
	      "scandir.o dirent.o" 'dirfd|fdopendir|scandir|alphasort' \
	      '#include <dirent.h>' \
	      'int main(void) { DIR *d = 0; struct dirent **l = 0; (void)dirfd(d); (void)fdopendir(0); (void)scandir("/", &l, 0, alphasort); return 0; }'

# ── libc completion archive audit (T1) ────────────────────────────
# make PROFILE=x86_64-clang test-libc-api-audit
#
# The header contract is checked by the api host group; this target checks
# the OTHER half of the same manifest — what the shipped archives actually
# export.  The manifest in scripts/audit_libc_api.py is the single source of
# truth for both: it names the headers, the interfaces and the task that
# owns each group, so a manifest that drifts away from the headers is an
# audit failure rather than a silent staleness.
#
# The libk baseline is per profile (an x86_64 libk.a and an aarch64 libk.a
# export different subsets), so the profile is passed explicitly instead of
# being guessed from the archive path.
#
# LIBC_GROUPS narrows the audit to a comma separated subset of the enabled
# groups (default: all enabled groups).  It is for debugging a single group;
# the gate always covers every enabled group when it is unset.
#
# Profiles with the `userland` capability (both x86_64-clang and aarch64-clang)
# build libc.a alongside libk.a; kernel-only profiles build only libk.a, so
# the user-libc archive is audited whenever libc.a exists.  The libk.a itself
# comes from `lib` on a userland profile and from the kernel build on a
# kernel-only profile.
LIBC_API_AUDIT_PREREQ := $(if $(filter userland,$(PROFILE_CAPABILITIES)),lib,$(BUILD_DIR)/artifacts/kernel.elf)

.PHONY: test-libc-api-audit
test-libc-api-audit: $(LIBC_API_AUDIT_PREREQ)
	@echo "  [test-libc-api-audit] profile=$(PROFILE)"
	python3 $(OS01_ROOT)/scripts/audit_libc_api.py --audit \
	    --profile "$(PROFILE)" \
	    $(if $(strip $(LIBC_GROUPS)),--groups $(LIBC_GROUPS)) \
	    --include-dir $(OS01_ROOT)/libc/include \
	    $(if $(wildcard $(LIBC_BUILD_DIR)/lib/libc.a),--libc-archive $(LIBC_BUILD_DIR)/lib/libc.a) \
	    $(if $(wildcard $(LIBC_BUILD_DIR)/lib/libm.a),--libm-archive $(LIBC_BUILD_DIR)/lib/libm.a) \
	    --libk-archive $(LIBC_BUILD_DIR)/lib/libk.a

# ── libm instruction audit ──────────────────────────────────────────────
# The base-algebra math set must compute with integer bit operations
# only: no floating-point arithmetic, no FMA, no floating-point
# rounding, and no soft-float helper.  The host test proves the
# RESULTS against the host glibc and the guest case proves them inside
# the running system; this audit proves the METHOD, by reading the
# disassembly of the PUBLISHED archive.
#
# The profile decides the architecture: an archive of another
# architecture is a failure, not a warning.  The audit also refuses a
# missing symbol, an unparseable instruction and any forbidden
# floating-point instruction.
.PHONY: test-libm-asm
test-libm-asm: lib
	@echo "  [test-libm-asm] profile=$(PROFILE)"
	python3 $(OS01_ROOT)/scripts/audit_libm_asm.py \
	    --archive $(SYSROOT)/usr/lib/libm.a \
	    --profile "$(PROFILE)" \
	    --objdump "$(LLVM_OBJDUMP)"
