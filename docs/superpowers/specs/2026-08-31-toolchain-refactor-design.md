---
title: OS01 Toolchain Refactor — Cross-compiler-version Build Robustness
created: 2026-08-31
updated: 2026-08-31
type: spec
status: draft
version: 1
tags: [osdev, build, toolchain, portability, kernel, refactor]
related: [os01-roadmap-and-phases]
---

# OS01 Toolchain Refactor — Cross-compiler-version Build Robustness

> **Goal**: Make the OS01 build chain (UEFI bootloader + kernel + libc + user-space programs) **robust across compiler / binutils / libc versions and host distributions**, without pinning to a specific clang/gcc/llvm version.
>
> **Scope**: x86_64 build chain end-to-end (UEFI, kernel, libc, busybox, mbedtls, lwIP). aarch64 port is in scope as a beneficiary but does not need new code here — it inherits the same `toolchain.mk`.
>
> **Out of scope**: introducing CMake/Meson, adding CI matrix (user has explicitly declined CI), rewriting the Makefile in a new DSL.

## 0. Motivation (why now)

Two recent observations that converge on the same root cause — **the build chain silently encodes the host's specific compiler layout**:

| Observation | Symptom | Source |
|---|---|---|
| `test_libc_vsprintf` 6/56 fail | `printf("%s", big_5000)` returns 5006 instead of 5000 | Pre-existing, fix in commit c8626a2 (already pushed) |
| kernel link fail under clang/lld ≥ 17 | `failed to convert GOTPCREL relocation against 'jiffies'; relink with --no-relax` | kernel/Makefile missing `-z norelro --no-relax` (wiki §2 already documents this) |
| kernel/net compile fail under clang 18 | `typedef redefinition with different types ('unsigned long long' vs 'unsigned long')` for uint64_t | `kernel/include/net/arch/cc.h` re-typedefs what clang's builtin stdint.h already typedefs |
| kallsyms host tool fail | `fatal error: 'stdarg.h' file not found` | kallsyms inherits `CFLAGS=--sysroot=OS01/sysroot`, sysroot lacks headers |
| busybox link fail under clang 18 (Ubuntu 24.04) | `cannot find -lgcc` / `cannot find -lgcc_eh` | busybox.config hard-codes `-isystem/usr/lib/clang/22/include`; sysroot lacks libgcc.a stub |
| `llvm-ar` not in PATH | `make` invokes `llvm-ar` but Ubuntu 24.04 ships `llvm-ar-18` only | root Makefile hard-codes `AR=llvm-ar` |

The first item is unrelated (purely test-fixture bug). All other items share one property: **they only manifest when the host's clang/gcc/llvm version differs from the version the project was last verified against**. Each is a single-line fix in isolation; together they reveal that the OS01 build chain is **brittle to environmental drift**.

We want one design, not five patches, so that the next compiler update is a no-op.

## 1. Root cause analysis

OS01's build chain encodes three categories of environmental assumption:

### 1.1 Hard-coded absolute paths

```makefile
# root Makefile
export AR=llvm-ar
export OBJ_CPY=llvm-objcopy

# config/busybox.config (line 52)
CONFIG_EXTRA_CFLAGS="... -isystem/usr/lib/clang/22/include ..."
```

Each of these will fail on a different host layout. Ubuntu 24.04 ships `llvm-ar-18` (no bare `llvm-ar`), and homeserver (Arch) ships `clang-22` while the laptop (Ubuntu 24.04) ships `clang-18`. Both have been used to build OS01; neither fully works today.

### 1.2 Implicit header-path ordering

```
clang -c kernel/foo.c
  → include path order: /usr/lib/llvm-18/lib/clang/18/include (builtin)
  → then: <--sysroot>/usr/include (OS01 sysroot)
```

The OS01 sysroot's `sysroot/usr/include/stdio.h` is a verbatim copy of `libc/include/stdio.h`, which `#include <stdarg.h>`. The clang builtin `<stdarg.h>` exists, but the sysroot's `<stdarg.h>` **does not** because `libc/include/` never had one — `install-headers` was missing a copy step.

