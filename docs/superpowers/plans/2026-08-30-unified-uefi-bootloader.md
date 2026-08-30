# Unified UEFI Bootloader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge the x86_64 and aarch64 UEFI bootloaders into one shared lifecycle `main.c` with per-arch hooks, and migrate the x86 kernel entry from legacy `BOOT_INFO` to `boot_context` v2.

**Architecture:** `boot/uefi/main.c` owns the common UEFI lifecycle (kernel-file read, GOP capture, `boot_context` construction, GetMemoryMap→ExitBootServices map-key retry, error cleanup); `boot/uefi/arch/{x86_64,aarch64}/boot.c` implement the per-arch hooks behind `arch/arch.h`. Both builds compile the same `main.c` with `UEFI_NO_UTF8` via one parameterized `boot/uefi/Makefile`. The kernel handoff becomes `boot_context` v2 on both arches.

**Tech Stack:** C (clang `--target=<arch>-pc-win32-coff`), posix-uefi runtime, GNU Make, lld, QEMU (x86_64 + aarch64), AAVMF/OVMF.

**Spec:** `docs/superpowers/specs/2026-08-30-unified-uefi-bootloader-design.md`

## Global Constraints

- Work in a **git worktree** (project convention: never touch the master working tree). Create it at execution time with the `superpowers:using-git-worktrees` skill.
- `boot_context` v2 ABI is frozen: magic `0x4f533031`, version `2`, size `104`. All fields fixed-size (`uint32_t`/`uint64_t`) — bootloader is LLP64, kernel is LP64, never `unsigned long`.
- **Both** builds compile with `-DUEFI_NO_UTF8` (`char_t == wchar_t`). Narrow strings only via `arch_puts`'s `%S` or the local `narrow_*` helpers in the x86 arch file.
- Memory-map policy unchanged: x86 → `BOOT_MEMORY_FORMAT_E820`, aarch64 → `BOOT_MEMORY_FORMAT_UEFI_RAW`.
- `MAP_SLACK = 2` (shared constant in common `main.c`).
- x86 layout: kernel @ 0x100000; handoff @ 0x60000 (4 pages: boot_context @0x60000, raw descriptors @0x61000 cap 0x1000, E820 output @0x62000 cap 0x2000).
- aarch64 layout: kernel entry 0x40080000, handoff @ 0x401e0000 (31 data pages), trampoline @ 0x401ff000 (1 page). The aarch64 kernel's handoff address must stay 0x401e0000.
- aarch64 SMP (`smp.c`/`psci.c`/`test_spinlock.c`) stays **orphaned** — compiled, never called. Do not delete or enable it.
- No change to memory-map contents, boot_context layout, or boot behavior beyond this spec.
- Verification depth: build both EFI apps + boot smoke (x86 to shell, aarch64 to `[tick]`/`uefi handoff ok`) + `make test` host suite. No full `test-syscall` run.
- Commit per task. End commit messages with `Co-Authored-By: Claude Code <noreply@anthropic.com>`.

---

### Task 1: x86 kernel + loader switch to `boot_context` (new tree)

The x86 side of the whole change, atomically: the kernel consumes `boot_context`, and the bootloader is restructured into the common `main.c` + `arch/` layout with the unified build wrapper. This task leaves x86 **bootable**.

**Files:**
- Create: `boot/uefi/arch/arch.h`, `boot/uefi/main.c` (replaces old), `boot/uefi/arch/x86_64/boot.c`
- Modify: `boot/uefi/Makefile` (rewrite as wrapper), `Makefile` (root: x86 paths), `kernel/kernel/main.c`, `kernel/memory/pmm.c`, `kernel/include/kernel/memory.h`, `kernel/arch/x86_64/head.S`
- Delete: `boot/uefi/uefi` (tracked symlink → posix-uefi runtime; the wrapper no longer uses it)

**Interfaces:**
- Consumes: `kernel/include/kernel/bootinfo.h` (`struct boot_context`, `boot_context_init`, `boot_context_valid`, `struct E820_ENTRY`, `enum BOOT_CONTEXT_FLAGS`, `enum BOOT_MEMORY_FORMAT`) — these stay unchanged in this task.
- Produces: `arch/arch.h` hooks (below); `build/x86_64/uefi/BOOTX64.EFI`.

- [ ] **Step 1: Create `boot/uefi/arch/arch.h`**

```c
#ifndef OS01_UEFI_ARCH_H
#define OS01_UEFI_ARCH_H

#include <uefi.h>

/* bootinfo.h uses these; the posix-uefi runtime does not define them and
 * each bootloader translation unit (main.c + arch/*/boot.c) includes this
 * header first, so provide them here. */
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

#include "../../../kernel/include/kernel/bootinfo.h"

/* Shared helpers provided by common main.c, used by arch code. */
efi_status_t capture_graphics(struct boot_context *ctx);
int guid_equal(const efi_guid_t *left, const efi_guid_t *right);

/* Per-arch hooks (only one arch dir is compiled per build). */
const char_t *arch_kernel_path(void);
efi_status_t arch_init_handoff(struct boot_context **ctx_out);
efi_status_t arch_load_kernel(const void *image, uint64_t image_size,
                              uint64_t *entry_out);
efi_status_t arch_setup_graphics(struct boot_context *ctx);
efi_status_t arch_fill_firmware(struct boot_context *ctx);
void arch_memory_buffer(efi_physical_address_t *phys_out,
                        uint64_t *capacity_out);
void arch_build_memory(struct boot_context *ctx,
                       efi_physical_address_t desc_phys,
                       uintn_t desc_size, uintn_t desc_count,
                       uint32_t desc_version);
void arch_release(void);
void arch_puts(const char *s);
__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys);

#endif
```

- [ ] **Step 2: Write common `boot/uefi/main.c` (replaces the old x86 main.c)**

