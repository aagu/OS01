# ── aarch64-clang profile ───────────────────────────────────────
# aarch64 UEFI profile. Capabilities: kernel, uefi and (2026-11-18,
# M4-remaining product userspace) userland — the aarch64 userland build
# chain publishes its own sysroot generation and user ELFs.  BusyBox joined
# the userland surface (2026-11-21, arch-parametrized adapter); it still
# does not depend on mbedTLS, a general rootfs, or the x86 test targets.

PROFILE_CAPABILITIES := kernel uefi userland

# Source root (repo root) — recomputed here so the profile can also be
# included directly by component Makefiles (via OS01_PROFILE_FILE) without
# project.mk. Identical to project.mk's value when included through it.
OS01_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

# ── AArch64 boot block configuration (spec 2026-11-17-aarch64-kernel-lma) ──
# AARCH64_BOOT_BLOCK_BASE selects the physical boot block B for THIS build.
# Default is the QEMU virt layout (0x40000000). One build = one address:
# the value is validated here and forwarded as -D to kernel C, assembly
# and the preprocessed linker script (AARCH64_LAYOUT_CFLAGS below); there
# is no mask-based correction and no fallback to the default.
#
# An explicitly empty value is an error (the default is chosen by NOT
# setting the variable, not by setting it empty).
ifneq ($(origin AARCH64_BOOT_BLOCK_BASE),undefined)
ifeq ($(strip $(AARCH64_BOOT_BLOCK_BASE)),)
$(error AARCH64_BOOT_BLOCK_BASE is explicitly empty; unset it for the 0x40000000 default or give a decimal/0x hex base)
endif
else
AARCH64_BOOT_BLOCK_BASE := 0x40000000
endif

# Value validation. The probe runs in bash so 0x literals are evaluated
# without GNU-make arithmetic. The value travels to bash as a
# POSIX-quoted positional argument, NOT through `export`: GNU Make 4.3
# (the version in the CI container) does not pass exported variables into
# $(shell) — only 4.4 and newer do — so `export` + `$$VAR` silently
# yields an empty value there and every base looks invalid. Quoting the
# value keeps it out of the command string (no quoting/injection
# surface) on every make version. Contract (§2):
# single decimal or 0x hex integer, 2 MiB aligned, within the M0 map
# [0, 0x80000000), disjoint from the boot device window
# D = [0x08000000, 0x0a000000), and enough room below 0x80000000 for the
# minimum 20 MiB early-arena window. The check must echo exactly "OK":
# a missing bash / failed probe yields empty output and fails closed.
# POSIX single-quote escaping: close the quote, emit an escaped quote,
# reopen — so the value stays exactly one shell word.
AARCH64_BOOT_BLOCK_BASE_SH := $(subst ','\'',$(AARCH64_BOOT_BLOCK_BASE))
AARCH64_BOOT_BLOCK_BASE_CHECK := $(shell bash -c 'v="$$1"; if ! [[ "$$v" =~ ^(0x[0-9a-fA-F]+|[0-9]+)$$ ]]; then echo "must be a single decimal or 0x hex integer"; exit 0; fi; n=$$(("$$1")); if (( n < 0 )); then echo "outside the M0 map [0..0x80000000]"; elif (( n % 0x200000 != 0 )); then echo "not 2 MiB aligned"; elif (( n < 0x0a000000 && n + 0x200000 > 0x08000000 )); then echo "overlaps the boot device window D [0x08000000..0x0a000000]"; elif (( n + 0x200000 > 0x80000000 )); then echo "outside the M0 map [0..0x80000000]"; elif (( 0x80000000 - n - 0x200000 < 0x01400000 )); then echo "no room for the minimum 20 MiB early-arena window"; else echo OK; fi' _ '$(AARCH64_BOOT_BLOCK_BASE_SH)' 2>/dev/null)
ifneq ($(strip $(AARCH64_BOOT_BLOCK_BASE_CHECK)),OK)
$(error invalid AARCH64_BOOT_BLOCK_BASE='$(AARCH64_BOOT_BLOCK_BASE)': $(AARCH64_BOOT_BLOCK_BASE_CHECK))
endif

