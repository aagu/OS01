---
title: OS01 Toolchain Refactor v4
created: 2026-08-31
updated: 2026-08-31
type: spec
status: v4-draft, redesign after codex round-3 REJECT
version: 4
---

# OS01 Toolchain Refactor v4 Design

Goal: make the x86_64 UEFI, kernel, libc, user, and BusyBox build select a validated Clang/LLVM family without host-version paths. Preserve v3's driver/raw-ld split, owned stdarg.h, surgical cc.h decision, and BusyBox content stamp.

Non-goals: do not change libc/include/stdint.h, aarch64 implementation, thirdpart/posix-uefi/uefi/Makefile, GNU Make structure, CI, or add CMake/Meson.

## 1. Mechanical audit

| Current location | v4 resolution |
|---|---|
| Makefile:1-22 | x86-only include/export dispatch |
| Makefile:104-143,158-167 | mbedTLS/BusyBox use TARGET variables, never clang/llvm-ar/direct sysroot usr paths |
| kernel/arch/x86_64/make.config:4-5,38-40 | origin-safe defaults and separate driver/raw flags |
| kernel/Makefile:77,158-206 | TARGET_LIBDIR and explicit raw auxiliary/final links |
| libc/Makefile:6-18,89-95; user/Makefile:6-24,69-93 | shared file, target directories and raw user flags |
| boot/uefi/Makefile:42-58; thirdpart/posix-uefi/uefi/Makefile:31-68 | wrapper-only recursive-make isolation |
| tools/Makefile:2-12; test/Makefile:8-17 | host-only policy |

## 2. Location-independent common file

Capture the including Makefile before the include; do not depend on make cwd.

    # root Makefile
    ROOT_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
    ifeq ($(ARCH),x86_64)
    include $(dir $(ROOT_MAKEFILE))toolchain.mk
    endif

    # kernel/Makefile, libc/Makefile, user/Makefile
    LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
    include $(dir $(LOCAL_MAKEFILE))../toolchain.mk

    # kernel/arch/x86_64/make.config
    ARCH_CONFIG := $(abspath $(lastword $(MAKEFILE_LIST)))
    include $(dir $(ARCH_CONFIG))../../../toolchain.mk

libc/arch/x86_64/make.config needs no include: libc/Makefile includes the common file before its existing line-15 config include, and the config has no compiler override.

Inside toolchain.mk, anchor sysroot to toolchain.mk itself:

    TOOLCHAIN_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
    SYSROOT       ?= $(TOOLCHAIN_DIR)sysroot
    PREFIX        ?= /usr
    INCLUDEDIR    ?= $(PREFIX)/include
    LIBDIR        ?= $(PREFIX)/lib
    TARGET_INCLUDEDIR := $(SYSROOT)$(INCLUDEDIR)
    TARGET_LIBDIR     := $(SYSROOT)$(LIBDIR)

## 3. Clang-only override and discovery contract

CLANG=clang-18 is the preferred override. Explicit CC is supported only if it runs as Clang. The GNU make built-in CC=cc is the only value replaced:

    CLANG ?= clang
    CLANG_RESOURCE_DIR := $(shell $(CLANG) -print-resource-dir 2>/dev/null)
    ifeq ($(strip $(CLANG_RESOURCE_DIR)),)
    $(error CLANG='$(CLANG)' is unavailable or lacks -print-resource-dir)
    endif
    TARGET_TRIPLE ?= x86_64-unknown-none
    TARGET_CC := $(CLANG) --target=$(TARGET_TRIPLE) -ffreestanding -fno-builtin
    TARGET_CCLD := $(TARGET_CC)

    CC_ORIGIN := $(origin CC)
    ifeq ($(CC_ORIGIN),default)
    override CC := $(TARGET_CC)
    else ifeq ($(CC_ORIGIN),undefined)
    CC := $(TARGET_CC)
    else
    CC_ID := $(shell $(CC) --version 2>/dev/null | head -1)
    ifeq ($(findstring clang,$(CC_ID)),)
    $(error CC='$(CC)' (origin $(CC_ORIGIN)) is not Clang; use CLANG=clang-N or a Clang wrapper)
    endif
    endif

This rejects /usr/bin/gcc, ccache gcc, CC=cc, and gcc-12 by their executed banner, unlike filter gcc%. It permits clang-18, absolute Clang, and ccache clang without silently replacing explicit CC.

Conditional defaults and PATH-safe validation:

    LLVM_AR      ?= $(shell $(CLANG) -print-prog-name=llvm-ar 2>/dev/null)
    LLVM_NM      ?= $(shell $(CLANG) -print-prog-name=llvm-nm 2>/dev/null)
    LLVM_OBJCOPY ?= $(shell $(CLANG) -print-prog-name=llvm-objcopy 2>/dev/null)
    LLVM_READOBJ ?= $(shell $(CLANG) -print-prog-name=llvm-readobj 2>/dev/null)
    LLVM_READELF ?= $(shell $(CLANG) -print-prog-name=llvm-readelf 2>/dev/null)
    TARGET_LD    ?= $(shell $(CLANG) -print-prog-name=ld.lld 2>/dev/null)

    define require_program
    ifneq ($(shell command -v "$(strip $($(1)))" >/dev/null 2>&1 && printf y),y)
    $(error $(1)='$($(1))' is not executable or on PATH; override $(1)=/absolute/path)
    endif
    endef
    $(foreach p,LLVM_AR LLVM_NM LLVM_OBJCOPY LLVM_READOBJ LLVM_READELF TARGET_LD,$(eval $(call require_program,$(p))))

TARGET_LD is a validated raw executable, not ld.lld plus flags. Every validation uses LLVM_NM, LLVM_READELF, and LLVM_READOBJ variables, never bare names.

