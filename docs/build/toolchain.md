# OS01 x86_64 Clang/LLVM Toolchain Override Contract

This document is the operator-facing reference for the x86_64 Clang/LLVM build
contract that Commits A-H of the toolchain refactor (see
[`docs/superpowers/specs/2026-08-31-toolchain-refactor-design.md`](../../superpowers/specs/2026-08-31-toolchain-refactor-design.md))
established on top of the pre-existing OS01 Makefiles. It documents the
overrides you can pass on the `make` command line, where each override is
validated, and which `make` target to run to check the resulting artifacts.

> **2026-09-02 update (GNU Make build refactor):** the toolchain contract
> moved into the profile system. The single source of truth is now
> `mk/toolchains/clang.mk`, included by the `x86_64-clang` profile
> (`mk/profiles/x86_64-clang.mk`); every component receives the tools through
> the profile (`OS01_PROFILE_FILE`), never through the environment. The
> validation rules below (Clang-only `CLANG`, `CC=cc` rejection, `LLVM_*`
> PATH-safety, `UEFI_CLANG`) are unchanged in behavior — see
> [Profile mode and allowed overrides](#profile-mode-and-allowed-overrides)
> for the current entry points, and note the **controlled environment** rule.

## Profile mode and allowed overrides

Since the GNU Make refactor (2026-09-02), builds are profile-driven:
`make PROFILE=x86_64-clang <target>` (the default profile) or
`make PROFILE=aarch64-clang <target>` (aarch64 UEFI bring-up). The x86_64
profile includes `mk/toolchains/clang.mk` (discovery + validation) and
`mk/targets/x86_64.mk` (QEMU/run parameters).

### Overrides that reach the build

The refactor defines an explicit **whitelist of overrides** that may cross
the controlled sub-make boundary (`OS01_SUBMAKE_ALLOWED` in `mk/project.mk`).
Everything else — `CFLAGS`, `CPPFLAGS`, `CXXFLAGS`, `ASFLAGS`, `LDFLAGS`,
`CC`, `LD`, `AR`, `PREFIX` and any unlisted environment variable — is
**never** propagated to component or third-party builds.

Allowed on the command line:

| Override | Effect |
|----------|--------|
| `CLANG=clang-N` / `CLANG=/abs/path/clang` / `CLANG='ccache clang'` | validated Clang for kernel + userland (preferred override) |
| `UEFI_CLANG=...` | validated Clang for the UEFI COFF build (defaults to `CLANG`) |
| `LLVM_AR` / `LLVM_NM` / `LLVM_OBJCOPY` / `LLVM_READOBJ` / `LLVM_READELF` | individual LLVM tool overrides (PATH-validated) |
| `TARGET_LD=...` | linker override (PATH-validated) |
| `QEMU_BIN=...` / `AARCH64_QEMU=...` | QEMU binary overrides |
| `AARCH64_UEFI_FIRMWARE_SOURCE=...` | aarch64 firmware source path |
| `LOG_TARGET=serial\|fb\|both` | kernel log output target |
| `KERNEL_SELFTEST=1` | in-kernel selftests at boot |
| `OS01_SYSTEST=1` | **variant switch** — systest image variant; also the only compile-affecting variant (adds `-DOS01_SYSTEST` to user CFLAGS) |
| `OS01_NETTEST=1` | **variant switch** — nettest image variant |
| `INITTAB_FILE=config/inittab.test` | **variant switch** — inittab-test image variant |

Each `name=value` whitelisted override is passed explicitly as a command-line
argument to the controlled sub-makes; it is never replayed through
`MAKEFLAGS`.

### The controlled environment

Every OS01-owned recursive Make (components and third-party adapters) runs
under the `os01_submake` helper (`mk/project.mk`):

```make
+env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
    MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
    -C <component> OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" <whitelisted-args>
```

`env -i` starts from an empty environment; only `PATH`, `HOME`, `TMPDIR`, a
sanitized `MAKEFLAGS` (GNU Make options only — no `VAR=VALUE` assignments)
and the whitelisted overrides cross the boundary. A sub-Make therefore never
inherits an ambient `CFLAGS`, `CC`, `PREFIX` or other unlisted variable from
the calling shell. Third-party upstream builds (BusyBox, posix-uefi) are
called the same way, receiving only the whitelist-derived tools, flags and
output directories they support.

### Capability errors

Entry points are capability-aware. Invoking a target the selected profile
cannot provide fails at parse time, before any compilation:

```text
$ make PROFILE=aarch64-clang run
Makefile:...: *** PROFILE='aarch64-clang' lacks capability 'rootfs'.  Stop.

$ make PROFILE=x86_64-clang aarch64-uefi
mk/components/run.mk:...: *** PROFILE='x86_64-clang' lacks capability 'uefi-bringup'.  Stop.
```

`make validate` also prints the resolved profile identity:

```text
$ make validate-profile
profile=x86_64-clang triple=x86_64-unknown-none sysroot=/.../build/x86_64-clang/sysroot capabilities=kernel userland rootfs uefi
```

### Deferred Termux `-lgcc` work (out of scope)

Per the build-system design spec (2026-09-02), replacing the `-lgcc` link
argument with a compiler-runtime archive (`TARGET_RUNTIME_ARCHIVE`) is a
**separate, future toolchain spec**. This refactor keeps the existing link
arguments and the behavior of the working profiles exactly as they were; the
`-lgcc` resolution, discovery and validation work is not part of this
change. (Historical context on why `-lgcc` is required today — the
`__udivti3` symbol in `kernel/time/clocksource.c` — is in the
[libgcc / `-lgcc`](#libgcc---lgcc) section below.)

## Single source of truth: root `toolchain.mk`

> The sections below document the pre-refactor standalone contract. The root
> `toolchain.mk` still exists and is still included by component Makefiles in
> their **standalone** (non-profile) mode, but the refactored build path goes
> through `mk/toolchains/clang.mk` via the profiles. The validation behavior
> described below is preserved in both paths.

`toolchain.mk` is included, **only when `ARCH=x86_64`**, by:

| Makefile | Include line |
|----------|--------------|
| Root `Makefile` | `include $(dir $(ROOT_MAKEFILE))toolchain.mk` |
| `kernel/Makefile` | `include $(dir $(LOCAL_MAKEFILE))../toolchain.mk` |
| `libc/Makefile` | `include $(dir $(LOCAL_MAKEFILE))../toolchain.mk` |
| `user/Makefile` | `include $(dir $(LOCAL_MAKEFILE))../toolchain.mk` |
| `boot/uefi/Makefile` | `include $(ROOT)/toolchain.mk` |
| `kernel/arch/x86_64/make.config` | `include $(dir $(ARCH_CONFIG))../../../toolchain.mk` |

Each include is gated by `ifeq ($(ARCH),x86_64)`. Under `make ARCH=aarch64
-C kernel` (or any other aarch64 entry point) the guard is false, the include
is skipped, and **no x86 toolchain variables leak into the aarch64 subtree**.
This is the load-bearing property that lets both the root `Makefile:285` and
`kernel/Makefile:289` keep their plain aarch64 recursive invocations. See
the [aarch64 out-of-scope](#aarch64-out-of-scope) section for the proof.

`libc/arch/x86_64/make.config` does not include `toolchain.mk`: the parent
`libc/Makefile` includes the common file before it includes the arch config
on its own line, and the config defines no compiler override. Idempotency:
`toolchain.mk` is included twice in kernel builds (once from `kernel/Makefile`,
once from `kernel/arch/x86_64/make.config`); GNU make parses it twice with no
ill effect, but this is a known minor inefficiency flagged for future work.

## Override variables

All overrides below are command-line `name=value` settings, e.g.
`make kernel.bin CLANG=clang-22 LLVM_NM=/opt/llvm/bin/llvm-nm`. Most have
sensible defaults derived from `CLANG` itself, so you rarely need to set
more than `CLANG`.

### `CLANG` — the preferred override

`CLANG ?= clang`. `CLANG=clang-N` is the preferred override. The contract
accepts:

- a basename on `PATH` (e.g. `CLANG=clang-22`)
- an absolute path (`CLANG=/opt/llvm/bin/clang`)
- a Clang wrapper (e.g. `CLANG=ccache clang-22`) — wrapper output must list a
  Clang target in its banner
- the unset default (`CLANG` defaults to `clang`)

It rejects (with a hard `$(error)`) anything whose banner does not contain
the literal `clang` substring:

```
CLANG='<value>' is not Clang
```

The validation is the executed-binary banner — `CLANG=foo` is rejected even
if `foo` happens to be a shell alias for Clang, because the shell never
sees it. The validation also requires that `$(CLANG) -print-resource-dir`
returns a non-empty string:

```
CLANG='<value>' is unavailable or lacks -print-resource-dir
```

Resource-dir validation is what protects `$(TARGET_CC)` from silently
falling back to the host default include search paths when a wrapper or
sanitised binary strips driver plumbing.

### `CC` — accepted only when it is Clang

GNU make's built-in default `CC=cc` is the only implicit value replaced.
The policy, evaluated by `toolchain.mk`, is:

1. **Default / undefined `CC`** → `EFFECTIVE_CC := $(TARGET_CC)` (i.e.
   `$(CLANG) --target=x86_64-unknown-none -ffreestanding -fno-builtin`).
   You never need to think about `CC` on x86_64.
2. **`CC=cc` (explicit)** → rejected:
   ```
   CC=cc is not permitted; use CLANG=clang-N or an explicit Clang command
   ```
   This is the categorical rejection that runs **before** the banner check,
   so even if `/usr/bin/cc` happens to be a Clang symlink on your host, it is
   still rejected — using `cc` was the failure mode that motivated the
   policy, and we do not want to silently allow it.
3. **`CC='ccache gcc'` / `CC=gcc-12` / `CC=gcc` etc.** → rejected by the
   banner check:
   ```
   CC='<value>' (origin <origin>) is not Clang; use CLANG=clang-N or a Clang wrapper
   ```
4. **`CC=clang-22` / `CC=/opt/clang/bin/clang` / `CC='ccache clang'`** →
   accepted. `EFFECTIVE_CC := $(CC) --target=$(TARGET_TRIPLE)
   -ffreestanding -fno-builtin`. The Clang driver's `--target=`,
   `-ffreestanding`, and `-fno-builtin` are appended to your `CC`, so the
   target identity is preserved regardless of what `$(CC)` itself prints.

`EFFECTIVE_CC` is the only `CC` the x86_64 target recipes consume. A
`ccache`-wrapped Clang that accepts ordinary Clang driver flags works fine;
a wrapper that filters flags will not.

### `LLVM_*` and `TARGET_LD` — discovered from `CLANG`

These are discovered lazily by `$(CLANG) -print-prog-name=...`:

| Variable | Default (when `LLVM_X=` is unset) |
|----------|-----------------------------------|
| `LLVM_AR` | `$(CLANG) -print-prog-name=llvm-ar` |
| `LLVM_NM` | `$(CLANG) -print-prog-name=llvm-nm` |
| `LLVM_OBJCOPY` | `$(CLANG) -print-prog-name=llvm-objcopy` |
| `LLVM_READOBJ` | `$(CLANG) -print-prog-name=llvm-readobj` |
| `LLVM_READELF` | `$(CLANG) -print-prog-name=llvm-readelf` |
| `TARGET_LD` | `$(CLANG) -print-prog-name=ld.lld` |

After discovery, each is passed through a `require_program` macro that
verifies `command -v` resolves to an executable on `PATH`:

```
<NAME>='<value>' is not executable or on PATH; override <NAME>=/absolute/path
```

This is the second-line defence: a `CLANG=clang-22` that ships a Clang
binary without the matching LLVM utilities (rare but possible) is caught
here, and you can override each individually (`LLVM_NM=/abs/path/to/llvm-nm`
on the command line). The PATH-safety means a wrapper around `clang` that
shadows `llvm-ar` with a different toolchain's `llvm-ar` cannot silently
break the build.

`toolchain.mk` then exposes:

```
AR := $(LLVM_AR)
LD := $(TARGET_LD)
OBJ_CPY := $(LLVM_OBJCOPY)
```

as immediate (`:=`) assignments — these are immune to GNU make's built-in
default `AR=ar`, `LD=ld`, `OBJ_CPY=objcopy` quirk and are still overridable
on the command line.

## Driver / raw linker split

OS01 splits kernel and user linking into two passes with two distinct flag
groups, because the Clang driver and raw `ld.lld` speak different flag
dialects.

### `KERNEL_DRIVER_LDFLAGS` — for the driver-linked stage 1

Used by the kernel stage1 recipe (`kernel/Makefile:206`) and the early
BusyBox config probe. All flags are prefixed with `-Wl,` because the
Clang driver expects driver-syntax:

```
KERNEL_DRIVER_LDFLAGS := -Wl,-m -Wl,elf_x86_64 -static -Wl,-z,muldefs \
                         -Wl,-z,norelro -Wl,--no-relax
```

### `KERNEL_RAW_LDFLAGS` — for the raw final link

Used by the kernel final-link recipe (`kernel/Makefile:226`) and the user
program final-link recipes (`user/Makefile:73,83,95`). The same flags, minus
the `-Wl,` prefix because raw `ld.lld` does not understand driver syntax:

```
KERNEL_RAW_LDFLAGS := -m elf_x86_64 -static -z muldefs -z norelro --no-relax
KERNEL_RAW_LIBDIR := -L$(TARGET_LIBDIR)
USER_RAW_LDFLAGS  := -m elf_x86_64 -static -no-pie -T linker.ld -L$(TARGET_LIBDIR)
```

Conversions are exact:

| Driver | Raw |
|--------|-----|
| `-Wl,-m -Wl,elf_x86_64` | `-m elf_x86_64` |
| `-Wl,-z,norelro` | `-z norelro` |
| `-Wl,-z,muldefs` | `-z muldefs` |
| `-Wl,--no-relax` | `--no-relax` |

`-nostdlib` is driver-only — it never reaches raw `ld.lld`. The font,
trampoline-embed, and trampoline-bin recipes need `RAW_LD_EMULATION`:

```
RAW_LD_EMULATION := -m elf_x86_64
```

because `ld.lld` errors with `target emulation unknown` when every input is
`-b binary` and no `-m` is given.

### libgcc / `-lgcc`

The kernel's `ARCH_LIBS` are `-nostdlib -lk -lgcc`. `-lgcc` is resolved by
the Clang driver against the host GCC's private `libgcc` directory, which
raw `ld.lld` does **not** search by default. To keep the raw final link
behaving identically to the driver stage1, `kernel/arch/x86_64/make.config`
appends one extra `-L`:

```
KERNEL_RAW_LIBDIR := -L$(TARGET_LIBDIR) -L$(dir $(shell $(CLANG) -print-libgcc-file-name))
```

Discovery via the validated `$(CLANG)` preserves the exact prior driver
resolution.

`-lgcc` is genuinely required today: `kernel/time/clocksource.c` uses
`__uint128_t` division in `compute_mult_shift`, which lowers to the
compiler-rt `__udivti3` symbol that only `-lgcc` provides. The gated
"remove `-lgcc`" experiment in Commit K verified this by failing the
clean-build link with:

```
undefined reference to `__udivti3'
relocation truncated to fit: R_X86_64_PLT32 against undefined symbol `__udivti3'
```

(see `.superpowers/sdd/2026-08-31-toolchain-refactor-plan/task-K-report.md`).
Removing `-lgcc` is a future kernel-runtime series, **not** part of this
toolchain refactor.

## `make validate`

`make validate` runs seven independent artifact checks against the unstripped
kernel ELF and the generated UEFI EFI binary. Each check fails the build on
a non-zero exit, so the entire target fails closed if any single check fails.

```
.PHONY: validate validate-kernel validate-uefi
validate: validate-kernel validate-uefi
validate-kernel: kernel.bin
        @echo "  [validate] kernel.elf has no undefined symbols"
        @test -z "$$($(LLVM_NM) --undefined-only $(KERNEL_ELF))"
        @echo "  [validate] kernel.elf has no INTERP/DYNAMIC program headers"
        @! $(LLVM_READELF) -Wl $(KERNEL_ELF) | grep -E 'INTERP|DYNAMIC'
        @echo "  [validate] kernel.elf machine is EM_X86_64"
        @$(LLVM_READOBJ) --file-headers $(KERNEL_ELF) | grep -F 'EM_X86_64'
        @echo "  [validate] GLOBAL _start present"
        @$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\b_start\b'
        @echo "  [validate] GLOBAL kernel_main present"
        @$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\bkernel_main\b'
        @echo "  [validate] GLOBAL _text present"
        @$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\b_text\b'
validate-uefi: $(BUILD_X86_64_UEFI)
        @echo "  [validate] BOOTX64.EFI coff-exports"
        @$(LLVM_READOBJ) --coff-exports $(UEFI_EFI) >/dev/null
```

What each check proves:

1. `$(LLVM_NM) --undefined-only` is empty → the kernel ELF has zero
   unresolved symbols (statically linked, no dynamic loader).
2. `$(LLVM_READELF) -Wl` has no `INTERP` / `DYNAMIC` program headers →
   the kernel is a true static binary, not a PIE waiting for a loader.
   (`-Wl` reads program headers; `-h` reads only the ELF header, so this
   matters.)
3. `$(LLVM_READOBJ) --file-headers` reports `EM_X86_64` → the kernel ELF
   machine identity is x86_64.
4. Three independent `$(LLVM_READELF) -Ws` checks each require `GLOBAL`
   binding for `_start`, `kernel_main`, and `_text` → the three load-bearing
   entry symbols are global (not local / weak / hidden) and exist.
   They are asserted **separately** — one broad match is not a substitute.
5. `$(LLVM_READOBJ) --coff-exports` on `$(UEFI_EFI)` succeeds → the
   generated EFI app has a parseable COFF export table (proves the EFI is
   a valid PE32+ image, not an ELF masquerading as one).

`KERNEL_ELF` and `UEFI_EFI` defaults live in `toolchain.mk`:

```
KERNEL_ELF ?= $(TOOLCHAIN_DIR)build/x86_64/kernel/kernel.elf
UEFI_EFI   ?= $(TOOLCHAIN_DIR)build/x86_64/uefi/BOOTX64.EFI
```

Both are overridable, so you can run `make validate KERNEL_ELF=...` against
an out-of-tree artifact.

## UEFI x86_64 contract (`boot/uefi/Makefile`)

The UEFI bootloader runs the Clang driver and `lld -flavor link` against a
**separate** copied runtime at `build/x86_64/uefi-runtime/` to keep x86 COFF
objects isolated from kernel ELF objects. The wrapper is
`boot/uefi/Makefile`, with two extra overrides layered on top of the
`toolchain.mk` contract.

### `UEFI_CLANG`

`UEFI_CLANG ?= $(CLANG)` — by default inherits the validated `CLANG` (so you
do not need to set it). A separate `UEFI_CLANG_ID` banner check plus a
`-print-resource-dir` validation reject a UEFI-only override that is not
Clang or lacks a resource dir (the failures look the same as the `CLANG`
failures above but cite `UEFI_CLANG`). You can prove the guard with:

```
$ make -C boot/uefi ARCH=x86_64 UEFI_CLANG=gcc -n
Makefile:50: *** UEFI_CLANG='gcc' is not Clang.  Stop.
```

### `UEFI_LD_TOOL` / `UEFI_LD`

`UEFI_LD_TOOL ?= $(TARGET_LD)`, validated by `require_program` so a missing
or non-executable `ld.lld` is caught. `UEFI_LD := $(UEFI_LD_TOOL) -flavor
link` is what the inner posix-uefi make actually sees. The proof of the
`require_program` check:

```
$ make -C boot/uefi ARCH=x86_64 UEFI_LD_TOOL=/nonexistent -n
Makefile:57: *** UEFI_LD_TOOL='/nonexistent' is not executable or on PATH; override UEFI_LD_TOOL=/absolute/path.  Stop.
```

### Inner-make invocation

The inner `$(MAKE) -C $(RUNTIME_DIR)` is invoked with:

```
MAKEOVERRIDES= USE_GCC= CC="$(UEFI_CLANG)" LD="$(UEFI_LD)"
```

- `MAKEOVERRIDES=` blocks the parent's command-line overrides from being
  replayed into the child (so a root-level `CFLAGS=...` does not silently
  leak).
- `USE_GCC=` forces the inner Makefile to Clang even if its auto-detect
  looks at the host's default `gcc`.
- Command-line `CC=` / `LD=` overrides **copy** the assignments, so the
  inner make sees the validated wrappers.
- `unexport CFLAGS LDFLAGS` is set in the wrapper so any outer environment
  `CFLAGS` / `LDFLAGS` cannot reach the inner make. This is critical:
  a literal `CFLAGS=` on the inner command line would **wipe** the inner
  `CFLAGS += --target=x86_64-pc-win32-coff` and the COFF linker flags at
  `uefi/Makefile:63-65` (GNU make ignores `+=` appends against a
  command-line-origin variable), producing ELF objects instead of COFF.
  `unexport` is the correct fix: it blocks inheritance without resetting
  the inner make's own `+=` chain.

Proof of the dry-run contract line:

```
$ rm -f build/x86_64/uefi/BOOTX64.EFI
$ make -C boot/uefi ARCH=x86_64 -n
make -C /home/aagu/OS01/build/x86_64/uefi-runtime ARCH=x86_64 TARGET=BOOTX64.EFI \
  OUTDIR=/home/aagu/OS01/build/x86_64/uefi/ \
  SRCS="/home/aagu/OS01/boot/uefi/main.c /home/aagu/OS01/boot/uefi/arch/x86_64/boot.c" \
  MAKEOVERRIDES= USE_GCC= CC="clang" LD="/usr/bin/ld.lld -flavor link"
```

## aarch64 out of scope

The aarch64 build deliberately has **no x86 toolchain variables**: it
does not include `toolchain.mk`, it does not see `--target=x86_64-unknown-none`,
and it does not export any of the validated tools to the inner make. Proof:

```
$ make -C boot/uefi ARCH=aarch64 -n > /tmp/aarch64-dryrun.log
$ grep -c 'x86_64-unknown-none' /tmp/aarch64-dryrun.log
0
$ grep -c 'CC="clang"' /tmp/aarch64-dryrun.log
0       # UEFI_CLANG not set; inner make auto-detects clang itself
$ grep -c 'MAKEOVERRIDES' /tmp/aarch64-dryrun.log
0       # wrapper else-branch is the plain recursive invocation
```

```
$ make -C kernel ARCH=aarch64 -n > /tmp/aarch64-kernel-dryrun.log
$ grep -c 'x86_64-unknown-none' /tmp/aarch64-kernel-dryrun.log
0
```

The aarch64 kernel uses its own per-arch make.config (`kernel/arch/aarch64/make.config`)
which sets `clang -target aarch64-none-elf` directly. `toolchain.mk`'s
`ifeq ($(ARCH),x86_64)` guards never fire for aarch64, so neither
`CLANG_ID`, `EFFECTIVE_CC`, nor any `LLVM_*` variable is ever set in the
aarch64 DAG.

## Verification matrix (this host)

Host: homeserver, `clang-22.1.8` (`/usr/bin/clang` = `clang-22.1.8`).
`clang-18` is **not** installed on this host — see [Manual follow-up](#manual-follow-up-clang-18).

| Step | Check | Result |
|------|-------|--------|
| 1 | `make validate` | **7/7 pass**, exit 0 (all labels printed; `Machine: EM_X86_64 (0x3E)`, three `GLOBAL` symbols, BOOTX64.EFI coff-exports OK) |
| 2 | `make test` | **16/16 suites**, 0 failed (full pass: `Suites: 16 \| Failed: 0`) |
| 3 | `make kernel.bin` | exit 0; `kernel.bin` = 1,706,256 bytes |
| 3 | `make user` | exit 0; 19 ELFs in `build/x86_64/user/`, including `busybox.elf` = 192,976 bytes |
| 4 | `rm -f disk.img && make disk.img` | exit 0; `disk.img` = 202,375,168 bytes (192 MB, GPT dual-partition) |
| 5 | Headless QEMU boot (`-M q35 -smp 2 -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd … -serial stdio -no-reboot`) | reached userspace; last lines of the boot log:<br>`+--------------------------------+`<br>`\|  OS01 Init v1.0 (PID 1)        \|`<br>`+--------------------------------+`<br>`init: running as PID 2`<br>`init: phase SYSINIT`<br>`init: phase WAIT`<br>`init: phase ONCE`<br>`init: entering supervision loop`<br>`init: started pid 3: '/bin/terminal' (respawn)`<br>`BusyBox v1.36.1 (2026-09-01 00:14:57 CST) built-in shell (ash)`<br>`#` |
| 6 | `make -C boot/uefi ARCH=aarch64 -n` then `grep -c x86_64-unknown-none` | **0** (no x86 toolchain triple in the aarch64 dry-run) |

### Manual follow-up: `clang-18`

The plan (S16) calls for a clean-build matrix on both `CLANG=clang-18` and
`CLANG=clang-22`. This host has only `clang-22.1.8` installed, so the
`clang-18` leg is recorded here as a **manual follow-up**. To reproduce the
verification matrix on a host that has both Clang versions:

```
make clean && CLANG=clang-18 make kernel.bin && CLANG=clang-18 make validate
CLANG=clang-18 make test
make clean && CLANG=clang-22 make kernel.bin && CLANG=clang-22 make validate
CLANG=clang-22 make test
```

The expected outcome, based on the validation contract, is that all seven
`make validate` checks pass identically under either Clang version, and
that `make test` reports `Suites: 16 | Failed: 0`. The contract is designed
to be version-agnostic — `CLANG=clang-N` only requires that `clang-N`'s
banner contains `clang` and that `clang-N -print-resource-dir` returns a
non-empty path.

## Worked examples

**Default build** — equivalent to `CLANG=clang`:

```
make kernel.bin           # uses clang, target=x86_64-unknown-none
make test                 # 16 host-test suites
```

**Pin a specific Clang**:

```
make kernel.bin CLANG=clang-22
make test    CLANG=clang-22
make validate KERNEL_ELF=$PWD/build/x86_64/kernel/kernel.elf \
             UEFI_EFI=$PWD/build/x86_64/uefi/BOOTX64.EFI
```

**Use a toolchain outside `PATH`** (e.g. a custom-built LLVM):

```
LLVM_NM=/opt/llvm-22/bin/llvm-nm \
LLVM_READELF=/opt/llvm-22/bin/llvm-readelf \
LLVM_READOBJ=/opt/llvm-22/bin/llvm-readobj \
make validate
```

**Override the UEFI Clang independently** (rare — usually `UEFI_CLANG` is
inherited from `CLANG`):

```
make boot/uefi/OVMF.fd UEFI_CLANG=clang-22 UEFI_LD_TOOL=/opt/llvm-22/bin/ld.lld
```

## Cross-references

- Spec: [`docs/superpowers/specs/2026-08-31-toolchain-refactor-design.md`](../../superpowers/specs/2026-08-31-toolchain-refactor-design.md)
  (§2 common file, §3 Clang contract, §4 driver/raw split, §6 UEFI isolation,
   §7 aarch64 proof, §8 validation)
- Plan: [`docs/superpowers/plans/2026-08-31-toolchain-refactor-plan.md`](../../superpowers/plans/2026-08-31-toolchain-refactor-plan.md)
  (Commit M: lines 78-84; matrix S1-S17)
- Build (Chinese): [`docs/build.md`](../build.md)
- Build/run/debug (English): [`docs/build-run-debug.md`](../build-run-debug.md)
- AGENTS quick reference: [`AGENTS.md`](../../AGENTS.md)
