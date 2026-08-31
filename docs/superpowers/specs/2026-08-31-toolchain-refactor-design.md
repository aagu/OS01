---
title: OS01 Toolchain Refactor — Cross-compiler-version Build Robustness
created: 2026-08-31
updated: 2026-08-31
type: spec
status: v3-draft (revised after codex round-2 review, REJECT → redesign)
version: 3
tags: [osdev, build, toolchain, portability, kernel, refactor]
related: [os01-roadmap-and-phases]
---

# OS01 Toolchain Refactor — Cross-compiler-version Build Robustness

> **Goal**: Make the OS01 build chain (UEFI bootloader + kernel + libc + user-space programs) **robust across compiler / binutils / libc versions and host distributions**, without pinning to a specific clang/gcc/llvm version.
>
> **Scope** (this series): **x86_64 build chain** end-to-end. UEFI has its own isolated toolchain contract. aarch64 is **explicitly excluded** from this series — it is a separate work item tracked elsewhere.
>
> **Out of scope**: introducing CMake/Meson, adding CI matrix (user explicitly declined CI), rewriting the Makefile in a new DSL, the aarch64 port.
>
> **Versioning**: v1 (round 1): APPROVE-WITH-CHANGES (7 blocking + 9 suggested + 2 missed). v2 (round 2): **REJECT** — proposed `toolchain.mk` conflated compiler-driver links with raw-linker links, and the "single source of truth" was defeated by `:=` overrides in every first-party Makefile. v3 addresses all 9 round-2 blocking issues + 5 suggestions.

## 0. Motivation

Five pre-existing build failures converge on one root cause — **the build chain silently encodes the host's specific compiler layout**:

| Observation | Symptom |
|---|---|
| kernel link fail under clang/lld ≥ 17 | `failed to convert GOTPCREL relocation against 'jiffies'` |
| kernel/net compile fail under clang 18 | `typedef redefinition with different types ('unsigned long long' vs 'unsigned long')` for uint64_t |
| kallsyms host tool fail | `fatal error: 'stdarg.h' file not found` |
| busybox link fail under clang 18 | `cannot find -lgcc` / `cannot find -lgcc_eh`; busybox.config hard-codes `/usr/lib/clang/22/include` |
| `llvm-ar` not in PATH on Ubuntu 24.04 | only `llvm-ar-18` available |

All five share one property: **they only manifest when the host's clang/gcc/llvm version differs from the version the project was last verified against**. Each is a single-line fix in isolation; together they reveal that OS01's build chain is brittle to environmental drift.

## 1. Root cause analysis (REVISED — five categories)

### 1.1 Hard-coded absolute paths

`config/busybox.config:52` literally contains `-isystem/usr/lib/clang/22/include`. Root Makefile line 6–7 hard-codes `export AR=llvm-ar`. Ubuntu 24.04 ships `llvm-ar-18` (no bare `llvm-ar`). homeserver has clang-22; the laptop has clang-18. Both have been used to build OS01; neither fully works today.

### 1.2 Implicit header-path ordering and toolchain ownership of headers

OS01's sysroot's `<stdio.h>` is a verbatim copy of `libc/include/stdio.h`, which `#include <stdarg.h>`. The sysroot never carried `<stdarg.h>` because OS01 considered it "clang's responsibility" — but user-space programs cannot trust a header the sysroot doesn't ship.

**Resolution**: OS01 owns `<stdarg.h>` (own file `libc/include/stdarg.h`) using `__builtin_va_*` intrinsics (supported by both Clang and GCC; the spec does not promise GCC compilation, but `__builtin_va_*` is portable C99 and works on any toolchain that supports the C99 varargs model).

### 1.3 Kernel link flag misspecification — **already fixed in 0444aeb (NOT in scope for this series)**

`kernel/arch/x86_64/make.config` already carries `-Wl,-z,norelro -Wl,--no-relax` (commit 0444aeb, on master). **This commit is the baseline**; the series does not re-introduce it. Verified post-0444aeb that `kernel.elf` links successfully on clang/lld 17+.

### 1.4 The `-lgcc` question

`ARCH_LIBS = -nostdlib -lk -lgcc` includes `-lgcc`. v1's `llvm-nm --undefined-only` evidence was weak. **v3 mandates the strong evidence path**:

1. Remove `-lgcc` from `ARCH_LIBS`.
2. Clean rebuild (`make clean`).
3. Re-link without `-lgcc`.
4. `readelf -Wl kernel.elf`: fail if `INTERP` or `DYNAMIC` segment present. **`-h` is insufficient** — it does not show program headers.
5. `llvm-nm --undefined-only kernel.elf`: must be empty.
6. Kernel identity check: `readelf -s kernel.elf | grep -E ' (_start|kernel_main|_text)$'` — all three must be present as global symbols.
7. `readelf -h kernel.elf` confirms Machine = `Advanced Micro Devices X86-64`.

