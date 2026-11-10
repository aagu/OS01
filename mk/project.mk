# ── OS01 project configuration ──────────────────────────────────
# Included at the very top of the root Makefile. Selects the profile,
# includes it (toolchain + target config), and defines the capability
# gate and the controlled recursive-Make helper (os01_submake).
#
# Can also be invoked standalone for profile introspection:
#   make -f mk/project.mk PROFILE=<name> -n

PROFILE ?= x86_64-clang
# Default profile — owns the project-root kernel.bin / disk.img compat copies;
# `clean` removes them only when PROFILE == DEFAULT_PROFILE.
DEFAULT_PROFILE ?= x86_64-clang
KERNEL_SELFTEST ?=
KERNEL_CANARY_SELFTEST ?=
# Extra kernel compile flags (test/debug knob; also the flags-cache contract
# test's handle). Whitelisted below so it crosses the controlled sub-make
# boundary, and folded into ALL_CFLAGS by kernel/Makefile so the CFLAGS
# fingerprint stamp sees it.
KERNEL_EXTRA_CFLAGS ?=

# ── DRIVER_MODEL_FAULT — driver model matrix fault fixture ─────────
# Each non-none value enables a kernel-internal fault injection that the
# matrix harness drives per-case.  The enum lives here so both the
# profile (kernel build dir) and the matrix harness (path slugs) reject
# unknown values at parse time.  Non-none + OS01_SYSTEST / OS01_NETTEST
# / KERNEL_SELFTEST / KERNEL_CANARY_SELFTEST / KERNEL_TEST_FORCE_NO_RNDRRS
# / non-default INITTAB_FILE is refused — the matrix runs against the
# normal inittab only.
DRIVER_MODEL_FAULT ?= none
DRIVER_MODEL_FAULTS_VALID := none observe bad-nic-bar adapter-fail ahci-empty irq-conflict
ifeq ($(filter $(DRIVER_MODEL_FAULT),$(DRIVER_MODEL_FAULTS_VALID)),)
$(error DRIVER_MODEL_FAULT='$(DRIVER_MODEL_FAULT)' is not one of: $(DRIVER_MODEL_FAULTS_VALID))
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
# DRIVER_MODEL_FAULT=none is the canonical build; no extra rejection needed.
else
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_CANARY_SELFTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(CAPACITY_TEST) $(ARCH6_FSSTRESS_TEST)),)
$(error DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT) cannot be combined with KERNEL_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_CANARY_SELFTEST/KERNEL_TEST_FORCE_NO_RNDRRS/CAPACITY_TEST/ARCH6_FSSTRESS_TEST)
endif
ifneq ($(filter config/inittab.test,$(INITTAB_FILE)),)
$(error DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT) cannot be combined with INITTAB_FILE=config/inittab.test)
endif
endif

# ── FB_RESOLUTION_TEST — test-only framebuffer fault surface ────
# 1 selects the isolated resolution-test kernel/user/image variant and
# compiles the /dev/fbtest control node + injection hooks.  0 (the
# production default) installs no control node, command or injection
# branch.  It is test config only and must not be mixed with any other
# test/driver-model variant.
FB_RESOLUTION_TEST ?= 0
ifeq ($(filter $(FB_RESOLUTION_TEST),0 1),)
$(error FB_RESOLUTION_TEST='$(FB_RESOLUTION_TEST)' must be 0 or 1)
endif
ifneq ($(filter 1,$(FB_RESOLUTION_TEST)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(CAPACITY_TEST)),)
$(error FB_RESOLUTION_TEST=1 cannot be combined with KERNEL_SELFTEST/KERNEL_CANARY_SELFTEST/OS01_SYSTEST/OS01_NETTEST/CAPACITY_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error FB_RESOLUTION_TEST=1 cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
endif

# ── X86_INTR_FAULT_TEST — isolated destructive dispatch probe ────
# The intr-dispatch phase-2 work needs ISOLATED evidence for the two
# fatal entries that cannot be exercised through the ordinary boot or
# the kernel-selftest image: the kernel #BP (int3) trap and a real
# hardware NMI injected over QMP.  Each case gets its OWN kernel and
# image variant (intr-x86-fault-bp / intr-x86-fault-nmi), so the two
# cases never share objects or images and the normal build never
# carries the probe.
#
# The probe-selection MACRO travels through KERNEL_EXTRA_CFLAGS — the
# channel that already crosses the controlled sub-make boundary
# (OS01_SUBMAKE_ALLOWED below).  The variant slug is derived from that
# SAME macro, so the compiled probe and the artifact paths can never
# disagree.  The selector below is the user-facing knob; it only
# contributes the macro.
#
# The selector is x86-only (the aarch64 profile rejects it) and is
# refused in combination with every other destructive / test variant.
X86_INTR_FAULT_TEST ?=
X86_INTR_FAULT_TESTS_VALID := none bp nmi
ifneq ($(strip $(X86_INTR_FAULT_TEST)),)
ifeq ($(filter $(X86_INTR_FAULT_TEST),$(X86_INTR_FAULT_TESTS_VALID)),)
$(error X86_INTR_FAULT_TEST='$(X86_INTR_FAULT_TEST)' is not one of: $(X86_INTR_FAULT_TESTS_VALID))
endif
endif
ifeq ($(X86_INTR_FAULT_TEST),bp)
X86_INTR_FAULT_CFLAGS := -DX86_INTR_FAULT_TEST_BP=1
else ifeq ($(X86_INTR_FAULT_TEST),nmi)
X86_INTR_FAULT_CFLAGS := -DX86_INTR_FAULT_TEST_NMI=1
else
X86_INTR_FAULT_CFLAGS :=
endif
ifneq ($(strip $(X86_INTR_FAULT_CFLAGS)),)
ifeq ($(findstring $(X86_INTR_FAULT_CFLAGS),$(KERNEL_EXTRA_CFLAGS)),)
# `override` so a command-line KERNEL_EXTRA_CFLAGS keeps its own flags
# and still gains the probe selection.
override KERNEL_EXTRA_CFLAGS := $(strip $(KERNEL_EXTRA_CFLAGS) $(X86_INTR_FAULT_CFLAGS))
endif
endif
# The effective case — derived from the macro, which is the single
# source of truth (the selector above only contributes to it).
X86_INTR_FAULT_CASE := $(if $(findstring X86_INTR_FAULT_TEST_BP,$(KERNEL_EXTRA_CFLAGS)),bp,$(if $(findstring X86_INTR_FAULT_TEST_NMI,$(KERNEL_EXTRA_CFLAGS)),nmi,))
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_CANARY_SELFTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(CAPACITY_TEST)),)
$(error X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE) is an x86-only destructive probe and cannot be combined with KERNEL_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_CANARY_SELFTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/CAPACITY_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
# DRIVER_MODEL_FAULT=none is the canonical build; the destructive probe and
# the driver-model fault fixture must never share a build.
else
$(error X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE) cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
endif

