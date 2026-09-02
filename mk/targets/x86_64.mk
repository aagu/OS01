# ── x86_64 host / run / link parameters ─────────────────────────
# Host QEMU for the x86_64 profile. On aarch64 hosts the bundled cross
# build (toolchain/cross) provides the x86_64 emulator; CROSS_BASE is
# exported for the tools recipes. The run parameters shared with the
# legacy aarch64 recipes (MEMORY/SMP/DISPLAY) stay in the root Makefile —
# they are profile-agnostic.

ifneq ($(shell uname -m),aarch64)
QEMU_BIN=qemu-system-x86_64
else
export CROSS_BASE=$(OS01_ROOT)/toolchain/cross
QEMU_BIN=$(CROSS_BASE)/bin/qemu-system-x86_64
endif

# ── x86 UEFI firmware (profile-private) ────────────────────────
# Every x86 QEMU consumer uses $(OVMF_FIRMWARE) — a profile-private real
# file under build/<profile>/firmware, acquired by the root-owned rule in
# mk/components/run.mk. OVMF_FIRMWARE_SOURCE accepts ONLY an https:// URL
# (downloaded to a temp file and atomically renamed) or an existing
# absolute local file path (content-guarded copy); anything else is
# rejected before any download/copy.
OVMF_FIRMWARE ?= $(BUILD_DIR)/firmware/OVMF.fd
OVMF_FIRMWARE_SOURCE ?= https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd
