/* boot/uefi/arch/x86_64/boot.c — x86_64 arch hooks.
 *
 * Kernel: raw kernel.bin at fixed 0x100000 (no ELF).
 * Handoff: boot_context at 0x60000 in a 4-page allocation:
 *   0x60000 boot_context (104 B)
 *   0x61000 raw UEFI descriptor scratch (8 KB, ~170 x 48 B entries)
 *   0x63000 E820 output array (4 KB, ~200 x 20 B entries)
 * Memory map: E820 (BOOT_MEMORY_FORMAT_E820).
 */
#include <uefi.h>
#include "../arch.h"

#define X86_KERNEL_BASE       UINT64_C(0x100000)
#define X86_HANDOFF_BASE      UINT64_C(0x60000)
#define X86_HANDOFF_PAGES     4
#define X86_DESC_BASE         UINT64_C(0x61000)
#define X86_DESC_CAPACITY     UINT64_C(0x2000)
#define X86_E820_BASE         UINT64_C(0x63000)
#define X86_E820_CAPACITY     UINT64_C(0x1000)
#define X86_E820_ENTRY_COUNT  (X86_E820_CAPACITY / sizeof(struct E820_ENTRY))

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
                           uintn_t desc_size, uint32_t capacity,
                           struct E820_ENTRY *out, uint32_t *count_out)
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
            /* Always safe to extend the last entry — it was already
             * counted toward capacity. */
            last->length += (uint64_t)m->NumberOfPages << 12;
            last_end += (uint64_t)m->NumberOfPages << 12;
        } else {
            if (count >= capacity) {
                /* E820 output full: stop writing new entries. Any
                 * remaining descriptors would only be a coarse split
                 * of an already-recorded RAM/reserved run, and the
                 * kernel's 32-entry cap (pmm.c) makes further growth
                 * unproductive. The kernel never sees more than
                 * `capacity` entries. */
                break;
            }
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

    /* x86_build_e820 caps its writes at X86_E820_ENTRY_COUNT, so the
     * 4 KB E820 range cannot overflow regardless of how the firmware
     * splits the descriptor stream. */
    x86_build_e820((efi_memory_descriptor_t *)(uintptr_t)desc_phys,
                   (uintn_t)desc_count * desc_size, desc_size,
                   X86_E820_ENTRY_COUNT, e820, &count);

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
