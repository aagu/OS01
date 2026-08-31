# OS01 Toolchain Refactor — Implementation Plan

Spec: `docs/superpowers/specs/2026-08-31-toolchain-refactor-design.md` (v1)
Date: 2026-08-31

## Summary

Five commits, sequenced for rollback safety. Each commit's RED-GREEN-REFACTOR is independent.

## Commit A: introduce `toolchain.mk` (no behavioral change)

### Files
- **NEW**: `toolchain.mk` (per spec §3)
- `Makefile` (root): replace `export AR=llvm-ar` and `export OBJ_CPY=llvm-objcopy` with `include toolchain.mk`

### Steps
1. Write `toolchain.mk` matching spec §3 verbatim
2. In root `Makefile`, replace lines 6–7 with:
   ```makefile
   include toolchain.mk
   ```
3. Verify: `make test` still 16/16
4. Verify: `make build/x86_64/uefi/BOOTX64.EFI` still succeeds
5. Verify: `make kernel.bin` still succeeds on clang 18 host

### Rollback
Delete `toolchain.mk`, restore original lines 6–7 in root `Makefile`.

## Commit B: link-flag fix + kallsyms host build fix

### Files
- `kernel/arch/x86_64/make.config`: add `-Wl,-z,norelro -Wl,--no-relax` to `ARCH_LDFLAGS`
- `kernel/Makefile`: kallsyms rule uses `$(HOST_CC) $(HOST_CFLAGS)`

### Steps
1. Edit `make.config` `ARCH_LDFLAGS` per spec §4 row 3
2. Edit `kernel/Makefile` line ~191 to:
   ```makefile
   $(HOST_CC) $(HOST_CFLAGS) -o $(BUILD_DIR)/kernel/kallsyms kernel/kallsyms.c
   ```
3. Verify on clang 18 host: `make kernel.bin` links without GOTPCREL error
4. Verify on clang 18 host: kallsyms compiles (no stdarg.h error)

### Rollback
Revert both edits; expect GOTPCREL error on clang ≥ 17 to return.

## Commit C: cc.h typedef from clang builtins

### Files
- `kernel/include/net/arch/cc.h`: replace eight typedefs with builtin-derived versions per spec §5

### Steps
1. Edit cc.h per spec §5
2. Verify on clang 18 host: `make kernel.bin`; `kernel/net/net.o` compiles without typedef redefinition
3. Verify on clang 22 host (homeserver): same

### Rollback
Restore original cc.h typedefs.

## Commit D: user-space sysroot completion + busybox templating

### Files
- `libc/Makefile` install-headers: copy `stdarg.h` from clang resource dir
- `Makefile` (root): busybox clang-path templating + libgcc stub

### Steps
1. Edit `libc/Makefile` install-headers to add:
   ```makefile
   @cmp -s $(CLANG_RESOURCE_DIR)/include/stdarg.h $(INCLUDEDIR)/stdarg.h || \
       cp $(CLANG_RESOURCE_DIR)/include/stdarg.h $(INCLUDEDIR)/stdarg.h
   ```
2. Add comment explaining stdarg.h is from clang builtin
3. Edit `config/busybox.config` line 52: replace `/usr/lib/clang/22/include` with `$(CLANG_RESOURCE_DIR)/include`
4. In root `Makefile` busybox target, add libgcc stub creation alongside existing libm/librt stubs
5. Verify on clang 18 host: `make disk.img` reaches busybox link step (may still fail with other issues, but should clear stdarg.h and libgcc issues)
6. Verify on clang 22 host (homeserver): busybox still compiles (no regression)

### Rollback
Revert all four edits.

## Commit E: documentation

### Files
- **NEW**: `docs/build/toolchain.md`
- `docs/build.md`: add a "Toolchain abstraction" section pointing to `toolchain.md`
- `AGENTS.md`: add "Toolchain overrides" line near "Critical gotchas"

### Steps
1. Write `docs/build/toolchain.md` covering: design principles (P1–P7), override examples (`make CLANG=clang-22`), troubleshooting ("clang not found" → set CLANG env)
2. Add cross-reference in `docs/build.md`
3. Add cross-reference in `AGENTS.md`

### Rollback
Delete `docs/build/toolchain.md`; revert doc edits.

## End-to-end verification (after all 5 commits)

Run on aoostar-n100 (clang 18):

```
make clean
make test             # must be 16/16
make disk.img         # must complete
make run              # QEMU boots; kernel banner visible; init.elf runs
```

Run on homeserver (clang 22):

```
make clean
make test             # must be 16/16
make disk.img         # must complete
```

Pass criteria: both hosts produce identical boot output up to and including init.elf launching.

## Risk contingencies

| Scenario | Response |
|---|---|
| Commit B introduces a regression on clang 17 (homeserver) | Revert Commit B on homeserver only; file separate issue |
| Commit C cc.h change breaks a clang-version we haven't tested | Add `if defined(__clang_major__) && __clang_major__ >= 14` guard around builtin typedefs (defensive) |
| Commit D busybox templating breaks older busybox versions that expect different CONFIG_EXTRA_CFLAGS syntax | Pin `BUSYBOX_TAG` in toolchain.mk; allow override |
