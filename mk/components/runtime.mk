# ── Compiler-runtime provider and variant contract ──────────────
# Included by the root Makefile after project.mk and usable from a small
# profile-only harness.  The initial enabled consumer is the x86_64 kernel;
# profiles without a kernel runtime declaration are deliberately unchanged.

ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)

ifdef RUNTIME_TARGET_kernel

RUNTIME_PROVIDER ?= selfhosted
ifeq ($(RUNTIME_PROVIDER),selfhosted)
else ifeq ($(RUNTIME_PROVIDER),compiler-rt)
else
$(error ERROR: runtime provider '$(RUNTIME_PROVIDER)' is unsupported; expected selfhosted or compiler-rt)
endif

define runtime_require_kernel_var
ifeq ($(strip $($(1))),)
$$(error ERROR: kernel runtime variant requires $(1))
endif
endef
$(foreach v,RUNTIME_TARGET_kernel RUNTIME_CFLAGS_kernel RUNTIME_OBJECT_FORMAT_kernel RUNTIME_MACHINE_kernel RUNTIME_ABI_kernel CLANG CLANG_ID CLANG_RESOURCE_DIR LLVM_AR LLVM_NM LLVM_READOBJ TARGET_AR,$(eval $(call runtime_require_kernel_var,$(v))))

override RUNTIME_KERNEL_CONSUMER := kernel
override RUNTIME_KERNEL_CFLAGS_NORMALIZED := $(strip $(RUNTIME_CFLAGS_kernel))
override RUNTIME_KERNEL_SOURCE_INPUTS := \
    $(OS01_ROOT)/runtime/include/os01/compiler_rt.h \
    $(OS01_ROOT)/runtime/builtins/udivti3.c
override RUNTIME_KERNEL_SOURCE_DIGEST := $(strip $(shell \
    sha256sum $(RUNTIME_KERNEL_SOURCE_INPUTS) 2>/dev/null | \
    awk '{print $$1}' | sha256sum | cut -d' ' -f1))
ifeq ($(RUNTIME_KERNEL_SOURCE_DIGEST),)
$(error ERROR: failed to compute kernel runtime source digest)
endif

