#ifndef _KERNEL_ARCH_X86_64_BOOTINFO_X86_H
#define _KERNEL_ARCH_X86_64_BOOTINFO_X86_H

// ─────────────────────────────────────────────────────────
//  x86_64 bootinfo extensions
//
//  bootinfo.h (kernel/include/kernel/bootinfo.h) defines the
//  arch-neutral boot_context ABI shared by every arch. This header
//  carries the x86_64-specific bits — currently the legacy BIOS E820
//  record layout and the format enum value used by the x86_64 UEFI
//  bootloader (boot/uefi/arch/x86_64/boot.c) to mark the handoff's
//  memory map as E820.
//
//  AArch64 / future arches never include this header and never see
//  these symbols.
//
//  Wire-level note: BOOT_MEMORY_FORMAT_E820 must remain at value 1
//  because the UEFI bootloader writes this exact constant into
//  ctx->memory.format, and the kernel-side `format` field is part of
//  the ABI. Defined here as `#define` (not an enum value) so it can
//  coexist with the arch-neutral enum in bootinfo.h.
// ─────────────────────────────────────────────────────────

#include <stdint.h>

/* Legacy BIOS E820 entry — 20 bytes, packed.
 * Still produced by the x86_64 UEFI loader (boot/uefi/arch/x86_64/boot.c)
 * and consumed by the x86_64 physical memory manager
 * (kernel/arch/x86_64/pmm_arch.c). AArch64 uses the arch-neutral
 * BOOT_MEMORY_FORMAT_UEFI_RAW path instead. */
struct E820_ENTRY {
    uint64_t address;
    uint64_t length;
    uint32_t type;
} __attribute__((packed));

#define BOOT_MEMORY_FORMAT_E820 1u

#endif /* _KERNEL_ARCH_X86_64_BOOTINFO_X86_H */