# The single -D consumed by kernel C, assembly and the linker-script
# preprocessing; boot_layout.h derives every other layout constant from it.
AARCH64_LAYOUT_CFLAGS := -DAARCH64_BOOT_BLOCK_BASE=$(AARCH64_BOOT_BLOCK_BASE)

# ── Independent output root ─────────────────────────────────────
# AARCH64_BUILD_ROOT relocates the ENTIRE profile layout (kernel/UEFI/
# image/libc/sysroot/...) to an isolated tree, so alternative-layout
# builds never touch the default root. Default: the profile's own
# BUILD_DIR. Explicit values must be absolute. Propagated across the
# controlled sub-make boundary via OS01_SUBMAKE_ALLOWED (mk/project.mk) —
# the kernel sub-make inherits it through this same profile include and
# derives KERNEL_BUILD_DIR from BUILD_DIR below (never overwritten
# directly).
AARCH64_BUILD_ROOT ?=
ifneq ($(strip $(AARCH64_BUILD_ROOT)),)
ifeq ($(filter /%,$(AARCH64_BUILD_ROOT)),)
$(error AARCH64_BUILD_ROOT='$(AARCH64_BUILD_ROOT)' must be an absolute path)
endif
BUILD_DIR := $(AARCH64_BUILD_ROOT)
endif

# Profile-private layout (same shape as the x86_64 profile). The
# AARCH64_BUILD_ROOT block above has already re-pointed BUILD_DIR when set.
ifeq ($(strip $(AARCH64_BUILD_ROOT)),)
BUILD_DIR := $(OS01_ROOT)/build/$(PROFILE)
endif
SYSROOT := $(BUILD_DIR)/sysroot
TARGET_INCLUDEDIR := $(SYSROOT)/usr/include
TARGET_LIBDIR := $(SYSROOT)/usr/lib
LIBC_BUILD_DIR := $(BUILD_DIR)/libc
LIBGFX_BUILD_DIR := $(BUILD_DIR)/libgfx

# Compile-affecting variant — mirror x86_64 profile's KERNEL_VARIANT block
# so weak-selftest build produces an isolated kernel/weak-selftest/ dir.
# sync-fault must take precedence over selftest so the dedicated fault
# variant's kernel/image/firmware paths never collide with the ordinary
# selftest build (spec §5). The flag is only meaningful together with
# KERNEL_SELFTEST=1 (sync-fault is a selftest variant, not a free-standing
# mode); flagging it alone is rejected at parse time below.

# DRIVER_MODEL_FAULT is x86-only: the driver model matrix harness drives PCI NIC
# / AHCI faults that aarch64 does not implement. Reject non-none here so
# the matrix never silently degrades to a passing aarch64 variant.
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
# pass — none is the canonical build for aarch64 too
else
$(error DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT) is x86-only; aarch64 profile must keep DRIVER_MODEL_FAULT=none)
endif
#
# CAPACITY_TEST is x86-only
ifneq ($(filter 1,$(CAPACITY_TEST)),)
$(error CAPACITY_TEST=1 is x86-only; aarch64 profile must keep CAPACITY_TEST=0)
endif

