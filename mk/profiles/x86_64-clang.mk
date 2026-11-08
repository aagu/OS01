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
# KERNEL_SELFTEST and KERNEL_CANARY_SELFTEST change generated kernel objects,
# so each gets a distinct kernel build/artifact namespace.  Keep the ordinary
# paths exactly stable; the destructive canary variant cannot share an image
# with any normal selftest, syscall, or network-test mode.
ifneq ($(filter 1,$(KERNEL_CANARY_SELFTEST)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST)),)
$(error KERNEL_CANARY_SELFTEST=1 cannot be combined with KERNEL_SELFTEST=1, OS01_SYSTEST=1, or OS01_NETTEST=1)
endif
endif
# ── aarch64-only selectors ─────────────────────────────────────
# The per-CPU tick accounting variant (2026-11-06) is aarch64-only:
# the local CNTP tick, its accounting and its evidence producer have no
# x86_64 counterpart.  The x86_64 profile rejects the selector so a
# "tick" image can never be built here by accident.
ifneq ($(strip $(AARCH64_SCHED_TICK_TEST)),)
$(error AARCH64_SCHED_TICK_TEST='$(AARCH64_SCHED_TICK_TEST)' is aarch64-only; the x86_64 profile must not select the per-CPU tick variant)
endif
# ── Isolated destructive dispatch probe (Task 5, spec §9.3) ──────
# The probe-selection MACRO arrives through KERNEL_EXTRA_CFLAGS — the
# channel that already crosses the controlled sub-make boundary
# (mk/project.mk's OS01_SUBMAKE_ALLOWED).  The variant slug is derived
# from that SAME macro, so the kernel build dir, the kernel artifact
# and the image dir can never disagree with the compiled probe.
#
# The macro NAME (not the whole flag) is matched, so the spelling stays
# identical to the C sources' `#if defined(X86_INTR_FAULT_TEST_BP)`.
X86_INTR_FAULT_CASE := $(if $(findstring X86_INTR_FAULT_TEST_BP,$(KERNEL_EXTRA_CFLAGS)),bp,$(if $(findstring X86_INTR_FAULT_TEST_NMI,$(KERNEL_EXTRA_CFLAGS)),nmi,))
# BOOT-1 CPU-lifecycle fault matrix (Task 3): same macro-derived case —
# a direct KERNEL_EXTRA_CFLAGS selection resolves here too.
X86_SMP_BOOT_CASE := $(if $(findstring X86_SMP_BOOT_TEST_DELAY_ONLINE,$(KERNEL_EXTRA_CFLAGS)),delay-online,$(if $(findstring X86_SMP_BOOT_TEST_NO_ONLINE,$(KERNEL_EXTRA_CFLAGS)),no-online,$(if $(findstring X86_SMP_BOOT_TEST_NO_SCHEDULER,$(KERNEL_EXTRA_CFLAGS)),no-scheduler,)))
# BOOT-2 early-IRQ/time fault matrix: same macro-derived case (the
# selector contributes a single -DX86_BOOT_IRQ_TIME_TEST=<case> token).
X86_BOOT_IRQ_TIME_CASE := $(if $(findstring X86_BOOT_IRQ_TIME_TEST=rtc-timeout,$(KERNEL_EXTRA_CFLAGS)),rtc-timeout,$(if $(findstring X86_BOOT_IRQ_TIME_TEST=force-rtc,$(KERNEL_EXTRA_CFLAGS)),force-rtc,$(if $(findstring X86_BOOT_IRQ_TIME_TEST=bsp-lapic-fail,$(KERNEL_EXTRA_CFLAGS)),bsp-lapic-fail,$(if $(findstring X86_BOOT_IRQ_TIME_TEST=unsafe-open,$(KERNEL_EXTRA_CFLAGS)),unsafe-open,))))
# BOOT-3 startup failure policy matrix:
BOOT_FAILURE_CASE := $(if $(findstring BOOT_FAILURE_TEST=required-subsys,$(KERNEL_EXTRA_CFLAGS)),required-subsys,$(if $(findstring BOOT_FAILURE_TEST=optional-subsys,$(KERNEL_EXTRA_CFLAGS)),optional-subsys,$(if $(findstring BOOT_FAILURE_TEST=registration,$(KERNEL_EXTRA_CFLAGS)),registration,$(if $(findstring BOOT_FAILURE_TEST=root,$(KERNEL_EXTRA_CFLAGS)),root,$(if $(findstring BOOT_FAILURE_TEST=tty,$(KERNEL_EXTRA_CFLAGS)),tty,$(if $(findstring BOOT_FAILURE_TEST=pty,$(KERNEL_EXTRA_CFLAGS)),pty,$(if $(findstring BOOT_FAILURE_TEST=stdio,$(KERNEL_EXTRA_CFLAGS)),stdio,$(if $(findstring BOOT_FAILURE_TEST=init,$(KERNEL_EXTRA_CFLAGS)),init,$(if $(findstring BOOT_FAILURE_TEST=child-fds,$(KERNEL_EXTRA_CFLAGS)),child-fds,)))))))))
# BOOT-4 stage contract matrix:
BOOT_CONTRACT_CASE := $(if $(findstring BOOT_CONTRACT_TEST=reverse,$(KERNEL_EXTRA_CFLAGS)),reverse,$(if $(findstring BOOT_CONTRACT_TEST=missing,$(KERNEL_EXTRA_CFLAGS)),missing,$(if $(findstring BOOT_CONTRACT_TEST=cycle,$(KERNEL_EXTRA_CFLAGS)),cycle,)))
# BOOT-5 scheduler and services matrix:
BOOT_SERVICES_CASE := $(if $(findstring BOOT_SERVICES_TEST=overlap,$(KERNEL_EXTRA_CFLAGS)),overlap,$(if $(filter overlap,$(BOOT_SERVICES_TEST)),overlap,))
ifneq ($(filter 1,$(KERNEL_TEST_FORCE_NO_RNDRRS)),)
KERNEL_VARIANT := weak-selftest
else ifneq ($(filter 1,$(KERNEL_SELFTEST)),)
KERNEL_VARIANT := selftest
else ifneq ($(filter 1,$(KERNEL_CANARY_SELFTEST)),)
KERNEL_VARIANT := canary-selftest
else ifneq ($(filter 1,$(FB_RESOLUTION_TEST)),)
# FB_RESOLUTION_TEST=1 compiles the /dev/fbtest control surface; isolate its
# kernel/image artifacts so the production tree is never touched.
KERNEL_VARIANT := resolution-test
else ifneq ($(filter 1,$(CAPACITY_TEST)),)
KERNEL_VARIANT := arch10-capacity
else ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
KERNEL_VARIANT :=
else
# DRIVER_MODEL_FAULT != none — kernel/image live under driver-model-<fault>/.
KERNEL_VARIANT := driver-model-$(DRIVER_MODEL_FAULT)
endif
# The destructive fault variant wins over every other variant: it must
# never share objects or images with another test mode (mk/project.mk
# refuses the conflicting combinations at parse time).
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
KERNEL_VARIANT := intr-x86-fault-$(X86_INTR_FAULT_CASE)
endif
# The BOOT-1 fault variant also wins over every other variant (mk/project.mk
# refuses the conflicting combinations at parse time).
ifneq ($(strip $(X86_SMP_BOOT_CASE)),)
KERNEL_VARIANT := boot-lifecycle-$(X86_SMP_BOOT_CASE)
endif
ifneq ($(strip $(X86_BOOT_IRQ_TIME_CASE)),)
KERNEL_VARIANT := boot-irq-time-$(X86_BOOT_IRQ_TIME_CASE)
endif
ifneq ($(strip $(BOOT_FAILURE_CASE)),)
KERNEL_VARIANT := boot-failure-$(BOOT_FAILURE_CASE)
endif
ifneq ($(strip $(BOOT_CONTRACT_CASE)),)
KERNEL_VARIANT := boot-contract-$(BOOT_CONTRACT_CASE)
endif
ifneq ($(strip $(BOOT_SERVICES_CASE)),)
KERNEL_VARIANT := boot-services-$(BOOT_SERVICES_CASE)
endif
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))
LIBC_BUILD_DIR := $(BUILD_DIR)/libc
LIBGFX_BUILD_DIR := $(BUILD_DIR)/libgfx
LIBGUI_BUILD_DIR := $(BUILD_DIR)/libgui
# Compile-affecting variant (OS01_SYSTEST / FB_RESOLUTION_TEST / USER_DEBUG
# re-key the user dirs).
# project.mk sets USER_VARIANT before including this profile; the ?= default
# lets component sub-makes that include ONLY the profile (via
# OS01_PROFILE_FILE, e.g. user/Makefile) resolve it from OS01_SYSTEST on
# their own command line (passed through OS01_SUBMAKE_ARGS).
USER_VARIANT ?= $(if $(filter 1,$(ARCH6_FSSTRESS_TEST)),arch6-fsstress)$(if $(filter 1,$(CAPACITY_TEST)),arch10-capacity)$(if $(filter 1,$(OS01_SYSTEST)),systest)$(if $(filter 1,$(FB_RESOLUTION_TEST)),resolution-test)$(if $(filter 1,$(USER_DEBUG)),user-debug)
# Variant-scoped user dirs: the compile-affecting variant keys the user object
# dir and the user artifact dir, so a systest build compiles fresh objects
# into build/<profile>/user/systest and publishes artifacts into
# build/<profile>/artifacts/user/systest — it can never contaminate the
# normal build/artifacts. Plain paths when no variant.
USER_BUILD_DIR := $(BUILD_DIR)/user$(if $(USER_VARIANT),/$(USER_VARIANT))
UEFI_BUILD_DIR := $(BUILD_DIR)/uefi
UEFI_RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
# Host unit tests are an OS01 build: every test object/binary lives under
# the profile's host-test dir (hosttests/Makefile includes this profile).
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
# profile flags). Goes through the STABLE $(SYSROOT) symlink — the flag
# lands in ALL_CFLAGS / the .cflags fingerprint stamp and in .d files, so
# a generation-relative path would go stale on every republish and defeat
# header-level incremental rebuilds. Recursive so it resolves inside the
# kernel sub-make.
KERNEL_STDINT_FLAGS = -D__CLANG_STDINT_H -include $(SYSROOT)/usr/include/stdint.h

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

# User runtime consumer (monitor plan Task 5, spec §2.3).  The user archive
# is linked into freestanding non-PIE ring-3 ELFs, so it gets the USER code
# model and flags — never the kernel variant's -mcmodel=kernel objects.
# Everything a user consumer needs is declared here so the kernel-only flags
# above can never leak into it.
RUNTIME_TARGET_user := $(TARGET_TRIPLE)
RUNTIME_CFLAGS_user := -ffreestanding -fno-builtin -fno-stack-protector \
                       -fno-asynchronous-unwind-tables -fno-pic -fno-pie \
                       -mno-red-zone -mcmodel=small
RUNTIME_OBJECT_FORMAT_user := ELF
RUNTIME_MACHINE_user := EM_X86_64
RUNTIME_ABI_user := x86_64-sysv
# v1 user consumer supports only the selfhosted provider; any other value is
# rejected at parse time by mk/components/runtime.mk.
USER_RUNTIME_PROVIDER ?= selfhosted
