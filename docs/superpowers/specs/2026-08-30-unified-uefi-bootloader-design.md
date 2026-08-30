# Unified UEFI Bootloader — shared lifecycle `main.c` + `boot_context` handoff

Date: 2026-08-30 · Status: draft · Branch: master (worktree pending)

## 1. Goal

Consolidate the two UEFI bootloaders (`boot/uefi/main.c` for x86_64,
`boot/uefi/aarch64/main.c` + `elf.c` + `handoff.S`) into one common UEFI
lifecycle in `boot/uefi/main.c`, with architecture differences under
`boot/uefi/arch/{x86_64,aarch64}/`. In the same change, unify the kernel
handoff ABI: both loaders deliver `boot_context` v2 and the x86 kernel
entry consumes it directly, retiring the legacy `BOOT_INFO` path.

Scope is the bootloader consolidation **and** the kernel-entry migration
it implies. No change to memory-map policy, kernel ABI layout
(`boot_context` v2 is unchanged), or boot behavior other than what is
listed below.

## 2. Current state

- `boot/uefi/main.c` (x86, 394 lines): reads `kernel.bin` to fixed
  0x100000, parses `config.txt` resolution, selects a GOP mode, builds an
  E820 map + `BOOT_INFO` @ 0x60000, finds RSDP, calls runtime `exit_bs()`,
  jumps to 0x100000. Built by the posix-uefi runtime **without**
  `UEFI_NO_UTF8` (`char_t == char`).
- `boot/uefi/aarch64/main.c` (613 lines) + `elf.c` + `loader.h` +
  `handoff.S`: reads `kernel.elf`, validates + loads ELF PT_LOADs at fixed
  paddr (entry 0x40080000), copies the firmware FDT, builds `boot_context`
  @ 0x401e0000 with a **raw** UEFI memory map, prepares a trampoline page
  @ 0x401ff000 (MMU/TLB teardown), exits boot services with an explicit
  map-key retry, enters via `aarch64_enter_kernel`. Built with
  `UEFI_NO_UTF8` (`char_t == wchar_t`) via a private runtime copy.
- Kernel: `kernel/kernel/main.c::kernel_main(struct BOOT_INFO *)` converts
  via `boot_context_from_legacy()` and reads `bootinfo->E820_Info` directly
  for `pmm_init()`. `kernel/arch/x86_64/head.S` saves the handoff pointer
  in a `BOOT_INFO` global and passes it as the kernel_main argument.
  `kernel/arch/aarch64/main.c` discriminates UEFI vs direct boot via
  `aarch64_select_boot_mode(x0)`.
- Both posix-uefi CRTs call `main(argc, argv)` and end with
  `return ret ? EFIERR(ret) : EFI_SUCCESS;` — so a `main_error_code()`
  helper that masks a status to its low bits round-trips under both.

## 3. Target structure

```
boot/uefi/
  main.c                      — common UEFI lifecycle (no arch #ifdef)
  Makefile                    — single parameterized build wrapper (ARCH)
  arch/
    arch.h                    — arch hook interface (+ layout types)
    x86_64/
      boot.c                  — x86 arch hooks (from old main.c x86 body)
    aarch64/
      boot.c                  — aarch64 arch hooks (from old aarch64/main.c)
      elf.c                   — unchanged
      loader.h                — unchanged
      handoff.S               — unchanged
```

`boot/uefi/aarch64/` is removed as a source tree (its `Makefile` is folded
into the parameterized wrapper; its sources move under `arch/aarch64/`).

## 4. Arch hook interface (`boot/uefi/arch/arch.h`)

Only one arch dir is compiled per build, so hooks are plain C functions
(no ops table, no `#ifdef` in `main.c`).

