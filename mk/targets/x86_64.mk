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