```c
/* boot/uefi/main.c — common UEFI bootloader lifecycle.
 *
 * Shared by the x86_64 and aarch64 UEFI bootloaders.  Architecture
 * differences live in arch/<arch>/boot.c behind the hooks in arch/arch.h.
 * Compiled with UEFI_NO_UTF8 on both targets (char_t == wchar_t).
 */
#include <uefi.h>
#include "arch/arch.h"

#ifndef UINT32_MAX
#define UINT32_MAX UINT32_C(0xffffffff)
#endif

/* Absorbs descriptor records split by a firmware allocation between the
 * size query and the fetch (aarch64's existing AARCH64_MEMORY_MAP_SLACK). */
#define UEFI_MAP_SLACK UINT64_C(2)

static EFI_STATUS read_kernel_file(const char_t *path, void **image_out,
                                   uint64_t *size_out)
{
    FILE *file;
    void *image;
    long size;

    if (!image_out || !size_out)
        return EFI_INVALID_PARAMETER;

    file = fopen(path, L"r");
    if (!file)
        return EFI_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return EFI_LOAD_ERROR;
    }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return EFI_LOAD_ERROR;
    }

    image = malloc((size_t)size);
    if (!image) {
        fclose(file);
        return EFI_OUT_OF_RESOURCES;
    }
    if (fread(image, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(image);
        return EFI_LOAD_ERROR;
    }
    fclose(file);

    *image_out = image;
    *size_out = (uint64_t)size;
    return EFI_SUCCESS;
}

int guid_equal(const efi_guid_t *left, const efi_guid_t *right)
{
    uint32_t index;

    if (left->Data1 != right->Data1 || left->Data2 != right->Data2 ||
        left->Data3 != right->Data3)
        return 0;
    for (index = 0; index < sizeof(left->Data4); index++) {
        if (left->Data4[index] != right->Data4[index])
            return 0;
    }
    return 1;
}

efi_status_t capture_graphics(struct boot_context *ctx)
{
    efi_guid_t gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    efi_gop_t *gop = NULL;
    EFI_STATUS status;

    if (!ctx)
        return EFI_INVALID_PARAMETER;

    status = BS->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Information)
        return EFI_DEVICE_ERROR;

    ctx->graphics.HorizontalResolution =
        gop->Mode->Information->HorizontalResolution;
    ctx->graphics.VerticalResolution =
        gop->Mode->Information->VerticalResolution;
    ctx->graphics.PixelsPerScanLine =
        gop->Mode->Information->PixelsPerScanLine;
    ctx->graphics.FrameBufferBase = (uint64_t)gop->Mode->FrameBufferBase;
    ctx->graphics.FrameBufferSize = (uint64_t)gop->Mode->FrameBufferSize;
    ctx->flags |= BOOT_CONTEXT_HAS_FRAMEBUFFER;
    return EFI_SUCCESS;
}

/* Mask a status to its low bits; the CRT's `ret ? EFIERR(ret) : EFI_SUCCESS`
 * re-applies the error bit at the application entry boundary. */
static int main_error_code(EFI_STATUS status)
{
    uint32_t code = (uint32_t)((uint64_t)status & UINT64_C(0x7fffffffffffffff));

    if (code == 0)
        code = (uint32_t)((uint64_t)EFI_LOAD_ERROR & UINT64_C(0x7fffffffffffffff));
    return (int)code;
}

int main(int argc, char_t **argv)
{
    void *image = NULL;
    uint64_t image_size = 0;
    uint64_t entry = 0;
    struct boot_context *ctx = NULL;
    efi_physical_address_t desc_phys = 0;
    uint64_t desc_capacity = 0;
    uintn_t map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_version = 0;
    EFI_STATUS status;

    (void)argc;
    (void)argv;

    status = arch_init_handoff(&ctx);
    if (EFI_ERROR(status)) {
        arch_puts("UEFI: handoff init failed\r\n");
        goto fail;
    }

    status = read_kernel_file(arch_kernel_path(), &image, &image_size);
    if (EFI_ERROR(status)) {
        arch_puts("UEFI: kernel image read failed\r\n");
        goto fail;
    }

    status = arch_load_kernel(image, image_size, &entry);
    free(image);
    image = NULL;
    if (EFI_ERROR(status)) {
        arch_puts("UEFI: kernel load failed\r\n");
        goto fail;
    }

    status = arch_setup_graphics(ctx);
    if (EFI_ERROR(status)) {
        arch_puts("UEFI: graphics setup failed\r\n");
        goto fail;
    }

    status = arch_fill_firmware(ctx);
    if (EFI_ERROR(status)) {
        arch_puts("UEFI: firmware tables failed\r\n");
        goto fail;
    }

    arch_memory_buffer(&desc_phys, &desc_capacity);
    for (;;) {
        status = BS->GetMemoryMap(&map_size, NULL, &map_key,
                                  &desc_size, &desc_version);
        if (status != EFI_BUFFER_TOO_SMALL || desc_size == 0)
            break;
        if ((uint64_t)map_size + UEFI_MAP_SLACK * desc_size > desc_capacity) {
            arch_puts("UEFI: memory map overflow\r\n");
            status = EFI_BUFFER_TOO_SMALL;
            break;
        }
        if (map_size / desc_size > UINT32_MAX) {
            arch_puts("UEFI: memory map too large\r\n");
            status = EFI_LOAD_ERROR;
            break;
        }
        status = BS->GetMemoryMap(&map_size, (void *)(uintptr_t)desc_phys,
                                  &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(status))
            break;
        arch_build_memory(ctx, desc_phys, desc_size,
                          (uint32_t)(map_size / desc_size), desc_version);
        if (!boot_context_valid(ctx)) {
            arch_puts("UEFI: corrupt handoff\r\n");
            status = EFI_LOAD_ERROR;
            break;
        }
        status = BS->ExitBootServices(IM, map_key);
        if (status == EFI_INVALID_PARAMETER)
            continue;                    /* map changed: refetch */
        if (EFI_ERROR(status))
            break;
        arch_enter_kernel(entry, (uint64_t)ctx);   /* noreturn */
    }

fail:
    if (image)
        free(image);
    arch_release();
    return main_error_code(status);
}
```

- [ ] **Step 3: Write `boot/uefi/arch/x86_64/boot.c`**