If any step fails, the build is invalid; do not remove `-lgcc`. The kernel identity check guards against an empty or symbol-stripped kernel that "links successfully" but does not run.

### 1.5 Target/runtime selection — driver vs raw linker (NEW in v3, REVISED)

Round 2 made this concrete: **OS01 currently mixes two distinct link styles**, and conflating them in `toolchain.mk` would silently break things.

| Link style | Variable | Used by |
|---|---|---|
| **Compiler-driver link** | `clang --target=... -Wl,...` | BusyBox (its `Makefile` invokes `CC` for both compile and link and expects `-Wl,...` options to pass through), mbedtls, sysroot installs |
| **Raw-linker link** | `ld.lld -m elf_x86_64` | kernel.elf (final link uses `$(LD)` directly to control `--no-relax`, exact emulation, etc.), user/Makefile (already uses raw ld for syscall ELF), font.o / trampoline_bin.o (raw ld `--format binary`) |

A single `LD ?= ld.lld` plus global `LDFLAGS=--target=...` cannot drive both styles. Each recipe must explicitly choose its link tool. v3 introduces **three distinct variables**:

- `TARGET_CC` — compiler for compile + driver-based link. Always carries `--target=$(TARGET_TRIPLE) -ffreestanding -fno-builtin`.
- `TARGET_CCLD` — same as `TARGET_CC`, used in recipes that link via the driver (BusyBox, mbedtls).
- `TARGET_LD` — raw `ld.lld -m elf_x86_64`. Used in recipes that need explicit control.

`LDFLAGS` (driver flags) and `LD_FLAGS` (raw-ld flags) are separate variables so neither is silently inherited by the wrong recipe.

### 1.6 ABI ownership — separate from header ordering (NEW in v3)

OS01's public headers must agree with the selected target compiler's ABI. The `cc.h` header in `kernel/include/net/arch/cc.h` carries `uint64_t = unsigned long long` (LP64-as-LLP64 style); Clang's x86_64 builtin type is `unsigned long`. Bit-identical sizes, but C considers them **distinct types** and the redefinition fight surfaces at compile time.

**v3 resolves this with surgical precision, not by rewriting OS01's public headers wholesale**:

`kernel/include/net/arch/cc.h` was **written deliberately** to bypass `<stdint.h>` (see file comment lines 4–8: "<stdint.h> from the sysroot pulls in <sys/types.h> which defines ssize_t = long. lwIP arch.h defines ssize_t = int (when SSIZE_MAX is not set). The two conflict."). It does NOT reliably receive stdint.h typedefs transitively. **v3 does not delete cc.h's typedefs**; instead, it adds `#include <stdint.h>` at the top, immediately guarded by the existing `LWIP_NO_STDINT_H` pattern. stdint.h does not define `ssize_t`, so the existing `ssize_t` typedef remains valid.

If, after this, both OS01 sysroot `<stdint.h>` and `cc.h` define the same name (because stdint.h is now properly installed into sysroot with consistent typedefs from Clang builtins), the OS01 sysroot's `<stdint.h>` becomes the canonical source. We do not yet rewrite `libc/include/stdint.h`; that is tracked as a future item.

## 2. Design principles (REVISED — added scope limits)