# X86_INTR_FAULT_TEST is x86-only as well: the destructive #BP / NMI
# probes target the x86 exception entries (int3 and the NMI gate).  The
# probe-selection macro travels through KERNEL_EXTRA_CFLAGS, so reject a
# non-empty selection here — this profile is also included directly by
# the aarch64 kernel sub-make, which never sees mk/project.mk.
X86_INTR_FAULT_CASE := $(if $(findstring X86_INTR_FAULT_TEST_BP,$(KERNEL_EXTRA_CFLAGS)),bp,$(if $(findstring X86_INTR_FAULT_TEST_NMI,$(KERNEL_EXTRA_CFLAGS)),nmi,$(if $(findstring X86_INTR_FAULT_TEST_TRACE_READ,$(KERNEL_EXTRA_CFLAGS)),trace-read,$(if $(findstring X86_INTR_FAULT_TEST_TRACE_BAD_FP,$(KERNEL_EXTRA_CFLAGS)),trace-bad-fp,))))
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE) is x86-only; the aarch64 profile must not select a destructive fault variant)
endif
# X86_SMP_BOOT_TEST is x86-only as well: the BOOT-1 CPU-lifecycle fault
# matrix injects into the x86 AP boot path (kernel/arch/x86_64/smp/smp.c).
X86_SMP_BOOT_CASE := $(if $(findstring X86_SMP_BOOT_TEST_DELAY_ONLINE,$(KERNEL_EXTRA_CFLAGS)),delay-online,$(if $(findstring X86_SMP_BOOT_TEST_NO_ONLINE,$(KERNEL_EXTRA_CFLAGS)),no-online,$(if $(findstring X86_SMP_BOOT_TEST_NO_SCHEDULER,$(KERNEL_EXTRA_CFLAGS)),no-scheduler,)))
ifneq ($(strip $(X86_SMP_BOOT_CASE)),)
$(error X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE) is x86-only; the aarch64 profile must not select a boot-lifecycle fault variant)
endif
X86_BOOT_IRQ_TIME_CASE := $(if $(findstring X86_BOOT_IRQ_TIME_TEST=1,$(KERNEL_EXTRA_CFLAGS)),boot-irq-time,)
ifneq ($(strip $(X86_BOOT_IRQ_TIME_CASE)),)
$(error X86_BOOT_IRQ_TIME_TEST is x86-only; the aarch64 profile must not select a boot-irq-time fault variant)
endif
BOOT_FAILURE_CASE := $(if $(findstring BOOT_FAILURE_TEST=,$(KERNEL_EXTRA_CFLAGS)),boot-failure,)
ifneq ($(strip $(BOOT_FAILURE_CASE)),)
$(error BOOT_FAILURE_TEST is x86-only; the aarch64 profile must not select a boot-failure fault variant)
endif
ifneq ($(strip $(BOOT_FAILURE_TEST)),)
$(error BOOT_FAILURE_TEST is x86-only; the aarch64 profile must not select a boot-failure fault variant)
endif
ifneq ($(strip $(BOOT_SERVICES_TEST)),)
ifneq ($(BOOT_SERVICES_TEST),none)
$(error BOOT_SERVICES_TEST is x86-only; the aarch64 profile must not select a boot-services test variant)
endif
endif
# BOOT-4 stage contract matrix:
BOOT_CONTRACT_CASE := $(if $(findstring BOOT_CONTRACT_TEST=reverse,$(KERNEL_EXTRA_CFLAGS)),reverse,$(if $(findstring BOOT_CONTRACT_TEST=missing,$(KERNEL_EXTRA_CFLAGS)),missing,$(if $(findstring BOOT_CONTRACT_TEST=cycle,$(KERNEL_EXTRA_CFLAGS)),cycle,)))
#
AARCH64_SERROR_TESTS_VALID := el1t el1h el0t
ifneq ($(strip $(AARCH64_SERROR_TEST)),)
ifeq ($(filter $(AARCH64_SERROR_TEST),$(AARCH64_SERROR_TESTS_VALID)),)
$(error invalid AARCH64_SERROR_TEST='$(AARCH64_SERROR_TEST)'; valid: $(AARCH64_SERROR_TESTS_VALID))
endif
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_SERROR_TEST='$(AARCH64_SERROR_TEST)' requires KERNEL_SELFTEST=1)
endif
ifneq ($(strip $(AARCH64_SCHED_TEST) $(AARCH64_AP_SCHED_TEST) $(AARCH64_SCHED_TICK_TEST) $(AARCH64_USER_RUNTIME_TEST) $(AARCH64_M1_TEST) $(AARCH64_EL0_TEST) $(AARCH64_SYNC_FAULT_TEST) $(AARCH64_EXCEPTION_TRACE_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST)),)
$(error AARCH64_SERROR_TEST='$(AARCH64_SERROR_TEST)' cannot be combined with other compile-affecting test variants)
endif
ifneq ($(strip $(AARCH64_SMP_TEST_NO_ACK_CPU)),)
ifeq ($(filter 0,$(strip $(AARCH64_SMP_TEST_NO_ACK_CPU))),)
$(error AARCH64_SERROR_TEST='$(AARCH64_SERROR_TEST)' cannot be combined with AARCH64_SMP_TEST_NO_ACK_CPU=$(AARCH64_SMP_TEST_NO_ACK_CPU))
endif
endif
endif