This is a structural problem, not a missing file: OS01's libc considers `stdarg.h` someone else's responsibility (clang's). But user-space programs (`busybox`, mbedtls, ...) cannot "trust" a header that the OS01 sysroot doesn't carry, because sysroot is the contract.

### 1.3 Kernel link flag misspecification

Wiki `os01-x86_64-os.md §2` documents:
```
LDFLAGS = -static -z norelro ...
```

But `kernel/arch/x86_64/make.config` (the new centralized file from `6e71e7d`) only carries:
```
ARCH_LDFLAGS = -Wl,-m -Wl,elf_x86_64 -static -Wl,-z,muldefs
```

`-z norelro` and `--no-relax` are missing. They were carried by the old (pre-`6e71e7d`) LDFLAGS because the original commit that introduced `-fpie -mcmodel=kernel` also added them, but the refactor's goal was "centralize" — it should have carried them forward but didn't. This is a pure oversight, not a design defect.

The fact that this slipped through review is itself evidence that **OS01's build does not have a verification step that would catch missing flags**. A CI matrix would have caught it; the user has explicitly declined CI. So the fix must be structural: one place defines toolchain defaults, and that place is the source of truth that everyone reads.

### 1.4 The `-lgcc` question

`ARCH_LIBS = -nostdlib -lk -lgcc` includes `-lgcc`. Investigation (`llvm-nm --undefined-only build/x86_64/kernel/kernel.elf`) shows that `kernel.elf` has **zero undefined external symbols**. OS01's kernel never generates a 64-bit div/mod call (x86_64 has direct `div`/`idiv` instructions for 64-bit operands, and clang emits them). Therefore `-lgcc` is dead weight: it's a GCC-runtime library that contributes nothing the kernel needs. Removing it removes one more source of cross-host variance (since `libgcc.a` lives at `/usr/lib/gcc/x86_64-linux-gnu/<gcc-version>/`, which moves with every gcc update).

## 2. Design principles

These are the principles the implementation must satisfy:

| # | Principle | Concretely |
|---|---|---|
| P1 | **No hard-coded paths** | Every absolute path comes from `$(shell program --print-...)` or `$(shell which ...)` at make time |
| P2 | **Defaults overridable by env** | `CLANG ?= clang` so `make CLANG=clang-22` works |
| P3 | **Single source of truth** | One `toolchain.mk` defines `CC`/`CLANG_RESOURCE_DIR`/`LLVM_AR`/etc; everything else `include`s it |
| P4 | **Host vs target separation** | `HOST_CC`/`HOST_CFLAGS` for build tools (kallsyms, mkdisk), `CC`/`ARCH_CFLAGS` for kernel/user-space |
| P5 | **Fail loud, not silent** | Detection failures use `$(error ...)` so `make` aborts with a clear message |
| P6 | **Minimize change surface** | Refactor only the build files, not OS01 C/asm sources (with one documented exception: `cc.h` typedef) |
| P7 | **Document the assumptions** | A `docs/build/toolchain.md` describes the design and override mechanism |

## 3. The new `toolchain.mk`

A new file at the project root. All current build files `include` it.

```makefile
# ────────────────────────────────────────────────────────────
# toolchain.mk — single source of truth for compiler/linker
# detection and default flag composition.
#
# Override anything via env: `make CLANG=clang-22 AR=/path/ar`.
# ────────────────────────────────────────────────────────────

# ── Compiler discovery ─────────────────────────────────────
# Default to clang; allow override. Detect its "resource dir"
# (where stdint.h, stdarg.h, intrinsics live) at make time so we
# never hard-code /usr/lib/clang/XX/include.
CLANG        ?= clang
CLANG_RESOURCE_DIR := $(shell $(CLANG) -print-resource-dir 2>/dev/null)
ifeq ($(CLANG_RESOURCE_DIR),)
$(error $(CLANG) not found or '-print-resource-dir' failed; set CLANG=... in env)
endif

# ── Binutils discovery ─────────────────────────────────────
# Default to LLVM binutils (newer clang prefers them over GNU
# binutils for diagnostic quality). Override via env.
LLVM_AR       ?= $(shell command -v llvm-ar     || command -v $(CLANG)-ar     || echo $(CLANG_RESOURCE_DIR)/../../bin/llvm-ar)
LLVM_NM       ?= $(shell command -v llvm-nm     || command -v $(CLANG)-nm     || echo llvm-nm)
LLVM_OBJCOPY  ?= $(shell command -v llvm-objcopy || command -v $(CLANG)-objcopy || echo llvm-objcopy)
LLVM_STRINGS  ?= $(shell command -v llvm-strings || command -v $(CLANG)-strings || echo llvm-strings)

# ── Cross-compile target ───────────────────────────────────
# Currently fixed at x86_64-unknown-none for x86 hosts. aarch64
# port will introduce a separate platform/x86_64.mk vs
# platform/aarch64.mk layer.
TARGET_TRIPLE ?= x86_64-unknown-none
CC            ?= $(CLANG) -target $(TARGET_TRIPLE)
LD            ?= ld.lld
LD_EMULATION  ?= elf_x86_64

# ── Sysroot ────────────────────────────────────────────────
# Built into $(SYSROOT) by `make install-headers` /
# `make install-libs`. The user-space userland (busybox, mbedtls,
# init.elf, ...) compiles against this sysroot; the kernel also
# references it for OS01-provided libc headers, but does NOT
# link user-space libs.
SYSROOT       ?= $(abspath sysroot)
INCLUDEDIR    ?= $(SYSROOT)/usr/include
LIBDIR        ?= $(SYSROOT)/usr/lib

# ── Global default CFLAGS/LDFLAGS ─────────────────────────
# These are the base; arch-specific and host-specific makefiles
# extend them via += rather than redefining.
#
# Notable choices:
#   -isystem $(CLANG_RESOURCE_DIR)/include
#     clang builtin headers (stdint.h, stdarg.h, ...) for any
#     source that asks for them. Replaces the old
#     `-isystem=/usr/include` which dragged in glibc-foreign
#     typedefs.
#   -nostdinc is NOT set here. User-space userland (busybox)
#     sets it itself in its own CONFIG_EXTRA_CFLAGS.
CFLAGS        ?= --sysroot=$(SYSROOT) -isystem $(CLANG_RESOURCE_DIR)/include \
                  -isystem $(INCLUDEDIR) -g -fno-stack-protector
LDFLAGS       ?= --sysroot=$(SYSROOT)

# ── Host tools (build helpers, not target binaries) ────────
# kallsyms.c, mkdisk, etc.  These run on the build host and
# link against the host libc.  They MUST NOT inherit the
# --sysroot above.
HOST_CC        ?= $(CLANG)
HOST_CFLAGS    ?= -O2 -g
HOST_LDFLAGS   ?=

# Export everything so recursive make sees consistent values.
export CLANG CLANG_RESOURCE_DIR CC LD LLVM_AR LLVM_NM LLVM_OBJCOPY
export TARGET_TRIPLE SYSROOT INCLUDEDIR LIBDIR CFLAGS LDFLAGS
export HOST_CC HOST_CFLAGS HOST_LDFLAGS
```

## 4. Concrete file changes

| # | File | Change |
|---|---|---|
| 1 | **NEW** `toolchain.mk` | Per §3 |
| 2 | `Makefile` (root) | Replace lines 6–7 with `include toolchain.mk`. Remove `export AR=llvm-ar`/`export OBJ_CPY=llvm-objcopy` (now derived). Update line 21 CFLAGS to use `$(CLANG_RESOURCE_DIR)`. Update lines 142, 163, 164 to use `$(LLVM_AR)` instead of `llvm-ar`. |
| 3 | `kernel/arch/x86_64/make.config` | Add `-Wl,-z,norelro -Wl,--no-relax` to `ARCH_LDFLAGS` (wiki §2). Drop `-lgcc` from `ARCH_LIBS` (verified unnecessary). |
| 4 | `kernel/Makefile` | In the kallsyms rule, use `$(HOST_CC) $(HOST_CFLAGS)` instead of bare `clang`, dropping the `env -u CFLAGS` hack. |
| 5 | `kernel/include/net/arch/cc.h` | Replace the eight `typedef unsigned long long uint64_t; ...` lines with typedefs from clang's `__UINT8_TYPE__`/`__INT8_TYPE__`/etc. builtins. This makes the typedefs match clang's own stdint.h regardless of version. |
| 6 | `libc/Makefile` install-headers | Copy `$(CLANG_RESOURCE_DIR)/include/stdarg.h` into the sysroot as part of install-headers. Document why. |
| 7 | `config/busybox.config` template generation | Replace hard-coded `/usr/lib/clang/22/include` with a `@@CLANG_RESOURCE_DIR@@` placeholder; root Makefile substitutes via sed before invoking `make -C thirdpart/busybox`. |
| 8 | root `Makefile` busybox target | Add libgcc stub creation (same pattern as existing libm/librt stub at lines 162–166). Use `$(LLVM_AR)`. |
| 9 | **NEW** `docs/build/toolchain.md` | User-facing documentation of the design + override mechanism. Cross-link from `docs/build.md` and AGENTS.md. |
| 10 | **NEW** `docs/superpowers/plans/2026-08-31-toolchain-refactor-plan.md` | Implementation plan with steps, RED-GREEN verification for each. |

## 5. The one C-source exception: `kernel/include/net/arch/cc.h`

Every other change touches build files. This one touches a C header because the existing typedefs fight with clang's builtin stdint.h. We can either:

- **Option A** (chosen): Replace the eight typedefs with versions sourced from clang's `__UINT8_TYPE__` / `__INT8_TYPE__` / ... builtins. This means the typedefs in `cc.h` always match what clang builtin stdint.h provides, regardless of clang version or LP64/LLP64 platform.
- **Option B**: Drop the eight typedefs entirely and rely on the clang builtin `<stdint.h>` (already included transitively via `<kernel/log.h>` → `<device/timer.h>` → `<stdint.h>`). Risk: if a future code path removes that transitive include, we silently lose uint64_t/int64_t.

Chosen: **Option A**. Reasons: (1) cc.h was written specifically to bypass a known `<sys/types.h>`/`ssize_t` conflict; we should preserve that intent. (2) The builtins are guaranteed to exist on any clang target.

The new cc.h typedef block:

```c
typedef __UINT8_TYPE__   uint8_t;
typedef __INT8_TYPE__    int8_t;
typedef __UINT16_TYPE__  uint16_t;
typedef __INT16_TYPE__   int16_t;
typedef __UINT32_TYPE__  uint32_t;
typedef __INT32_TYPE__   int32_t;
typedef __UINT64_TYPE__  uint64_t;
typedef __INT64_TYPE__   int64_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTPTR_TYPE__  intptr_t;
```

Verified locally on clang 18 that this resolves the redefinition error and preserves the build.

## 6. The `-lgcc` removal justification

Investigation:

```
$ llvm-nm --undefined-only build/x86_64/kernel/kernel.elf
(empty — no undefined externals)
```

`kernel.elf` has no unresolved symbols. x86_64 has direct `div r64` / `idiv r64` instructions for 64-bit operands; clang emits them inline rather than calling `__divdi3` / `__udivdi3`. libgcc's purpose (providing soft-float and 64-bit-div helpers for targets that lack them) is therefore moot on x86_64.

`ARCH_LIBS = -nostdlib -lk` is sufficient. `-lgcc` is removed.

If a future aarch64 port needs soft-float helpers, we can add `-lgcc` back conditionally (e.g. `ifneq ($(filter aarch64%,$(TARGET_TRIPLE)),) ARCH_LIBS += -lgcc endif`) at that time.

## 7. Verification matrix

Each step has a RED-GREEN-REFACTOR test. Steps are sequenced so each one's GREEN unblocks the next.

| Step | Test (RED → GREEN) | Pass criterion |
|---|---|---|
| S1: introduce toolchain.mk, no behavioral change | `make test` | 16/16 suites pass (unchanged from baseline) |
| S2: switch root Makefile to `include toolchain.mk` | `make test` + `make build/x86_64/uefi/BOOTX64.EFI` | UEFI build succeeds; tests pass |
| S3: add `-Wl,-z,norelro -Wl,--no-relax` to ARCH_LDFLAGS | `make kernel.bin` on clang 18 host | kernel.elf links without GOTPCREL error |
| S4: kallsyms uses HOST_CC/HOST_CFLAGS | `make kernel.bin` on clang 18 host | kallsyms compiles + assembles without stdarg.h error |
| S5: cc.h typedef from builtins | `make kernel.bin` on clang 18 host | kernel/net compiles without typedef-redefinition |
| S6: install stdarg.h to sysroot | `make install-headers` then `make disk.img` | busybox starts compiling past stdio.h |
| S7: busybox clang path templated | `make disk.img` on clang 18 host | busybox links without `-lgcc` error |
| S8: libgcc stub created in sysroot | `make disk.img` | busybox link completes; `busybox_unstripped` produced |
| S9: full E2E | `make disk.img && make run` (timeout 60s) | QEMU boots, kernel prints banner, init.elf runs |
| S10: cross-version check | Repeat S9 with `make CLANG=clang-22` (on homeserver) | Same boot sequence |

## 8. Risk analysis

| Risk | Likelihood | Mitigation |
|---|---|---|
| kallsyms output format depends on `llvm-nm` flags that vary across versions | Low | Use `-n` (sort by address); universally supported. If needed, pin to a flag set explicitly. |
| `clang -print-resource-dir` returns a different layout on aarch64 host | Medium | Detect and warn but don't fail; the resource dir is used only for builtin headers, which clang itself consumes correctly. |
| A future code path reintroduces a `__divdi3`-style libgcc dependency | Low | Re-add `-lgcc` at that point with a comment explaining why. CI matrix would catch it; without CI, we rely on `llvm-nm --undefined-only` as a smoke test in the make recipe. |
| The change touches a `cc.h` source file, breaking the "no source change" principle | N/A | Documented exception (§5). Pre-existing breakage that the refactor must fix. |

## 9. Non-goals

- Not introducing a separate `platform/x86_64.mk`/`platform/aarch64.mk` split (the `kernel/arch/<arch>/make.config` files already serve this role and are well-designed)
- Not changing the directory layout
- Not modifying any OS01 kernel C/asm sources except `kernel/include/net/arch/cc.h` (the documented exception)
- Not introducing CI (explicitly declined)
- Not addressing the **separate** aarch64 build issue (root Makefile line 21 `export CFLAGS=--sysroot=${SYSROOT}` overriding aarch64-specific sysroot); that is a distinct problem tracked elsewhere

## 10. Rollout plan

1. Land `toolchain.mk` and the root Makefile change as commit **A**. No other change.
2. Verify S1+S2 on aoostar-n100 (Ubuntu 24.04, clang 18) and homeserver (Arch, clang 22).
3. Land `make.config` link-flag fix + `Makefile` kallsyms HOST_CC switch as commit **B**.
4. Verify S3+S4.
5. Land `cc.h` typedef change as commit **C** (smallest possible diff).
6. Verify S5.
7. Land libc `install-headers` (stdarg.h) + root Makefile busybox templating + libgcc stub as commit **D**.
8. Verify S6+S7+S8.
9. Run S9 E2E on aoostar-n100; S10 on homeserver.
10. Land `docs/build/toolchain.md` as commit **E**.

Each commit lands independently and is revertably safe. Any commit that breaks `make test` is reverted before proceeding.

## 11. Open questions for review

1. Should `toolchain.mk` live at repo root (`toolchain.mk`) or under `build/toolchain.mk`? Current proposal: repo root, sibling to top-level `Makefile`. Both work; the root-level position matches GNU Make convention.
2. Should the `-lgcc` removal happen in this series or be left for a separate audit? Current proposal: in this series, with a clear commit message explaining the verification (`llvm-nm --undefined-only`).
3. Should we add a `make doctor` target that prints toolchain detection results (CLANG version, resource dir, sysroot layout, expected `libgcc` availability)? Useful for debugging "why does my build fail" reports.
4. Should we keep `-lgcc` in ARCH_LIBS but make it conditional on `ifneq ($(filter aarch64%,$(TARGET_TRIPLE)),)` so the aarch64 port inherits it when needed? Adds ~3 lines; lowers future risk.