# ── X86_SMP_BOOT_TEST — BOOT-1 CPU-lifecycle fault matrix ────────
# The x86 isolation matrix needs three controlled CPU1 boot faults
# (delay-online / no-online / no-scheduler) that CANNOT be exercised on
# the normal image: each case gets its OWN kernel/image variant
# (boot-lifecycle-<case>), and the normal build must never carry the
# injection code or the fault marker strings.
#
# Same plumbing as X86_INTR_FAULT_TEST: the selection MACRO travels
# through KERNEL_EXTRA_CFLAGS — the channel that already crosses the
# controlled sub-make boundary (OS01_SUBMAKE_ALLOWED below) — and the
# variant slug is derived from that SAME macro, so the compiled fault
# and the artifact paths can never disagree.  The injection code lives
# in kernel/arch/x86_64/smp/smp.c under the macros ONLY, so a normal
# kernel contains neither the injection nor the markers (the suite's
# ELF audit proves it).
X86_SMP_BOOT_TEST ?=
X86_SMP_BOOT_TESTS_VALID := none delay-online no-online no-scheduler
ifneq ($(strip $(X86_SMP_BOOT_TEST)),)
ifeq ($(filter $(X86_SMP_BOOT_TEST),$(X86_SMP_BOOT_TESTS_VALID)),)
$(error X86_SMP_BOOT_TEST='$(X86_SMP_BOOT_TEST)' is not one of: $(X86_SMP_BOOT_TESTS_VALID))
endif
endif
ifeq ($(X86_SMP_BOOT_TEST),delay-online)
X86_SMP_BOOT_CFLAGS := -DX86_SMP_BOOT_TEST_DELAY_ONLINE=1
else ifeq ($(X86_SMP_BOOT_TEST),no-online)
X86_SMP_BOOT_CFLAGS := -DX86_SMP_BOOT_TEST_NO_ONLINE=1
else ifeq ($(X86_SMP_BOOT_TEST),no-scheduler)
X86_SMP_BOOT_CFLAGS := -DX86_SMP_BOOT_TEST_NO_SCHEDULER=1
else
X86_SMP_BOOT_CFLAGS :=
endif
ifneq ($(strip $(X86_SMP_BOOT_CFLAGS)),)
ifeq ($(findstring $(X86_SMP_BOOT_CFLAGS),$(KERNEL_EXTRA_CFLAGS)),)
override KERNEL_EXTRA_CFLAGS := $(strip $(KERNEL_EXTRA_CFLAGS) $(X86_SMP_BOOT_CFLAGS))
endif
endif
# The effective case — derived from the macro, which is the single
# source of truth (the selector above only contributes to it).
X86_SMP_BOOT_CASE := $(if $(findstring X86_SMP_BOOT_TEST_DELAY_ONLINE,$(KERNEL_EXTRA_CFLAGS)),delay-online,$(if $(findstring X86_SMP_BOOT_TEST_NO_ONLINE,$(KERNEL_EXTRA_CFLAGS)),no-online,$(if $(findstring X86_SMP_BOOT_TEST_NO_SCHEDULER,$(KERNEL_EXTRA_CFLAGS)),no-scheduler,)))
ifneq ($(strip $(X86_SMP_BOOT_CASE)),)
# Fault images boot normal init until the fatal point: they must never
# share a build with another test variant.
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_CANARY_SELFTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(CAPACITY_TEST) $(ARCH6_FSSTRESS_TEST)),)
$(error X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE) is an x86-only fault variant and cannot be combined with KERNEL_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_CANARY_SELFTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/CAPACITY_TEST/ARCH6_FSSTRESS_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE) cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE) cannot be combined with X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE))
endif
endif

