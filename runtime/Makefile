# ── Profile-only compiler-runtime variant build ────────────────
override RUNTIME_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang runtime-kernel)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)

define runtime_require_input
ifeq ($(strip $($(1))),)
$$(error ERROR: runtime build requires $(1))
endif
endef
$(foreach v,RUNTIME_CONSUMER RUNTIME_BUILD_DIR RUNTIME_ARCHIVE RUNTIME_RECEIPT RUNTIME_TARGET RUNTIME_CFLAGS RUNTIME_OBJECT_FORMAT RUNTIME_MACHINE RUNTIME_ABI RUNTIME_PROVIDER RUNTIME_SOURCE_DIGEST CLANG CLANG_ID CLANG_RESOURCE_DIR LLVM_NM TARGET_AR,$(eval $(call runtime_require_input,$(v))))

ifneq ($(RUNTIME_PROVIDER),selfhosted)
$(error ERROR: runtime/Makefile only builds the selfhosted provider, got '$(RUNTIME_PROVIDER)')
endif

RUNTIME_SOURCE_DIR := $(CURDIR)
RUNTIME_OBJ := $(RUNTIME_BUILD_DIR)/udivti3.o
RUNTIME_SOURCE := $(RUNTIME_SOURCE_DIR)/builtins/udivti3.c
RUNTIME_HEADER := $(RUNTIME_SOURCE_DIR)/include/os01/compiler_rt.h
RUNTIME_CFLAGS_NORMALIZED := $(strip $(RUNTIME_CFLAGS))
RUNTIME_VARIANT_TUPLE := profile=$(PROFILE)|consumer=$(RUNTIME_CONSUMER)|target=$(RUNTIME_TARGET)|format=$(RUNTIME_OBJECT_FORMAT)|machine=$(RUNTIME_MACHINE)|abi=$(RUNTIME_ABI)|provider=$(RUNTIME_PROVIDER)|clang=$(CLANG_ID)|resource=$(CLANG_RESOURCE_DIR)|cflags=$(RUNTIME_CFLAGS_NORMALIZED)|source=$(RUNTIME_SOURCE_DIGEST)

.PHONY: runtime-archive runtime-receipt
runtime-archive: $(RUNTIME_ARCHIVE)

runtime-receipt: $(RUNTIME_ARCHIVE)
	@set -eu; \
	mkdir -p "$(dir $(RUNTIME_RECEIPT))"; \
	tmp="$(RUNTIME_RECEIPT).tmp.$$$$"; \
	trap 'rm -f "'"$$tmp"'"' EXIT; \
	archive_sha=$$(sha256sum "$(RUNTIME_ARCHIVE)" | cut -d' ' -f1); \
	{ \
	  printf 'variant=%s\n' '$(RUNTIME_VARIANT_TUPLE)'; \
	  printf 'profile=%s\nconsumer=%s\ntarget=%s\nformat=%s\nmachine=%s\nabi=%s\n' \
	    '$(PROFILE)' '$(RUNTIME_CONSUMER)' '$(RUNTIME_TARGET)' \
	    '$(RUNTIME_OBJECT_FORMAT)' '$(RUNTIME_MACHINE)' '$(RUNTIME_ABI)'; \
	  printf 'provider=%s\nclang=%s\nresource_dir=%s\ncflags=%s\nsource_sha256=%s\narchive_sha256=%s\n' \
	    '$(RUNTIME_PROVIDER)' '$(CLANG_ID)' '$(CLANG_RESOURCE_DIR)' \
	    '$(RUNTIME_CFLAGS_NORMALIZED)' '$(RUNTIME_SOURCE_DIGEST)' "$$archive_sha"; \
	} > "$$tmp"; \
	mv "$$tmp" "$(RUNTIME_RECEIPT)"

$(RUNTIME_OBJ): $(RUNTIME_SOURCE) $(RUNTIME_HEADER) $(RUNTIME_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CLANG) --target=$(RUNTIME_TARGET) $(RUNTIME_CFLAGS_NORMALIZED) \
		-I$(RUNTIME_SOURCE_DIR)/include -c $< -o $@

$(RUNTIME_ARCHIVE): $(RUNTIME_OBJ)
	@nm_tmp="$@.nm.tmp"; trap 'rm -f "'"$$nm_tmp"'"' EXIT; \
	if ! $(LLVM_NM) --undefined-only $(RUNTIME_OBJ) > "$$nm_tmp"; then \
	  echo "ERROR: llvm-nm failed for $(RUNTIME_OBJ)" >&2; exit 1; \
	fi; \
	if test -s "$$nm_tmp"; then \
	  echo "ERROR: runtime object has undefined symbols" >&2; cat "$$nm_tmp" >&2; exit 1; \
	fi
	$(TARGET_AR) rcs $@ $<