AARCH64_EXCEPTION_TRACE_TESTS_VALID := read bad-fp task
ifneq ($(strip $(AARCH64_EXCEPTION_TRACE_TEST)),)
ifeq ($(filter $(AARCH64_EXCEPTION_TRACE_TEST),$(AARCH64_EXCEPTION_TRACE_TESTS_VALID)),)
$(error invalid AARCH64_EXCEPTION_TRACE_TEST='$(AARCH64_EXCEPTION_TRACE_TEST)'; valid: $(AARCH64_EXCEPTION_TRACE_TESTS_VALID))
endif
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_EXCEPTION_TRACE_TEST='$(AARCH64_EXCEPTION_TRACE_TEST)' requires KERNEL_SELFTEST=1)
endif
ifneq ($(strip $(AARCH64_SCHED_TEST) $(AARCH64_AP_SCHED_TEST) $(AARCH64_SCHED_TICK_TEST) $(AARCH64_USER_RUNTIME_TEST) $(AARCH64_M1_TEST) $(AARCH64_EL0_TEST) $(AARCH64_SYNC_FAULT_TEST) $(AARCH64_SERROR_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST)),)
$(error AARCH64_EXCEPTION_TRACE_TEST='$(AARCH64_EXCEPTION_TRACE_TEST)' cannot be combined with other compile-affecting test variants)
endif
ifneq ($(strip $(AARCH64_SMP_TEST_NO_ACK_CPU)),)
ifeq ($(filter 0,$(strip $(AARCH64_SMP_TEST_NO_ACK_CPU))),)
$(error AARCH64_EXCEPTION_TRACE_TEST='$(AARCH64_EXCEPTION_TRACE_TEST)' cannot be combined with AARCH64_SMP_TEST_NO_ACK_CPU=$(AARCH64_SMP_TEST_NO_ACK_CPU))
endif
endif
endif
#
ifneq ($(filter 1,$(AARCH64_SYNC_FAULT_TEST)),)
ifeq ($(filter 1,$(KERNEL_SELFTEST)),)
$(error AARCH64_SYNC_FAULT_TEST=1 requires KERNEL_SELFTEST=1)
endif
endif
#
# AARCH64_SCHED_TEST — the MODE=sched acceptance selector (spec §10.2,
# P5 Task 1). `positive` selects KERNEL_VARIANT=sched (the strict
# evidence image); the four fault values select sched-fault-<value>
# and each maps to exactly ONE numeric compile define in kernel/Makefile
# (AARCH64_SCHED_FAULT_<TOKEN>=1 — no collision with DRIVER_MODEL_FAULT /
# FB_RESOLUTION_TEST / AARCH64_M1_TEST / AARCH64_SYNC_FAULT_TEST).
# Nonempty requires KERNEL_SELFTEST=1 (the sched variants are selftest
# images) and rejects every other compile-affecting test variant so the
# dedicated kernel/image/firmware trees never collide. mk/project.mk
# whitelists the variable across the controlled sub-make boundary.
# ── P1 AP cooperative scheduler selector (AARCH64_AP_SCHED_TEST) ──
# `ap` (P1 Task 4) and `fatal-owner` (P1 Task 3) select the dedicated
# sched-ap / sched-ap-fatal-owner kernel+image trees.  The selector is
# the MODE=sched-ap surface: it must be a selftest build and it refuses
# every other compile-affecting variant (old sched / M1 / sync-fault /
# no-ack / canary / systest) exactly like AARCH64_SCHED_TEST does.
AARCH64_AP_SCHED_TESTS_VALID := ap wake fatal-owner
ifneq ($(strip $(AARCH64_AP_SCHED_TEST)),)
ifeq ($(filter $(AARCH64_AP_SCHED_TEST),$(AARCH64_AP_SCHED_TESTS_VALID)),)
$(error invalid AARCH64_AP_SCHED_TEST='$(AARCH64_AP_SCHED_TEST)'; valid: $(AARCH64_AP_SCHED_TESTS_VALID))
endif
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_AP_SCHED_TEST='$(AARCH64_AP_SCHED_TEST)' requires KERNEL_SELFTEST=1)
endif
ifneq ($(strip $(AARCH64_SCHED_TEST) $(AARCH64_M1_TEST) $(AARCH64_SMP_TEST_NO_ACK_CPU) $(AARCH64_SYNC_FAULT_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST)),)
$(error AARCH64_AP_SCHED_TEST='$(AARCH64_AP_SCHED_TEST)' cannot be combined with other compile-affecting test variants)
endif
endif