| # | Principle | Concretely |
|---|---|---|
| P1 | **No hard-coded paths** | Every absolute path comes from `$(shell program --print-...)` |
| P2 | **Defaults overridable by env** | `CLANG ?= clang`, `LLVM_AR ?= ...` (verified executable) |
| P3 | **Single source of truth** | One `toolchain.mk`. First-party Makefiles `include` it AND **remove their `:= clang ...` overrides**. The override-removal IS part of the commit; "include" alone is insufficient. |
| P4 | **Host vs target separation** | `HOST_CC ?= cc`, `HOST_NM ?= nm`, `HOST_CFLAGS` for kallsyms/mkdisk |
| P5 | **Fail loud, not silent** | `$(error ...)` for missing tools; `[ -x "$$(LLVM_AR)" ]` verification before use |
| P6 | **Minimize change surface** | Build files + 3 source files (cc.h, libc/include/stdarg.h new, kernel/include/net/arch/cc.h minimal header guard change). No wholesale stdint.h rewrite. |
| P7 | **Document the assumptions** | `docs/build/toolchain.md` |
| P8 | **Explicit target identity per artifact** | Every target binary names its triple. BusyBox uses driver. Kernel uses raw ld. UEFI uses isolated COFF contract. |
| P9 | **Coherent tool-family selection** (scope-revised) | **Scope limited to Clang/LLVM**. The spec does not promise GCC compilation. `toolchain.mk` detects Clang; if `CC=gcc` is set, it errors clearly. No GCC branch. |
| P10 | **Generated config as tracked artifact** | BusyBox `.config` regenerated via phony Make rule + content-stamp comparison. `$(CLANG_RESOURCE_DIR)` alone is not sufficient — its value can change without Make noticing. |
| P11 | **Artifact validation** | `readelf -Wl` (not `-h`); kernel identity check; UEFI uses `llvm-readobj` PE/COFF checks, not ELF. |
| P12 | **(NEW) x86_64 entry-point scope** | This series covers x86_64 Makefiles only. aarch64 `kernel/arch/aarch64/make.config` and any UEFI-arch-specific files are explicitly out of scope. A separate series will refactor aarch64. |

## 3. The new `toolchain.mk` (REVISED — driver / raw-ld split)

```makefile
# ────────────────────────────────────────────────────────────
# toolchain.mk — single source of truth for OS01's x86_64
# build chain (kernel + libc + user-space + UEFI).
#
# Scope (P12): x86_64 only. aarch64 port is tracked separately.
# Compiler family (P9): Clang/LLVM only. No GCC support claimed.
#
# Override via env: `make CLANG=clang-22 LLVM_AR=/path/ar`.
# Documented in docs/build/toolchain.md.
# ────────────────────────────────────────────────────────────

# ── Compiler discovery ─────────────────────────────────────
# Default to clang; allow override. Resource dir is where
# stdint.h/stdarg.h builtins live — never hard-code a path.
CLANG        ?= clang
CLANG_RESOURCE_DIR := $(shell $(CLANG) -print-resource-dir 2>/dev/null)
ifeq ($(CLANG_RESOURCE_DIR),)
$(error $(CLANG) not found or '-print-resource-dir' failed; set CLANG=...)
endif
LLVM_VERSION := $(shell $(CLANG) --version 2>/dev/null | head -1)

# ── Binutils discovery (P9 coherent with Clang) ───────────
# Derive from clang itself. Validate each is executable.
LLVM_AR      := $(shell $(CLANG) -print-prog-name=llvm-ar       2>/dev/null)
LLVM_NM      := $(shell $(CLANG) -print-prog-name=llvm-nm       2>/dev/null)
LLVM_OBJCOPY := $(shell $(CLANG) -print-prog-name=llvm-objcopy  2>/dev/null)
LLVM_STRINGS := $(shell $(CLANG) -print-prog-name=llvm-strings  2>/dev/null)
LLVM_READOBJ := $(shell $(CLANG) -print-prog-name=llvm-readobj 2>/dev/null)

# Verify each is executable (round-2 §8.1: -print-prog-name may
# return a non-existent basename). One if-block per tool.
ifeq ($(shell test -x "$(LLVM_AR)" && echo y),y)
else
$(error '$(CLANG) -print-prog-name=llvm-ar' returned '$(LLVM_AR)' but it is not executable)
endif
# Same for NM, OBJCOPY, STRINGS, READOBJ — abbreviated.

# ── Compiler family enforcement (P9 scope-revised) ────────
# If user sets CC=gcc explicitly, refuse; this series only
# supports Clang.  No GCC branch is implemented.
ifeq ($(CC),)
# CC not explicitly set; safe to default.
else ifneq ($(filter gcc%,$(CC)),)
$(error GCC compilation is not supported in this series; unset CC to use default Clang)
endif

# ── Cross-compile target (x86_64 only; P12) ───────────────
TARGET_TRIPLE ?= x86_64-unknown-none

# ── Driver vs raw-linker split (round-2 §1.5) ───────────
# TARGET_CC:    compile via driver (carries --target + --sysroot)
# TARGET_CCLD:  same, for driver-based link recipes (BusyBox, mbedtls)
# TARGET_LD:    raw ld.lld for explicit-link recipes (kernel.elf, user ELF, binary embed)
TARGET_CC   := $(CLANG) --target=$(TARGET_TRIPLE) -ffreestanding -fno-builtin
TARGET_CCLD := $(TARGET_CC)
TARGET_LD   := ld.lld -m elf_x86_64

# ── Sysroot layout (round-2 P3 path split) ────────────────
# Install-relative (used by libc/Makefile install-headers):
PREFIX        ?= /usr
INCLUDEDIR    ?= $(PREFIX)/include
LIBDIR        ?= $(PREFIX)/lib
# Target-absolute (used by compile rules):
SYSROOT        ?= $(abspath sysroot)
TARGET_INCLUDEDIR := $(SYSROOT)$(INCLUDEDIR)
TARGET_LIBDIR     := $(SYSROOT)$(LIBDIR)

# ── Flag groups (round-2 P-EXTRA-CFLAGS) ─────────────────
# BASE_* holds target invariants that users must NOT be able
# to silently strip with `make CFLAGS=...`.  EXTRA_* holds
# optional user flags that are appended on top.
BASE_CFLAGS := --target=$(TARGET_TRIPLE) \
               -isystem $(TARGET_INCLUDEDIR) \
               -isystem $(CLANG_RESOURCE_DIR)/include \
               -g -fno-stack-protector
BASE_LDFLAGS := --target=$(TARGET_TRIPLE) --sysroot=$(SYSROOT)
BASE_LD_FLAGS := -m elf_x86_64

EXTRA_CFLAGS  ?=
EXTRA_LDFLAGS ?=
EXTRA_LD_FLAGS ?=

CFLAGS      = $(BASE_CFLAGS) $(EXTRA_CFLAGS)
LDFLAGS     = $(BASE_LDFLAGS) $(EXTRA_LDFLAGS)
LD_FLAGS    = $(BASE_LD_FLAGS) $(EXTRA_LD_FLAGS)

# ── Host tools (P4 explicit boundary) ─────────────────────
HOST_CC       ?= cc
HOST_NM       ?= nm
HOST_CFLAGS   ?= -O2 -g
HOST_LDFLAGS  ?=

# ── Export so recursive make sees consistent values ────────
export CLANG CLANG_RESOURCE_DIR LLVM_VERSION
export LLVM_AR LLVM_NM LLVM_OBJCOPY LLVM_STRINGS LLVM_READOBJ
export TARGET_TRIPLE
export SYSROOT PREFIX INCLUDEDIR LIBDIR TARGET_INCLUDEDIR TARGET_LIBDIR
export BASE_CFLAGS BASE_LDFLAGS BASE_LD_FLAGS
export CFLAGS LDFLAGS LD_FLAGS
export TARGET_CC TARGET_CCLD TARGET_LD
export HOST_CC HOST_NM HOST_CFLAGS HOST_LDFLAGS
```