```c
/* boot/uefi/arch/x86_64/boot.c — x86_64 arch hooks.
 *
 * Kernel: raw kernel.bin at fixed 0x100000 (no ELF).
 * Handoff: boot_context at 0x60000 in a 4-page allocation:
 *   0x60000 boot_context (104 B)
 *   0x61000 raw UEFI descriptor scratch (4 KB, ~85 x 48 B entries)
 *   0x62000 E820 output array (8 KB, 400 x 20 B entries)
 * Memory map: E820 (BOOT_MEMORY_FORMAT_E820).
 */
#include <uefi.h>
#include "../arch.h"

#define X86_KERNEL_BASE       UINT64_C(0x100000)
#define X86_HANDOFF_BASE      UINT64_C(0x60000)
#define X86_HANDOFF_PAGES     4
#define X86_DESC_BASE         UINT64_C(0x61000)
#define X86_DESC_CAPACITY     UINT64_C(0x1000)
#define X86_E820_BASE         UINT64_C(0x62000)

static int x86_kernel_reserved;       /* pages at 0x100000 allocated */
static uintn_t x86_kernel_pages;
static int x86_handoff_reserved;      /* pages at 0x60000 allocated */

/* Narrow-string helpers.  config.txt / kernel.bin are ASCII byte streams;
 * under UEFI_NO_UTF8 the runtime's strchr/strlen/strcmp/atoi are wide and
 * must not be used on them. */
static size_t narrow_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static const char *narrow_strchr(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (*s == (char)c) ? s : (const char *)0;
}

static int narrow_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static long narrow_atoi(const char *s)
{
    long value = 0;
    while (*s >= '0' && *s <= '9')
        value = value * 10 + (*s++ - '0');
    return value;
}

const char_t *arch_kernel_path(void)
{
    return L"kernel.bin";
}

efi_status_t arch_init_handoff(struct boot_context **ctx_out)
{
    efi_physical_address_t addr = X86_HANDOFF_BASE;
    EFI_STATUS status;

    status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
                               (uintn_t)X86_HANDOFF_PAGES, &addr);
    if (EFI_ERROR(status) || addr != X86_HANDOFF_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(addr, (uintn_t)X86_HANDOFF_PAGES);
        return EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
    }
    x86_handoff_reserved = 1;
    memset((void *)(uintptr_t)X86_HANDOFF_BASE, 0,
           (size_t)X86_HANDOFF_PAGES * 0x1000);
    boot_context_init((struct boot_context *)(uintptr_t)X86_HANDOFF_BASE);
    *ctx_out = (struct boot_context *)(uintptr_t)X86_HANDOFF_BASE;
    return EFI_SUCCESS;
}

efi_status_t arch_load_kernel(const void *image, uint64_t image_size,
                              uint64_t *entry_out)
{
    efi_physical_address_t addr = X86_KERNEL_BASE;
    uintn_t pages = (uintn_t)((image_size + 0xfff) / 0x1000);
    EFI_STATUS status;

    if (pages == 0)
        return EFI_LOAD_ERROR;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(status) || addr != X86_KERNEL_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(addr, pages);
        return EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
    }
    x86_kernel_reserved = 1;
    x86_kernel_pages = pages;
    memcpy((void *)(uintptr_t)X86_KERNEL_BASE, image, (size_t)image_size);
    *entry_out = X86_KERNEL_BASE;
    return EFI_SUCCESS;
}

static void x86_parse_config(int *want_width, int *want_height)
{
    FILE *cfg;
    long size;

    *want_width = 1440;
    *want_height = 900;
    cfg = fopen(L"config.txt", L"r");
    if (!cfg)
        return;
    if (fseek(cfg, 0, SEEK_END) != 0 || (size = ftell(cfg)) <= 0 ||
        fseek(cfg, 0, SEEK_SET) != 0) {
        fclose(cfg);
        return;
    }
    {
        char *buf = malloc((size_t)size + 1);
        if (!buf) {
            fclose(cfg);
            return;
        }
        if (fread(buf, 1, (size_t)size, cfg) != (size_t)size) {
            free(buf);
            fclose(cfg);
            return;
        }
        fclose(cfg);
        buf[size] = 0;

        /* Narrow scan: each line is "resolution WxH" or ignored. */
        {
            char *line = buf;
            while (*line) {
                char *nl = narrow_strchr(line, '\n');
                char *tok = line;
                char *sp;
                if (nl)
                    *nl = 0;
                while (*tok == ' ' || *tok == '\t')
                    tok++;
                sp = narrow_strchr(tok, ' ');
                if (sp)
                    *sp = 0;
                if (narrow_strcmp(tok, "resolution") == 0 && sp) {
                    char *dim = sp + 1;
                    char *x;
                    while (*dim == ' ' || *dim == '\t')
                        dim++;
                    x = narrow_strchr(dim, 'x');
                    if (x) {
                        *x = 0;
                        *want_width = (int)narrow_atoi(dim);
                        *want_height = (int)narrow_atoi(x + 1);
                    }
                }
                line = nl ? nl + 1 : line + narrow_strlen(line);
            }
        }
        free(buf);
    }
}

efi_status_t arch_setup_graphics(struct boot_context *ctx)
{
    efi_guid_t gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    efi_gop_t *gop = NULL;
    efi_gop_mode_info_t *info = NULL;
    uintn_t isiz = sizeof(efi_gop_mode_info_t), i;
    long current_h = 0, current_w = 0;
    int want_w, want_h;
    int expect_mode = 0;
    EFI_STATUS status;

    x86_parse_config(&want_w, &want_h);

    status = BS->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (EFI_ERROR(status) || !gop)
        return EFI_DEVICE_ERROR;

    for (i = 0; i < gop->Mode->MaxMode; i++) {
        status = gop->QueryMode(gop, i, &isiz, &info);
        if (EFI_ERROR(status) || !info || info->PixelFormat > PixelBitMask)
            continue;
        if (info->HorizontalResolution > current_w &&
            info->HorizontalResolution <= (long)want_w &&
            info->VerticalResolution > current_h &&
            info->VerticalResolution <= (long)want_h) {
            current_w = info->HorizontalResolution;
            current_h = info->VerticalResolution;
            expect_mode = (int)i;
        }
    }
    printf(CL("set VBE mode to %d\n"), expect_mode);
    status = gop->SetMode(gop, expect_mode);
    if (EFI_ERROR(status))
        return EFI_DEVICE_ERROR;

    status = gop->QueryMode(gop, gop->Mode ? gop->Mode->Mode : 0, &isiz, &info);
    if (status == EFI_NOT_STARTED || !gop->Mode) {
        status = gop->SetMode(gop, 0);
        ST->ConOut->Reset(ST->ConOut, 0);
        ST->StdErr->Reset(ST->StdErr, 0);
    }
    if (EFI_ERROR(status))
        return EFI_DEVICE_ERROR;

    return capture_graphics(ctx);
}

efi_status_t arch_fill_firmware(struct boot_context *ctx)
{
    efi_guid_t acpi_guid = ACPI_20_TABLE_GUID;
    uintn_t i;

    for (i = 0; i < ST->NumberOfTableEntries; i++) {
        if (guid_equal(&ST->ConfigurationTable[i].VendorGuid, &acpi_guid)) {
            ctx->firmware.acpi_rsdp =
                (uint64_t)ST->ConfigurationTable[i].VendorTable;
            ctx->flags |= BOOT_CONTEXT_HAS_ACPI;
            return EFI_SUCCESS;
        }
    }
    return EFI_SUCCESS;
}

void arch_memory_buffer(efi_physical_address_t *phys_out,
                        uint64_t *capacity_out)
{
    *phys_out = X86_DESC_BASE;
    *capacity_out = X86_DESC_CAPACITY;
}

static void x86_build_e820(efi_memory_descriptor_t *map, uintn_t map_size,
                           uintn_t desc_size, struct E820_ENTRY *out,
                           uint32_t *count_out)
{
    struct E820_ENTRY *last = NULL;
    uint64_t last_end = 0;
    uint32_t count = 0;
    efi_memory_descriptor_t *m;

    for (m = map; (uint8_t *)m < (uint8_t *)map + map_size;
         m = NextMemoryDescriptor(m, desc_size)) {
        int type;
        switch (m->Type) {
        case EfiReservedMemoryType:
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace:
        case EfiPalCode:
            type = 2;                     /* ROM or reserved */
            break;
        case EfiUnusableMemory:
            type = 5;                     /* unusable */
            break;
        case EfiACPIReclaimMemory:
            type = 3;                     /* ACPI reclaim */
            break;
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiConventionalMemory:
            type = 1;                     /* RAM */
            break;
        case EfiACPIMemoryNVS:
            type = 4;                     /* ACPI NVS */
            break;
        default:
            continue;
        }
        if (last && last->type == (uint32_t)type &&
            m->PhysicalStart == last_end) {
            last->length += (uint64_t)m->NumberOfPages << 12;
            last_end += (uint64_t)m->NumberOfPages << 12;
        } else {
            out[count].address = m->PhysicalStart;
            out[count].length = (uint64_t)m->NumberOfPages << 12;
            out[count].type = (uint32_t)type;
            last = &out[count];
            last_end = m->PhysicalStart + ((uint64_t)m->NumberOfPages << 12);
            count++;
        }
    }
    *count_out = count;
}

void arch_build_memory(struct boot_context *ctx,
                       efi_physical_address_t desc_phys,
                       uintn_t desc_size, uintn_t desc_count,
                       uint32_t desc_version)
{
    struct E820_ENTRY *e820 = (struct E820_ENTRY *)(uintptr_t)X86_E820_BASE;
    uint32_t count = 0;
    uint32_t i, j;

    (void)desc_version;

    /* E820 count <= raw descriptor count <= 85 (4 KB desc cap), well under
     * the 400-entry E820 range, so no separate overflow path is needed. */
    x86_build_e820((efi_memory_descriptor_t *)(uintptr_t)desc_phys,
                   (uintn_t)desc_count * desc_size, desc_size, e820, &count);

    /* Sort ascending by address (the raw map is not guaranteed sorted). */
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (e820[i].address > e820[j].address) {
                struct E820_ENTRY tmp = e820[i];
                e820[i] = e820[j];
                e820[j] = tmp;
            }
        }
    }

    ctx->memory.entries = X86_E820_BASE;
    ctx->memory.entry_count = count;
    ctx->memory.entry_size = (uint32_t)sizeof(struct E820_ENTRY);
    ctx->memory.format = BOOT_MEMORY_FORMAT_E820;
    ctx->memory.descriptor_version = 0;
    ctx->flags |= BOOT_CONTEXT_HAS_MEMORY_MAP;
}

void arch_release(void)
{
    if (x86_kernel_reserved) {
        (void)BS->FreePages((efi_physical_address_t)X86_KERNEL_BASE,
                            x86_kernel_pages);
        x86_kernel_reserved = 0;
    }
    if (x86_handoff_reserved) {
        (void)BS->FreePages((efi_physical_address_t)X86_HANDOFF_BASE,
                            (uintn_t)X86_HANDOFF_PAGES);
        x86_handoff_reserved = 0;
    }
}

void arch_puts(const char *s)
{
    /* Uppercase %S: under UEFI_NO_UTF8 the wide printf reads a narrow
     * UTF-8 char* vararg (%s would read a wide char_t* and garble). */
    printf(CL("%S"), s);
}

__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys)
{
    int (*kernel_main)(const struct boot_context *);

    kernel_main = (void *)(uintptr_t)entry;
    kernel_main((const struct boot_context *)(uintptr_t)context_phys);
    for (;;)
        ;
}
```

- [ ] **Step 4: Rewrite `boot/uefi/Makefile` as the parameterized wrapper**

```make
# boot/uefi/Makefile — unified UEFI bootloader build wrapper.
#
# Builds both EFI applications from the shared main.c + per-arch hooks:
#   make -C boot/uefi ARCH=x86_64    -> build/x86_64/uefi/BOOTX64.EFI
#   make -C boot/uefi ARCH=aarch64   -> build/aarch64/uefi/BOOTAA64.EFI
#
# Each ARCH gets a private copy of the posix-uefi runtime
# (build/<arch>/uefi-runtime/) compiled with UEFI_NO_UTF8, so objects
# never cross-pollute between architectures.

ARCH ?= x86_64
ROOT := $(abspath ../..)
BUILD_DIR := $(ROOT)/build/$(ARCH)
RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
OUTDIR := $(BUILD_DIR)/uefi

UEFI_RUNTIME_SOURCE ?= $(ROOT)/thirdpart/posix-uefi/uefi

ifeq ($(ARCH),x86_64)
TARGET := BOOTX64.EFI
SRCS := $(ROOT)/boot/uefi/main.c \
        $(ROOT)/boot/uefi/arch/x86_64/boot.c
else ifeq ($(ARCH),aarch64)
TARGET := BOOTAA64.EFI
SRCS := $(ROOT)/boot/uefi/main.c \
        $(ROOT)/boot/uefi/arch/aarch64/boot.c \
        $(ROOT)/boot/uefi/arch/aarch64/elf.c \
        $(ROOT)/boot/uefi/arch/aarch64/handoff.S
else
$(error "unsupported ARCH: $(ARCH)")
endif

.PHONY: all clean clean-all
all: $(OUTDIR)/$(TARGET)

$(OUTDIR)/$(TARGET): $(SRCS) $(ROOT)/boot/uefi/arch/arch.h \
		$(ROOT)/kernel/include/kernel/bootinfo.h
	@test -f $(UEFI_RUNTIME_SOURCE)/Makefile || { \
		echo "ERROR: posix-uefi runtime is unavailable at $(UEFI_RUNTIME_SOURCE)"; \
		false; \
	}
	rm -rf $(RUNTIME_DIR) $(OUTDIR)
	mkdir -p $(RUNTIME_DIR) $(OUTDIR)
	cp -a $(UEFI_RUNTIME_SOURCE) $(RUNTIME_DIR)/uefi
	cp $(RUNTIME_DIR)/uefi/Makefile $(RUNTIME_DIR)/Makefile
	rm -f $(RUNTIME_DIR)/uefi/*.o $(RUNTIME_DIR)/uefi/*.a
	sed -i 's/ARCH=$$(ARCH)/ARCH=$$(ARCH) OUTDIR=/' $(RUNTIME_DIR)/Makefile
	sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += -DUEFI_NO_UTF8' $(RUNTIME_DIR)/Makefile
	sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += -DUEFI_NO_UTF8' $(RUNTIME_DIR)/uefi/Makefile
ifeq ($(DEBUG),1)
	@sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += -DDEBUG=1' $(RUNTIME_DIR)/Makefile
	@sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += -DDEBUG=1' $(RUNTIME_DIR)/uefi/Makefile
endif
	@$(foreach source,$(SRCS),mkdir -p $(OUTDIR)$(dir $(source));)
	$(MAKE) -C $(RUNTIME_DIR) ARCH=$(ARCH) TARGET=$(TARGET) \
		OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)"

OVMF.fd:
	wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O $@

clean:
	rm -rf $(RUNTIME_DIR) $(OUTDIR)

clean-all:
	rm -rf $(RUNTIME_DIR) $(OUTDIR)
	rm -f OVMF.fd
```