# ── Per-CPU tick accounting selector (AARCH64_SCHED_TICK_TEST) ──
# The tick variant (2026-11-06, Task 4) is the ONLY selector of its
# kind: AARCH64_SCHED_TICK_TEST=1 selects the dedicated sched-tick
# kernel/image/firmware trees and compiles the product evidence
# producer (kernel/selftest/test_aarch64_sched_tick.c).  It is a
# selftest image, it refuses every other compile-affecting variant
# (old sched / AP / M1 / no-ack / sync-fault / canary / weak /
# OS01_SYSTEST) exactly like AARCH64_SCHED_TEST does, and only the
# value 1 is accepted.
AARCH64_SCHED_TICK_TESTS_VALID := 1
ifneq ($(strip $(AARCH64_SCHED_TICK_TEST)),)
ifeq ($(filter $(AARCH64_SCHED_TICK_TEST),$(AARCH64_SCHED_TICK_TESTS_VALID)),)
$(error invalid AARCH64_SCHED_TICK_TEST='$(AARCH64_SCHED_TICK_TEST)'; valid: $(AARCH64_SCHED_TICK_TESTS_VALID))
endif
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_SCHED_TICK_TEST='$(AARCH64_SCHED_TICK_TEST)' requires KERNEL_SELFTEST=1)
endif
ifneq ($(strip $(AARCH64_SCHED_TEST) $(AARCH64_AP_SCHED_TEST) $(AARCH64_M1_TEST) $(AARCH64_SYNC_FAULT_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST)),)
$(error AARCH64_SCHED_TICK_TEST='$(AARCH64_SCHED_TICK_TEST)' cannot be combined with other compile-affecting test variants)
endif
ifneq ($(strip $(AARCH64_SMP_TEST_NO_ACK_CPU)),)
ifeq ($(filter 0,$(strip $(AARCH64_SMP_TEST_NO_ACK_CPU))),)
$(error AARCH64_SCHED_TICK_TEST='$(AARCH64_SCHED_TICK_TEST)' cannot be combined with AARCH64_SMP_TEST_NO_ACK_CPU=$(AARCH64_SMP_TEST_NO_ACK_CPU))
endif
endif
endif