```c
#include <uefi.h>
#include "../../../kernel/include/kernel/bootinfo.h"

const char_t *arch_kernel_path(void);
    /* x86: L"kernel.bin" · aarch64: L"\\kernel.elf" (both wide, UEFI_NO_UTF8) */

efi_status_t arch_init_handoff(struct boot_context **ctx_out);
    /* AllocateAddress the fixed handoff region, zero it, boot_context_init().
     * x86:   0x60000, 4 pages (E820 + raw-descriptor scratch share it)
     * aarch64: 0x401e0000, HANDOFF_DATA_PAGES (trampoline page is separate) */

efi_status_t arch_load_kernel(const void *image, uint64_t size,
                              uint64_t *entry_out);
    /* x86:     AllocateAddress 0x100000, copy raw kernel.bin, entry=0x100000
     * aarch64: validate ELF (elf.c), allocate+copy PT_LOAD intervals, entry=e_entry */

void arch_fill_firmware(struct boot_context *ctx);
    /* x86:     scan ST->ConfigurationTable for ACPI_20_TABLE_GUID →
     *          ctx->firmware.acpi_rsdp (+ HAS_ACPI)
     * aarch64: find FDT table guid, copy FDT into handoff region →
     *          ctx->firmware.dtb (+ HAS_DTB) */

void arch_memory_buffer(efi_physical_address_t *phys_out, uint64_t *capacity_out);
    /* Fixed pre-reserved scratch for the raw UEFI descriptor array, returned
     * AFTER arch_init_handoff + arch_fill_firmware have placed their data.
     * x86:     tail of the 0x60000 region
     * aarch64: cursor after context + DTB, up to the trampoline page */

void arch_build_memory(struct boot_context *ctx,
                       efi_physical_address_t desc_phys,
                       uintn_t desc_size, uintn_t desc_count,
                       uint32_t desc_version);
    /* x86:     merge+sort the descriptors into an E820 array in the handoff
     *          region; ctx->memory = { entries=E820_phys, count, entry_size=
     *          sizeof(struct E820_ENTRY), format=E820 }
     * aarch64: ctx->memory = { entries=desc_phys, count=desc_count,
     *          entry_size=desc_size, format=UEFI_RAW, descriptor_version } */

void arch_release(void);
    /* Error cleanup: free kernel + handoff allocations. x86: new (frees
     * 0x100000 + 0x60000 pages). aarch64: release_kernel_allocations() +
     * release_handoff_allocations(). Must be safe to call at any point
     * after main() begins (both arches already track partial reservations). */

void arch_puts(const char *s);
    /* Console for common lifecycle messages. x86: printf("%s", s)
     * aarch64: pl011_puts(s). Keeps aarch64 on the raw UART. */

__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys);
    /* x86:     direct call through function pointer
     * aarch64: prepare_trampoline() then aarch64_enter_kernel() (handoff.S) */
```

Physical layout constants and reserved regions are per-arch constants
inside the arch dirs (x86 kernel base 0x100000 / handoff 0x60000·4 pages;
aarch64 entry 0x40080000 / handoff 0x401e0000 / trampoline 0x401ff000).
The aarch64 kernel's own `AARCH64_UEFI_HANDOFF_ADDRESS` (0x401e0000) must
keep matching `loader.h`'s `AARCH64_HANDOFF_BASE` — already true, kept in
sync by review.

## 5. Common `main.c` lifecycle (points 3, 4)

```c
int main(int argc, char_t **argv)
{
    void *image = NULL; uint64_t image_size = 0, entry = 0;
    struct boot_context *ctx = NULL;
    EFI_STATUS status;

    status = arch_init_handoff(&ctx);
    if (EFI_ERROR(status)) goto fail;
    status = read_kernel_file(arch_kernel_path(), &image, &image_size);
    if (EFI_ERROR(status)) goto fail;
    status = arch_load_kernel(image, image_size, &entry);
    free(image); image = NULL;
    if (EFI_ERROR(status)) goto fail;

    if (!capture_graphics(&ctx->graphics)) { status = EFI_DEVICE_ERROR; goto fail; }
    ctx->flags |= BOOT_CONTEXT_HAS_FRAMEBUFFER;

    arch_fill_firmware(ctx);                       /* RSDP / FDT */

    arch_memory_buffer(&desc_phys, &desc_capacity);
    for (;;) {                                     /* map-key retry discipline */
        status = BS->GetMemoryMap(&req, NULL, &key, &dsz, &dver);
        if (status != EFI_BUFFER_TOO_SMALL || dsz == 0) goto fail;
        if (req + MAP_SLACK * dsz > desc_capacity) { status = EFI_BUFFER_TOO_SMALL; goto fail; }
        status = BS->GetMemoryMap(&req, (void *)desc_phys, &key, &dsz, &dver);
        if (EFI_ERROR(status)) goto fail;
        arch_build_memory(ctx, desc_phys, dsz, req / dsz, dver);
        if (!boot_context_valid(ctx)) { status = EFI_LOAD_ERROR; goto fail; }
        status = BS->ExitBootServices(IM, key);
        if (status == EFI_INVALID_PARAMETER) continue;   /* map changed: refetch */
        if (EFI_ERROR(status)) goto fail;
        break;
    }
    arch_enter_kernel(entry, (uint64_t)ctx);       /* noreturn */

fail:
    if (image) free(image);
    arch_release();
    return main_error_code(status);
}
```