### 3.1 Audit of existing `$(SYSROOT)/usr/...` references (round-2 "Anything I missed")

After landing, the following files must be audited and any direct `$(SYSROOT)/usr/...` rewritten to `$(TARGET_INCLUDEDIR)` / `$(TARGET_LIBDIR)`:

- `libc/Makefile` install-headers + install-libs targets
- `user/Makefile` includes / link paths
- `Makefile` (root) busybox invocation
- `boot/uefi/Makefile` (only for non-UEFI helper objects; the COFF recipe is isolated)

If any direct `$(SYSROOT)/usr/...` remains after the refactor, it is a bug — track as a follow-up.

## 4. Concrete file changes (REVISED — drives commit list)

The series produces **nine commits** (was seven). Code states explicitly which baseline each assumes.

| # | File | Change |
|---|---|---|
| 1 | **NEW** `toolchain.mk` | Per §3 |
| 2 | `Makefile` (root) | Add `include toolchain.mk`. Remove `export AR=llvm-ar`/`export OBJ_CPY=llvm-objcopy` (now derived). Update `CFLAGS=...` line to use `$(BASE_CFLAGS)` + `$(EXTRA_CFLAGS)`. busybox recipe: pass `CC=$(TARGET_CCLD) LD=$(TARGET_CCLD)` so BusyBox uses driver-based link. Add phony rule to regenerate `config/busybox.config` from `.in` template + `$(CLANG_RESOURCE_DIR)` stamp file. |
| 3 | `kernel/arch/x86_64/make.config` | Add `include toolchain.mk`. **Remove** `CC := clang -target ...` and `LD := ld.lld -m elf_x86_64` (lines 4–5) — replaced by `CC ?= $(TARGET_CC)` and `LD ?= $(TARGET_LD)`. (`?=` so arch config can override, but defaults defer to toolchain.mk.) |
| 4 | `kernel/arch/aarch64/make.config` | **OUT OF SCOPE** for this series (P12). Add a comment `TODO: refactor under aarch64 toolchain series; current `:=` overrides remain as-is for the aarch64 port's existing baseline.` |
| 5 | `kernel/Makefile` | Add `include toolchain.mk`. Update kallsyms rule to use `$(HOST_CC) $(HOST_CFLAGS)` and `$(HOST_NM) -n`. Update kernel.elf final-link rule to use `$(TARGET_LD) $(LD_FLAGS)` (raw ld), not `$(CC) $(LDFLAGS)`. |
| 6 | `libc/Makefile` | Add `include toolchain.mk`. **Remove** `CC := clang ...` / `LD := ld.lld ...` / `AR := llvm-ar` (lines 10–12). Replace with `CC ?= $(TARGET_CC)`, `LD ?= $(TARGET_LD)`, `AR ?= $(LLVM_AR)`. Update install-headers to use `$(INCLUDEDIR)` (install-relative), install-libs to use `$(LIBDIR)`. |
| 7 | `libc/include/stdarg.h` (NEW) | Per §5a. Owns stdarg.h with `__builtin_va_*`. |
| 8 | `libc/Makefile` install-headers | Add stdarg.h install step. |
| 9 | `user/Makefile` | Add `include toolchain.mk`. **Remove** `CC ?= clang ...` / `LD ?= ld.lld ...` / `OBJ_CPY ?= llvm-objcopy` (lines 6–8). Replace with `?=` deferring to `$(TARGET_*)/$(LLVM_OBJCOPY)`. Update user.elf link recipe to use `$(TARGET_LD) $(LD_FLAGS)` (raw ld) — same pattern as kernel.elf. |
| 10 | `boot/uefi/Makefile` | **Isolated UEFI toolchain contract** (per round-2 §1.5/§3): invoke the inner posix-uefi runtime with **explicitly cleared/UEFI-specific** CFLAGS/LDFLAGS. Pass only the chosen `$(CLANG)` executable. Do NOT inherit `$(CFLAGS)/$(LDFLAGS)` from toolchain.mk. Add a separate `UEFI_CLANG=$(CLANG)` variable so the inner runtime's `--target=$(ARCH)-pc-win32-coff` doesn't conflict with the x86_64 ELF defaults. |
| 11 | `tools/Makefile` | Add `include toolchain.mk`. Use `$(HOST_CC) $(HOST_CFLAGS)` for mkdisk. |
| 12 | `test/Makefile` | Add `include toolchain.mk`. Confirm all tests run via `$(HOST_CC)`. |
| 13 | `libc/arch/x86_64/make.config` | Add `include toolchain.mk`. **Remove** any `:=` clang/ld/llvm-ar overrides. |
| 14 | `kernel/include/net/arch/cc.h` | **Add** `#include <stdint.h>` at the top (inside the `LWIP_ARCH_CC_H` guard, before the typedef block). Add a comment explaining why stdint.h is now safe to include (it does not define ssize_t). Do NOT delete the existing typedefs (v3 leaves stdint.h canonicalization for a future series). |
| 15 | `config/busybox.config.in` (rename from `config/busybox.config`) | Convert to a template. Replace `/usr/lib/clang/22/include` with `@@CLANG_RESOURCE_DIR@@`. |
| 16 | `config/busybox.config` (NEW, generated, gitignored) | Generated at build time from `.in` template via phony rule. Tracked in `.gitignore`. Regeneration triggers: stamp file `build/.busybox.config.stamp` whose contents equal `$(CLANG_RESOURCE_DIR)`. |
| 17 | `docs/build/toolchain.md` (NEW) | User-facing documentation: design principles, override examples, troubleshooting. |
| 18 | `docs/superpowers/plans/2026-08-31-toolchain-refactor-plan.md` (UPDATE) | Updated plan with the new 9-commit list. |
| 19 | `AGENTS.md` | Add a "Toolchain overrides" line near "Critical gotchas". |

