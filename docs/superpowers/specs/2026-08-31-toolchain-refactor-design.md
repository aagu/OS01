---
title: OS01 Toolchain Refactor v5
created: 2026-08-31
updated: 2026-08-31
type: spec
status: v5-draft, patch round-4 PARTIAL/NEW
version: 5
---

# OS01 Toolchain Refactor v5 Design

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

    # kernel/Makefile (the sibling directory is kernel/, hence ../toolchain.mk)
    LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
    ifeq ($(ARCH),x86_64)
    include $(dir $(LOCAL_MAKEFILE))../toolchain.mk
    endif

    # libc/Makefile (the sibling directory is libc/, hence ../toolchain.mk)
    LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
    ifeq ($(ARCH),x86_64)
    include $(dir $(LOCAL_MAKEFILE))../toolchain.mk
    endif

    # user/Makefile (the sibling directory is user/, hence ../toolchain.mk)
    LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
    ifeq ($(ARCH),x86_64)
    include $(dir $(LOCAL_MAKEFILE))../toolchain.mk
    endif

    # kernel/arch/x86_64/make.config
    ARCH_CONFIG := $(abspath $(lastword $(MAKEFILE_LIST)))
    include $(dir $(ARCH_CONFIG))../../../toolchain.mk

The `../toolchain.mk` spelling is deliberate: each child Makefile sits one directory below the root; `$(dir $(LOCAL_MAKEFILE))toolchain.mk` would incorrectly seek `kernel/toolchain.mk`, `libc/toolchain.mk`, or `user/toolchain.mk`. `kernel/arch/x86_64/make.config` keeps its `../../../toolchain.mk` include because it is only selected after `ARCH=x86_64`; libc/arch/x86_64/make.config needs no include because libc/Makefile includes the common file before its existing line-15 config include, and the config has no compiler override. In particular, `make ARCH=aarch64 -C kernel` evaluates the child guard false, so no x86 target variables or flags are defined in that subtree.

Inside toolchain.mk, anchor sysroot to toolchain.mk itself:

    TOOLCHAIN_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
    SYSROOT       ?= $(TOOLCHAIN_DIR)sysroot
    PREFIX        ?= /usr
    INCLUDEDIR    ?= $(PREFIX)/include
    LIBDIR        ?= $(PREFIX)/lib
    TARGET_INCLUDEDIR := $(SYSROOT)$(INCLUDEDIR)
    TARGET_LIBDIR     := $(SYSROOT)$(LIBDIR)

## 3. Clang-only override and discovery contract

CLANG=clang-18 is the preferred override. Option A is used for an explicit `CC`: after identity validation, `EFFECTIVE_CC` appends the target identity flags and is the compiler used by every x86 target recipe. This preserves a user's Clang version or wrapper choice while preventing a host-default compile; the trade-off is that a wrapper must accept ordinary Clang driver flags. `CC=cc` is categorically rejected, even if its banner happens to identify Clang. The GNU make built-in CC=cc is the only implicit value replaced:

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

Use `$(EFFECTIVE_CC)`, rather than `$(CC)`, in all x86 target compile and driver-link recipes. `TARGET_CC`/`TARGET_CCLD` remain the CLANG-selected default command for recursive target consumers. This rejects /usr/bin/gcc, ccache gcc, and gcc-12 by their executed banner, and rejects CC=cc before any banner check, unlike filter gcc%. It permits clang-18, absolute Clang, and ccache clang without silently replacing an explicit CC.

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
    RAW_LD_EMULATION := -m elf_x86_64
    ARCH_LIBS := -nostdlib -lk -lgcc

    # stage1: $(EFFECTIVE_CC) $(KERNEL_DRIVER_LDFLAGS) ... -T linker.ld $(ARCH_LIBS)
    # final: $(TARGET_LD) $(KERNEL_RAW_LDFLAGS) $(KERNEL_RAW_LIBDIR) -T linker.ld ... -lk -lgcc
    # kernel/Makefile:160 font.o: $(TARGET_LD) $(RAW_LD_EMULATION) -r -b binary -o $@ $<
    # trampoline.elf: $(TARGET_LD) -m elf_x86_64 -T trampoline.ld $< -o $@
    # kernel/Makefile:175 trampoline_bin.o: $(TARGET_LD) $(RAW_LD_EMULATION) -r -b binary trampoline.bin -o $@

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
    UEFI_CLANG_ID := $(shell $(UEFI_CLANG) --version 2>/dev/null | head -1)
    ifeq ($(findstring clang,$(UEFI_CLANG_ID)),)
    $(error UEFI_CLANG='$(UEFI_CLANG)' is not Clang)
    endif
    UEFI_LD_TOOL ?= $(TARGET_LD)
    $(eval $(call require_program,UEFI_LD_TOOL))
    UEFI_LD := $(UEFI_LD_TOOL) -flavor link
    $(MAKE) -C $(RUNTIME_DIR) ARCH=$(ARCH) TARGET=$(TARGET) OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)" \
        MAKEOVERRIDES= USE_GCC= CC="$(UEFI_CLANG)" LD="$(UEFI_LD)" CFLAGS= LDFLAGS=