# ── X86_BOOT_IRQ_TIME_TEST — BOOT-2 early-IRQ fault matrix ───────
# Same plumbing as the BOOT-1/X86_INTR selectors: the macro travels
# through KERNEL_EXTRA_CFLAGS (the controlled sub-make channel) and the
# variant slug is derived from that macro, so the compiled fixture and
# the artifact paths can never disagree.  Cases:
#   force-rtc      bypass the CPUID nominal TSC answer (real RTC window)
#   rtc-timeout    force-rtc + keep the RTC route masked (bounded timeout)
#   bsp-lapic-fail the BSP handoff fails; AP timer starts unaffected
#   unsafe-open    open the controlled window before the context exists
# The fixture (kernel/selftest/boot_irq_time_fixture.c) and the entry.S
# test tail are compiled ONLY under these macros — a normal kernel
# carries neither fixture symbols nor marker strings.
X86_BOOT_IRQ_TIME_TEST ?=
X86_BOOT_IRQ_TIME_TESTS_VALID := none force-rtc rtc-timeout bsp-lapic-fail unsafe-open
ifneq ($(strip $(X86_BOOT_IRQ_TIME_TEST)),)
ifeq ($(filter $(X86_BOOT_IRQ_TIME_TEST),$(X86_BOOT_IRQ_TIME_TESTS_VALID)),)
$(error X86_BOOT_IRQ_TIME_TEST='$(X86_BOOT_IRQ_TIME_TEST)' is not one of: $(X86_BOOT_IRQ_TIME_TESTS_VALID))
endif
endif
ifeq ($(X86_BOOT_IRQ_TIME_TEST),none)
X86_BOOT_IRQ_TIME_CFLAGS :=
else ifneq ($(strip $(X86_BOOT_IRQ_TIME_TEST)),)
# Single-token macro: the kernel build derives the per-case macros (the
# user/runtime sub-make chain cannot take multi-word KERNEL_EXTRA values).
X86_BOOT_IRQ_TIME_CFLAGS := -DX86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_TEST)
else
X86_BOOT_IRQ_TIME_CFLAGS :=
endif
ifneq ($(strip $(X86_BOOT_IRQ_TIME_CFLAGS)),)
ifeq ($(findstring X86_BOOT_IRQ_TIME_TEST=,$(KERNEL_EXTRA_CFLAGS)),)
override KERNEL_EXTRA_CFLAGS := $(strip $(KERNEL_EXTRA_CFLAGS) $(X86_BOOT_IRQ_TIME_CFLAGS))
endif
endif
X86_BOOT_IRQ_TIME_CASE := $(if $(findstring X86_BOOT_IRQ_TIME_TEST=rtc-timeout,$(KERNEL_EXTRA_CFLAGS)),rtc-timeout,$(if $(findstring X86_BOOT_IRQ_TIME_TEST=force-rtc,$(KERNEL_EXTRA_CFLAGS)),force-rtc,$(if $(findstring X86_BOOT_IRQ_TIME_TEST=bsp-lapic-fail,$(KERNEL_EXTRA_CFLAGS)),bsp-lapic-fail,$(if $(findstring X86_BOOT_IRQ_TIME_TEST=unsafe-open,$(KERNEL_EXTRA_CFLAGS)),unsafe-open,))))
ifneq ($(strip $(X86_BOOT_IRQ_TIME_CASE)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_CANARY_SELFTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(CAPACITY_TEST) $(ARCH6_FSSTRESS_TEST)),)
$(error X86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_CASE) cannot be combined with KERNEL_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_CANARY_SELFTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/CAPACITY_TEST/ARCH6_FSSTRESS_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error X86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_CASE) cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error X86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_CASE) cannot be combined with X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE))
endif
ifneq ($(strip $(X86_SMP_BOOT_CASE)),)
$(error X86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_CASE) cannot be combined with X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE))
endif
endif