# ── User runtime acceptance selector (AARCH64_USER_RUNTIME_TEST) ──
# Selects the dedicated user-runtime-<case> kernel/image/firmware
# trees (e.g. irq, and future asid/smp variants). Requires
# KERNEL_SELFTEST=1 and is mutually exclusive with every other
# compile-affecting test variant.
AARCH64_USER_RUNTIME_TESTS_VALID := irq asid smp migration
ifneq ($(strip $(AARCH64_USER_RUNTIME_TEST)),)
ifeq ($(filter $(AARCH64_USER_RUNTIME_TEST),$(AARCH64_USER_RUNTIME_TESTS_VALID)),)
$(error invalid AARCH64_USER_RUNTIME_TEST='$(AARCH64_USER_RUNTIME_TEST)'; valid: $(AARCH64_USER_RUNTIME_TESTS_VALID))
endif
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_USER_RUNTIME_TEST='$(AARCH64_USER_RUNTIME_TEST)' requires KERNEL_SELFTEST=1)
endif
ifneq ($(strip $(AARCH64_EL0_TEST) $(AARCH64_SCHED_TEST) $(AARCH64_AP_SCHED_TEST) $(AARCH64_SCHED_TICK_TEST) $(AARCH64_M1_TEST) $(AARCH64_SYNC_FAULT_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST) $(BOOT_CONTRACT_CASE)),)
$(error AARCH64_USER_RUNTIME_TEST='$(AARCH64_USER_RUNTIME_TEST)' cannot be combined with other compile-affecting test variants)
endif
ifneq ($(strip $(AARCH64_SMP_TEST_NO_ACK_CPU)),)
ifeq ($(filter 0,$(strip $(AARCH64_SMP_TEST_NO_ACK_CPU))),)
$(error AARCH64_USER_RUNTIME_TEST='$(AARCH64_USER_RUNTIME_TEST)' cannot be combined with AARCH64_SMP_TEST_NO_ACK_CPU=$(AARCH64_SMP_TEST_NO_ACK_CPU))
endif
endif
endif