Common helpers in `main.c`: `read_kernel_file` (fopen/fseek/ftell/malloc/
fread), `capture_graphics` (LocateProtocol GOP + QueryMode current),
`guid_equal` (merges x86 `CompareGuid` + aarch64 `guid_equal`),
`main_error_code` (shared, masks status to low bits for the CRT's
`EFIERR()` round-trip). All common progress/error output goes through
`arch_puts`.

**EBS discipline (point 4):** the loop above is the aarch64 discipline
applied to both. x86 drops the runtime `exit_bs()` helper entirely; its raw
descriptor array moves from a `malloc` into the fixed handoff scratch, so
no allocation or protocol call happens after the final `GetMemoryMap`.

## 6. Unified handoff ABI (point 1)

- Both loaders emit `boot_context` v2 (x86 now builds one too; no more
  `struct BOOT_INFO`).
- **Kernel x86:**
  - `kernel_main(struct BOOT_INFO *)` → `kernel_main(const struct
    boot_context *)`; the `boot_context_from_legacy()` call is dropped and
    the struct fields are read straight off `bootctx` (`graphics`,
    `memory`, `firmware.acpi_rsdp`).
  - `pmm_init(struct MEMORY_INFO)` → `pmm_init(const struct BOOT_MEMORY_MAP
    *)` (decl in `kernel/include/kernel/memory.h`); iterates entries using
    `entry_size`, requires `format == BOOT_MEMORY_FORMAT_E820` (x86 path
    only; no raw consumer exists yet).
  - `kernel/arch/x86_64/head.S`: rename the `BOOT_INFO` scratch global to
    `BOOT_CONTEXT` (internal-only; no C references).
- **bootinfo.h cleanup:** remove `struct BOOT_INFO`, `struct MEMORY_INFO`,
  `boot_context_from_legacy()`, and `boot_context_from_aarch64()` (all dead
  once x86 migrates and direct boot is removed). Keep `struct
  E820_ENTRY` (loader E820 gen + pmm), `struct GRAPHICS_INFO` (embedded in
  boot_context), the `boot_context` ABI, flags, and formats.
- `boot_context_from_legacy()` is retired now — it was the migration
  adapter and this is the migration.

## 7. Memory map policy (point 2)

Unchanged: x86 loader generates **E820** (`BOOT_MEMORY_FORMAT_E820`);
aarch64 passes the **raw** UEFI map (`BOOT_MEMORY_FORMAT_UEFI_RAW`). Both
ride the same `struct BOOT_MEMORY_MAP` inside `boot_context`; only
`arch_build_memory` differs.

## 8. Build wrapper (point 5)

Single parameterized `boot/uefi/Makefile`:

```
make -C boot/uefi ARCH=x86_64    → build/x86_64/uefi/BOOTX64.EFI
make -C boot/uefi ARCH=aarch64   → build/aarch64/uefi/BOOTAA64.EFI
```

Recipe (per ARCH): copy `thirdpart/posix-uefi/uefi` to a private
`build/<arch>/uefi-runtime/`, inject `-DUEFI_NO_UTF8` into the runtime
CFLAGS, build `SRCS="main.c arch/<arch>/boot.c [arch/aarch64/elf.c
arch/aarch64/handoff.S]"` with `OUTDIR=build/<arch>/uefi/`. Per-arch
private runtime and object directories (no cross-arch object pollution;
x86 no longer builds objects in `boot/uefi/`).

**UEFI_NO_UTF8 on both sides.** `char_t == wchar_t` uniformly; main
signature `int main(int argc, char_t **argv)` (argv unused in both). x86
consequences:
- all `printf`/`fprintf` format strings become wide via the `CL()` macro
  (posix-uefi provides it: `CL("x")` → `L"x"` under UEFI_NO_UTF8),
