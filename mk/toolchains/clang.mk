# ── Clang/LLVM discovery and validation ─────────────────────────
# Location-independent common file, included by the x86_64 profile
# (mk/profiles/x86_64-clang.mk). Selects a validated Clang/LLVM family
# without host-version paths. Component Makefiles receive these tools via
# the profile include; the root toolchain.mk keeps the same logic for the
# one-release standalone-component compatibility path.

# ── Clang-only override and discovery contract ────────────────
# CLANG=clang-N is the preferred override. An explicit CC is accepted only
# when its executed banner identifies Clang; CC=cc is categorically rejected
# (even if it happens to be a Clang symlink). EFFECTIVE_CC is the compiler
# used by every x86 target compile and driver-link recipe.
CLANG ?= clang
CLANG_ID := $(shell $(CLANG) --version 2>/dev/null | head -1)
ifeq ($(findstring clang,$(CLANG_ID)),)
$(error CLANG='$(CLANG)' is not Clang)
endif
CLANG_RESOURCE_DIR := $(shell $(CLANG) -print-resource-dir 2>/dev/null)
ifeq ($(strip $(CLANG_RESOURCE_DIR)),)
$(error CLANG='$(CLANG)' is unavailable or lacks -print-resource-dir)
endif
TARGET_TRIPLE ?= x86_64-unknown-none
TARGET_CC := $(CLANG) --target=$(TARGET_TRIPLE) -ffreestanding -fno-builtin
TARGET_CCLD := $(TARGET_CC)

CC_ORIGIN := $(origin CC)
ifeq ($(CC_ORIGIN),default)
EFFECTIVE_CC := $(TARGET_CC)
else ifeq ($(CC_ORIGIN),undefined)
EFFECTIVE_CC := $(TARGET_CC)
else
ifeq ($(strip $(CC)),cc)
$(error CC=cc is not permitted; use CLANG=clang-N or an explicit Clang command)
endif
CC_ID := $(shell $(CC) --version 2>/dev/null | head -1)
ifeq ($(findstring clang,$(CC_ID)),)
$(error CC='$(CC)' (origin $(CC_ORIGIN)) is not Clang; use CLANG=clang-N or a Clang wrapper)
endif
EFFECTIVE_CC := $(CC) --target=$(TARGET_TRIPLE) -ffreestanding -fno-builtin
endif

# Conditional defaults and PATH-safe validation
LLVM_AR      ?= $(shell $(CLANG) -print-prog-name=llvm-ar 2>/dev/null)
LLVM_NM      ?= $(shell $(CLANG) -print-prog-name=llvm-nm 2>/dev/null)
LLVM_OBJCOPY ?= $(shell $(CLANG) -print-prog-name=llvm-objcopy 2>/dev/null)
LLVM_READOBJ ?= $(shell $(CLANG) -print-prog-name=llvm-readobj 2>/dev/null)
LLVM_READELF ?= $(shell $(CLANG) -print-prog-name=llvm-readelf 2>/dev/null)
TARGET_LD    ?= $(shell $(CLANG) -print-prog-name=ld.lld 2>/dev/null)

define require_program
ifneq ($(shell command -v "$(strip $($(1)))" >/dev/null 2>&1 && printf y),y)
$$(error $(1)='$$($(1))' is not executable or on PATH; override $(1)=/absolute/path)
endif
endef
$(foreach p,LLVM_AR LLVM_NM LLVM_OBJCOPY LLVM_READOBJ LLVM_READELF TARGET_LD,$(eval $(call require_program,$(p))))

# ── Consumer aliases ─────────────────────────────────────────
# Immediate := assignments: immune to make's built-in default CC=cc quirk,
# still overridable on the command line. Consumers use AR/LD/OBJ_CPY.
AR := $(LLVM_AR)
LD := $(TARGET_LD)
OBJ_CPY := $(LLVM_OBJCOPY)
