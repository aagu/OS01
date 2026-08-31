# OS01 Toolchain Refactor — Implementation Plan

Spec: `docs/superpowers/specs/2026-08-31-toolchain-refactor-design.md` (v3)
Date: 2026-08-31
Status: pending codex review of spec v3

## Summary

Nine commits (A, B, C, D, E, F, G, H, K, M) sequenced for rollback safety. Each commit's verification runs on **both** clang 18 (aoostar-n100) and clang 22 (homeserver). `-lgcc` removal is Commit K and is the last code change, gated by the seven-step validation in spec §6.

---

## Commit A: introduce `toolchain.mk` + remove `:=` overrides from every x86_64 first-party Makefile

### Files (this is a large commit; review carefully)
- **NEW**: `toolchain.mk` (per spec §3)
- `Makefile` (root): add `include toolchain.mk`; remove `export AR=llvm-ar` and `export OBJ_CPY=llvm-objcopy`
- `kernel/Makefile`: add `include toolchain.mk`
- `kernel/arch/x86_64/make.config`: add `include toolchain.mk`; **remove** `CC := clang -target x86_64-unknown-none` (line 4) and `LD := ld.lld -m elf_x86_64` (line 5); replace with `CC ?= $(TARGET_CC)` and `LD ?= $(TARGET_LD)`
- `libc/Makefile`: add `include toolchain.mk`; **remove** `CC := clang -target x86_64-unknown-none` (line 10), `LD := ld.lld -m elf_x86_64` (line 11), `AR := llvm-ar` (line 12); replace with `?=` deferring to `$(TARGET_CC)`/`$(TARGET_LD)`/`$(LLVM_AR)`
- `user/Makefile`: add `include toolchain.mk`; **remove** `CC ?= clang ...` / `LD ?= ld.lld ...` / `OBJ_CPY ?= llvm-objcopy` (lines 6–8); replace with `?=` deferring to `$(TARGET_CC)`/`$(TARGET_LD)`/`$(LLVM_OBJCOPY)`
- `libc/arch/x86_64/make.config`: add `include toolchain.mk`; remove any `:=` overrides
- `tools/Makefile`: add `include toolchain.mk`; use `$(HOST_CC) $(HOST_CFLAGS)` for mkdisk
- `test/Makefile`: add `include toolchain.mk`; confirm all tests run via `$(HOST_CC)`

### Steps
1. Write `toolchain.mk` matching spec §3 verbatim
2. In each first-party Makefile, add the include line at the top
3. In each Makefile that has `:= clang ...` / `:= ld.lld ...` / `:= llvm-ar` / `:= llvm-objcopy`, **delete those lines and replace with `?=` deferring to `$(TARGET_*)`/`$(LLVM_*)`** (the `?=` matters — it lets arch configs override while defaulting to toolchain.mk)
4. In root Makefile busybox invocation, change `CC=clang LD=clang` to `CC=$(TARGET_CCLD) LD=$(TARGET_CCLD)`
5. Add a content-stamp file `build/.busybox.config.stamp` whose contents equal `$(CLANG_RESOURCE_DIR)`; the phony busybox rule regenerates when stamp contents differ
6. Verify on **both hosts**: `make test` 16/16 suites pass
7. Verify on **both hosts**: `readelf -Wl kernel.elf | grep -E 'INTERP|DYNAMIC'` returns empty (baseline)

### Rollback
Delete `toolchain.mk`; restore all `:=` overrides.

---

## Commit B: `kernel/Makefile` and `user/Makefile` use `$(TARGET_LD) $(LD_FLAGS)` for raw-ld links

### Files
- `kernel/Makefile`: kernel.elf final-link rule uses `$(TARGET_LD) $(LD_FLAGS)` (raw ld), not `$(CC) $(LDFLAGS)` (driver). kallsyms remains `$(HOST_CC) $(HOST_CFLAGS)`.
- `user/Makefile`: user ELF link rule uses `$(TARGET_LD) $(LD_FLAGS)`.

