#ifndef OS01_UEFI_ARCH_H
#define OS01_UEFI_ARCH_H

#include <uefi.h>

/* The posix-uefi runtime spells the status type efi_status_t; the common
 * lifecycle code uses the EFI-specced alias, provided here (mirrors the
 * local typedef each arch boot.c carries). */
typedef efi_status_t EFI_STATUS;

/* bootinfo.h uses these; the posix-uefi runtime does not define them and
 * each bootloader translation unit (main.c and the arch boot.c) includes
 * this header first, so provide them here. */
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