# ── BOOT_FAILURE_TEST — BOOT-3 startup failure policy matrix ───────
BOOT_FAILURE_TEST ?=
BOOT_FAILURE_TESTS_VALID := none required-subsys optional-subsys registration root tty pty stdio init child-fds
ifneq ($(strip $(BOOT_FAILURE_TEST)),)
ifeq ($(filter $(BOOT_FAILURE_TEST),$(BOOT_FAILURE_TESTS_VALID)),)
$(error BOOT_FAILURE_TEST='$(BOOT_FAILURE_TEST)' is not one of: $(BOOT_FAILURE_TESTS_VALID))
endif
endif
ifeq ($(BOOT_FAILURE_TEST),none)
BOOT_FAILURE_CFLAGS :=
else ifneq ($(strip $(BOOT_FAILURE_TEST)),)
BOOT_FAILURE_CFLAGS := -DBOOT_FAILURE_TEST=$(BOOT_FAILURE_TEST)
else
BOOT_FAILURE_CFLAGS :=
endif
ifneq ($(strip $(BOOT_FAILURE_CFLAGS)),)
ifeq ($(findstring BOOT_FAILURE_TEST=,$(KERNEL_EXTRA_CFLAGS)),)
override KERNEL_EXTRA_CFLAGS := $(strip $(KERNEL_EXTRA_CFLAGS) $(BOOT_FAILURE_CFLAGS))
endif
endif
BOOT_FAILURE_CASE := $(if $(findstring BOOT_FAILURE_TEST=required-subsys,$(KERNEL_EXTRA_CFLAGS)),required-subsys,$(if $(findstring BOOT_FAILURE_TEST=optional-subsys,$(KERNEL_EXTRA_CFLAGS)),optional-subsys,$(if $(findstring BOOT_FAILURE_TEST=registration,$(KERNEL_EXTRA_CFLAGS)),registration,$(if $(findstring BOOT_FAILURE_TEST=root,$(KERNEL_EXTRA_CFLAGS)),root,$(if $(findstring BOOT_FAILURE_TEST=tty,$(KERNEL_EXTRA_CFLAGS)),tty,$(if $(findstring BOOT_FAILURE_TEST=pty,$(KERNEL_EXTRA_CFLAGS)),pty,$(if $(findstring BOOT_FAILURE_TEST=stdio,$(KERNEL_EXTRA_CFLAGS)),stdio,$(if $(findstring BOOT_FAILURE_TEST=init,$(KERNEL_EXTRA_CFLAGS)),init,$(if $(findstring BOOT_FAILURE_TEST=child-fds,$(KERNEL_EXTRA_CFLAGS)),child-fds,)))))))))
ifneq ($(strip $(BOOT_FAILURE_CASE)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_CANARY_SELFTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(CAPACITY_TEST) $(ARCH6_FSSTRESS_TEST)),)
$(error BOOT_FAILURE_TEST=$(BOOT_FAILURE_CASE) cannot be combined with KERNEL_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_CANARY_SELFTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/CAPACITY_TEST/ARCH6_FSSTRESS_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error BOOT_FAILURE_TEST=$(BOOT_FAILURE_CASE) cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error BOOT_FAILURE_TEST=$(BOOT_FAILURE_CASE) cannot be combined with X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE))
endif
ifneq ($(strip $(X86_SMP_BOOT_CASE)),)
$(error BOOT_FAILURE_TEST=$(BOOT_FAILURE_CASE) cannot be combined with X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE))
endif
ifneq ($(strip $(X86_BOOT_IRQ_TIME_CASE)),)
$(error BOOT_FAILURE_TEST=$(BOOT_FAILURE_CASE) cannot be combined with X86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_CASE))
endif
endif

# ── BOOT_CONTRACT_TEST — BOOT-4 stage contracts and dependency matrix ─
BOOT_CONTRACT_TEST ?=
BOOT_CONTRACT_TESTS_VALID := none reverse missing cycle
ifneq ($(strip $(BOOT_CONTRACT_TEST)),)
ifeq ($(filter $(BOOT_CONTRACT_TEST),$(BOOT_CONTRACT_TESTS_VALID)),)
$(error BOOT_CONTRACT_TEST='$(BOOT_CONTRACT_TEST)' is not one of: $(BOOT_CONTRACT_TESTS_VALID))
endif
endif
ifeq ($(BOOT_CONTRACT_TEST),none)
BOOT_CONTRACT_CFLAGS :=
else ifneq ($(strip $(BOOT_CONTRACT_TEST)),)
BOOT_CONTRACT_CFLAGS := -DBOOT_CONTRACT_TEST=$(BOOT_CONTRACT_TEST)
else
BOOT_CONTRACT_CFLAGS :=
endif
ifneq ($(strip $(BOOT_CONTRACT_CFLAGS)),)
ifeq ($(findstring BOOT_CONTRACT_TEST=,$(KERNEL_EXTRA_CFLAGS)),)
override KERNEL_EXTRA_CFLAGS := $(strip $(KERNEL_EXTRA_CFLAGS) $(BOOT_CONTRACT_CFLAGS))
endif
endif
BOOT_CONTRACT_CASE := $(if $(findstring BOOT_CONTRACT_TEST=reverse,$(KERNEL_EXTRA_CFLAGS)),reverse,$(if $(findstring BOOT_CONTRACT_TEST=missing,$(KERNEL_EXTRA_CFLAGS)),missing,$(if $(findstring BOOT_CONTRACT_TEST=cycle,$(KERNEL_EXTRA_CFLAGS)),cycle,)))
ifneq ($(strip $(BOOT_CONTRACT_CASE)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_CANARY_SELFTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(CAPACITY_TEST) $(ARCH6_FSSTRESS_TEST)),)
$(error BOOT_CONTRACT_TEST=$(BOOT_CONTRACT_CASE) cannot be combined with KERNEL_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_CANARY_SELFTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/CAPACITY_TEST/ARCH6_FSSTRESS_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error BOOT_CONTRACT_TEST=$(BOOT_CONTRACT_CASE) cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error BOOT_CONTRACT_TEST=$(BOOT_CONTRACT_CASE) cannot be combined with X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE))
endif
ifneq ($(strip $(X86_SMP_BOOT_CASE)),)
$(error BOOT_CONTRACT_TEST=$(BOOT_CONTRACT_CASE) cannot be combined with X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE))
endif
ifneq ($(strip $(X86_BOOT_IRQ_TIME_CASE)),)
$(error BOOT_CONTRACT_TEST=$(BOOT_CONTRACT_CASE) cannot be combined with X86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_CASE))
endif
ifneq ($(strip $(BOOT_FAILURE_CASE)),)
$(error BOOT_CONTRACT_TEST=$(BOOT_CONTRACT_CASE) cannot be combined with BOOT_FAILURE_TEST=$(BOOT_FAILURE_CASE))
endif
endif