### Steps
1. Edit `kernel/Makefile` kernel.elf final-link per spec §4 row 5
2. Edit `user/Makefile` ELF link rule per spec §4 row 9
3. Verify on **clang 18 host**: `make kernel.bin`; `readelf -Wl build/x86_64/kernel/kernel.elf` shows no PT_INTERP, no PT_DYNAMIC
4. Verify on **clang 18 host**: `make -C user`; user ELF links successfully
5. Verify on **clang 22 host**: same

### Rollback
Revert both edits; recipes use driver-based link again.

---

## Commit C: `libc/include/stdarg.h` (NEW) + install-headers

### Files
- **NEW**: `libc/include/stdarg.h` (per spec §5a)
- `libc/Makefile` install-headers: copy stdarg.h to `$(INCLUDEDIR)/stdarg.h`

### Steps
1. Write `libc/include/stdarg.h` per spec §5a
2. Edit `libc/Makefile` install-headers to install stdarg.h
3. Verify on **both hosts**: `make clean && make -C libc install-headers` succeeds; `find sysroot/usr/include -name stdarg.h` exists
4. Verify on **both hosts**: `make -C kernel kernel.bin` succeeds (kernel uses libc's stdarg.h via sysroot)

### Rollback
Delete stdarg.h; revert libc/Makefile edit.

---

## Commit D: kallsyms uses `$(HOST_CC)/$(HOST_NM)`

### Files
- `kernel/Makefile` kallsyms rule

### Note
c221c89 (already on master) does this. If the diff in this commit is empty against master, this commit is a no-op baseline verification. The plan accounts for both cases.

### Steps
1. Verify kallsyms rule already uses `$(HOST_CC) $(HOST_CFLAGS)` and `$(HOST_NM) -n` (or apply if not)
2. Verify on **both hosts**: `make clean && make kernel.bin` succeeds; kallsyms compiles without stdarg.h error

### Rollback
Restore any earlier kallsyms rule.

---

## Commit E: `kernel/include/net/arch/cc.h` adds `#include <stdint.h>`

### Files
- `kernel/include/net/arch/cc.h`: add `#include <stdint.h>` at the top, before the existing typedef block. Do NOT delete the typedef block.

### Steps
1. Edit cc.h per spec §5b
2. Verify on **clang 18 host**: `make clean && make kernel.bin`; `kernel/net/net.o` compiles; no typedef redefinition
3. Verify on **clang 22 host**: same
4. Verify on **both hosts**: `make test` 16/16 suites pass (cc.h addition is non-breaking)

### Rollback
Remove the `#include <stdint.h>` line.

---

## Commit F: busybox config template + phony regeneration + driver-based link

### Files
- `config/busybox.config` → renamed `config/busybox.config.in` (template via git mv)
- Replace `/usr/lib/clang/22/include` with `@@CLANG_RESOURCE_DIR@@` placeholder
- Add `config/busybox.config` to `.gitignore`
- `Makefile` (root): add phony rule `generate-busybox-config` that runs `sed` on `.in` template; busybox target depends on this rule AND on stamp file `build/.busybox.config.stamp` whose contents equal `$(CLANG_RESOURCE_DIR)`
- busybox invocation passes `CC=$(TARGET_CCLD) LD=$(TARGET_CCLD)`

### Steps
1. `git mv config/busybox.config config/busybox.config.in`
2. Replace hard-coded path with `@@CLANG_RESOURCE_DIR@@` placeholder in the `.in` file
3. Add `config/busybox.config` to `.gitignore`
4. Add phony rule + stamp file dependency in root Makefile
5. Change busybox invocation to `CC=$(TARGET_CCLD) LD=$(TARGET_CCLD)`
6. Verify on **clang 18 host**: `make clean && make disk.img` — busybox compiles past stdio.h, links successfully against OS01 target driver
7. Verify on **clang 22 host**: same
8. Spot-check: after regeneration, `cat config/busybox.config` shows the new `$(CLANG_RESOURCE_DIR)` on **both** hosts; the two hosts' generated configs differ (correct — that's the whole point)

### Rollback
Revert the rename; restore the original busybox.config as a tracked file.

---

## Commit G: UEFI isolated toolchain contract