Note: do NOT pass `CFLAGS=` to the inner make — a command-line `CFLAGS` would override the runtime's own flags; DEBUG is injected via `sed` like `UEFI_NO_UTF8`.

- [ ] **Step 5: Remove the now-unused runtime symlink**

```bash
git rm boot/uefi/uefi
```

- [ ] **Step 6: Update root `Makefile` (x86 EFI app path)**

Replace the bootloader block (around lines 47-48):

```make
boot/uefi/BOOTX64.EFI: boot/uefi/main.c
	make -C boot/uefi
```

with:

```make
BUILD_X86_64_UEFI := build/x86_64/uefi/BOOTX64.EFI

$(BUILD_X86_64_UEFI): boot/uefi/Makefile boot/uefi/main.c \
		boot/uefi/arch/arch.h boot/uefi/arch/x86_64/boot.c \
		kernel/include/kernel/bootinfo.h
	make -C boot/uefi ARCH=x86_64
```

Then change the `disk.img` dependency (line 167) `disk.img: boot/uefi/BOOTX64.EFI ...` to `disk.img: $(BUILD_X86_64_UEFI) ...`, and the `tools/mkdisk` invocation (line 220) `--efi boot/uefi/BOOTX64.EFI` to `--efi $(BUILD_X86_64_UEFI)`.

- [ ] **Step 7: Migrate the x86 kernel to `boot_context`**

`kernel/kernel/main.c`:
- Line 121: `int kernel_main(struct BOOT_INFO *bootinfo)` → `int kernel_main(const struct boot_context *bootctx)`
- Delete lines 126-127 (`struct boot_context bootctx;` + `boot_context_from_legacy(&bootctx, bootinfo);`)
- Line 159: `pmm_init(bootinfo->E820_Info)` → `pmm_init(&bootctx->memory)`
- Convert every remaining `bootctx.` to `bootctx->` (lines ~130-133 graphics fields, ~165 `bootctx.firmware.acpi_rsdp`).
- Verify with `grep -n 'bootinfo\|bootctx' kernel/kernel/main.c` that no `bootinfo` reference remains.

`kernel/include/kernel/memory.h` (line 26):
```c
void pmm_init(const struct BOOT_MEMORY_MAP *mmap);
```

`kernel/memory/pmm.c` — change the signature and the iteration loop (drop the read-past-end sentinel check):

```c
void pmm_init(const struct BOOT_MEMORY_MAP *mmap)
{
    uint32_t i, j;
    uint64_t TotalMem = 0;
    struct E820_ENTRY *p = (struct E820_ENTRY *)(uintptr_t)mmap->entries;

    debug_mm("Display Physics Address MAP,Type(1:RAM,2:ROM or Reserved,3:ACPI Reclaim Memory,4:ACPI NVS Memory,Others:Undefine)\n");
    for (i = 0; i < mmap->entry_count; i++)
    {
        debug_mm("Address:%#018lx\tLength:%#018lx\tType:%2d\n",p->address,p->length,p->type);
		if(p->type == 1)
		{
			TotalMem += p->length;
		}

        PMMngr.e820_entrys[i].address =  p->address;
        PMMngr.e820_entrys[i].length = p->length;
        PMMngr.e820_entrys[i].type = p->type;
        PMMngr.e820_length = i;

		p++;
    }
    /* ... the rest of the function body is UNCHANGED ... */
```

The old loop read one entry past the array (`p++; if (p->type > 4 || p->type < 1) break;`) relying on zero-terminated memory after the E820 array; the new fixed E820 range is not zero-terminated, so the loop must stop exactly at `entry_count`.

`kernel/arch/x86_64/head.S` — rename the handoff-pointer global `BOOT_INFO` → `BOOT_CONTEXT` (lines 15, 113, 251, 253). It is internal-only (no C references).

- [ ] **Step 8: Build the x86 EFI app**

Run: `make -C boot/uefi ARCH=x86_64`
Expected: `build/x86_64/uefi/BOOTX64.EFI` exists. If compilation errors, fix them (most likely a wide/narrow literal or an include-path issue).

- [ ] **Step 9: Build the full x86 disk and boot it**

```bash
rm -f disk.img
make disk.img
```

Then boot headless and confirm the shell is alive:

```bash
( sleep 25; printf 'echo X86_BOOT_OK_123\n'; sleep 5; ) \
  | timeout 60 qemu-system-x86_64 -M q35 -pflash boot/uefi/OVMF.fd \
      -drive file=disk.img,format=raw,if=none,id=disk -device ahci,id=ahci \
      -device ide-hd,drive=disk,bus=ahci.0 -m 512 -smp 1 -serial stdio \
      -display none -no-reboot -no-shutdown 2>&1 | tee /tmp/x86boot.log
grep -q 'X86_BOOT_OK_123' /tmp/x86boot.log && echo X86_BOOT_OK
```

Expected: `X86_BOOT_OK` — the kernel booted via `boot_context` and the shell is alive. If it hangs, check `/tmp/x86boot.log` for the kernel banner / panic; a mismatch between loader `boot_context` and kernel `kernel_main` reads shows up as a garbage `graphics`/`memory` deref very early.

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "boot: unified UEFI lifecycle main.c + arch/x86_64, x86 handoff -> boot_context

Restructure the x86 UEFI bootloader into a shared boot/uefi/main.c (common
lifecycle: kernel read, GOP capture, boot_context construction,
GetMemoryMap->ExitBootServices map-key retry, error cleanup) with per-arch
hooks in boot/uefi/arch/, behind a parameterized build wrapper. The x86
kernel entry now consumes boot_context v2 directly (kernel_main, pmm_init,
head.S), retiring the legacy BOOT_INFO path on the loader side.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: aarch64 loader → new tree

Move the aarch64 bootloader onto the shared `main.c` + `arch/aarch64/` layout. The aarch64 kernel is untouched in this task.

**Files:**
- Create: `boot/uefi/arch/aarch64/boot.c` (port of old `boot/uefi/aarch64/main.c` minus the common parts)
- Move: `boot/uefi/aarch64/{elf.c, loader.h, handoff.S}` → `boot/uefi/arch/aarch64/` (git mv, content unchanged)
- Delete: `boot/uefi/aarch64/main.c`, `boot/uefi/aarch64/Makefile`
- Modify: `Makefile` (root: aarch64 target deps/recipe)

**Interfaces:**
- Consumes: `arch/arch.h` (Task 1), `loader.h`, `elf.c`, `handoff.S`, common `read_kernel_file`/`capture_graphics`/`guid_equal`/`main_error_code` in `main.c`.
- Produces: `build/aarch64/uefi/BOOTAA64.EFI`.

- [ ] **Step 1: Move the aarch64 support files**

```bash
git mv boot/uefi/aarch64/elf.c    boot/uefi/arch/aarch64/elf.c
git mv boot/uefi/aarch64/loader.h boot/uefi/arch/aarch64/loader.h
git mv boot/uefi/aarch64/handoff.S boot/uefi/arch/aarch64/handoff.S
git rm boot/uefi/aarch64/main.c
git rm boot/uefi/aarch64/Makefile
```

