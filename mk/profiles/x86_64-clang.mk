# ── x86_64-clang profile ────────────────────────────────────────
# Full host + target configuration for the default x86_64 profile.
# Capabilities: kernel, userland, rootfs, uefi.

PROFILE_CAPABILITIES := kernel userland rootfs uefi

# Source root (repo root) — recomputed here so the profile can also be
# included directly by component Makefiles (via OS01_PROFILE_FILE) without
# project.mk. Identical to project.mk's value when included through it.
OS01_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

# Profile-private layout: every intermediate and final output for this
# profile lives under build/<profile>.
BUILD_DIR := $(OS01_ROOT)/build/$(PROFILE)
SYSROOT := $(BUILD_DIR)/sysroot
TARGET_INCLUDEDIR := $(SYSROOT)/usr/include
TARGET_LIBDIR := $(SYSROOT)/usr/lib
# KERNEL_SELFTEST changes generated kernel objects, so it gets a distinct
# kernel build/artifact namespace.  Keep the ordinary paths exactly stable.
KERNEL_VARIANT := $(if $(filter 1,$(KERNEL_SELFTEST)),selftest)
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))
LIBC_BUILD_DIR := $(BUILD_DIR)/libc
# Compile-affecting variant (only OS01_SYSTEST re-keys the user dirs).
# project.mk sets USER_VARIANT before including this profile; the ?= default
# lets component sub-makes that include ONLY the profile (via
# OS01_PROFILE_FILE, e.g. user/Makefile) resolve it from OS01_SYSTEST on
# their own command line (passed through OS01_SUBMAKE_ARGS).
USER_VARIANT ?= $(if $(filter 1,$(OS01_SYSTEST)),systest)
# Variant-scoped user dirs: the compile-affecting variant keys the user object
# dir and the user artifact dir, so a systest build compiles fresh objects
# into build/<profile>/user/systest and publishes artifacts into
# build/<profile>/artifacts/user/systest — it can never contaminate the
# normal build/artifacts. Plain paths when no variant.
USER_BUILD_DIR := $(BUILD_DIR)/user$(if $(USER_VARIANT),/$(USER_VARIANT))
UEFI_BUILD_DIR := $(BUILD_DIR)/uefi
UEFI_RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
# Host unit tests are an OS01 build: every test object/binary lives under
# the profile's host-test dir (test/Makefile includes this profile).
HOST_TEST_BUILD_DIR := $(BUILD_DIR)/host-test

# Staging roots: component installs write ONLY their private staging tree
# (INSTALL_ROOT), never the final sysroot. The single writer of the sysroot
# is sysroot.mk, which assembles immutable generations under
# build/<profile>/sysroot-generations/ and publishes the $(SYSROOT) symlink.
STAGING_DIR := $(BUILD_DIR)/staging

# Kernel artifact path (spec artifact-path table) — the profile is the single
# source of artifact paths; kernel.mk's artifact rule consumes this.
KERNEL_ARTIFACT := $(if $(KERNEL_VARIANT),$(BUILD_DIR)/artifacts/kernel/$(KERNEL_VARIANT)/kernel.bin,$(BUILD_DIR)/artifacts/kernel.bin)

# User artifact paths (spec artifact-path table) — the profile is the single
# source of artifact paths; user.mk's artifact rules consume this.
USER_ARTIFACT_DIR := $(BUILD_DIR)/artifacts/user$(if $(USER_VARIANT),/$(USER_VARIANT))

# Explicit kernel-profile flag for the stdint.h injection (spec: kernel
# profile flags), resolved against the immutable sysroot generation the
# kernel is compiled against. SYSROOT_GENERATION_DIR is passed to the kernel
# sub-make by the root artifact rule (mk/components/kernel.mk) under the
# generation lease; recursive so it resolves inside that sub-make.
KERNEL_STDINT_FLAGS = -D__CLANG_STDINT_H -include $(SYSROOT_GENERATION_DIR)/usr/include/stdint.h

# Artifacts consumed by the root validation recipes. UEFI_EFI is the EFI app
# artifact (spec artifact-path table); uefi.mk owns the rule that produces it
# and validate-uefi depends on it.
KERNEL_ELF ?= $(KERNEL_BUILD_DIR)/kernel.elf
UEFI_EFI   ?= $(BUILD_DIR)/artifacts/uefi/BOOTX64.EFI

# x86 Clang/LLVM discovery + validation, and host/run/link parameters.
include $(OS01_ROOT)/mk/toolchains/clang.mk
include $(OS01_ROOT)/mk/targets/x86_64.mk

# Archive tool for the target (spec profile variable TARGET_AR); command-line
# overridable.
TARGET_AR ?= $(LLVM_AR)

# Compiler-runtime policy and the x86_64 kernel variant.  Consumer-specific
# declarations live in the profile so kernel-only flags never leak into a
# future userland or UEFI runtime archive.
RUNTIME_PROVIDER ?= selfhosted
RUNTIME_TARGET_kernel := $(TARGET_TRIPLE)
RUNTIME_CFLAGS_kernel := -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -mcmodel=kernel
RUNTIME_OBJECT_FORMAT_kernel := ELF
RUNTIME_MACHINE_kernel := EM_X86_64
RUNTIME_ABI_kernel := x86_64-sysv
# compiler-rt remains opt-in for the kernel.  A separately audited, profile-
# owned manifest must name the exact archive before this provider is eligible.
RUNTIME_COMPILER_RT_MANIFEST_kernel ?=