## 5. Three source-file exceptions (REVISED — surgical, not wholesale)

### 5a. `libc/include/stdarg.h` (NEW)

OS01 owns this header. Implementation:

```c
// libc/include/stdarg.h
#ifndef _STDARG_H
#define _STDARG_H 1

typedef __builtin_va_list va_list;

#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_arg(ap, type)        __builtin_va_arg(ap, type)
#define va_end(ap)              __builtin_va_end(ap)
#define va_copy(dst, src)       __builtin_va_copy(dst, src)

#endif
```

`__builtin_va_*` are supported by both Clang and GCC. The spec does not promise GCC compilation, but these builtins are the most portable C99 varargs implementation. If a future GCC-target build is desired, this file is the only place that needs a `__GNUC__` branch — and that branch can be added when GCC support is implemented.

### 5b. `kernel/include/net/arch/cc.h` — add include, NOT delete typedefs

**v2 proposed deleting the eight typedefs**; **v3 explicitly does NOT**. Reasoning:

- The cc.h file was deliberately written to avoid `<stdint.h>` (see file comment lines 4–8). Its typedefs are part of why it works.
- Even after `libc/include/stdarg.h` is installed and BusyBox's stdint.h is correct, **`cc.h` cannot reliably assume stdint.h reaches it transitively** (the file is included before `<kernel/log.h>`, so the chain is: cc.h → typedefs → kernel/log.h → kernel/device/timer.h → stdint.h. Currently the chain WORKS only because timer.h happens to include stdint.h first; future refactors could break it).
- The safe surgical change: **add `#include <stdint.h>` at the top of cc.h, before the typedef block**, with a guard that bails out if `ssize_t` would conflict.