### Files
- `boot/uefi/Makefile`: invoke the inner posix-uefi runtime with **explicitly cleared/UEFI-specific** CFLAGS/LDFLAGS environment. Pass `UEFI_CLANG=$(CLANG)` separately. Do NOT inherit `$(CFLAGS)/$(LDFLAGS)` from toolchain.mk.

### Steps
1. Edit `boot/uefi/Makefile` per spec §4 row 10
2. The `$(MAKE) -C $(RUNTIME_DIR)` invocation must be preceded by `env -u CFLAGS -u LDFLAGS -u TARGET_CCLD ...`
3. Verify on **clang 18 host**: `make boot/uefi/BOOTX64.EFI` succeeds
4. Verify on **clang 22 host**: same

### Rollback
Restore the original UEFI invocation (which had no env-clear).

---

## Commit H: UEFI artifact validation with `llvm-readobj`

### Files
- `Makefile` (root): add `verify-uefi` target that runs `llvm-readobj --coff-summary build/x86_64/uefi/BOOTX64.EFI` and checks for PE/COFF characteristics
- Optional `verify-kernel` target: `readelf -Wl kernel.elf` + identity check

### Steps
1. Add `verify-uefi` target with `llvm-readobj` validation
2. Verify on **both hosts**: target prints "OK: UEFI is PE/COFF" on success
3. Verify on **both hosts**: deliberately corrupting the UEFI ELF header should make the verification fail

### Rollback
Remove the verification target.

---

## Commit K: `-lgcc` removal from `kernel/arch/x86_64/make.config`

### Files
- `kernel/arch/x86_64/make.config`: change `ARCH_LIBS = -nostdlib -lk -lgcc` to `ARCH_LIBS = -nostdlib -lk`

### Strict validation (per spec §6 seven steps)
On **each host** independently, after the change:

```
cd ~/OS01
make clean
make kernel.bin

# Step 1: link exits 0 (kernel.elf produced)
test -f build/x86_64/kernel/kernel.elf || { echo FAIL; exit 1; }

# Step 2: llvm-nm --undefined-only is empty
[ -z "$(llvm-nm --undefined-only build/x86_64/kernel/kernel.elf)" ] || { echo FAIL; exit 1; }

# Step 3: readelf -Wl shows no PT_INTERP, no PT_DYNAMIC
readelf -Wl build/x86_64/kernel/kernel.elf | grep -E 'INTERP|DYNAMIC' && { echo FAIL; exit 1; }

# Step 4: kernel identity check
SYMS=$(readelf -s build/x86_64/kernel/kernel.elf | grep -E ' (_start|kernel_main|_text)$' | wc -l)
[ "$SYMS" = "3" ] || { echo FAIL; exit 1; }

# Step 5: readelf -h confirms x86_64
readelf -h build/x86_64/kernel/kernel.elf | grep 'Machine:' | grep -q 'X86-64' || { echo FAIL; exit 1; }
```

If **any** step fails on **either** host, Commit K is **reverted** and the series stops. Commits A–H + M are still a net improvement; Commit K is documented as "deferred; need to investigate <failure>".

### Steps
1. Edit `kernel/arch/x86_64/make.config` per spec §4 row K
2. Run the strict validation on **both** hosts
3. If both pass, the commit lands
4. If either fails, the commit is reverted; document the failure mode; the series proceeds with K reverted

### Rollback
Restore `-lgcc` to `ARCH_LIBS`.

---

## Commit M: documentation

### Files
- **NEW**: `docs/build/toolchain.md`
- `docs/build.md`: add a "Toolchain abstraction" section pointing to `toolchain.md`
- `AGENTS.md`: add a "Toolchain overrides" line near "Critical gotchas"

### Steps
1. Write `docs/build/toolchain.md` covering: design principles (P1–P12), override examples (`make CLANG=clang-22`, `make EXTRA_CFLAGS=...`), troubleshooting ("clang not found" → set CLANG env), the driver vs raw-ld distinction
2. Add cross-reference in `docs/build.md`
3. Add cross-reference in `AGENTS.md`
4. Verify: docs render correctly; cross-links resolve