# ── BOOT_SERVICES_TEST — BOOT-5 scheduler and services test ─────────
BOOT_SERVICES_TEST ?= none
BOOT_SERVICES_TESTS_VALID := none overlap
ifeq ($(filter $(BOOT_SERVICES_TEST),$(BOOT_SERVICES_TESTS_VALID)),)
$(error unsupported BOOT_SERVICES_TEST='$(BOOT_SERVICES_TEST)', must be one of: $(BOOT_SERVICES_TESTS_VALID))
endif
ifeq ($(BOOT_SERVICES_TEST),none)
BOOT_SERVICES_CFLAGS :=
else ifneq ($(strip $(BOOT_SERVICES_TEST)),)
BOOT_SERVICES_CFLAGS := -DBOOT_SERVICES_TEST=$(BOOT_SERVICES_TEST)
else
BOOT_SERVICES_CFLAGS :=
endif
ifneq ($(strip $(BOOT_SERVICES_CFLAGS)),)
ifeq ($(findstring BOOT_SERVICES_TEST=,$(KERNEL_EXTRA_CFLAGS)),)
override KERNEL_EXTRA_CFLAGS := $(strip $(KERNEL_EXTRA_CFLAGS) $(BOOT_SERVICES_CFLAGS))
endif
endif
BOOT_SERVICES_CASE := $(if $(findstring BOOT_SERVICES_TEST=overlap,$(KERNEL_EXTRA_CFLAGS)),overlap,$(if $(filter overlap,$(BOOT_SERVICES_TEST)),overlap,))
ifneq ($(strip $(BOOT_SERVICES_CASE)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_CANARY_SELFTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(CAPACITY_TEST) $(ARCH6_FSSTRESS_TEST)),)
$(error BOOT_SERVICES_TEST=$(BOOT_SERVICES_CASE) cannot be combined with KERNEL_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_CANARY_SELFTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/CAPACITY_TEST/ARCH6_FSSTRESS_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error BOOT_SERVICES_TEST=$(BOOT_SERVICES_CASE) cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error BOOT_SERVICES_TEST=$(BOOT_SERVICES_CASE) cannot be combined with X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE))
endif
ifneq ($(strip $(X86_SMP_BOOT_CASE)),)
$(error BOOT_SERVICES_TEST=$(BOOT_SERVICES_CASE) cannot be combined with X86_SMP_BOOT_TEST=$(X86_SMP_BOOT_CASE))
endif
ifneq ($(strip $(X86_BOOT_IRQ_TIME_CASE)),)
$(error BOOT_SERVICES_TEST=$(BOOT_SERVICES_CASE) cannot be combined with X86_BOOT_IRQ_TIME_TEST=$(X86_BOOT_IRQ_TIME_CASE))
endif
ifneq ($(strip $(BOOT_FAILURE_CASE)),)
$(error BOOT_SERVICES_TEST=$(BOOT_SERVICES_CASE) cannot be combined with BOOT_FAILURE_TEST=$(BOOT_FAILURE_CASE))
endif
ifneq ($(strip $(BOOT_CONTRACT_CASE)),)
$(error BOOT_SERVICES_TEST=$(BOOT_SERVICES_CASE) cannot be combined with BOOT_CONTRACT_TEST=$(BOOT_CONTRACT_CASE))
endif
endif

