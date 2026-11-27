#ifndef OS01_UEFI_BOOT_CONFIG_H
#define OS01_UEFI_BOOT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UEFI_BOOT_CONFIG_MAX_BYTES      4096u
#define UEFI_BOOT_CONFIG_DEFAULT_WIDTH  1440u
#define UEFI_BOOT_CONFIG_DEFAULT_HEIGHT 900u

enum uefi_boot_platform {
    UEFI_BOOT_PLATFORM_AUTO = 0,
    UEFI_BOOT_PLATFORM_QEMU_VIRT = 1,
    UEFI_BOOT_PLATFORM_RPI5 = 2,
    UEFI_BOOT_PLATFORM_DRAGON_Q8B = 3,
};

enum uefi_boot_console {
    UEFI_BOOT_CONSOLE_DTB = 0,
    UEFI_BOOT_CONSOLE_RP1_HEADER = 1,
    UEFI_BOOT_CONSOLE_ACPI = 2,
};

struct uefi_boot_config {
    enum uefi_boot_platform platform;
    enum uefi_boot_console console;
    uint32_t width;
    uint32_t height;
};

/* Parse an ESP /config.txt byte buffer into *out.
 * (NULL, 0) or ("", 0) represents a missing or empty file and yields defaults.
 * Returns 0 on success, negative on file-level error (size > 4096, embedded
 * NUL, bare CR) or (when aarch64 is true) invalid/duplicate/incompatible
 * platform or console directives.  If error_line is non-NULL, sets it to the
 * 1-based line number of the failure (or 0 if not line-specific). */
int uefi_boot_config_parse(const char *data, size_t size, bool aarch64,
                           struct uefi_boot_config *out,
                           unsigned *error_line);

#endif