AARCH64_SCHED_TESTS_VALID := positive sp-top sp-low pc-low pc-data
ifneq ($(strip $(AARCH64_SCHED_TEST)),)
ifeq ($(filter $(AARCH64_SCHED_TEST),$(AARCH64_SCHED_TESTS_VALID)),)
$(error invalid AARCH64_SCHED_TEST='$(AARCH64_SCHED_TEST)'; valid: $(AARCH64_SCHED_TESTS_VALID))
endif
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_SCHED_TEST='$(AARCH64_SCHED_TEST)' requires KERNEL_SELFTEST=1)
endif
ifneq ($(filter 1,$(AARCH64_SYNC_FAULT_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST)),)
$(error AARCH64_SCHED_TEST='$(AARCH64_SCHED_TEST)' cannot be combined with sync-fault/weak-selftest/canary/OS01_SYSTEST)
endif
ifneq ($(strip $(AARCH64_M1_TEST)),)
$(error AARCH64_SCHED_TEST='$(AARCH64_SCHED_TEST)' cannot be combined with AARCH64_M1_TEST='$(AARCH64_M1_TEST)')
endif
ifneq ($(strip $(AARCH64_SMP_TEST_NO_ACK_CPU)),)
ifeq ($(filter 0,$(strip $(AARCH64_SMP_TEST_NO_ACK_CPU))),)
$(error AARCH64_SCHED_TEST='$(AARCH64_SCHED_TEST)' cannot be combined with AARCH64_SMP_TEST_NO_ACK_CPU=$(AARCH64_SMP_TEST_NO_ACK_CPU))
endif
endif
endif
ifneq ($(filter-out 0 1,$(AARCH64_EL0_TEST)),)
$(error invalid AARCH64_EL0_TEST='$(AARCH64_EL0_TEST)'; expected 0 or 1)
endif
ifeq ($(AARCH64_EL0_TEST),1)
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_EL0_TEST=1 requires KERNEL_SELFTEST=1)
endif
ifneq ($(strip $(BOOT_CONTRACT_CASE) $(AARCH64_M1_TEST) $(AARCH64_SCHED_TEST) $(AARCH64_AP_SCHED_TEST) $(AARCH64_SCHED_TICK_TEST) $(AARCH64_USER_RUNTIME_TEST) $(filter-out 0,$(AARCH64_SYNC_FAULT_TEST) $(AARCH64_SMP_TEST_NO_ACK_CPU) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST))),)
$(error AARCH64_EL0_TEST=1 cannot be combined with another test variant)
endif
KERNEL_VARIANT := m4-1
else ifneq ($(strip $(BOOT_CONTRACT_CASE)),)
KERNEL_VARIANT := boot-contract-$(BOOT_CONTRACT_CASE)
else ifneq ($(strip $(AARCH64_M1_TEST)),)
ifeq ($(filter $(AARCH64_M1_TEST),sparse arena-exhaust table-exhaust ap-bad-root),)
$(error invalid AARCH64_M1_TEST='$(AARCH64_M1_TEST)')
endif
ifneq ($(KERNEL_SELFTEST),1)
$(error AARCH64_M1_TEST requires KERNEL_SELFTEST=1)
endif
ifneq ($(filter 1,$(AARCH64_SYNC_FAULT_TEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(KERNEL_CANARY_SELFTEST)),)
$(error AARCH64_M1_TEST cannot be combined with sync-fault/weak-selftest/canary)
endif
KERNEL_VARIANT := m1-$(AARCH64_M1_TEST)
else ifneq ($(strip $(AARCH64_SCHED_TEST)),)
# Order matters: the sched acceptance variants win over every other
# selftest flavor (all combinations were rejected above, so the branch
# is only reached alone). `positive` owns kernel/sched + image/sched;
# each fault owns kernel/sched-fault-<value> + image/sched-fault-<value>.
# Further down, sync-fault likewise wins over weak-selftest when both
# KERNEL_TEST_FORCE_NO_RNDRRS=1 and AARCH64_SYNC_FAULT_TEST=1 are set,
# so the dedicated image path (`image/sync-fault/`) matches the
# harness's sync-fault target path.
KERNEL_VARIANT := $(if $(filter positive,$(AARCH64_SCHED_TEST)),sched,sched-fault-$(AARCH64_SCHED_TEST))
else ifneq ($(strip $(AARCH64_AP_SCHED_TEST)),)
ifeq ($(filter wake,$(AARCH64_AP_SCHED_TEST)),wake)
KERNEL_VARIANT := sched-wake
else
KERNEL_VARIANT := sched-ap$(if $(filter fatal-owner,$(AARCH64_AP_SCHED_TEST)),-fatal-owner)
endif
else ifneq ($(strip $(AARCH64_SCHED_TICK_TEST)),)
# The per-CPU tick variant owns its own kernel/image/firmware trees.
KERNEL_VARIANT := sched-tick
else ifneq ($(strip $(AARCH64_USER_RUNTIME_TEST)),)
KERNEL_VARIANT := user-runtime-$(AARCH64_USER_RUNTIME_TEST)
else ifneq ($(strip $(AARCH64_SERROR_TEST)),)
KERNEL_VARIANT := serror-$(AARCH64_SERROR_TEST)
else ifneq ($(strip $(AARCH64_EXCEPTION_TRACE_TEST)),)
KERNEL_VARIANT := exception-trace-$(AARCH64_EXCEPTION_TRACE_TEST)
else ifneq ($(filter 1,$(AARCH64_SYNC_FAULT_TEST)),)
KERNEL_VARIANT := sync-fault
else ifneq ($(filter 1,$(KERNEL_TEST_FORCE_NO_RNDRRS)),)
KERNEL_VARIANT := weak-selftest
else ifneq ($(filter 1,$(KERNEL_SELFTEST)),)
KERNEL_VARIANT := selftest
else ifneq ($(filter 1,$(KERNEL_CANARY_SELFTEST)),)
KERNEL_VARIANT := canary-selftest
else
KERNEL_VARIANT :=
endif
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))
# Compile-affecting variant (OS01_SYSTEST / USER_DEBUG re-key the user dirs).
# project.mk sets USER_VARIANT before including this profile; the ?= default
# lets component sub-makes that include ONLY the profile resolve it from
# OS01_SYSTEST. Since the 2026-11-18 userland capability the aarch64 build
# produces user ELFs, keyed the same way.
USER_VARIANT ?= $(if $(filter 1,$(OS01_SYSTEST)),systest)$(if $(filter 1,$(USER_DEBUG)),user-debug)
USER_BUILD_DIR := $(BUILD_DIR)/user$(if $(USER_VARIANT),/$(USER_VARIANT))
USER_ARTIFACT_DIR := $(BUILD_DIR)/artifacts/user$(if $(USER_VARIANT),/$(USER_VARIANT))
# Directory apps that link the gfx/gui/lvgl/mbedTLS sysroot libraries are
# x86-only until those libraries exist on this arch (user/apps.mk filters
# these out of USER_PROGRAMS).
USER_EXCLUDED_PROGRAMS := desktop doom doom_testctl test_gui_fault test_gui_input test_lvgl tetris usb_hid_probe test_display_owner test_gfx netmodeltest systest test_monitor test_monitor_test
UEFI_BUILD_DIR := $(BUILD_DIR)/uefi
UEFI_RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
UEFI_EFI   ?= $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI
# Host unit tests are an OS01 build: every test object/binary lives under
# the profile's host-test dir (hosttests/Makefile includes this profile).
HOST_TEST_BUILD_DIR := $(BUILD_DIR)/host-test