# ── CAPACITY_TEST — isolated capacity variant (spec §10) ──────────
CAPACITY_TEST ?= 0
ifeq ($(filter $(CAPACITY_TEST),0 1),)
$(error CAPACITY_TEST='$(CAPACITY_TEST)' must be 0 or 1)
endif
ifneq ($(filter 1,$(CAPACITY_TEST)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(ARCH6_FSSTRESS_TEST)),)
$(error CAPACITY_TEST=1 cannot be combined with KERNEL_SELFTEST/KERNEL_CANARY_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/ARCH6_FSSTRESS_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error CAPACITY_TEST=1 cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error CAPACITY_TEST=1 cannot be combined with X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE))
endif
endif

# ── ARCH6_FSSTRESS_TEST — four-CPU FS stress variant (ARCH-6) ──────
# Image-variant switch only: no kernel fixture consumes the flag; it
# selects config/inittab.arch6-fsstress (root Makefile) and the stress
# program's rootfs inclusion (config/rootfs.mk).
ARCH6_FSSTRESS_TEST ?= 0
ifeq ($(filter $(ARCH6_FSSTRESS_TEST),0 1),)
$(error ARCH6_FSSTRESS_TEST='$(ARCH6_FSSTRESS_TEST)' must be 0 or 1)
endif
ifneq ($(filter 1,$(ARCH6_FSSTRESS_TEST)),)
ifneq ($(filter 1,$(TERM_PRESENT_TEST)),)
$(error ARCH6_FSSTRESS_TEST=1 cannot be combined with TERM_PRESENT_TEST=1)
endif
ifneq ($(INITTAB_FILE),config/inittab.arch6-fsstress)
$(error ARCH6_FSSTRESS_TEST=1 requires INITTAB_FILE=config/inittab.arch6-fsstress)
endif
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(KERNEL_CANARY_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(KERNEL_TEST_FORCE_NO_RNDRRS) $(FB_RESOLUTION_TEST) $(CAPACITY_TEST)),)
$(error ARCH6_FSSTRESS_TEST=1 cannot be combined with KERNEL_SELFTEST/KERNEL_CANARY_SELFTEST/OS01_SYSTEST/OS01_NETTEST/KERNEL_TEST_FORCE_NO_RNDRRS/FB_RESOLUTION_TEST/CAPACITY_TEST)
endif
ifneq ($(filter none,$(DRIVER_MODEL_FAULT)),)
else
$(error ARCH6_FSSTRESS_TEST=1 cannot be combined with DRIVER_MODEL_FAULT=$(DRIVER_MODEL_FAULT))
endif
ifneq ($(strip $(X86_INTR_FAULT_CASE)),)
$(error ARCH6_FSSTRESS_TEST=1 cannot be combined with X86_INTR_FAULT_TEST=$(X86_INTR_FAULT_CASE))
endif
endif

# ── Variant slugs (BEFORE the profile include!) ───────────────
# IMAGE_VARIANT — the image/manifest dirs' variant suffix, derived from the
# explicit switch variables. The root Makefile applies OS01_SYSTEST /
# OS01_NETTEST to INITTAB_FILE BEFORE including project.mk, so all three
# sources are resolved here. Each variant gets its own isolated image dir
# (build/<profile>/image/<variant>/); a variant build NEVER writes the normal
# image. USER_VARIANT — the compile-affecting variant: OS01_SYSTEST changes
# user CFLAGS (-DOS01_SYSTEST) and USER_DEBUG changes them too
# (-DUSER_DEBUG=1, the user-space debug log), so those variants re-key the
# user build/artifact dirs (build/<profile>/user/<variant> and
# build/<profile>/artifacts/user/<variant>). Both are immediate (:=) so the
# profile file and every component that consumes them sees the resolved
# value at parse time — defining them after the profile include would make
# the profile's own USER_BUILD_DIR/USER_ARTIFACT_DIR compute with an empty
# USER_VARIANT.
ifneq ($(filter 1,$(KERNEL_CANARY_SELFTEST)),)
ifneq ($(filter 1,$(KERNEL_SELFTEST) $(OS01_SYSTEST) $(OS01_NETTEST) $(CAPACITY_TEST)),)
$(error KERNEL_CANARY_SELFTEST=1 cannot be combined with KERNEL_SELFTEST=1, OS01_SYSTEST=1, OS01_NETTEST=1, or CAPACITY_TEST=1)
endif
endif
# TERM_PRESENT_TEST — the terminal-present E2E suite needs the terminal's
# per-second [term-present] records on the serial port.  Those records are a
# user-space DEBUG-level diagnostic (user/common/user_log.h), so the suite
# builds its own image + user variant with USER_DEBUG=1; the normal image
# keeps the serial port clean.
ifeq ($(filter 1,$(TERM_PRESENT_TEST)),1)
USER_DEBUG := 1
endif