- `fopen(L"kernel.bin", L"r")` / `fopen(L"config.txt", L"r")`,
- config.txt parsing: file content is ASCII bytes, so the runtime
  wide `strtok`/`atoi`/`strcmp` cannot be used — add a small narrow
  tokenizer in `arch/x86_64/boot.c` for the `resolution WxH` line.

Root `Makefile` updates: x86 EFI app path becomes
`build/x86_64/uefi/BOOTX64.EFI` (`disk.img` `--efi` + dependency);
aarch64 deps drop `boot/uefi/aarch64/Makefile` in favor of the
parameterized wrapper; the `boot/uefi/OVMF.fd` download target stays.

## 9. Kernel entry migration + regression (point 6)

**aarch64 direct `-kernel` boot removed:**
- `run-aarch64` and `debug-aarch64` Makefile targets removed (both launch
  `-kernel kernel.elf`); the `.vscode/launch.json` aarch64 entry is
  removed with it (flag: no more gdb bootstrap straight into the aarch64
  kernel; re-add later as a UEFI debug flow).
- `kernel/arch/aarch64/main.c`: remove `aarch64_select_boot_mode()` + its
  enum, the direct-boot branches (scratch `boot_context_from_aarch64`
  path, direct `smp_boot_aps()` call), and the
  `AARCH64_BOOT_MODE_HOST_TEST` guard. `aarch64_main` now always validates
  `boot_context_valid(handoff)` at 0x401e0000 and proceeds (corrupt →
  PL011 report + halt).
- **aarch64 SMP stays orphaned (user decision):** `smp.c`/`psci.c`/
  `test_spinlock.c` remain compiled into the kernel but are no longer
  called (UEFI boot is single-BSP). No deletion, no enablement.

## 10. Test updates

- `test/cases/test_bootinfo_abi.c`: drop the `BOOT_INFO`/legacy and
  `aarch64_select_boot_mode` assertions (and the legacy/adapter helper
  tests). Keep the `boot_context` init/valid + layout static asserts and
  the `BOOT_MEMORY_MAP`/`BOOT_FIRMWARE` asserts.
- `test/Makefile`: remove the `aarch64_boot_mode.o` compile unit (it
  compiled `kernel/arch/aarch64/main.c` with `AARCH64_BOOT_MODE_HOST_TEST`)
  and its link rule.

## 11. Docs

- `docs/boot.md`: x86 chain → `BOOTX64.EFI → boot_context → kernel_main` ;
  aarch64 → UEFI only.
- `AGENTS.md`: update the Boot line and the `BOOT_INFO ABI` gotcha to the
  `boot_context` ABI; update the key-files table row for `bootinfo.h`.

## 12. Verification (agreed: build + boot smoke)

1. `make -C boot/uefi ARCH=x86_64` — BOOTX64.EFI links.
2. `make -C boot/uefi ARCH=aarch64` — BOOTAA64.EFI links.
3. `make test` (host suite) — updated `test_bootinfo_abi` passes.
4. x86: `make disk.img` + QEMU UEFI boot to the shell prompt (headless
   serial smoke, e.g. the AGENTS.md cat/Ctrl-C pattern reduced to an
   `echo` check).
5. aarch64: `make aarch64-uefi` + `run-aarch64-uefi` → `OS01 aarch64 uefi
   handoff ok` / `[tick]` banner on the serial console.

## 13. Non-goals / out of scope

- No change to E820 vs raw memory-map policy, boot_context layout (v2), or
  x86 kernel memory-map contents.
- No aarch64 config.txt/GOP mode selection (stays x86-only).
- No full `test-syscall` run (per agreed verification depth).
- No aarch64 SMP enablement under UEFI.

## 14. Risks

- **Wide-literal conversion** (~50 x86 printf sites): mechanical but a
  missed `CL()` produces a wide/narrow mismatch at compile time — caught by
  the build.
- **config.txt narrow parsing** must not regress resolution selection.
- **E820 region sizing**: the x86 handoff region grows 2 → 4 pages
  (16 KB) to hold boot_context + E820 + raw descriptors for a QEMU-class
  map; a map that overflows fails cleanly (handoff overflow) like aarch64.
- **UEFI_NO_UTF8 on x86** exercises the wide printf→ConOut path for the
  first time on x86 (aarch64 never used printf) — covered by the boot
  smoke.
- **Kernel ABI**: boot_context v2 layout is unchanged, so the kernel-side
  switch is field-access only.