## 4. Driver/raw linker interface

Kernel stage 1 remains driver-linked at kernel/Makefile:186. Final and auxiliary links at :158-176 and :205-206 are raw:

    KERNEL_DRIVER_LDFLAGS := -Wl,-m -Wl,elf_x86_64 -static -Wl,-z,muldefs \
                             -Wl,-z,norelro -Wl,--no-relax
    KERNEL_RAW_LDFLAGS := -m elf_x86_64 -static -z muldefs -z norelro --no-relax
    KERNEL_RAW_LIBDIR := -L$(TARGET_LIBDIR)
    ARCH_LIBS := -nostdlib -lk -lgcc

    # stage1: $(CC) $(KERNEL_DRIVER_LDFLAGS) ... -T linker.ld $(ARCH_LIBS)
    # final: $(TARGET_LD) $(KERNEL_RAW_LDFLAGS) $(KERNEL_RAW_LIBDIR) -T linker.ld ... -lk -lgcc
    # font.o: $(TARGET_LD) -r -b binary -o $@ $<
    # trampoline.elf: $(TARGET_LD) -m elf_x86_64 -T trampoline.ld $< -o $@
    # trampoline_bin.o: $(TARGET_LD) -r -b binary trampoline.bin -o $@

    # all user ELF rules at user/Makefile:69-93
    USER_RAW_LDFLAGS := -m elf_x86_64 -static -no-pie -T linker.ld -L$(TARGET_LIBDIR)

The conversions are exact: -Wl,-z,norelro to -z norelro; -Wl,--no-relax to --no-relax; -Wl,-m -Wl,elf_x86_64 to -m elf_x86_64. Driver-only -nostdlib never reaches raw ld. This covers stage1, final kernel, font.o, trampoline.elf, trampoline_bin.o, user script and user library path. Generic CC ?= and LD ?= are prohibited because make built-ins make them no-ops.

## 5. Complete single-source audit and retained scope

| Existing lines | Replacement |
|---|---|
| Makefile:6-7 | remove AR=llvm-ar and OBJ_CPY=llvm-objcopy exports |
| Makefile:89,104,111,117-143,160-164 | TARGET_LIBDIR, TARGET_INCLUDEDIR, TARGET_CC, LLVM_AR |
| Makefile:158-159,167 | CC="$(TARGET_CCLD)" LD="$(TARGET_CCLD)" |
| kernel/Makefile:13-14,77 | discovered utilities and TARGET_LIBDIR |
| libc/Makefile:10-12,89-95 | no immediate tools; target directories |
| user/Makefile:6-24,69-93 | target tools/directories and USER_RAW_LDFLAGS |

tools/Makefile is host-only with HOST_CC ?= cc. test/Makefile is host-only with HOST_CC ?= clang. Neither includes target flags.

Add libc/include/stdarg.h using builtin va_list and va operations; do not rewrite libc/include/stdint.h. Do not include stdint.h in kernel/include/net/arch/cc.h: its uint64_t is unsigned long long, matching libc/include/stdint.h:15-20, not canonical Clang typedef ownership. A future stdint rewrite must re-review cc.h's local typedef and ssize_t rationale at cc.h:5-23.

BusyBox config generation may be phony only when it renders to a temporary, uses cmp -s, atomically moves only on difference, and removes the equal temporary.

## 6. UEFI isolation

The copied runtime detects GCC at thirdpart/posix-uefi/uefi/Makefile:32-61 and assigns CC/LD at :63-68. Change only wrapper line 57-58:

    UEFI_CLANG ?= $(CLANG)
    UEFI_LD_TOOL ?= $(TARGET_LD)
    $(eval $(call require_program,UEFI_LD_TOOL))
    UEFI_LD := $(UEFI_LD_TOOL) -flavor link
    $(MAKE) -C $(RUNTIME_DIR) ARCH=$(ARCH) TARGET=$(TARGET) OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)" \
        MAKEOVERRIDES= USE_GCC= CC="$(UEFI_CLANG)" LD="$(UEFI_LD)" CFLAGS= LDFLAGS=

USE_GCC= forces Clang. Command-line CC/LD override copied assignments; MAKEOVERRIDES= blocks parent override replay. Empty CFLAGS/LDFLAGS block outer ELF flags but retain the inner --target=$(ARCH)-pc-win32-coff and COFF flags at lines 63-65. Apply only for x86_64.

## 7. aarch64 proof

    ARCH ?= x86_64
    ROOT_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
    ifeq ($(ARCH),x86_64)
    include $(dir $(ROOT_MAKEFILE))toolchain.mk
    export CLANG CLANG_RESOURCE_DIR LLVM_AR LLVM_NM LLVM_OBJCOPY LLVM_READOBJ
    export TARGET_TRIPLE TARGET_CC TARGET_CCLD TARGET_LD SYSROOT TARGET_INCLUDEDIR TARGET_LIBDIR CFLAGS
    else ifeq ($(ARCH),aarch64)
    # no x86 include or exports
    else
    $(error unsupported ARCH='$(ARCH)')
    endif

make ARCH=aarch64 aarch64-uefi bypasses x86 state. Proof: a make -n ARCH=aarch64 aarch64-uefi output grep for --target=x86_64-unknown-none has no match. kernel/arch/aarch64/make.config remains unchanged.

## 8. Validation and accounting

The gated libgcc experiment clean-builds first, checks no undefined symbols with LLVM_NM, no INTERP/DYNAMIC, X86-64 machine, then checks each of _start, kernel_main, and _text separately with LLVM_READELF and requires GLOBAL binding.

Exactly 10 commits: A, B, C, D, E, F, G, H, K, M. Exactly 17 matrix steps: S1-S17; same-host CLANG=clang-18 / CLANG=clang-22 is S16. Revert K if proof fails.