IMAGE_VARIANT := $(strip $(if $(filter 1,$(ARCH6_FSSTRESS_TEST)),arch6-fsstress)$(if $(filter 1,$(CAPACITY_TEST)),arch10-capacity)$(if $(filter 1,$(OS01_SYSTEST)),systest)$(if $(filter 1,$(OS01_NETTEST)),nettest)$(if $(filter config/inittab.test,$(INITTAB_FILE)),inittab-test)$(if $(filter 1,$(KERNEL_SELFTEST)),selftest)$(if $(filter 1,$(KERNEL_CANARY_SELFTEST)),canary-selftest)$(if $(filter 1,$(FB_RESOLUTION_TEST)),resolution-test)$(if $(filter 1,$(TERM_PRESENT_TEST)),terminal-present)$(if $(filter none,$(DRIVER_MODEL_FAULT)),,driver-model-$(DRIVER_MODEL_FAULT))$(if $(strip $(X86_INTR_FAULT_CASE)),intr-x86-fault-$(X86_INTR_FAULT_CASE))$(if $(strip $(X86_SMP_BOOT_CASE)),boot-lifecycle-$(X86_SMP_BOOT_CASE))$(if $(strip $(X86_BOOT_IRQ_TIME_CASE)),boot-irq-time-$(X86_BOOT_IRQ_TIME_CASE))$(if $(strip $(BOOT_FAILURE_CASE)),boot-failure-$(BOOT_FAILURE_CASE))$(if $(strip $(BOOT_CONTRACT_CASE)),boot-contract-$(BOOT_CONTRACT_CASE))$(if $(strip $(BOOT_SERVICES_CASE)),boot-services-$(BOOT_SERVICES_CASE)))
USER_VARIANT  := $(if $(filter 1,$(ARCH6_FSSTRESS_TEST)),arch6-fsstress)$(if $(filter 1,$(CAPACITY_TEST)),arch10-capacity)$(if $(filter 1,$(OS01_SYSTEST)),systest)$(if $(filter 1,$(FB_RESOLUTION_TEST)),resolution-test)$(if $(filter 1,$(USER_DEBUG)),user-debug)

OS01_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
OS01_PROFILE_FILE := $(OS01_ROOT)/mk/profiles/$(PROFILE).mk
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error unsupported PROFILE='$(PROFILE)')
endif
include $(OS01_PROFILE_FILE)

# FB_RESOLUTION_TEST is x86-only (the BGA fault surface targets QEMU
# Standard VGA).  TARGET_TRIPLE is resolved by the profile include above.
ifneq ($(filter 1,$(FB_RESOLUTION_TEST)),)
ifeq ($(filter x86_64%,$(TARGET_TRIPLE)),)
$(error FB_RESOLUTION_TEST=1 is only supported on x86 targets; TARGET_TRIPLE='$(TARGET_TRIPLE)')
endif
endif

ifneq ($(filter 1,$(CAPACITY_TEST)),)
ifeq ($(filter x86_64%,$(TARGET_TRIPLE)),)
$(error CAPACITY_TEST=1 is only supported on x86 targets; TARGET_TRIPLE='$(TARGET_TRIPLE)')
endif
endif

ifneq ($(filter 1,$(KERNEL_CANARY_SELFTEST)),)
ifeq ($(filter x86_64%,$(TARGET_TRIPLE)),)
$(error KERNEL_CANARY_SELFTEST=1 is only supported on x86 targets; TARGET_TRIPLE='$(TARGET_TRIPLE)')
endif
endif

# ── Capability gate ────────────────────────────────────────────
# $(call require_capability,<cap>) aborts during parse / recipe
# expansion when the selected profile does not declare <cap>.
define require_capability
$(if $(filter $(1),$(PROFILE_CAPABILITIES)),,$(error PROFILE='$(PROFILE)' lacks capability '$(1)'))
endef

# ── Controlled recursive Make ──────────────────────────────────
# GNU Make's MFLAGS contains only options, never command-line variable
# definitions.  Using it directly avoids trying to parse MAKEFLAGS ourselves:
# escaped whitespace in a value (VAR=one\ two) spans Make words and can leave
# a tail that looks like an option or target after a word-at-a-time filter.
# MFLAGS also keeps concatenated letter flags in their required leading,
# dash-prefixed form and preserves jobserver/long options.  Whitelisted
# overrides are passed explicitly by OS01_SUBMAKE_ARGS below.
OS01_SUBMAKEFLAGS = $(MFLAGS)

COMPILER_LAUNCHER ?=
CCACHE_DIR ?=
CCACHE_BASEDIR ?=
CCACHE_COMPILERCHECK ?=

# os01_submake — root-only controlled sub-make. The leading "+" is a recipe
# prefix (runs even under -n) and is not part of the shell command; there is
# deliberately no second "+" in front of $(MAKE).
# os01_submake_cmd is the command without the "+" prefix, safe inside shell blocks.
os01_submake_cmd = env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
  $(if $(COMPILER_LAUNCHER),COMPILER_LAUNCHER="$(COMPILER_LAUNCHER)") \
  $(if $(CCACHE_DIR),CCACHE_DIR="$(CCACHE_DIR)") \
  $(if $(CCACHE_BASEDIR),CCACHE_BASEDIR="$(CCACHE_BASEDIR)") \
  $(if $(CCACHE_COMPILERCHECK),CCACHE_COMPILERCHECK="$(CCACHE_COMPILERCHECK)") \
  MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
  -C $(1) OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" $(2)