```c
// kernel/include/net/arch/cc.h — addition only
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

// Pull in stdint.h FIRST so the canonical typedefs win.
// stdint.h does not define ssize_t, so the ssize_t typedef below
// remains valid and the lwIP arch.h conflict is preserved.
#include <stdint.h>

// existing typedef block unchanged
typedef unsigned char      uint8_t;
// ... etc ...
```

The eight typedefs remain in place. If stdint.h provides them with compatible definitions (which it will once the OS01 sysroot stdint.h is rewritten in a future series), they are no-ops; if it does not (current state, on hosts without the future rewrite), they are necessary. **Belt and suspenders; reversible.**

## 6. `-lgcc` removal — strict validation (REVISED)

The seven-step validation in §1.4 must all pass on **both** clang 18 (aoostar-n100) and clang 22 (homeserver) for `-lgcc` removal to land.

Step-by-step:

```
# After removing -lgcc from kernel/arch/x86_64/make.config:
cd ~/OS01
make clean
make kernel.bin

# Validation
LLVM_NM=$(CLANG_RESOURCE_DIR)/../../bin/llvm-nm  # or `llvm-nm`
[ -z "$($LLVM_NM --undefined-only build/x86_64/kernel/kernel.elf)" ] \
    || { echo "FAIL: undefined externals"; exit 1; }

readelf -Wl build/x86_64/kernel/kernel.elf | grep -E 'INTERP|DYNAMIC' \
    && { echo "FAIL: ELF has PT_INTERP or PT_DYNAMIC"; exit 1; } \
    || echo "OK: no PT_INTERP, no PT_DYNAMIC"

# Kernel identity check
readelf -s build/x86_64/kernel/kernel.elf | grep -E ' (_start|kernel_main|_text)$' \
    | wc -l | grep -q '^3$' \
    || { echo "FAIL: kernel missing _start, kernel_main, or _text"; exit 1; }

readelf -h build/x86_64/kernel/kernel.elf | grep 'Machine:' | grep -q 'X86-64' \
    || { echo "FAIL: wrong machine"; exit 1; }
```

If any step fails on either host, `-lgcc` removal is reverted, the failure is investigated, and the series stops at Commit K (see §10). The series does NOT proceed to Commit L+ with a partial validation.

## 7. Verification matrix (REVISED — 9 commits, 14 verification steps)

Each commit has RED GREEN REFACTOR. Every commit is verified on **both** clang 18 (aoostar-n100) and clang 22 (homeserver), unless explicitly marked x86_64-only.

| Step | Commit | Test | Pass criterion |
|---|---|---|---|
| **S1** | Commit A | `make test` on both hosts | 16/16 suites pass |
| **S2** | Commit A | `readelf -Wl kernel.elf` on both hosts (BEFORE this series; baseline) | Documents current state |
| **S3** | Commit B | `make kernel.bin` on both hosts | kernel.elf links; toolchain.mk exports are valid |
| **S4** | Commit B | `make -C user` on both hosts | user ELF binaries link via `$(TARGET_LD) $(LD_FLAGS)` |
| **S5** | Commit C | `make -C libc` on both hosts | libc.a and libk.a build; install-headers installs stdarg.h |
| **S6** | Commit C | `find sysroot/usr/include -name stdarg.h` exists | OS01 owns stdarg.h |
| **S7** | Commit D | `make kernel.bin` on both hosts | kernel.elf links with `-z norelro --no-relax` (baseline flag, already present) |
| **S8** | Commit D | kallsyms compiles without stdarg.h error | Host-tool boundary works |
| **S9** | Commit E | `make kernel.bin` on both hosts | `kernel/include/net/arch/cc.h` compiles; `kernel/net/net.o` builds |
| **S10** | Commit E | `make test` on both hosts | 16/16 suites pass (cc.h addition is non-breaking) |
| **S11** | Commit F | `make clean && make disk.img` on both hosts | busybox compiles past stdio.h; links via OS01 target driver |
| **S12** | Commit F | Spot-check generated `config/busybox.config` | Shows new `$(CLANG_RESOURCE_DIR)`; differs across hosts |
| **S13** | Commit G | UEFI build `make boot/uefi/BOOTX64.EFI` on both hosts | UEFI builds; uses isolated `UEFI_CLANG=$(CLANG)`; does NOT inherit OS01 ELF CFLAGS |
| **S14** | Commit H | UEFI ELF readelf + `llvm-readobj` PE/COFF checks on both hosts | UEFI shows PE/COFF characteristics; NOT ELF |
| **S15** | Commit K (L′ in old numbering) | `-lgcc` removal evidence (per §6 seven-step validation) on both hosts | All seven pass on both hosts |
| **S16** | Commit K | Kernel identity check on both hosts | `_start`, `kernel_main`, `_text` all present |
| **S17** | Commit M | Docs render correctly; cross-links resolve | Manual review |