# Staging roots: component installs write ONLY their private staging tree
# (INSTALL_ROOT), never the final sysroot. The single writer of the sysroot
# is sysroot.mk (Task 4).
STAGING_DIR := $(BUILD_DIR)/staging

# Sysroot stage set (2026-11-18 userland capability): the aarch64 sysroot
# stages kernel-headers, libc (libc.a + libk.a) and the compat stubs — no
# libgfx/libgui/mbedTLS on this arch yet. SYSROOT_ARCH selects the libc
# cross-arch for the staging sub-make.
SYSROOT_ARCH   := aarch64
SYSROOT_STAGES := kernel-headers libc libgfx compat-libs

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
# LLVM tool discovery (mirrors mk/toolchains/clang.mk for the sysroot
# staging recipes, which this profile now drives under the userland
# capability).
LLVM_AR        ?= llvm-ar
OBJ_CPY        ?= llvm-objcopy
LLVM_OBJDUMP   ?= llvm-objdump
AARCH64_QEMU   ?= qemu-system-aarch64
AARCH64_UEFI_FIRMWARE_SOURCE ?= https://retrage.github.io/edk2-nightly/bin/RELEASEAARCH64_QEMU_EFI.fd
RUNTIME_MACHINE_kernel := EM_AARCH64

# ── User runtime consumer (2026-11-18, M4-remaining product userspace) ──
# The selfhosted provider compiles runtime/builtins/udivti3.c for the user
# target; the sources are arch-portable C, so only the target tuple differs
# from the x86 declaration.  The host compiler identity inputs (CLANG /
# CLANG_ID / CLANG_RESOURCE_DIR / LLVM_NM) mirror mk/toolchains/clang.mk,
# which this profile intentionally does not include.
CLANG            ?= clang
CLANG_ID         := $(shell $(CLANG) --version 2>/dev/null | head -1)
CLANG_RESOURCE_DIR := $(shell $(CLANG) -print-resource-dir 2>/dev/null)
LLVM_NM          ?= llvm-nm
RUNTIME_TARGET_user := $(TARGET_TRIPLE)
# Freestanding non-PIE static ring-3 ELFs: the user code model and flags —
# FP/SIMD is enabled for EL0 (CPACR_EL1.FPEN = 0b11, saved/restored per
# user task in arch_task_switch).
RUNTIME_CFLAGS_user := -ffreestanding -fno-builtin -fno-stack-protector \
                       -fno-asynchronous-unwind-tables -fno-pic -fno-pie
RUNTIME_OBJECT_FORMAT_user := ELF64
RUNTIME_MACHINE_user := EM_AARCH64
RUNTIME_ABI_user := aarch64-aapcs

# aarch64 link / UEFI / run parameters.
include $(OS01_ROOT)/mk/targets/aarch64.mk