- [ ] **Step 2: Write `boot/uefi/arch/aarch64/boot.c`**

```c
/* boot/uefi/arch/aarch64/boot.c — aarch64 arch hooks.
 *
 * Kernel: validated ELF loaded at fixed PT_LOAD physical addresses
 *   (entry 0x40080000, see loader.h).
 * Handoff: boot_context at 0x401e0000 in a 31-page allocation; the
 *   firmware FDT is copied after the context, and the raw UEFI memory
 *   map is placed after that, up to the trampoline page @ 0x401ff000.
 * Memory map: raw UEFI descriptors (BOOT_MEMORY_FORMAT_UEFI_RAW).
 */
#include <uefi.h>
#include "../arch.h"
#include "loader.h"

/* QEMU virt's PL011 UART: physical MMIO at 0x09000000. */
#define PL011_BASE       ((volatile uint32_t *)(uintptr_t)0x09000000U)
#define PL011_DR          0x00U
#define PL011_FR          0x18U
#define PL011_FR_TXFF     (1U << 5)

#define AARCH64_HANDOFF_END        UINT64_C(0x40200000)
#define AARCH64_HANDOFF_PAGES      ((AARCH64_HANDOFF_END - \
                                     AARCH64_HANDOFF_BASE) / \
                                    AARCH64_PAGE_SIZE)
#define AARCH64_TRAMPOLINE_BASE    (AARCH64_HANDOFF_END - \
                                    AARCH64_PAGE_SIZE)
#define AARCH64_HANDOFF_DATA_PAGES (AARCH64_HANDOFF_PAGES - 1)
#define AARCH64_FDT_MAGIC          UINT32_C(0xd00dfeed)
#define AARCH64_FDT_HEADER_SIZE    UINT32_C(40)

/* UEFI Device Tree Table GUID: b1b621d5-f19c-41a5-830b-d9152c69aae0. */
static const efi_guid_t fdt_table_guid = {
    0xb1b621d5U, 0xf19cU, 0x41a5U,
    { 0x83U, 0x0bU, 0xd9U, 0x15U, 0x2cU, 0x69U, 0xaaU, 0xe0U }
};

extern const uint8_t aarch64_handoff_stub_start[];
extern const uint8_t aarch64_handoff_stub_end[];
__attribute__((noreturn)) void aarch64_enter_kernel(uint64_t entry,
                                                     uint64_t context_phys);

struct aarch64_fixed_allocation {
    efi_physical_address_t address;
    uintn_t pages;
};

static struct aarch64_fixed_allocation *kernel_allocations;
static uint16_t kernel_allocation_count;
static uint64_t handoff_cursor;
static int aarch64_data_reserved;        /* 0x401e0000, DATA_PAGES */
static int aarch64_trampoline_reserved;  /* 0x401ff000, 1 page */

static void pl011_putc(char c)
{
    while (PL011_BASE[PL011_FR / sizeof(uint32_t)] & PL011_FR_TXFF)
        ;
    PL011_BASE[PL011_DR / sizeof(uint32_t)] = (uint8_t)c;
}

static void pl011_puts(const char *s)
{
    while (*s)
        pl011_putc(*s++);
}

static uint32_t load_be32(const void *address)
{
    const uint8_t *bytes = (const uint8_t *)address;

    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static uint64_t align_up_8(uint64_t value)
{
    return (value + UINT64_C(7)) & ~UINT64_C(7);
}

static uint64_t read_current_el(void)
{
    uint64_t current_el;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    return current_el;
}

static uint64_t read_mpidr(void)
{
    uint64_t mpidr;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr;
}

static void synchronize_code_range(const void *address, uint64_t size)
{
    uint64_t ctr;
    uint64_t data_line;
    uint64_t instruction_line;
    uint64_t cursor;
    uint64_t end = (uint64_t)(uintptr_t)address + size;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    data_line = UINT64_C(4) << ((ctr >> 16) & UINT64_C(0xf));
    instruction_line = UINT64_C(4) << (ctr & UINT64_C(0xf));

    cursor = (uint64_t)(uintptr_t)address & ~(data_line - 1);
    for (; cursor < end; cursor += data_line)
        __asm__ volatile("dc cvau, %0" : : "r"(cursor) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");

    cursor = (uint64_t)(uintptr_t)address & ~(instruction_line - 1);
    for (; cursor < end; cursor += instruction_line)
        __asm__ volatile("ic ivau, %0" : : "r"(cursor) : "memory");
    __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

static void release_kernel_allocations(void)
{
    while (kernel_allocation_count != 0) {
        const struct aarch64_fixed_allocation *allocation;

        kernel_allocation_count--;
        allocation = &kernel_allocations[kernel_allocation_count];
        (void)BS->FreePages(allocation->address, allocation->pages);
    }
    if (kernel_allocations) {
        free(kernel_allocations);
        kernel_allocations = NULL;
    }
}

static EFI_STATUS prepare_trampoline(void)
{
    uint64_t stub_size =
        (uint64_t)(aarch64_handoff_stub_end - aarch64_handoff_stub_start);
    void *destination = (void *)(uintptr_t)AARCH64_TRAMPOLINE_BASE;

    if (!aarch64_handoff_el_supported(read_current_el()) ||
        stub_size == 0 || stub_size > AARCH64_PAGE_SIZE) {
        pl011_puts("UEFI-A64: trampoline unreachable\r\n");
        return EFI_UNSUPPORTED;
    }

    memcpy(destination, aarch64_handoff_stub_start, (size_t)stub_size);
    if (memcmp(destination, aarch64_handoff_stub_start,
               (size_t)stub_size) != 0) {
        pl011_puts("UEFI-A64: trampoline unreachable\r\n");
        return EFI_DEVICE_ERROR;
    }
    synchronize_code_range(destination, stub_size);
    return EFI_SUCCESS;
}

static const void *find_fdt(uint32_t *size_out)
{
    uintn_t index;

    if (!size_out || !ST || !ST->ConfigurationTable)
        return NULL;
    for (index = 0; index < ST->NumberOfTableEntries; index++) {
        const efi_configuration_table_t *table = &ST->ConfigurationTable[index];
        const uint8_t *fdt = (const uint8_t *)table->VendorTable;
        uint32_t total_size;

        if (!guid_equal(&table->VendorGuid, &fdt_table_guid) || !fdt)
            continue;
        if (load_be32(fdt) != AARCH64_FDT_MAGIC)
            return NULL;
        total_size = load_be32(fdt + sizeof(uint32_t));
        if (total_size < AARCH64_FDT_HEADER_SIZE)
            return NULL;
        *size_out = total_size;
        return fdt;
    }
    return NULL;
}

const char_t *arch_kernel_path(void)
{
    return L"\\kernel.elf";
}

efi_status_t arch_init_handoff(struct boot_context **ctx_out)
{
    efi_physical_address_t addr;
    struct boot_context *ctx;
    EFI_STATUS status;

    addr = (efi_physical_address_t)AARCH64_HANDOFF_BASE;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
                               (uintn_t)AARCH64_HANDOFF_DATA_PAGES, &addr);
    if (EFI_ERROR(status) || addr != AARCH64_HANDOFF_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(addr, (uintn_t)AARCH64_HANDOFF_DATA_PAGES);
        return EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
    }
    aarch64_data_reserved = 1;

    addr = (efi_physical_address_t)AARCH64_TRAMPOLINE_BASE;
    status = BS->AllocatePages(AllocateAddress, EfiLoaderCode, 1, &addr);
    if (EFI_ERROR(status) || addr != AARCH64_TRAMPOLINE_BASE) {
        if (!EFI_ERROR(status))
            (void)BS->FreePages(addr, 1);
        status = EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
        goto fail;
    }
    aarch64_trampoline_reserved = 1;

    memset((void *)(uintptr_t)AARCH64_HANDOFF_BASE, 0,
           (size_t)(AARCH64_HANDOFF_END - AARCH64_HANDOFF_BASE));
    ctx = (struct boot_context *)(uintptr_t)AARCH64_HANDOFF_BASE;
    boot_context_init(ctx);
    ctx->boot_cpu_id = read_mpidr();
    ctx->flags |= BOOT_CONTEXT_HAS_BOOT_CPU_ID;
    handoff_cursor = align_up_8(AARCH64_HANDOFF_BASE + sizeof(*ctx));

    status = prepare_trampoline();   /* pre-EBS so failure is reportable */
    if (EFI_ERROR(status))
        goto fail;

    *ctx_out = ctx;
    return EFI_SUCCESS;

fail:
    arch_release();
    return status;
}

efi_status_t arch_load_kernel(const void *image, uint64_t image_size,
                              uint64_t *entry_out)
{
    const uint8_t *bytes = (const uint8_t *)image;
    const struct aarch64_elf64_ehdr *ehdr;
    uint16_t index;

    release_kernel_allocations();
    if (aarch64_elf_validate(image, image_size, entry_out) != 0) {
        pl011_puts("UEFI-A64: bad ELF\r\n");
        return EFI_LOAD_ERROR;
    }

    ehdr = (const struct aarch64_elf64_ehdr *)bytes;
    kernel_allocations =
        malloc((size_t)ehdr->e_phnum * sizeof(*kernel_allocations));
    if (!kernel_allocations) {
        pl011_puts("UEFI-A64: load allocation failed\r\n");
        return EFI_OUT_OF_RESOURCES;
    }
    for (index = 0; index < ehdr->e_phnum; index++) {
        const struct aarch64_elf64_phdr *phdr =
            (const struct aarch64_elf64_phdr *)
            (bytes + ehdr->e_phoff +
             (uint64_t)index * ehdr->e_phentsize);
        efi_physical_address_t allocation;
        uint64_t component_end;
        uint64_t component_pages;
        uint64_t component_start;
        uint64_t interval_start;
        uint64_t interval_pages;
        uint16_t previous;
        int changed;
        int previously_allocated = 0;
        EFI_STATUS status;

        if (phdr->p_type != AARCH64_PT_LOAD)
            continue;
        if (aarch64_page_interval(phdr->p_paddr, phdr->p_memsz,
                                  &interval_start, &interval_pages) != 0) {
            release_kernel_allocations();
            pl011_puts("UEFI-A64: bad ELF\r\n");
            return EFI_LOAD_ERROR;
        }
        if (interval_pages == 0)
            continue;

        component_start = interval_start;
        component_end = interval_start + interval_pages * AARCH64_PAGE_SIZE;
        do {
            uint16_t scan;

            changed = 0;
            for (scan = 0; scan < ehdr->e_phnum; scan++) {
                const struct aarch64_elf64_phdr *other =
                    (const struct aarch64_elf64_phdr *)
                    (bytes + ehdr->e_phoff +
                     (uint64_t)scan * ehdr->e_phentsize);
                uint64_t other_end;
                uint64_t other_pages;
                uint64_t other_start;

                if (other->p_type != AARCH64_PT_LOAD ||
                    aarch64_page_interval(other->p_paddr, other->p_memsz,
                                          &other_start, &other_pages) != 0 ||
                    other_pages == 0)
                    continue;
                other_end = other_start + other_pages * AARCH64_PAGE_SIZE;
                if (other_start < component_end &&
                    component_start < other_end) {
                    if (other_start < component_start) {
                        component_start = other_start;
                        changed = 1;
                    }
                    if (other_end > component_end) {
                        component_end = other_end;
                        changed = 1;
                    }
                }
            }
        } while (changed);

        for (previous = 0; previous < index; previous++) {
            const struct aarch64_elf64_phdr *other =
                (const struct aarch64_elf64_phdr *)
                (bytes + ehdr->e_phoff +
                 (uint64_t)previous * ehdr->e_phentsize);
            uint64_t other_end;
            uint64_t other_pages;
            uint64_t other_start;

            if (other->p_type != AARCH64_PT_LOAD ||
                aarch64_page_interval(other->p_paddr, other->p_memsz,
                                      &other_start, &other_pages) != 0 ||
                other_pages == 0)
                continue;
            other_end = other_start + other_pages * AARCH64_PAGE_SIZE;
            if (other_start < component_end && component_start < other_end) {
                previously_allocated = 1;
                break;
            }
        }
        if (previously_allocated)
            continue;

        component_pages =
            (component_end - component_start) / AARCH64_PAGE_SIZE;
        allocation = (efi_physical_address_t)component_start;
        status = BS->AllocatePages(AllocateAddress, EfiLoaderData,
                                   (uintn_t)component_pages, &allocation);
        if (EFI_ERROR(status) || allocation != component_start) {
            if (!EFI_ERROR(status)) {
                (void)BS->FreePages(allocation, (uintn_t)component_pages);
            }
            release_kernel_allocations();
            pl011_puts("UEFI-A64: load allocation failed\r\n");
            return EFI_ERROR(status) ? status : EFI_OUT_OF_RESOURCES;
        }
        kernel_allocations[kernel_allocation_count].address = allocation;
        kernel_allocations[kernel_allocation_count].pages =
            (uintn_t)component_pages;
        kernel_allocation_count++;
    }

    /* Copy only after every coalesced page interval has been reserved. */
    for (index = 0; index < ehdr->e_phnum; index++) {
        const struct aarch64_elf64_phdr *phdr =
            (const struct aarch64_elf64_phdr *)
            (bytes + ehdr->e_phoff +
             (uint64_t)index * ehdr->e_phentsize);

        if (phdr->p_type != AARCH64_PT_LOAD)
            continue;
        memcpy((void *)(uintptr_t)phdr->p_paddr,
               bytes + phdr->p_offset, (size_t)phdr->p_filesz);
        memset((void *)(uintptr_t)(phdr->p_paddr + phdr->p_filesz), 0,
               (size_t)(phdr->p_memsz - phdr->p_filesz));
        if (phdr->p_memsz != 0)
            synchronize_code_range((const void *)(uintptr_t)phdr->p_paddr,
                                   phdr->p_memsz);
    }

    return EFI_SUCCESS;
}

efi_status_t arch_setup_graphics(struct boot_context *ctx)
{
    /* Best-effort: a missing GOP must not abort the boot. */
    (void)capture_graphics(ctx);
    return EFI_SUCCESS;
}

efi_status_t arch_fill_firmware(struct boot_context *ctx)
{
    const void *firmware_fdt;
    uint32_t fdt_size = 0;

    firmware_fdt = find_fdt(&fdt_size);
    if (!firmware_fdt)
        return EFI_SUCCESS;
    if ((uint64_t)fdt_size > AARCH64_TRAMPOLINE_BASE - handoff_cursor)
        return EFI_BUFFER_TOO_SMALL;
    memcpy((void *)(uintptr_t)handoff_cursor, firmware_fdt, (size_t)fdt_size);
    ctx->firmware.dtb = handoff_cursor;
    ctx->flags |= BOOT_CONTEXT_HAS_DTB;
    handoff_cursor = align_up_8(handoff_cursor + (uint64_t)fdt_size);
    return EFI_SUCCESS;
}

void arch_memory_buffer(efi_physical_address_t *phys_out,
                        uint64_t *capacity_out)
{
    *phys_out = handoff_cursor;
    *capacity_out = AARCH64_TRAMPOLINE_BASE - handoff_cursor;
}

void arch_build_memory(struct boot_context *ctx,
                       efi_physical_address_t desc_phys,
                       uintn_t desc_size, uintn_t desc_count,
                       uint32_t desc_version)
{
    ctx->memory.entries = desc_phys;
    ctx->memory.entry_count = (uint32_t)desc_count;
    ctx->memory.entry_size = (uint32_t)desc_size;
    ctx->memory.format = BOOT_MEMORY_FORMAT_UEFI_RAW;
    ctx->memory.descriptor_version = desc_version;
    ctx->flags |= BOOT_CONTEXT_HAS_MEMORY_MAP;
}

void arch_release(void)
{
    release_kernel_allocations();
    if (aarch64_trampoline_reserved) {
        (void)BS->FreePages((efi_physical_address_t)AARCH64_TRAMPOLINE_BASE, 1);
        aarch64_trampoline_reserved = 0;
    }
    if (aarch64_data_reserved) {
        (void)BS->FreePages((efi_physical_address_t)AARCH64_HANDOFF_BASE,
                            (uintn_t)AARCH64_HANDOFF_DATA_PAGES);
        aarch64_data_reserved = 0;
    }
}

void arch_puts(const char *s)
{
    pl011_puts(s);
}

__attribute__((noreturn)) void arch_enter_kernel(uint64_t entry,
                                                 uint64_t context_phys)
{
    aarch64_enter_kernel(entry, context_phys);
}
```