## 8. Risk analysis (REVISED — added explicit risks)

| Risk | Likelihood | Mitigation |
|---|---|---|
| Driver vs raw-ld mixing silently breaks a recipe | Medium | §3 defines three separate vars; every first-party Makefile `include`s toolchain.mk AND removes its `:=` overrides. Commit A removes them; verification S3–S5 catch mis-use. |
| UEFI inner runtime inherits OS01 x86 ELF CFLAGS | High | §4 row 10 explicitly clears env before invoking posix-uefi. Commit G verification S13–S14 catch mis-use. |
| busybox silent CC=clang LD=clang leaks host defaults | High | §4 row 2 forces `CC=$(TARGET_CCLD) LD=$(TARGET_CCLD)`. Commit F verification S11–S12 catch mis-use. |
| `$(CLANG_RESOURCE_DIR)` changes but busybox.config does not regenerate | Medium | §3 + §4 row 2 use phony rule + content-stamp `build/.busybox.config.stamp`. Make re-runs the rule when stamp contents differ from `$(CLANG_RESOURCE_DIR)`. |
| `clang -print-prog-name=` returns a non-existent path | Medium | §3 has explicit `[ -x "$$(LLVM_AR)" ]` validation; `$(error ...)` on failure. |
| aarch64 build breaks when this series lands | Low | §2 P12 + §4 row 4 explicitly mark aarch64 OUT OF SCOPE; aarch64/make.config keeps its `:=` overrides unchanged with a TODO comment. aarch64's existing baseline continues to work. |
| `make CFLAGS=...` strips target identity | Medium | §3 split BASE_CFLAGS (target invariants) from EXTRA_CFLAGS (user flags). User who passes `CFLAGS=...` would override the whole thing — documented in docs/build/toolchain.md as "use EXTRA_CFLAGS instead". |
| future stdint.h rewrite breaks `cc.h` typedefs | Low | §5b adds stdint.h include without deleting the typedefs. Belt and suspenders; future rewrite simply makes the typedefs redundant, not wrong. |
| Code review for v3 surfaces yet another major design issue | Medium | v3 explicitly addresses ALL 9 round-2 blocking items + 5 suggestions. If a new major issue appears, this is v4 territory. |

## 9. Non-goals (unchanged)

- Not introducing CMake/Meson
- Not adding CI
- Not changing the directory layout
- Not addressing the **separate** aarch64 build issue (root Makefile line 21 `export CFLAGS=--sysroot=${SYSROOT}` overriding aarch64-specific sysroot)
- Not rewriting `libc/include/stdint.h` from Clang builtins (tracked as future series)

## 10. Rollout plan (REVISED — 9 commits)

Baseline: post-`0444aeb` master. The series begins **after** the already-applied `0444aeb` (link flag fix) and `c221c89` (kallsyms host fix).

