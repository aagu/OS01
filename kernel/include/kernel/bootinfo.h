#ifndef _KERNEL_BOOTINFO_H
#define _KERNEL_BOOTINFO_H

#include <stdint.h>

// All fields use fixed-size types (uint32_t, uint64_t) to ensure
// identical layout regardless of data model (LP64 vs LLP64).
// This matters because the EFI bootloader may be compiled with a
// different toolchain (clang --target=x86_64-pc-win32-coff) that
// uses 4-byte 'unsigned long', while the kernel uses 8-byte.

struct GRAPHICS_INFO
{
	uint32_t HorizontalResolution;
	uint32_t VerticalResolution;
	uint32_t PixelsPerScanLine;

	uint64_t FrameBufferBase;
	uint64_t FrameBufferSize;
};

struct E820_ENTRY
{
	uint64_t address;
	uint64_t length;
	uint32_t type;
}__attribute__((packed));

struct MEMORY_INFO
{
	uint32_t E820_Entry_count;
	uint64_t E820_Entry;  // physical address of E820 array
};

struct BOOT_INFO
{
	struct GRAPHICS_INFO Graphics_Info;
	struct MEMORY_INFO E820_Info;
	uint64_t RSDP;
	uint8_t  BootFromBIOS;
};

/* Architecture-neutral boot handoff.  BOOT_INFO remains the legacy UEFI
 * ABI consumed by the x86 kernel; new architecture entries should populate
 * this fixed-width descriptor instead.  Pointers are physical addresses. */
enum BOOT_CONTEXT_FLAGS {
    BOOT_CONTEXT_HAS_FRAMEBUFFER = 1u << 0,
    BOOT_CONTEXT_HAS_MEMORY_MAP  = 1u << 1,
    BOOT_CONTEXT_HAS_DTB         = 1u << 2,
    BOOT_CONTEXT_HAS_ACPI        = 1u << 3,
    BOOT_CONTEXT_HAS_BOOT_CPU_ID = 1u << 4,
};

enum BOOT_MEMORY_FORMAT {
    BOOT_MEMORY_FORMAT_UNKNOWN = 0,
    BOOT_MEMORY_FORMAT_E820 = 1,
    BOOT_MEMORY_FORMAT_GENERIC = 2,
};

struct BOOT_MEMORY_MAP {
    uint64_t entries;       /* physical address of generic entries */
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t format;
};

struct BOOT_FIRMWARE {
    uint64_t dtb;            /* physical address, or zero */
    uint64_t acpi_rsdp;      /* physical address, or zero */
};

struct boot_context {
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint32_t reserved;
    struct GRAPHICS_INFO graphics;
    struct BOOT_MEMORY_MAP memory;
    struct BOOT_FIRMWARE firmware;
    uint64_t boot_cpu_id;
};

#define BOOT_CONTEXT_VERSION 1u

/* Keep construction trivial and freestanding so early arch code need not
 * pull in libc or any generic kernel subsystem. */
static inline void boot_context_init(struct boot_context *ctx)
{
    uint8_t *p = (uint8_t *)ctx;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof(*ctx); i++)
        p[i] = 0;
    ctx->version = BOOT_CONTEXT_VERSION;
    ctx->size = (uint32_t)sizeof(*ctx);
}

static inline void boot_context_from_legacy(struct boot_context *ctx,
                                             const struct BOOT_INFO *legacy)
{
    boot_context_init(ctx);
    ctx->graphics = legacy->Graphics_Info;
    if (ctx->graphics.FrameBufferBase != 0 &&
        ctx->graphics.FrameBufferSize != 0)
        ctx->flags |= BOOT_CONTEXT_HAS_FRAMEBUFFER;
    ctx->memory.entries = legacy->E820_Info.E820_Entry;
    ctx->memory.entry_count = legacy->E820_Info.E820_Entry_count;
    ctx->memory.entry_size = (uint32_t)sizeof(struct E820_ENTRY);
    ctx->memory.format = BOOT_MEMORY_FORMAT_E820;
    if (ctx->memory.entries != 0 && ctx->memory.entry_count != 0)
        ctx->flags |= BOOT_CONTEXT_HAS_MEMORY_MAP;
    ctx->firmware.acpi_rsdp = legacy->RSDP;
    if (ctx->firmware.acpi_rsdp != 0)
        ctx->flags |= BOOT_CONTEXT_HAS_ACPI;
}

static inline void boot_context_from_aarch64(struct boot_context *ctx,
                                              uint64_t dtb,
                                              uint64_t boot_cpu_id)
{
    boot_context_init(ctx);
    ctx->firmware.dtb = dtb;
    ctx->boot_cpu_id = boot_cpu_id;
    if (dtb != 0)
        ctx->flags |= BOOT_CONTEXT_HAS_DTB;
    ctx->flags |= BOOT_CONTEXT_HAS_BOOT_CPU_ID;
}

#endif