- [ ] **Step 3: Update root `Makefile` aarch64 target**

Replace the `$(AARCH64_UEFI_APP)` rule (around lines 258-262):

```make
$(AARCH64_UEFI_APP): boot/uefi/aarch64/Makefile \
		boot/uefi/aarch64/main.c boot/uefi/aarch64/elf.c \
		boot/uefi/aarch64/handoff.S \
		boot/uefi/aarch64/loader.h kernel/include/kernel/bootinfo.h
	$(MAKE) -C boot/uefi/aarch64
```

with:

```make
$(AARCH64_UEFI_APP): boot/uefi/Makefile \
		boot/uefi/main.c boot/uefi/arch/arch.h \
		boot/uefi/arch/aarch64/boot.c boot/uefi/arch/aarch64/elf.c \
		boot/uefi/arch/aarch64/handoff.S \
		boot/uefi/arch/aarch64/loader.h kernel/include/kernel/bootinfo.h
	$(MAKE) -C boot/uefi ARCH=aarch64
```

- [ ] **Step 4: Build BOOTAA64.EFI**

Run: `make -C boot/uefi ARCH=aarch64`
Expected: `build/aarch64/uefi/BOOTAA64.EFI` exists.

- [ ] **Step 5: Boot the aarch64 UEFI image**