The default `UEFI_CLANG=$(CLANG)` inherits the CLANG identity check above; the parallel `UEFI_CLANG_ID` check covers an explicit UEFI-only override. USE_GCC= forces Clang. Command-line CC/LD override copied assignments; MAKEOVERRIDES= blocks parent override replay. Empty CFLAGS/LDFLAGS block outer ELF flags but retain the inner --target=$(ARCH)-pc-win32-coff and COFF flags at lines 63-65. Apply only for x86_64.

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

The gated libgcc experiment clean-builds first. The selected-tool commands are concrete: program-header checks always use `$(LLVM_READELF) -Wl` (not `-h`, which reads only the ELF header); the machine identity uses `$(LLVM_READOBJ) --file-headers`; symbol binding uses the readelf symbol table. `KERNEL_ELF` below denotes the unstripped final kernel ELF and `UEFI_EFI` the generated BOOTX64.EFI.

    $(LLVM_NM) --undefined-only $(KERNEL_ELF)
    ! $(LLVM_READELF) -Wl $(KERNEL_ELF) | grep -E 'INTERP|DYNAMIC'
    $(LLVM_READOBJ) --file-headers $(KERNEL_ELF) | grep -F 'EM_X86_64'
    $(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*(_start|kernel_main|_text)'
    $(LLVM_READOBJ) --coff-exports $(UEFI_EFI)

Each of `_start`, `kernel_main`, and `_text` is asserted separately with `$(LLVM_READELF) -Ws $(KERNEL_ELF)` and requires `GLOBAL` binding; do not accept one broad match as a substitute. UEFI validation uses the discovered `$(LLVM_READOBJ) --coff-exports`, never an unqualified tool.

Exactly 10 commits: A, B, C, D, E, F, G, H, K, M. Exactly 17 matrix steps: S1-S17; same-host CLANG=clang-18 / CLANG=clang-22 is S16. Revert K if proof fails.

| Step | Commit | Pass criterion / selected-tool command where applicable |
|---|---|---|
| S1 | A | root-anchored x86 includes; `make ARCH=aarch64 -C kernel -n` contains no `--target=x86_64-unknown-none` |
| S2 | A | built-in CC replaced; `CC=gcc`, `CC='ccache gcc'`, `CC=cc`, and `CC=gcc-12` reject; explicit Clang makes `$(EFFECTIVE_CC)` retain `--target=$(TARGET_TRIPLE) -ffreestanding -fno-builtin` |
| S3 | A | basename/absolute LLVM override validates and target recipes use `$(EFFECTIVE_CC)` |
| S4 | A | host builds have no target flags |
| S5 | B | no Wl syntax reaches raw linker |
| S6 | B | kernel/user raw links build, including font.o and trampoline_bin.o with `$(RAW_LD_EMULATION)` |
| S7 | C | sysroot owns stdarg.h |
| S8 | D | kallsyms uses host variables |
| S9 | E | local cc.h ABI decision retained |
| S10 | F | no hard-coded tool/path in mbedTLS/BusyBox |
| S11 | F | identical config does not replace output |
| S12 | G | isolated UEFI Clang/COFF build; `$(LLVM_READOBJ) --coff-exports $(UEFI_EFI)` succeeds |
| S13 | H | selected LLVM artifact checks: `$(LLVM_READELF) -Wl $(KERNEL_ELF)` is used for program headers and `$(LLVM_READOBJ) --coff-exports $(UEFI_EFI)` for UEFI |
| S14 | K | kernel links without libgcc |
| S15 | K | `$(LLVM_NM) --undefined-only $(KERNEL_ELF)` is empty; `$(LLVM_READELF) -Wl $(KERNEL_ELF)` has no INTERP/DYNAMIC; `$(LLVM_READOBJ) --file-headers $(KERNEL_ELF)` reports EM_X86_64; separate `$(LLVM_READELF) -Ws $(KERNEL_ELF)` GLOBAL checks pass for all three symbols |
| S16 | M | same-host CLANG=clang-18 / clang-22 S15 proof; aarch64 proof remains free of x86 triple |
| S17 | M | full test/disk/QEMU matrix, including S12 UEFI export validation |

## 11. Round-review response ledger

| Review | Resolution |
|---|---|
| Round 2 | Retained from v4: driver/raw-ld split, owned stdarg.h, surgical cc.h decision, BusyBox content stamp, UEFI wrapper mechanics, and audit coverage. |
| Round 4 | v5 closes all five findings: composed `EFFECTIVE_CC` plus unconditional `CC=cc` rejection (§3); concrete selected-tool validators and S1-S17 mapping (§8); raw auxiliary-link emulation for kernel/Makefile:160 and :175 (§4); guarded child includes (§2); and UEFI_CLANG identity validation (§6). |