# Escape a single quote for a shell single-quoted field.  Hashing the complete
# tuple keeps filesystem paths short while the receipt preserves readable
# values for audit.
override runtime_sq = $(subst ','"'"',$(1))
override RUNTIME_KERNEL_VARIANT_TUPLE := profile=$(PROFILE)|consumer=kernel|target=$(RUNTIME_TARGET_kernel)|format=$(RUNTIME_OBJECT_FORMAT_kernel)|machine=$(RUNTIME_MACHINE_kernel)|abi=$(RUNTIME_ABI_kernel)|provider=$(RUNTIME_PROVIDER)|clang=$(CLANG_ID)|resource=$(CLANG_RESOURCE_DIR)|cflags=$(RUNTIME_KERNEL_CFLAGS_NORMALIZED)|source=$(RUNTIME_KERNEL_SOURCE_DIGEST)
override RUNTIME_KERNEL_VARIANT_DIGEST := $(strip $(shell \
    printf '%s' '$(call runtime_sq,$(RUNTIME_KERNEL_VARIANT_TUPLE))' | \
    sha256sum | cut -d' ' -f1))
ifeq ($(RUNTIME_KERNEL_VARIANT_DIGEST),)
$(error ERROR: failed to compute kernel runtime variant key)
endif

override KERNEL_RUNTIME_VARIANT_DIR := $(BUILD_DIR)/runtime/kernel/$(RUNTIME_PROVIDER)-$(RUNTIME_KERNEL_VARIANT_DIGEST)
override KERNEL_RUNTIME_RECEIPT := $(KERNEL_RUNTIME_VARIANT_DIR)/runtime.receipt
override KERNEL_RUNTIME_LINK_RECEIPT := $(BUILD_DIR)/runtime/kernel-link.receipt

ifeq ($(RUNTIME_PROVIDER),selfhosted)

override KERNEL_RUNTIME_ARCHIVE := $(KERNEL_RUNTIME_VARIANT_DIR)/libos01-builtins.a
override KERNEL_RUNTIME_PREREQ := $(KERNEL_RUNTIME_RECEIPT)
override KERNEL_RUNTIME_INPUTS := $(KERNEL_RUNTIME_ARCHIVE)

# Grouped targets prevent parallel requests for the archive and receipt from
# launching two component builds.  runtime/Makefile publishes both outputs.
$(KERNEL_RUNTIME_ARCHIVE) $(KERNEL_RUNTIME_RECEIPT) &: \
		$(RUNTIME_KERNEL_SOURCE_INPUTS) $(OS01_ROOT)/runtime/Makefile \
		$(OS01_PROFILE_FILE)
	$(call os01_submake,runtime,runtime-receipt \
		"RUNTIME_CONSUMER=$(RUNTIME_KERNEL_CONSUMER)" \
		"RUNTIME_BUILD_DIR=$(KERNEL_RUNTIME_VARIANT_DIR)" \
		"RUNTIME_ARCHIVE=$(KERNEL_RUNTIME_ARCHIVE)" \
		"RUNTIME_RECEIPT=$(KERNEL_RUNTIME_RECEIPT)" \
		"RUNTIME_TARGET=$(RUNTIME_TARGET_kernel)" \
		"RUNTIME_CFLAGS=$(RUNTIME_KERNEL_CFLAGS_NORMALIZED)" \
		"RUNTIME_OBJECT_FORMAT=$(RUNTIME_OBJECT_FORMAT_kernel)" \
		"RUNTIME_MACHINE=$(RUNTIME_MACHINE_kernel)" \
		"RUNTIME_ABI=$(RUNTIME_ABI_kernel)" \
		"RUNTIME_SOURCE_DIGEST=$(RUNTIME_KERNEL_SOURCE_DIGEST)" \
		$(OS01_SUBMAKE_ARGS))

else ifeq ($(RUNTIME_PROVIDER),compiler-rt)

# Compatibility discovery is explicit and target-specific.  The historical
# driver spelling is only a Clang compiler-rt query; no GCC executable,
# directory discovery, or fallback is permitted.
override RUNTIME_COMPILER_RT_CANDIDATE := $(strip $(shell \
    $(CLANG) --target=$(RUNTIME_TARGET_kernel) \
    $(RUNTIME_KERNEL_CFLAGS_NORMALIZED) -rtlib=compiler-rt \
    -print-libgcc-file-name 2>/dev/null))
override RUNTIME_COMPILER_RT_QUERY_STATUS := $(.SHELLSTATUS)
ifneq ($(RUNTIME_COMPILER_RT_QUERY_STATUS),0)
$(error ERROR: compiler-rt query failed for consumer 'kernel' target '$(RUNTIME_TARGET_kernel)' (exit $(RUNTIME_COMPILER_RT_QUERY_STATUS)))
endif
ifeq ($(RUNTIME_COMPILER_RT_CANDIDATE),)
$(error ERROR: compiler-rt query returned no archive for consumer 'kernel' target '$(RUNTIME_TARGET_kernel)')
endif
ifneq ($(findstring libgcc,$(RUNTIME_COMPILER_RT_CANDIDATE)),)
$(error ERROR: compiler-rt candidate path must not contain 'libgcc': $(RUNTIME_COMPILER_RT_CANDIDATE))
endif

override RUNTIME_COMPILER_RT_BASENAME := $(notdir $(RUNTIME_COMPILER_RT_CANDIDATE))
override RUNTIME_COMPILER_RT_ALLOWED_NAMES := libclang_rt.builtins.a libclang_rt.builtins-x86_64.a
ifeq ($(filter $(RUNTIME_COMPILER_RT_BASENAME),$(RUNTIME_COMPILER_RT_ALLOWED_NAMES)),)
$(error ERROR: compiler-rt candidate has unsupported archive name '$(RUNTIME_COMPILER_RT_BASENAME)' for consumer 'kernel')
endif

override KERNEL_RUNTIME_PREREQ := $(KERNEL_RUNTIME_RECEIPT)
override KERNEL_RUNTIME_INPUTS := $(RUNTIME_COMPILER_RT_CANDIDATE)

.PHONY: runtime-compiler-rt-force
runtime-compiler-rt-force:

$(KERNEL_RUNTIME_RECEIPT): runtime-compiler-rt-force $(OS01_PROFILE_FILE)
	@set -eu; \
	candidate='$(call runtime_sq,$(RUNTIME_COMPILER_RT_CANDIDATE))'; \
	if ! test -f "$$candidate"; then \
	  echo "ERROR: compiler-rt candidate is not a regular file: $$candidate" >&2; exit 1; \
	fi; \
	mkdir -p "$(KERNEL_RUNTIME_VARIANT_DIR)"; \
	members="$(KERNEL_RUNTIME_RECEIPT).members.tmp"; \
	extract="$(KERNEL_RUNTIME_RECEIPT).members.tmp.d"; \
	receipt="$(KERNEL_RUNTIME_RECEIPT).tmp.$$$$"; \
	trap 'rm -f "'"$$members"'" "'"$$receipt"'"; rm -rf "'"$$extract"'"' EXIT; \
	if ! $(LLVM_AR) t "$$candidate" > "$$members" 2>/dev/null || ! test -s "$$members"; then \
	  echo "ERROR: compiler-rt candidate is not a readable archive: $$candidate" >&2; exit 1; \
	fi; \
	duplicate=$$(sort "$$members" | uniq -d | head -1); \
	if test -n "$$duplicate"; then \
	  echo "ERROR: compiler-rt archive contains duplicate member basename '$$duplicate'" >&2; exit 1; \
	fi; \
	mkdir "$$extract"; \
	if ! (cd "$$extract" && $(LLVM_AR) x "$$candidate") >/dev/null 2>&1; then \
	  echo "ERROR: compiler-rt candidate is not a readable archive: $$candidate" >&2; exit 1; \
	fi; \
	while IFS= read -r member; do \
	  case "$$member" in ''|*/*|*..*) \
	    echo "ERROR: compiler-rt archive contains unsafe member name '$$member'" >&2; exit 1;; \
	  esac; \
	  header="$$extract/$$member.headers"; \
	  if ! $(LLVM_READOBJ) --file-headers "$$extract/$$member" > "$$header" 2>/dev/null; then \
	    echo "ERROR: compiler-rt member '$$member' is not a readable object" >&2; exit 1; \
	  fi; \
	  if ! grep -q '^Format: elf' "$$header" || \
	     ! grep -q 'Machine: $(RUNTIME_MACHINE_kernel)' "$$header"; then \
	    echo "ERROR: compiler-rt member '$$member' has wrong object format/machine for consumer 'kernel'; expected $(RUNTIME_OBJECT_FORMAT_kernel)/$(RUNTIME_MACHINE_kernel)" >&2; exit 1; \
	  fi; \
	done < "$$members"; \
	archive_sha=$$(sha256sum "$$candidate" | cut -d' ' -f1); \
	{ \
	  printf 'variant=%s\n' '$(call runtime_sq,$(RUNTIME_KERNEL_VARIANT_TUPLE))'; \
	  printf 'archive=%s\narchive_sha256=%s\n' "$$candidate" "$$archive_sha"; \
	} > "$$receipt"; \
	mv "$$receipt" "$(KERNEL_RUNTIME_RECEIPT)"

endif

.PHONY: runtime-kernel
runtime-kernel: $(KERNEL_RUNTIME_PREREQ)

endif
