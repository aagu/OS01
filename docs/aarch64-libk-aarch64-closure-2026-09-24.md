# AArch64 libk.a Linking + Cleanup of Freestanding Stubs — Closure Report (AAGU-29)

**Date**: 2026-09-24
**Branch**: `fix/aagu-29-libk-aarch64` (based on `refactor/aagu-6-p2-cleanup-batch @ f56ea77`)
**Issue**: AAGU-29 (parent: AAGU-6)
**Commits**: 1 functional closure (this commit; AAGU-6 carries the `kernel/compiler_rt/memset.c` weak-fallback it depends on)

## Goal achieved

aarch64 kernel now links `libk.a` for kernel-side libc symbols (memcpy/memset/memmove/strlen/strcmp/calloc/free/realloc/...), eliminating the aarch64 phase-1 "fully freestanding" assumption that required:

- `kernel/compiler_rt/memset.c` — weak byte-fill fallback (only memset source for aarch64 phase 1).
- `kernel/arch/aarch64/libc_stub.c` — calloc/free shims that delegated to slab.

Both files were deleted once libk.a's strong symbols resolved at link time and QEMU boot + kernel selftest stayed green.

## Build wiring changes

### 1. `kernel/arch/aarch64/make.config` — link libk.a

```
ARCH_LDFLAGS   = -fuse-ld=lld -static -Wl,-z,muldefs -Wl,-m -Wl,aarch64elf
ARCH_LDFLAGS  += -L$(LIBC_BUILD_DIR)/lib  # NEW (AAGU-29)
ARCH_LIBS      = -nostdlib -lk            # was -nostdlib
```

`$(LIBC_BUILD_DIR)` is profile-set (`mk/profiles/aarch64-clang.mk`: `LIBC_BUILD_DIR := $(BUILD_DIR)/libc`); `libk.a` lands at `$(LIBC_BUILD_DIR)/lib/libk.a` once the libc sub-make rule below builds it. Mirrors `kernel/arch/x86_64/make.config:45` (`ARCH_LIBS = -nostdlib -lk`).

### 2. `mk/components/image.mk` — aarch64 libk.a build-order prerequisite

The aarch64-clang profile has no `userland` capability, so `mk/components/sysroot.mk`'s libc staging + sysroot generation path doesn't fire. x86_64 reaches libk.a via the immutable sysroot generation; aarch64 needs its own directly. Added the rule below so the kernel link resolves `-lk`:

```
$(LIBC_BUILD_DIR)/lib/libk.a:
	@mkdir -p $(dir $@)
	@$(call os01_submake,libc,all ARCH=aarch64 $(OS01_SUBMAKE_ARGS))

$(BUILD_DIR)/artifacts/kernel.elf: $(LIBC_BUILD_DIR)/lib/libk.a FORCE
```

### 3. `libc/Makefile` — arch-aware source list

`HOSTARCH` now accepts `ARCH=<arch>` from the command line via `os01_submake`. Cross-compile builds use a kernel-side-only subset:

- aarch64: `ctype/*.c errno/*.c list/*.c rbtree/*.c stdlib/{calloc,malloc}.c string/*.c ssp/*.c-except-ssp.c`
- x86_64: full userland list (unchanged).

Excluded userland-only sources for aarch64 cross-compile: `network/*.c unistd/*.c signal/*.c syscall/*.c sys/stat/*.c socket/*.c termios/*.c stdio/*.c time/*.c sched/*.c csu/*.c pwd/*.c grp/*.c ...` — every one pulls in `libc/include/sys/syscall.h`, which uses x86_64 inline asm (`int $0x80`, `=a` constraint) and breaks the aarch64 front-end.

`LIBC_ARCHIVE` (libc.a) is empty for aarch64 — the kernel doesn't link libc.a and there's no aarch64 userland to stage it to. Build only libk.a.

### 4. `libc/arch/aarch64/make.config` — new aarch64 arch config (mirrors `arch/x86_64/make.config`)

```
ARCH_CFLAGS=-march=armv8-a -ffreestanding -mgeneral-regs-only -fno-pie -fno-pic
ARCH_FREEOBJS=
ARCH_HOSTEDOBJS=
```

### 5. `libc/stdlib/malloc.c` — `<sys/syscall.h>` + libc-mode helpers gated under `#ifndef __is_libk`

`malloc.c`'s libc mode (heap via `SYS_brk`) unconditionally included `<sys/syscall.h>`. Two changes:

- Wrapped `#include <sys/syscall.h>` in `#ifndef __is_libk` (the kernel side uses slab; the asm path is unreachable when `__is_libk`).
- Wrapped the libc-mode-only static helpers (`get_brk`, `set_brk`, `heap_init`, `align_up`, `try_coalesce`) and heap globals (`freelist`, `heap_initialized`) in `#ifndef __is_libk`. Keeps the libk build clean of x86_64 syscall references.
- The `malloc`/`free`/`realloc` `#if defined(__is_libk)` branches are unchanged (slab mode).

