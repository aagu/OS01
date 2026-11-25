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

static efi_status_t read_boot_config(struct uefi_boot_config *out)
{
#if defined(__aarch64__)
    const bool is_aarch64 = true;
#else
    const bool is_aarch64 = false;
#endif
    efi_guid_t sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    efi_simple_file_system_protocol_t *sfs = NULL;
    efi_file_handle_t *root = NULL;
    efi_file_handle_t *file = NULL;
    char buf[UEFI_BOOT_CONFIG_MAX_BYTES + 1u];
    size_t total = 0;
    unsigned err_line = 0;
    EFI_STATUS status;

    (void)uefi_boot_config_parse(NULL, 0, is_aarch64, out, NULL);

    if (!LIP || !LIP->DeviceHandle) {
        if (!is_aarch64)
            return EFI_SUCCESS;
        arch_puts("UEFI: config.txt volume unavailable\r\n");
        return EFI_LOAD_ERROR;
    }

    status = BS->HandleProtocol(LIP->DeviceHandle, &sfs_guid, (void **)&sfs);
    if (EFI_ERROR(status) || !sfs) {
        if (!is_aarch64)
            return EFI_SUCCESS;
        arch_puts("UEFI: config.txt filesystem unavailable\r\n");
        return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;
    }

    status = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(status) || !root) {
        if (!is_aarch64)
            return EFI_SUCCESS;
        arch_puts("UEFI: config.txt volume open failed\r\n");
        return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;
    }

    status = root->Open(root, &file, L"config.txt", EFI_FILE_MODE_READ, 0);
    if (status == EFI_NOT_FOUND) {
        (void)root->Close(root);
        return EFI_SUCCESS;
    }
    if (EFI_ERROR(status) || !file) {
        (void)root->Close(root);
        if (!is_aarch64)
            return EFI_SUCCESS;
        arch_puts("UEFI: config.txt open failed\r\n");
        return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;
    }

    while (total < sizeof(buf)) {
        uintn_t chunk = (uintn_t)(sizeof(buf) - total);
        status = file->Read(file, &chunk, buf + total);
        if (EFI_ERROR(status)) {
            (void)file->Close(file);
            (void)root->Close(root);
            if (!is_aarch64)
                return EFI_SUCCESS;
            arch_puts("UEFI: config.txt read failed\r\n");
            return status;
        }
        if (chunk == 0)
            break;
        total += (size_t)chunk;
    }

    (void)file->Close(file);
    (void)root->Close(root);

    if (uefi_boot_config_parse(buf, total, is_aarch64, out, &err_line) != 0) {
        if (!is_aarch64) {
            (void)uefi_boot_config_parse(NULL, 0, false, out, NULL);
            return EFI_SUCCESS;
        }
        if (total > UEFI_BOOT_CONFIG_MAX_BYTES) {
            arch_puts("UEFI: config.txt exceeds 4096 bytes\r\n");
        } else {
            char msg[48];
            char num[16];
            size_t n = 0;
            unsigned v = err_line;
            static const char prefix[] = "UEFI: config.txt invalid at line ";
            size_t i = 0;
            while (prefix[i] != '\0')
                msg[n++] = prefix[i++];
            if (v == 0) {
                num[0] = '0';
                i = 1;
            } else {
                i = 0;
                while (v > 0 && i < sizeof(num)) {
                    num[i++] = (char)('0' + (v % 10u));
                    v /= 10u;
                }
            }
            while (i > 0)
                msg[n++] = num[--i];
            msg[n++] = '\r';
            msg[n++] = '\n';
            msg[n] = '\0';
            arch_puts(msg);
        }
        return EFI_LOAD_ERROR;
    }

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
    struct uefi_boot_config boot_cfg;
    EFI_STATUS status;

    (void)argc;
    (void)argv;

    status = read_boot_config(&boot_cfg);
    if (EFI_ERROR(status))
        goto fail;

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

    status = arch_setup_graphics(ctx, &boot_cfg);
    if (EFI_ERROR(status)) {
        arch_puts("UEFI: graphics setup failed\r\n");
        goto fail;
    }

    status = arch_fill_firmware(ctx, &boot_cfg);
    if (EFI_ERROR(status)) {
        arch_puts("UEFI: firmware tables failed\r\n");
        goto fail;
    }

    arch_memory_buffer(&desc_phys, &desc_capacity);
    {
        int ebs_attempted = 0;

        for (;;) {
            map_size = 0;
            status = BS->GetMemoryMap(&map_size, NULL, &map_key,
                                      &desc_size, &desc_version);
            if (status != EFI_BUFFER_TOO_SMALL || desc_size == 0) {
                if (ebs_attempted)
                    for (;;) ;
                break;
            }
            /* Fixed-capacity discipline: if the map grows beyond the
             * pre-reserved buffer, fail rather than reallocate. */
            if ((uint64_t)map_size + UEFI_MAP_SLACK * desc_size > desc_capacity) {
                if (ebs_attempted)
                    for (;;) ;
                arch_puts("UEFI: memory map overflow\r\n");
                status = EFI_BUFFER_TOO_SMALL;
                break;
            }
            if (map_size / desc_size > UINT32_MAX) {
                if (ebs_attempted)
                    for (;;) ;
                arch_puts("UEFI: memory map too large\r\n");
                status = EFI_LOAD_ERROR;
                break;
            }
            map_size = (uintn_t)desc_capacity;
            status = BS->GetMemoryMap(&map_size, (void *)(uintptr_t)desc_phys,
                                      &map_key, &desc_size, &desc_version);
            if (EFI_ERROR(status)) {
                if (ebs_attempted)
                    for (;;) ;
                break;
            }
            if (map_size % desc_size != 0) {
                if (ebs_attempted)
                    for (;;) ;
                arch_puts("UEFI: corrupt handoff\r\n");
                status = EFI_LOAD_ERROR;
                break;
            }
            status = arch_build_memory(ctx, desc_phys, desc_size,
                                       (uint32_t)(map_size / desc_size),
                                       desc_version);
            if (EFI_ERROR(status)) {
                if (ebs_attempted)
                    for (;;) ;
                arch_puts("UEFI: unsafe memory map\r\n");
                break;
            }
            if (!boot_context_valid(ctx)) {
                if (ebs_attempted)
                    for (;;) ;
                arch_puts("UEFI: corrupt handoff\r\n");
                status = EFI_LOAD_ERROR;
                break;
            }
            ebs_attempted = 1;
            status = BS->ExitBootServices(IM, map_key);
            if (status == EFI_INVALID_PARAMETER)
                continue;                    /* map changed: refetch */
            if (EFI_ERROR(status)) {
                for (;;) ;
            }
            arch_enter_kernel(entry, (uint64_t)ctx);   /* noreturn */
        }
    }

fail:
    if (image)
        free(image);
    arch_release();
    return main_error_code(status);
}
