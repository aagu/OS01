/* boot/uefi/main.c — common UEFI bootloader lifecycle.
 *
 * Shared by the x86_64 and aarch64 UEFI bootloaders.  Architecture
 * differences live in arch/<arch>/boot.c behind the hooks in arch/arch.h.
 * Compiled with UEFI_NO_UTF8 on both targets (char_t == wchar_t).
 */
#include <uefi.h>
#include "arch/arch.h"
#include "boot_device.h"

_Static_assert(BOOT_CONTEXT_VERSION == 3u, "boot_context v3");
_Static_assert(sizeof(struct boot_device_info) == 40, "boot_device_info ABI");
_Static_assert(offsetof(struct boot_context, boot_device) == 136,
               "boot_device offset");
_Static_assert(offsetof(struct boot_context, early_mmio) == 176,
               "early_mmio offset");
_Static_assert(sizeof(struct boot_context) == 200, "boot_context v3 ABI");

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

/* Issue AAGU-2 §1 P0-1: fetch 32 bytes from EFI_RNG_PROTOCOL so the
 * kernel's CSPRNG has real entropy at boot. Without this, x86_64 in
 * QEMU (no RDRAND/RDSEED) and aarch64 (no RNDR yet) would land in
 * the cycle-counter ^ jiffies fallback, which random_init() now
 * rejects as fail-closed — every user-space program then aborts on
 * its all-zero stack canary. Best-effort: missing RNG must not
 * abort the boot (the kernel's fallback path will log "no HW
 * entropy" and the operator can decide whether to plug a TPM/RNG
 * or accept the fail-closed boot). */
efi_status_t capture_entropy(struct boot_context *ctx)
{
    efi_guid_t rng_guid = EFI_RNG_PROTOCOL_GUID;
    efi_rng_protocol_t *rng = NULL;
    EFI_STATUS status;

    if (!ctx)
        return EFI_INVALID_PARAMETER;

    status = BS->LocateProtocol(&rng_guid, NULL, (void **)&rng);
    if (EFI_ERROR(status) || !rng)
        return EFI_SUCCESS;   /* best-effort */

    status = rng->GetRNG(rng, NULL, sizeof(ctx->boot_entropy),
                         ctx->boot_entropy);
    if (EFI_ERROR(status))
        return EFI_SUCCESS;   /* best-effort */

    ctx->flags |= BOOT_CONTEXT_HAS_BOOT_ENTROPY;
    return EFI_SUCCESS;
}

#define OS01_DEVPATH_MAX_BYTES 4096u
#define OS01_EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID \
    { 0x0379be4eU, 0xd706U, 0x437dU, { 0xb0U, 0x37U, 0xedU, 0xb8U, 0x2fU, 0xb7U, 0x72U, 0xa4U } }

typedef uintn_t (EFIAPI *os01_get_device_path_size_t)(
    const efi_device_path_t *DevicePath);

typedef struct {
    os01_get_device_path_size_t GetDevicePathSize;
} os01_efi_dpu_protocol_t;

static size_t bounded_devpath_size(const uint8_t *dp, size_t max_bytes)
{
    size_t offset = 0;

    while (offset + 4u <= max_bytes) {
        uint8_t type = dp[offset];
        uint8_t subtype = dp[offset + 1u];
        size_t node_len = (size_t)dp[offset + 2u] |
                          ((size_t)dp[offset + 3u] << 8);

        if (node_len < 4u || node_len > max_bytes - offset)
            return 0;
        offset += node_len;
        if (type == 0x7fu && subtype == 0xffu)
            return (node_len == 4u) ? offset : 0;
    }
    return 0;
}

static void put_hex_byte(char *dst, uint8_t b)
{
    static const char hex[] = "0123456789abcdef";
    dst[0] = hex[(b >> 4) & 0xfu];
    dst[1] = hex[b & 0xfu];
}

static void format_efi_guid(const uint8_t g[16], char out[37])
{
    static const uint8_t order[16] = {
        3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15
    };
    size_t pos = 0;

    for (size_t i = 0; i < 16u; i++) {
        if (i == 4u || i == 6u || i == 8u || i == 10u)
            out[pos++] = '-';
        put_hex_byte(out + pos, g[order[i]]);
        pos += 2u;
    }
    out[pos] = '\0';
}

static void capture_boot_device(struct boot_context *ctx)
{
    efi_guid_t dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    efi_guid_t dpu_guid = OS01_EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID;
    efi_device_path_t *dp = NULL;
    os01_efi_dpu_protocol_t *dpu = NULL;
    struct boot_device_info info;
    size_t dp_size = 0;
    EFI_STATUS status;

    if (!ctx)
        return;

    ctx->flags &= ~BOOT_CONTEXT_HAS_BOOT_DEVICE;

    if (!LIP || !LIP->DeviceHandle) {
        arch_puts("UEFI: boot device absent (no device handle)\r\n");
        return;
    }

    status = BS->HandleProtocol(LIP->DeviceHandle, &dp_guid, (void **)&dp);
    if (EFI_ERROR(status) || !dp) {
        arch_puts("UEFI: boot device absent (no device path)\r\n");
        return;
    }

    status = BS->LocateProtocol(&dpu_guid, NULL, (void **)&dpu);
    if (!EFI_ERROR(status) && dpu && dpu->GetDevicePathSize) {
        uintn_t raw_size = dpu->GetDevicePathSize(dp);
        if (raw_size == 0 || raw_size > OS01_DEVPATH_MAX_BYTES) {
            arch_puts("UEFI: boot device absent (devpath size out of range)\r\n");
            return;
        }
        dp_size = (size_t)raw_size;
    } else {
        dp_size = bounded_devpath_size((const uint8_t *)dp,
                                       OS01_DEVPATH_MAX_BYTES);
        if (dp_size == 0) {
            arch_puts("UEFI: boot device absent (devpath walk failed)\r\n");
            return;
        }
    }

    if (os01_bootdev_from_devpath((const uint8_t *)dp, dp_size, &info) != 0) {
        arch_puts("UEFI: boot device absent (no valid GPT HD node)\r\n");
        return;
    }

    ctx->boot_device = info;
    ctx->flags |= BOOT_CONTEXT_HAS_BOOT_DEVICE;

    {
        char uuid[37];
        format_efi_guid(info.partition_guid, uuid);
        arch_puts("UEFI: ESP GUID=");
        arch_puts(uuid);
        arch_puts("\r\n");
    }
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

    capture_boot_device(ctx);

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
#if defined(AARCH64_SKIP_EBS) && AARCH64_SKIP_EBS
        /* Bring-up workaround: the Raspberry Pi 5 UEFI tears down the
         * PCIe/RP1 path as it shuts down, taking the CPU's access to the
         * header UART with it (and its USB shutdown is a known lockup
         * source). Cancel the watchdog and enter the kernel with boot
         * services resident; the kernel masks IRQs in its first
         * instruction and never calls back into firmware services. */
        if (BS->SetWatchdogTimer)
            (void)BS->SetWatchdogTimer(0, 0, 0, NULL);
        status = EFI_SUCCESS;
#else
        status = BS->ExitBootServices(IM, map_key);
        if (status == EFI_INVALID_PARAMETER)
            continue;                    /* map changed: refetch */
        if (EFI_ERROR(status))
            break;
#endif
        arch_enter_kernel(entry, (uint64_t)ctx);   /* noreturn */
    }

fail:
    if (image)
        free(image);
    arch_release();
    return main_error_code(status);
}