```bash
make aarch64-uefi
timeout 40 qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512 \
  -drive if=pflash,format=raw,file=build/aarch64/QEMU_EFI.fd \
  -drive if=none,file=build/aarch64/disk.img,format=raw,id=disk \
  -device virtio-blk-device,drive=disk -serial stdio -display none -no-reboot \
  2>&1 | tee /tmp/a64boot.log
grep -q 'uefi handoff ok' /tmp/a64boot.log && echo AARCH64_UEFI_OK
```

Expected: `AARCH64_UEFI_OK` (kernel reached via the new unified loader, printed `OS01 aarch64 uefi handoff ok`). If it fails, check `/tmp/a64boot.log` — a failure before `[tick]` is a loader handoff regression.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "boot: aarch64 loader onto shared lifecycle (arch/aarch64)

Port the aarch64 UEFI bootloader onto the common boot/uefi/main.c via
arch/aarch64/boot.c; elf.c/loader.h/handoff.S move under arch/aarch64/.
The old aarch64-specific Makefile and main.c are removed.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: Remove aarch64 direct `-kernel` boot

The aarch64 kernel only boots via UEFI now. Remove the boot-mode selector, the direct-boot branches in `aarch64_main`, the host-test guard, and the direct-boot launch targets.

**Files:**
- Modify: `kernel/arch/aarch64/main.c`, `Makefile` (root: remove `run-aarch64`, `debug-aarch64`), `.vscode/launch.json` (remove aarch64 entry), `test/cases/test_bootinfo_abi.c` (drop selector tests), `test/Makefile` (drop `aarch64_boot_mode.o`)
- Keep compiled but orphaned: `kernel/arch/aarch64/smp.c`, `psci.c`, `test_spinlock.c` (do NOT delete or wire them in)

**Interfaces:**
- Consumes: `struct boot_context` + `boot_context_valid` from `kernel/include/kernel/bootinfo.h` (still has `boot_context_from_aarch64` until Task 4).
- Produces: an aarch64 kernel that validates the handoff pointer directly.

- [ ] **Step 1: Rewrite `kernel/arch/aarch64/main.c`**

Delete the `AARCH64_UEFI_HANDOFF_ADDRESS` macro, the `enum aarch64_boot_mode`, and `aarch64_select_boot_mode()` entirely (lines ~43-61). Remove the `#ifndef AARCH64_BOOT_MODE_HOST_TEST` guard (line 63 and its closing `#endif` near the end). Replace `aarch64_main`:

```c
void aarch64_main(const struct boot_context *handoff)
{
    if (!boot_context_valid(handoff)) {
        pl011_init();
        kputs(corrupt_handoff_banner);
        for (;;) {
            arch_cpu_halt();
        }
    }

    /* Step 1: idempotent — a re-entry (early bring-up debugging)
     * shouldn't unmask IRQs by accident. */
    arch_local_irq_disable();

    /* Step 2: PL011.  QEMU already works with reset values, but
     * it doesn't hurt to program LCR_H+CR for a clean state. */
    pl011_init();

    /* Step 3: re-point VBAR_EL1 to the high-half vector table. */
    {
        uint64_t vbar = (uint64_t)(uintptr_t)exception_vectors;
        __asm__ __volatile__("msr vbar_el1, %0" :: "r"(vbar) : "memory");
        __asm__ __volatile__("isb" ::: "memory");
    }

    /* Step 4: DTB.  Parses 5 nodes (/cpus, /psci, /timer,
     * /interrupt-controller, /pl011); missing/unparsable DTB falls back
     * to QEMU virt defaults. */
    dtb_init(handoff->firmware.dtb);

    kputs(uefi_handoff_banner);
    kputs(banner);

    /* Step 5: GICv2.  Distributor + CPU interface, only PPI 30. */
    gic_init();

    /* Step 6: CNTP physical-timer period mode. */
    if (!arch_tick_start()) {
        kputs("[cntp] arch_tick_start FAILED\n");
        for (;;) {
            arch_cpu_halt();
        }
    }

    /* Step 8: ENABLE IRQs.  After this the tick ISR fires every 10 ms
     * and prints "[tick] N" once a second. */
    kputs("[IRQ] enabled (DAIF.IRQ cleared)\n");
    arch_local_irq_enable();
    __asm__ __volatile__("isb" ::: "memory");

    for (;;) {
        __asm__ __volatile__("dsb sy" ::: "memory");
        arch_cpu_halt();         /* wfi */
    }
}
```

Also remove from the file: the `smp_boot_aps` forward declaration, the `aarch64_memset` helper if now unused (check), the `aarch64_clear_bss` function only if it referenced removed code (it does not — it can stay as `__attribute__((used))`), and the `boot_context_from_aarch64` import (it came only from the direct-boot branch). Keep `banner`, `uefi_handoff_banner`, `corrupt_handoff_banner`, `pl011_init`, `dtb_init`, `gic_init`, `arch_tick_start`, `exception_vectors` (all still used).

- [ ] **Step 2: Remove the direct-boot Makefile targets and vscode entry**

In root `Makefile`, delete the `run-aarch64` target (lines ~247-253) and the `debug-aarch64` target (lines ~291-299).

In `.vscode/launch.json`, remove the aarch64 launch configuration (the one referencing `build/aarch64/kernel/kernel.elf`, around line 69).

- [ ] **Step 3: Update `test/cases/test_bootinfo_abi.c`**

Delete the `enum aarch64_boot_mode` definition, the `aarch64_select_boot_mode` declaration, and the final block of selector assertions (lines ~91-103). The remaining `boot_context_from_legacy` / `boot_context_from_aarch64` checks stay until Task 4.

- [ ] **Step 4: Update `test/Makefile`**

Delete the `aarch64_boot_mode.o` compile rule (lines ~237-241), and change the link rule (lines ~228-230) to:

```make
$(TEST_BLD)/test_bootinfo_abi.elf: $(TEST_BLD)/test_bootinfo_abi.o
	$(HOST_CC) -o $@ $^
```

- [ ] **Step 5: Build + host tests + aarch64 UEFI smoke**

```bash
make -C test
make aarch64-uefi
timeout 40 qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512 \
  -drive if=pflash,format=raw,file=build/aarch64/QEMU_EFI.fd \
  -drive if=none,file=build/aarch64/disk.img,format=raw,id=disk \
  -device virtio-blk-device,drive=disk -serial stdio -display none -no-reboot \
  2>&1 | tee /tmp/a64boot2.log
grep -q 'uefi handoff ok' /tmp/a64boot2.log && echo AARCH64_UEFI_OK
grep -q '\[tick\]' /tmp/a64boot2.log && echo AARCH64_TICK_OK
```

Expected: host tests pass; `AARCH64_UEFI_OK` and `AARCH64_TICK_OK`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "boot: remove aarch64 direct -kernel boot

The aarch64 kernel now boots only via UEFI: drop aarch64_select_boot_mode
and the direct-boot branches in aarch64_main, remove the
AARCH64_BOOT_MODE_HOST_TEST guard and its host test, and delete the
run-aarch64/debug-aarch64 Makefile targets and vscode launch entry.
aarch64 SMP (smp.c/psci/test_spinlock) stays compiled but orphaned.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: Remove legacy bootinfo ABI + update host test

Remove the now-dead legacy handoff types and adapters from `bootinfo.h` and the ABI test.

**Files:**
- Modify: `kernel/include/kernel/bootinfo.h`, `test/cases/test_bootinfo_abi.c`

