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
        /* Fixed-capacity discipline (aarch64's established behavior): if the
         * map grows between the size query and the fetch, fail rather than
         * reallocate — the descriptor buffer is pre-reserved and must not be
         * reallocated after it has been committed. A BUFFER_TOO_SMALL here
         * is intentionally an error, not a retry. */
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
        if (map_size % desc_size != 0) {
            /* Corrupt handoff: the firmware filled the buffer with a
             * size that is not a whole number of descriptors, so the
             * stream is unaligned with desc_size and cannot be safely
             * walked. */
            arch_puts("UEFI: corrupt handoff\r\n");
            status = EFI_LOAD_ERROR;
            break;
        }
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
