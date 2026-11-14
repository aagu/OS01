#ifndef OS01_UEFI_BOOT_DEVICE_H
#define OS01_UEFI_BOOT_DEVICE_H

#include <stddef.h>
#include <stdint.h>

struct boot_device_info;

/* Parse a single-instance UEFI Device Path ending with End Entire (0x7f 0xff)
 * and extract the unique GPT Hard Drive (0x04 0x01) node into *out.
 * Returns 0 on success; leaves *out untouched and returns -1 on any error. */
int os01_bootdev_from_devpath(const uint8_t *dp, size_t max_len,
                              struct boot_device_info *out);

#endif