define os01_submake
+$(call os01_submake_cmd,$(1),$(2))
endef

# Whitelisted overrides allowed across the controlled sub-make boundary.
# Each is appended as an explicit command-line argument only when set; never
# through MAKEFLAGS. UEFI_CLANG and the QEMU / firmware path overrides are
# explicitly allowed by the spec and included here so the adapters of later
# tasks can rely on them without rework. OS01_SUBMAKE_ARGS is recursive so it
# is evaluated at recipe-expansion time — after the root Makefile has applied
# its LOG_TARGET / INITTAB_FILE / ... defaults.
OS01_SUBMAKE_ALLOWED := CLANG UEFI_CLANG LLVM_AR LLVM_NM LLVM_OBJCOPY LLVM_OBJDUMP LLVM_READOBJ LLVM_READELF TARGET_LD RUNTIME_PROVIDER LOG_TARGET KERNEL_SELFTEST KERNEL_CANARY_SELFTEST KERNEL_TEST_FORCE_NO_RNDRRS KERNEL_EXTRA_CFLAGS KERNEL_EXTRA_LDFLAGS OS01_SYSTEST OS01_NETTEST INITTAB_FILE AARCH64_QEMU SMP AARCH64_SMP_TEST_NO_ACK_CPU AARCH64_SYNC_FAULT_TEST AARCH64_M1_TEST AARCH64_SCHED_TEST AARCH64_AP_SCHED_TEST AARCH64_SCHED_TICK_TEST AARCH64_UEFI_FIRMWARE_SOURCE QEMU_BIN DEBUG DEBUG_CHANNELS DRIVER_MODEL_FAULT FB_RESOLUTION_TEST USER_DEBUG TERM_PRESENT_TEST COMPILER_LAUNCHER CCACHE_DIR CCACHE_BASEDIR CCACHE_COMPILERCHECK HOST_TEST_JOBS CAPACITY_TEST ARCH6_FSSTRESS_TEST BOOT_FAILURE_TEST BOOT_CONTRACT_TEST BOOT_SERVICES_TEST
OS01_SUBMAKE_ARGS = $(foreach v,$(OS01_SUBMAKE_ALLOWED),$(if $($(v)),$(v)=$($(v))))

# ── D05: Doom asset root-level input validation ───────────────
unexport DOOM_IWAD DOOM_IWAD_SHA256 DOOM_IWAD_LICENSE DOOM_IWAD_SOURCE
DOOM_ASSET_COUNT := $(words $(if $(strip $(value DOOM_IWAD)),1,) $(if $(strip $(value DOOM_IWAD_SHA256)),1,) $(if $(strip $(value DOOM_IWAD_LICENSE)),1,) $(if $(strip $(value DOOM_IWAD_SOURCE)),1,))
ifneq ($(filter 1 2 3,$(DOOM_ASSET_COUNT)),)
  ifeq ($(filter clean,$(MAKECMDGOALS)),)
    $(error partial DOOM_IWAD asset inputs: all four variables (DOOM_IWAD, DOOM_IWAD_SHA256, DOOM_IWAD_LICENSE, DOOM_IWAD_SOURCE) must be provided together)
  endif
endif

# ── Sysroot generation protocol (spec: sysroot single-writer) ──
# The publish lock lives at build/.locks/<profile>/publish — OUTSIDE
# $(BUILD_DIR), so profile clean (rm -rf build/<profile>) can never remove it.
# It is acquired by atomic mkdir, retried for 60 s, and only a stale lock is
# removed by `make unlock-profile FORCE_UNLOCK=1`. publish, clean and
# unlock-profile all use it.
LOCK_DIR := $(OS01_ROOT)/build/.locks/$(PROFILE)/publish
# Generation read leases: one empty directory per in-flight artifact recipe
# consuming the published generation. publish/clean check them while holding
# the lock; clean fails (and deletes nothing) if any lease exists.
LEASES_DIR := $(BUILD_DIR)/leases
# Published immutable generations and the publish stamp (sysroot.mk is the
# only writer of both).
SYSROOT_GENERATIONS_DIR := $(BUILD_DIR)/sysroot-generations
SYSROOT_STAMP := $(BUILD_DIR)/stamps/sysroot.stamp

# FORCE — always out-of-date prerequisite for recipes that must run on every
# invocation (content-gated staging stamps, the kernel artifact recipe).
.PHONY: FORCE
FORCE:

# DRY_RUN — nonempty exactly when the current invocation is a dry run (-n).
# GNU make puts the bare-letter flags as the FIRST word of MAKEFLAGS whenever
# any letter flag is set (e.g. "n", "knw"); value flags (-j2, -Oline for
# --output-sync=line, ...) start with "-" and are excluded, so a bare-letter
# first word containing "n" means -n precisely.
DRY_RUN = $(if $(filter-out -%,$(firstword $(MAKEFLAGS))),$(findstring n,$(firstword $(MAKEFLAGS))))