**Interfaces:**
- Consumes: nothing new; every consumer of `struct BOOT_INFO`/`struct MEMORY_INFO`/`boot_context_from_legacy`/`boot_context_from_aarch64` was removed in Tasks 1-3.
- Produces: `bootinfo.h` containing only `E820_ENTRY`, `GRAPHICS_INFO`, `BOOT_MEMORY_MAP`, `BOOT_FIRMWARE`, `boot_context`, flags, formats, `boot_context_init`, `boot_context_valid`.

- [ ] **Step 1: Trim `kernel/include/kernel/bootinfo.h`**

Delete `struct E820_ENTRY` only if unused — it IS still used (loader E820 gen in `arch/x86_64/boot.c`, `pmm.c`), so KEEP it. Delete:
- `struct MEMORY_INFO`
- `struct BOOT_INFO`
- `boot_context_from_legacy()`
- `boot_context_from_aarch64()`

Keep: `struct GRAPHICS_INFO`, `struct E820_ENTRY`, `enum BOOT_CONTEXT_FLAGS`, `enum BOOT_MEMORY_FORMAT`, `struct BOOT_MEMORY_MAP`, `struct BOOT_FIRMWARE`, `struct boot_context`, `boot_context_init`, `boot_context_valid`. Update the file's leading comment (it mentions the legacy BOOT_INFO ABI) to describe the `boot_context` ABI.

- [ ] **Step 2: Rewrite `test/cases/test_bootinfo_abi.c`**

```c
#include <stddef.h>
#include <stdio.h>
#include <kernel/bootinfo.h>

_Static_assert(BOOT_CONTEXT_MAGIC == UINT32_C(0x4f533031),
               "boot context magic");
_Static_assert(BOOT_CONTEXT_VERSION == 2u, "boot context version");
_Static_assert(BOOT_MEMORY_FORMAT_UEFI_RAW == 3,
               "UEFI raw memory map format");
_Static_assert(sizeof(struct BOOT_MEMORY_MAP) == 24, "memory map ABI");
_Static_assert(offsetof(struct BOOT_MEMORY_MAP, descriptor_version) == 20,
               "UEFI descriptor version offset");
_Static_assert(sizeof(struct BOOT_FIRMWARE) == 16, "firmware ABI");
_Static_assert(offsetof(struct boot_context, magic) == 0,
               "magic leading offset");
_Static_assert(offsetof(struct boot_context, version) == 4,
               "version leading offset");
_Static_assert(offsetof(struct boot_context, size) == 8,
               "size leading offset");
_Static_assert(offsetof(struct boot_context, graphics) == 24,
               "graphics offset");
_Static_assert(offsetof(struct boot_context, memory) == 56,
               "memory offset");
_Static_assert(offsetof(struct boot_context, firmware) == 80,
               "firmware offset");
_Static_assert(sizeof(struct boot_context) == 104, "boot context ABI");

int main(void)
{
    struct boot_context ctx;

    boot_context_init(&ctx);
    if (ctx.magic != BOOT_CONTEXT_MAGIC ||
        ctx.version != BOOT_CONTEXT_VERSION ||
        ctx.size != sizeof(ctx) || ctx.memory.descriptor_version != 0 ||
        !boot_context_valid(&ctx))
        return 1;

    ctx.magic ^= 1u;
    if (boot_context_valid(&ctx))
        return 1;
    ctx.magic = BOOT_CONTEXT_MAGIC;

    ctx.version ^= 1u;
    if (boot_context_valid(&ctx))
        return 1;
    ctx.version = BOOT_CONTEXT_VERSION;

    ctx.size--;
    if (boot_context_valid(&ctx))
        return 1;

    puts("bootinfo ABI: PASS");
    return 0;
}
```

- [ ] **Step 3: Verify no legacy references remain + run host tests**

```bash
grep -rn 'BOOT_INFO\|MEMORY_INFO\|from_legacy\|from_aarch64' \
  --include='*.c' --include='*.h' --include='*.S' kernel/ boot/ test/ | grep -v toolchain
make -C test
```

Expected: the grep returns nothing (except possibly harmless comments/doc strings you may leave); `make -C test` passes with `bootinfo ABI: PASS`.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "boot: retire legacy BOOT_INFO/MEMORY_INFO and adapter helpers

bootinfo.h now exposes only the boot_context v2 ABI (plus E820_ENTRY and
GRAPHICS_INFO). boot_context_from_legacy and boot_context_from_aarch64 are
removed now that the x86 loader and kernel both speak boot_context and
aarch64 direct boot is gone. test_bootinfo_abi covers only the surviving
ABI.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 5: Docs + final verification

**Files:**
- Modify: `AGENTS.md`, `docs/boot.md`

- [ ] **Step 1: Update `AGENTS.md`**

- Line 29 (`Boot:` row of the Architecture block): `UEFI → BOOTX64.EFI → kernel.bin @ phys 0x100000` → note the handoff is `boot_context` now (both arches). E.g. `UEFI → BOOTX64.EFI → boot_context @ 0x60000 → kernel.bin @ 0x100000`.
- The `BOOT_INFO ABI` critical gotcha (line 39): rewrite as the **boot_context ABI** — both loaders build `boot_context` v2; bootloader is LLP64, kernel LP64; all fields `uint32_t`/`uint64_t`, never `unsigned long`.
- The key-files table row for `kernel/include/kernel/bootinfo.h` (line 100): update "Fixed-size types critical for ABI" to mention the `boot_context` v2 handoff shared by both UEFI loaders.

- [ ] **Step 2: Update `docs/boot.md`**

Read `docs/boot.md` and update the boot-chain description: x86 is `BOOTX64.EFI` (shared `boot/uefi/main.c`) → `boot_context` → `kernel_main(const struct boot_context *)`; aarch64 is UEFI-only (direct `-kernel` removed). Point at `boot/uefi/arch/` for the per-arch hooks.

- [ ] **Step 3: Full verification pass**

```bash
make -C boot/uefi ARCH=x86_64 && make -C boot/uefi ARCH=aarch64
make -C test
rm -f disk.img && make disk.img
# x86 smoke
( sleep 25; printf 'echo X86_BOOT_OK_123\n'; sleep 5; ) \
  | timeout 60 qemu-system-x86_64 -M q35 -pflash boot/uefi/OVMF.fd \
      -drive file=disk.img,format=raw,if=none,id=disk -device ahci,id=ahci \
      -device ide-hd,drive=disk,bus=ahci.0 -m 512 -smp 1 -serial stdio \
      -display none -no-reboot -no-shutdown 2>&1 | tee /tmp/x86final.log
grep -q 'X86_BOOT_OK_123' /tmp/x86final.log && echo X86_FINAL_OK
# aarch64 smoke
make aarch64-uefi
timeout 40 qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512 \
  -drive if=pflash,format=raw,file=build/aarch64/QEMU_EFI.fd \
  -drive if=none,file=build/aarch64/disk.img,format=raw,id=disk \
  -device virtio-blk-device,drive=disk -serial stdio -display none -no-reboot \
  2>&1 | tee /tmp/a64final.log
grep -q '\[tick\]' /tmp/a64final.log && echo AARCH64_FINAL_OK
```

Expected: `X86_FINAL_OK` and `AARCH64_FINAL_OK`, all host tests pass.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "docs: boot_context handoff for both UEFI bootloaders

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Self-Review Notes

- **Spec coverage:** §1/§4 (hooks + interface) → Task 1; §4a (x86 layout) → Task 1 boot.c; §5 (common lifecycle + EBS loop) → Task 1 main.c; §6 (unified ABI + kernel migration) → Task 1; §7 (E820 vs raw) → Tasks 1-2; §8 (wrapper + UEFI_NO_UTF8) → Task 1 Makefile; §9 (aarch64 direct boot removal) → Task 3; §10 (tests) → Tasks 3-4; §11 (docs) → Task 5; §12 (verification) → each task's smoke step + Task 5.
- **Placeholders:** none — all files are fully specified. P1-P5 review items are implemented inline (arch_puts `%S`, `MAP_SLACK=2`, x86 sub-regions, `EFI_NOT_STARTED` fallback, `types[]` not carried over into the new tree because the DEBUG print was in the old x86 body and is intentionally dropped from the new common path — the DEBUG `types[]` table from the old x86 main.c is NOT migrated; if DEBUG output is wanted later, it should use `%S`).
- **Type consistency:** `arch_fill_firmware` returns `efi_status_t` everywhere (arch.h, main.c, both boot.c). `capture_graphics`/`guid_equal` are non-static, declared in arch.h, defined in main.c. `arch_enter_kernel` is `noreturn` on both arches. `aarch64_handoff_el_supported` and `aarch64_page_interval` come from `loader.h`.