`malloc.c` continues to compile on x86_64 (the host build) with both branches intact.

### 6. `kernel/Makefile` — drop `arch/aarch64/libc_stub.c` from `KERNEL_C_SOURCES`

After deletion of `kernel/arch/aarch64/libc_stub.c`, the source list entry on line 51 had to be removed for the wildcard to resync (the dep file referenced the old target).

## Deletions (after verification)

| File | Replaced by | Verified |
|------|-------------|----------|
| `kernel/compiler_rt/memset.c` (weak memset fallback) | `libc/string/memset.c` (libk.a strong symbol) | aarch64 links + QEMU boot + selftest PASS |
| `kernel/arch/aarch64/libc_stub.c` (calloc/free slab shims) | `libc/stdlib/calloc.c` (libk.a, `kmalloc` via slab) + `libc/stdlib/malloc.c` (libk.a) | aarch64 links + QEMU boot + selftest PASS |

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi` (clean) | exit 0 (libk.a: 26 .libk.o members, 43 KB) |
| aarch64 kernel.bin size | 452984 bytes (was 451920 baseline, +1064 for added libc/kernel-header build deps in libk.a — both arches now byte-identical size of `libk.a` strong symbols it provides) |
| `nm -P libc/lib/libk.a` exposes `memset` `memcpy` `memmove` `strlen` `strcmp` `calloc` `free` `malloc` `realloc` as `T` | verified — strong symbols available to aarch64 kernel link |
| `nm -P kernel/kernel.elf` no longer carries `T memset` from `kernel/compiler_rt/memset.c` | verified |
| Single QEMU boot (--cpus 1 --repeat 1 --diagnostic-dtb=auto): kernel reaches `[gic] dispatch ready`, `[gic-probe] save-restore OK`, `[ipi] summary ... status=PASS`, `[clocksource] active=true`, 3+ `[tick] N` markers | `case result: PASS, complete: true, returncode: 0` |
| `make PROFILE=x86_64-clang test` — hosttests + qemutests + python E2E suites | **Suites: 27 \| Failed: 0** (byte-identical to AAGU-6 baseline; `libc/stdlib/malloc.c` libc-mode branch is unchanged when built on x86_64) |

## What's still NOT done (follow-ups)

1. **`kernel/include/compat/*` mirror headers** (Phase 2 #5 commit `23f916d`'s 6 mirror files: `string.h`, `stdlib.h`, `list.h`, `rbtree.h`, `sys/cdefs.h`, `sys/types.h`).
   - After AAGU-29, these are now removable: the kernel reaches `libc/include/<X.h>` paths through the same `-I$(CURDIR)/include/compat` step indirectly by staying at `kernel/include/compat/`. A follow-up issue should `git rm kernel/include/compat/` and update `kernel/Makefile:124` `-I` line to point at `$(CURDIR)/../libc/include` (still gated under `ifeq ($(ARCH),aarch64)`). Bundle this in a separate AAGU-29.x sub-issue.

2. **`kernel/arch/aarch64/subsys_stub.c`** still no-ops `register_subsys`/`subsys_init_phase` (Phase 2 #1 deferred item from `docs/aarch64-timer-phase2-closure-2026-09-18.md`). Pre-AAGU-29 scope; not changed by this commit.

3. **`kernel/arch/aarch64/idle_resume_stub.c`** likewise (Phase 2 #3 deferred). Pre-AAGU-29 scope.

## Connection to AAGU-4 / AAGU-6 / Phase 2 #5

Closes the latent cleanup queue recorded in:

- `docs/aarch64-timer-phase2-cntp-closure-2026-09-18.md:80` — "Replacing `libc_stub.c` with real libc — depends on libc sysroot for aarch64 (Phase 2 #5)" (now superseded by libk.a linking; the Phase 2 #5 userland sysroot path never engaged for aarch64 because the profile is `kernel uefi` only, but a **kernel-side** libk.a fully replaces it).
- AAGU-4 §2.4 design intent: "libc is SOLE public ABI" — aarch64 kernel now reuses the libc source tree for kernel-static libc instead of maintaining freestanding byte-fill fallback + slab-shim duplicates. The `kernel/compiler_rt/memset.c` weak fallback was the last remaining duplication (AAGU-6 P2-5 had already removed `kernel/arch/aarch64/memset.c`).

## Operational note

`make` doesn't recompile `.o` files on CFLAGS-only changes. Any verification run after CFLAGS edits must `make PROFILE=aarch64-clang clean` first, OR touch the affected `.c` files. The libc cross-compile path adds one more dependency on this rule (libk.a target uses `clang` invocation flags that include `__is_libk`; the libc.a invocation uses only `__is_libc` — both `.libk.o` and `.o` builds of the same `.c` are tracked separately so this is mostly invisible).
