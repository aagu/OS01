# ── aarch64-clang profile ───────────────────────────────────────
# aarch64 UEFI bring-up profile. Only the kernel and UEFI runtime
# capabilities are declared: it must not depend on libc, mbedTLS,
# BusyBox, a general rootfs, or the x86 test targets.

PROFILE_CAPABILITIES := kernel uefi-bringup

# Source root (repo root) — recomputed here so the profile can also be
# included directly by component Makefiles (via OS01_PROFILE_FILE) without
# project.mk. Identical to project.mk's value when included through it.
OS01_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

# Profile-private layout (same shape as the x86_64 profile).
BUILD_DIR := $(OS01_ROOT)/build/$(PROFILE)
SYSROOT := $(BUILD_DIR)/sysroot
TARGET_INCLUDEDIR := $(SYSROOT)/usr/include
TARGET_LIBDIR := $(SYSROOT)/usr/lib
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel
LIBC_BUILD_DIR := $(BUILD_DIR)/libc
# Compile-affecting variant (only OS01_SYSTEST re-keys the user dirs).
# project.mk sets USER_VARIANT before including this profile; the ?= default
# lets component sub-makes that include ONLY the profile resolve it from
# OS01_SYSTEST. The aarch64 bring-up profile builds no userland, so this only
# keeps the profile self-contained.
USER_VARIANT ?= $(if $(filter 1,$(OS01_SYSTEST)),systest)
USER_BUILD_DIR := $(BUILD_DIR)/user
UEFI_BUILD_DIR := $(BUILD_DIR)/uefi
UEFI_RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
# Host unit tests are an OS01 build: every test object/binary lives under
# the profile's host-test dir (test/Makefile includes this profile).
HOST_TEST_BUILD_DIR := $(BUILD_DIR)/host-test

# Staging roots: component installs write ONLY their private staging tree
# (INSTALL_ROOT), never the final sysroot. The single writer of the sysroot
# is sysroot.mk (Task 4).
STAGING_DIR := $(BUILD_DIR)/staging

# ── aarch64 compiler / QEMU settings (retained from the legacy Makefile) ──
# This profile does NOT include mk/toolchains/clang.mk — that is the x86
# Clang-family discovery. The aarch64 toolchain is the arch-specific pair
# below; the kernel's arch/aarch64/make.config re-asserts the same CC/LD.
CC             := clang -target aarch64-none-elf
LD             := ld.lld -m aarch64elf
TARGET_TRIPLE  := aarch64-none-elf
TARGET_CC      ?= $(CC)
TARGET_CCLD    ?= $(CC)
EFFECTIVE_CC   ?= $(CC)
# TARGET_LD is a whitelisted sub-make arg — it must be a BARE command (no
# embedded flags, or the space splits the make command line). The aarch64
# link emulation flag lives in LD / ARCH_LDFLAGS instead.
TARGET_LD      ?= ld.lld
AR             ?= llvm-ar
# Archive tool for the target (spec profile variable TARGET_AR).
TARGET_AR      ?= llvm-ar
OBJ_CPY        ?= llvm-objcopy
AARCH64_QEMU   ?= qemu-system-aarch64
AARCH64_SMP    ?= 4
AARCH64_UEFI_FIRMWARE_SOURCE ?= https://retrage.github.io/edk2-nightly/bin/RELEASEAARCH64_QEMU_EFI.fd

# aarch64 link / UEFI / run parameters.
include $(OS01_ROOT)/mk/targets/aarch64.mk
