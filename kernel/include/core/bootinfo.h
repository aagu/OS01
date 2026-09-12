#ifndef _KERNEL_BOOTINFO_H
#define _KERNEL_BOOTINFO_H

#include <stdint.h>
#include <stdbool.h>

// All fields use fixed-size types (uint32_t, uint64_t) to ensure
// identical layout regardless of data model (LP64 vs LLP64).
// This matters because the EFI bootloader may be compiled with a
// different toolchain (clang --target=x86_64-pc-win32-coff) that
// uses 4-byte 'unsigned long', while the kernel uses 8-byte.
//
// This header defines the boot_context v2 ABI handed off from the
// firmware-aware bootloader to the kernel. Pointers are physical
// addresses. The boot_context itself is fully arch-neutral — no
// architecture-specific data structure is defined here. x86_64-only
// E820 bits live in kernel/include/arch/x86_64/bootinfo_x86.h.

struct GRAPHICS_INFO
{
	uint32_t HorizontalResolution;
	uint32_t VerticalResolution;
	uint32_t PixelsPerScanLine;

	uint64_t FrameBufferBase;
	uint64_t FrameBufferSize;
};

enum BOOT_CONTEXT_FLAGS {
    BOOT_CONTEXT_HAS_FRAMEBUFFER = 1u << 0,
    BOOT_CONTEXT_HAS_MEMORY_MAP  = 1u << 1,
    BOOT_CONTEXT_HAS_DTB         = 1u << 2,
    BOOT_CONTEXT_HAS_ACPI        = 1u << 3,
    BOOT_CONTEXT_HAS_BOOT_CPU_ID = 1u << 4,
};

// Memory-map format tags. Values are wire-level: each arch's bootloader
// writes the corresponding constant into ctx->memory.format, and the
// kernel-side format dispatch (pmm_arch_normalize, aarch64_ram_init) reads
// it. BOOT_MEMORY_FORMAT_E820 (value 1) is x86_64-only and is declared in
// kernel/include/arch/x86_64/bootinfo_x86.h.
//
// The enum below intentionally leaves a hole at value 1 so the E820
// constant keeps its wire-level value without being defined here.
enum BOOT_MEMORY_FORMAT {
    BOOT_MEMORY_FORMAT_UNKNOWN = 0,
    /* value 1 reserved: BOOT_MEMORY_FORMAT_E820 (x86_64-only, see bootinfo_x86.h) */
    BOOT_MEMORY_FORMAT_GENERIC = 2,
    BOOT_MEMORY_FORMAT_UEFI_RAW = 3,
};

struct BOOT_MEMORY_MAP {
    uint64_t entries;       /* physical address of entries/descriptors */
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t format;
    uint32_t descriptor_version; /* UEFI descriptor version, else zero */
};

struct BOOT_FIRMWARE {
    uint64_t dtb;            /* physical address, or zero */
    uint64_t acpi_rsdp;      /* physical address, or zero */
};

struct boot_context {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint32_t reserved;
    struct GRAPHICS_INFO graphics;
    struct BOOT_MEMORY_MAP memory;
    struct BOOT_FIRMWARE firmware;
    uint64_t boot_cpu_id;
};

#define BOOT_CONTEXT_MAGIC UINT32_C(0x4f533031)
#define BOOT_CONTEXT_VERSION 2u

/* Keep construction trivial and freestanding so early arch code need not
 * pull in libc or any generic kernel subsystem. */
static inline void boot_context_init(struct boot_context *ctx)
{
    uint8_t *p = (uint8_t *)ctx;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*ctx); i++)
        p[i] = 0;
    ctx->magic = BOOT_CONTEXT_MAGIC;
    ctx->version = BOOT_CONTEXT_VERSION;
    ctx->size = (uint32_t)sizeof(*ctx);
}

static inline bool boot_context_valid(const struct boot_context *ctx)
{
    return ctx != (const struct boot_context *)0 &&
           ctx->magic == BOOT_CONTEXT_MAGIC &&
           ctx->version == BOOT_CONTEXT_VERSION &&
           ctx->size == (uint32_t)sizeof(*ctx);
}

#endif
