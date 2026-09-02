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

# ── Variant slugs (BEFORE the profile include!) ───────────────
# IMAGE_VARIANT — the image/manifest dirs' variant suffix, derived from the
# explicit switch variables. The root Makefile applies OS01_SYSTEST /
# OS01_NETTEST to INITTAB_FILE BEFORE including project.mk, so all three
# sources are resolved here. Each variant gets its own isolated image dir
# (build/<profile>/image/<variant>/); a variant build NEVER writes the normal
# image. USER_VARIANT — the compile-affecting variant: only OS01_SYSTEST
# changes user CFLAGS (-DOS01_SYSTEST), so it is the only variant that
# re-keys the user build/artifact dirs (build/<profile>/user/<variant> and
# build/<profile>/artifacts/user/<variant>). Both are immediate (:=) so the
# profile file and every component that consumes them sees the resolved
# value at parse time — defining them after the profile include would make
# the profile's own USER_BUILD_DIR/USER_ARTIFACT_DIR compute with an empty
# USER_VARIANT.
IMAGE_VARIANT := $(strip $(if $(filter 1,$(OS01_SYSTEST)),systest)$(if $(filter 1,$(OS01_NETTEST)),nettest)$(if $(filter config/inittab.test,$(INITTAB_FILE)),inittab-test))
USER_VARIANT  := $(if $(filter 1,$(OS01_SYSTEST)),systest)

OS01_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
OS01_PROFILE_FILE := $(OS01_ROOT)/mk/profiles/$(PROFILE).mk
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error unsupported PROFILE='$(PROFILE)')
endif
include $(OS01_PROFILE_FILE)

# ── Capability gate ────────────────────────────────────────────
# $(call require_capability,<cap>) aborts during parse / recipe
# expansion when the selected profile does not declare <cap>.
define require_capability
$(if $(filter $(1),$(PROFILE_CAPABILITIES)),,$(error PROFILE='$(PROFILE)' lacks capability '$(1)'))
endef

# ── Controlled recursive Make ──────────────────────────────────
# OS01_SUBMAKEFLAGS retains only GNU Make options — the bare letter flags
# GNU Make emits without a dash and concatenates into one word (n, Bs, knw,
# ...), then the dash-prefixed flags (-j..., --jobserver-auth=..., long
# options) — and drops every VAR=VALUE assignment. Assignment detection is by
# "=" (every assignment contains one); $(filter %=%,...) cannot be used
# because GNU Make's filter patterns treat only the first "%" as a wildcard,
# so "%=%" matches nothing. GNU Make 4.4+ puts command-line assignments after
# a literal "--" word on MAKEFLAGS, which filter-out removes. The bare-letter
# flags MUST stay the FIRST word: GNU Make only accepts concatenated letter
# flags in the leading word of MAKEFLAGS (a trailing bare "n" is re-parsed as
# a target name and fails with "No rule to make target 'n'"). Whitelisted
# overrides are passed explicitly as command-line arguments by the call
# sites (OS01_SUBMAKE_ARGS below); nothing but make options crosses via
# MAKEFLAGS.
OS01_SUBMAKEFLAGS = $(foreach w,$(filter-out -%,$(MAKEFLAGS)),$(if $(findstring =,$(w)),,$(w))) $(filter-out --,$(filter -%,$(MAKEFLAGS)))

# os01_submake — root-only controlled sub-make. The leading "+" is a recipe
# prefix (runs even under -n) and is not part of the shell command; there is
# deliberately no second "+" in front of $(MAKE).
define os01_submake
+env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
  MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
  -C $(1) OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" $(2)
endef

# Whitelisted overrides allowed across the controlled sub-make boundary.
# Each is appended as an explicit command-line argument only when set; never
# through MAKEFLAGS. UEFI_CLANG and the QEMU / firmware path overrides are
# explicitly allowed by the spec and included here so the adapters of later
# tasks can rely on them without rework. OS01_SUBMAKE_ARGS is recursive so it
# is evaluated at recipe-expansion time — after the root Makefile has applied
# its LOG_TARGET / INITTAB_FILE / ... defaults.
OS01_SUBMAKE_ALLOWED := CLANG UEFI_CLANG LLVM_AR LLVM_NM LLVM_OBJCOPY LLVM_READOBJ LLVM_READELF TARGET_LD LOG_TARGET KERNEL_SELFTEST OS01_SYSTEST OS01_NETTEST INITTAB_FILE AARCH64_QEMU AARCH64_SMP AARCH64_UEFI_FIRMWARE_SOURCE QEMU_BIN
OS01_SUBMAKE_ARGS = $(foreach v,$(OS01_SUBMAKE_ALLOWED),$(if $($(v)),$(v)=$($(v))))

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
