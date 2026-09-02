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
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel
LIBC_BUILD_DIR := $(BUILD_DIR)/libc
USER_BUILD_DIR := $(BUILD_DIR)/user
UEFI_BUILD_DIR := $(BUILD_DIR)/uefi
UEFI_RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime

# Staging roots: component installs write ONLY their private staging tree
# (INSTALL_ROOT), never the final sysroot. The single writer of the sysroot
# is sysroot.mk (Task 4).
STAGING_DIR := $(BUILD_DIR)/staging
# Task 3 transitional: kernel consumes libc headers from the libc staging
# tree while `lib` stages it. Task 4 replaces this with the immutable
# sysroot generation path.
LIBC_STAGING_INCLUDEDIR := $(STAGING_DIR)/libc/usr/include
# Task 3 transitional, parallel to the include dir: the kernel links -lk
# against the staged libk.a while the final sysroot is not yet populated.
LIBC_STAGING_LIBDIR := $(STAGING_DIR)/libc/usr/lib
# Explicit kernel-profile flag for the stdint.h injection (spec: kernel
# profile flags). Task 3 transitional — references the staged libc include.
KERNEL_STDINT_FLAGS := -D__CLANG_STDINT_H -include $(LIBC_STAGING_INCLUDEDIR)/stdint.h

# Artifacts consumed by the root validation recipes.
KERNEL_ELF ?= $(KERNEL_BUILD_DIR)/kernel.elf
UEFI_EFI   ?= $(UEFI_BUILD_DIR)/BOOTX64.EFI

# x86 Clang/LLVM discovery + validation, and host/run/link parameters.
include $(OS01_ROOT)/mk/toolchains/clang.mk
include $(OS01_ROOT)/mk/targets/x86_64.mk

# Archive tool for the target (spec profile variable TARGET_AR); command-line
# overridable.
TARGET_AR ?= $(LLVM_AR)