1. **Commit A** — `toolchain.mk` (new file) + every first-party x86_64 Makefile `include`s it AND removes its `:=` overrides (P3 actual). Verify S1.
2. **Commit B** — `kernel/Makefile` link uses `$(TARGET_LD) $(LD_FLAGS)`; `user/Makefile` link uses same. Verify S3 + S4.
3. **Commit C** — `libc/include/stdarg.h` (new) + `libc/Makefile` install-headers installs it. Verify S5 + S6.
4. **Commit D** — kallsyms rule uses `$(HOST_CC) $(HOST_CFLAGS)` (already in c221c89; if that's already on master, this commit is a no-op baseline verification only). Verify S7 + S8.
5. **Commit E** — `kernel/include/net/arch/cc.h` adds `#include <stdint.h>` before the existing typedef block. Verify S9 + S10.
6. **Commit F** — `config/busybox.config.in` template + phony regeneration rule + `make BUSYBOX=...` invocations use `$(TARGET_CCLD)`. Verify S11 + S12.
7. **Commit G** — `boot/uefi/Makefile` isolated UEFI toolchain contract (clears CFLAGS/LDFLAGS env before invoking posix-uefi, passes only `UEFI_CLANG=$(CLANG)`). Verify S13.
8. **Commit H** — UEFI artifact validation script using `llvm-readobj` for PE/COFF. Verify S14.
9. **Commit K** — `-lgcc` removal from `kernel/arch/x86_64/make.config`. STRICT seven-step validation per §6 on both hosts. Verify S15 + S16.
10. **Commit M** — `docs/build/toolchain.md` + AGENTS.md cross-reference. Verify S17.

Each commit lands independently and is revertably safe. **Any commit that breaks `make test` on either host is reverted before proceeding.** No forward-progress on a broken foundation.

Note: `-lgcc` removal (Commit K) is intentionally **last** so its strict validation runs against the most-stable baseline. If it fails, the failure is documented; revert Commit K; the series still produces Commits A–H + M as net improvements.

## 11. Round-2 review response table

| Round-2 issue | Resolution in v3 |
|---|---|
| Conflate driver and raw linker | §3: three variables `TARGET_CC`/`TARGET_CCLD`/`TARGET_LD`; §4 row 2 forces BusyBox to use `TARGET_CCLD`; §4 row 5 forces kernel.elf to use `TARGET_LD`; §4 row 9 forces user.elf to use `TARGET_LD` |
| "Single source of truth" defeated by `:=` overrides | §4 rows 3, 6, 9, 13 explicitly remove the `:= clang ...` lines from `make.config`, `libc/Makefile`, `user/Makefile`, `libc/arch/x86_64/make.config` |
| UEFI needs isolated toolchain contract | §4 row 10: explicit env-clear + UEFI-specific flag composition |
| aarch64 scope contradiction | §2 P12 + §4 row 4: aarch64 explicitly out of scope with TODO comment; its existing `:=` overrides remain untouched |
| cc.h unsafe to delete typedefs | §5b: add `#include <stdint.h>` before the typedef block; do NOT delete |
| `-lgcc` plan gap | §6 seven-step validation; §4 row 9 is explicit Commit K; §7 S15 + S16 are explicit verification |
| BusyBox config regeneration real | §3 + §4 row 2: phony rule + content-stamp `build/.busybox.config.stamp` |
| Tool discovery override semantics | §3 explicit `[ -x ... ]` validation per tool; `?=` for user override |
| GCC promise vs implementation | §2 P9 scope-revised: Clang/LLVM only. `toolchain.mk` errors if `CC=gcc` |
| B1 already present in `0444aeb` | §4 row 7 makes D a baseline verification (no code change); `0444aeb` is the baseline |
| Suggested: tools/Makefile, test/Makefile, libc/arch/x86_64/make.config | §4 rows 11, 12, 13 |
| Suggested: BASE_/EXTRA_ split | §3 BASE_CFLAGS / EXTRA_CFLAGS |
| Suggested: same-host CLANG=clang-18 vs CLANG=clang-22 | §7 S15 explicitly same-host (where both available) + cross-host as supplement |
| Suggested: readelf -Wl for static, llvm-readobj for PE/COFF | §6 + §7 S14 + S16 use these |
| Suggested: "make doctor" may remain deferred | §3 already includes fail-loud discovery; doc target deferred to follow-up |

## 12. Open questions (carried from v2, all answered)

| # | Question | v3 answer |
|---|---|---|
| 1 | Where should `toolchain.mk` live? | repo root |
| 2 | `-lgcc` removal timing | Last (Commit K), after strict validation |
| 3 | Add `make doctor`? | Deferred (follow-up; fail-loud discovery is sufficient) |
| 4 | Conditional aarch64 `-lgcc`? | n/a — aarch64 is out of scope for this series |
| 5 | Should `cc.h` typedefs be deleted or preserved? | **Preserved** (with stdint.h added) — see §5b |
| 6 | Should `libc/include/stdint.h` be rewritten? | **No** in this one; tracked as future series |
| 7 | Driver vs raw linker split? | Yes, three vars (§3) |
| 8 | UEFI toolchain contract? | Isolated (§4 row 10) |
| 9 | aarch64 scope? | Explicitly excluded (§2 P12) |