### Rollback
Delete `docs/build/toolchain.md`; revert doc edits.

---

## End-to-end verification (after all commits)

Run on **aoostar-n100 (clang 18)** and **homeserver (clang 22)**:

```
make clean
make test                       # S1: 16/16
make build/x86_64/uefi/BOOTX64.EFI   # UEFI builds with isolated contract
make kernel.bin                  # kernel builds; identity check
make -C user                     # user ELFs link via raw ld
make disk.img                    # full disk image, including busybox
make run DISPLAY=none            # QEMU boots; kernel banner visible; init.elf runs
```

**Artifact validation**:

```
readelf -Wl build/x86_64/kernel/kernel.elf | grep -E 'INTERP|DYNAMIC' && echo FAIL || echo OK
llvm-nm --undefined-only build/x86_64/kernel/kernel.elf | wc -l   # should be 0
readelf -s build/x86_64/kernel/kernel.elf | grep -E ' (_start|kernel_main|_text)$' | wc -l   # should be 3
llvm-readobj --coff-summary build/x86_64/uefi/BOOTX64.EFI   # should show PE/COFF
```

**Cross-version**: if both clang-18 and clang-22 are installed on the **same host**, run:

```
make clean && make CLANG=clang-18 kernel.bin
make clean && make CLANG=clang-22 kernel.bin
# Compare validation results (not byte-equality — timestamps/build IDs differ)
```

A second-host run remains valuable for distro/layout sensitivity but tests a broader set of variables.

## Risk contingencies

| Scenario | Response |
|---|---|
| Commit A's "remove `:=` overrides" breaks an arch config we didn't notice | Restore that one override; expand Commit A scope |
| Commit B driver/raw-ld split breaks a recipe we missed | Audit that recipe; revert Commit B; expand |
| Commit C stdarg.h fails on a future toolchain | Add `#ifdef __has_builtin(__builtin_va_list)` guard |
| Commit D is a no-op (c221c89 already does it) | Empty diff; verification only |
| Commit E cc.h includes stdarg.h conflicts | The `#include <stdint.h>` is guarded by `LWIP_ARCH_CC_H`; stdint.h itself does not define ssize_t. If conflict arises, add `#ifndef SSIZE_MAX` guard |
| Commit F busybox phony rule re-runs unnecessarily | Stamp file content comparison ensures it only re-runs when `$(CLANG_RESOURCE_DIR)` actually changes |
| Commit G UEFI env-clear breaks something we didn't anticipate | Revert; investigate; expand |
| Commit H verification script finds a "bad" UEFI | Investigate; reverting may not be appropriate if the UEFI was always bad |
| Commit K strict validation fails on clang 18 (Ubuntu 24.04) | Revert K; file separate issue; aarch64 + clang-22 hosts may still pass |
| Commit M docs cross-link doesn't resolve | Update link target; rerender |

## Commit summary table

| # | Name | Files | Verification surface | Risk |
|---|---|---|---|---|
| A | toolchain.mk + override-removal | 8 Makefiles + 1 new file | `make test` | Medium — large diff |
| B | driver/raw-ld split | 2 files | `make kernel.bin` + `make -C user` | Medium |
| C | stdarg.h (NEW) | 2 files | `make -C libc install-headers` | Low |
| D | kallsyms HOST_CC | 1 file (or no-op if c221c89 already there) | `make kernel.bin` | Very low |
| E | cc.h stdint.h include | 1 file | `make kernel.bin` + `make test` | Low |
| F | busybox config template + driver | 3 files + 1 gitignore + stamp file | `make disk.img` | Medium |
| G | UEFI isolated contract | 1 file | UEFI build | Medium |
| H | UEFI verification | 1 file (new target) | UEFI artifact check | Low |
| K | `-lgcc` removal | 1 file | strict seven-step validation | High (gated) |
| M | docs | 3 files | doc render | Very low |

Total files touched: ~22 (including new stdarg.h, toolchain.mk, busybox.config.in, stamp file, toolchain.md, verify-uefi target). Build files: ~13. Source files: 2 (cc.h, stdarg.h). Docs: 4.