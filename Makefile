ROOT_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
base := $(patsubst %/,%,$(dir $(ROOT_MAKEFILE)))

# ── Profile selection ───────────────────────────────────────
# mk/project.mk selects PROFILE (default x86_64-clang), includes the
# profile (validated toolchain + host/run/link parameters), and defines
# require_capability / os01_submake / OS01_SUBMAKEFLAGS. The old ARCH
# dispatch, root toolchain.mk include and broad exports are gone: component
# Makefiles consume the profile directly (via OS01_PROFILE_FILE), never
# through implicit environment inheritance.
#
# The variant switches MUST be resolved before the profile include:
# project.mk derives IMAGE_VARIANT / USER_VARIANT from OS01_SYSTEST /
# OS01_NETTEST / INITTAB_FILE, and the whole dependency graph (image dirs,
# user build/artifact dirs) is keyed on those slugs.
INITTAB_FILE ?= config/inittab
ifeq ($(OS01_SYSTEST),1)
INITTAB_FILE := config/inittab.systest
endif
ifeq ($(OS01_NETTEST),1)
INITTAB_FILE := config/inittab.nettest
endif

include $(base)/mk/project.mk
# Cross-component dependency graph (spec: mk/components/*.mk is the only
# place that wires components together). sysroot.mk is the single writer of
# the sysroot; kernel.mk owns the kernel artifact; uefi.mk owns the UEFI
# runtime adapter + EFI apps; image.mk owns the rootfs manifest and the disk
# images; run.mk owns the run/debug/test/validate/clean entry points.
include $(base)/mk/components/sysroot.mk
include $(base)/mk/components/kernel.mk
include $(base)/mk/components/user.mk
include $(base)/mk/components/uefi.mk
include $(base)/mk/components/image.mk

# ── Run parameters (profile-agnostic; shared by x86 and aarch64) ──
DISPLAY=gtk
MEMORY=512M
SMP ?= 2

# ── Log output target (serial | fb | both) ───────────────
LOG_TARGET ?= serial
DEBUG      ?=
KERNEL_SELFTEST ?=
export KERNEL_SELFTEST

include $(base)/mk/components/run.mk

# ── Default goal ──────────────────────────────────────────
# Bare `make` builds the default profile's disk image (the project-root
# disk.img compat copy of build/<profile>/image/disk.img).
.DEFAULT_GOAL := disk.img
all: disk.img

# Non-rootfs profiles (e.g. aarch64-clang) cannot build a disk image — give
# the clean capability error instead of "No rule to make target 'disk.img'".
ifneq ($(filter rootfs,$(PROFILE_CAPABILITIES)),rootfs)
disk.img:
	$(call require_capability,rootfs)
endif

# ── Libraries ───────────────────────────────────────────

# `lib` = the profile's sysroot libraries: stages kernel headers, libc/libk,
# mbedTLS and compat-libs, then publishes an immutable sysroot generation
# (the stamp's recipe in mk/components/sysroot.mk does the work). The
# sysroot.stamp prerequisite only exists for userland profiles, so on others
# this is a bare phony whose recipe fires the capability gate.
.PHONY: lib
lib: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(SYSROOT_STAMP))
	$(call require_capability,userland)

# ── User programs ───────────────────────────────────────

# user = the profile's user ELFs + BusyBox (mk/components/user.mk), all
# built against the leased immutable sysroot generation. Only defined for
# userland-capable profiles; on others the recipe fires the capability gate.
.PHONY: user
user: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf)
	$(call require_capability,userland)
