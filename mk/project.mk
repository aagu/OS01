# ── OS01 project configuration ──────────────────────────────────
# Included at the very top of the root Makefile. Selects the profile,
# includes it (toolchain + target config), and defines the capability
# gate and the controlled recursive-Make helper (os01_submake).
#
# Can also be invoked standalone for profile introspection:
#   make -f mk/project.mk PROFILE=<name> -n

PROFILE ?= x86_64-clang
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
# OS01_SUBMAKEFLAGS retains only GNU Make options — dash-prefixed flags
# (-j..., --jobserver-auth=..., long options) plus the bare letter flags GNU
# Make emits without a dash and concatenates into one word (n, Bs, knw, ...)
# — and drops every VAR=VALUE assignment. Assignment detection is by "="
# (every assignment contains one); $(filter %=%,...) cannot be used because
# GNU Make's filter patterns treat only the first "%" as a wildcard, so
# "%=%" matches nothing. GNU Make 4.4+ puts command-line assignments after a
# literal "--" word on MAKEFLAGS, which filter-out removes. Whitelisted
# overrides are passed explicitly as command-line arguments by the call
# sites (OS01_SUBMAKE_ARGS below); nothing but make options crosses via
# MAKEFLAGS.
OS01_SUBMAKEFLAGS = $(filter-out --,$(filter -%,$(MAKEFLAGS))) $(foreach w,$(filter-out -%,$(MAKEFLAGS)),$(if $(findstring =,$(w)),,$(w)))

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
