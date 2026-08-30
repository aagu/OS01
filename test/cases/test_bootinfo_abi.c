#include <stddef.h>
#include <stdio.h>
#include <kernel/bootinfo.h>

enum aarch64_boot_mode {
    AARCH64_BOOT_DIRECT_ZERO,
    AARCH64_BOOT_DIRECT_FDT,
    AARCH64_BOOT_UEFI,
    AARCH64_BOOT_CORRUPT,
};

enum aarch64_boot_mode aarch64_select_boot_mode(
    uint64_t x0, const struct boot_context *fixed_context);

_Static_assert(BOOT_CONTEXT_MAGIC == UINT32_C(0x4f533031),
               "boot context magic");
_Static_assert(BOOT_CONTEXT_VERSION == 2u, "boot context version");
_Static_assert(BOOT_MEMORY_FORMAT_UEFI_RAW == 3,
               "UEFI raw memory map format");
_Static_assert(sizeof(struct BOOT_MEMORY_MAP) == 24, "memory map ABI");
_Static_assert(sizeof(struct BOOT_FIRMWARE) == 16, "firmware ABI");
_Static_assert(offsetof(struct BOOT_INFO, Graphics_Info) == 0,
               "legacy graphics offset");
_Static_assert(offsetof(struct BOOT_INFO, E820_Info) == 32,
               "legacy memory offset");
_Static_assert(offsetof(struct BOOT_INFO, RSDP) == 48,
               "legacy rsdp offset");
_Static_assert(sizeof(struct BOOT_INFO) == 64, "legacy boot ABI");
_Static_assert(offsetof(struct boot_context, magic) == 0,
               "magic leading offset");
_Static_assert(offsetof(struct boot_context, version) == 4,
               "version leading offset");
_Static_assert(offsetof(struct boot_context, size) == 8,
               "size leading offset");
_Static_assert(offsetof(struct boot_context, graphics) == 24,
               "graphics offset");
_Static_assert(offsetof(struct boot_context, memory) == 56,
               "memory offset");
_Static_assert(offsetof(struct boot_context, firmware) == 80,
               "firmware offset");
_Static_assert(sizeof(struct boot_context) == 104, "boot context ABI");

int main(void)
{
    struct boot_context ctx;
    struct BOOT_INFO legacy = { 0 };

    boot_context_init(&ctx);
    if (ctx.magic != BOOT_CONTEXT_MAGIC ||
        ctx.version != BOOT_CONTEXT_VERSION ||
        ctx.size != sizeof(ctx) || !boot_context_valid(&ctx))
        return 1;

    ctx.magic ^= 1u;
    if (boot_context_valid(&ctx))
        return 1;
    ctx.magic = BOOT_CONTEXT_MAGIC;

    ctx.version ^= 1u;
    if (boot_context_valid(&ctx))
        return 1;
    ctx.version = BOOT_CONTEXT_VERSION;

    ctx.size--;
    if (boot_context_valid(&ctx))
        return 1;

    legacy.E820_Info.E820_Entry = 0x200000;
    legacy.E820_Info.E820_Entry_count = 2;
    legacy.RSDP = 0x12340000;
    boot_context_from_legacy(&ctx, &legacy);
    if (ctx.version != BOOT_CONTEXT_VERSION ||
        ctx.size != sizeof(ctx) || ctx.firmware.acpi_rsdp != legacy.RSDP ||
        ctx.memory.format != BOOT_MEMORY_FORMAT_E820 ||
        !(ctx.flags & BOOT_CONTEXT_HAS_ACPI) ||
        (ctx.flags & BOOT_CONTEXT_HAS_BOOT_CPU_ID))
        return 1;

    boot_context_from_aarch64(&ctx, 0x40000000, 7);
    if (ctx.firmware.dtb != 0x40000000 || ctx.boot_cpu_id != 7 ||
        !(ctx.flags & BOOT_CONTEXT_HAS_DTB) ||
        !(ctx.flags & BOOT_CONTEXT_HAS_BOOT_CPU_ID))
        return 1;

    /* These cases protect the kernel-entry boundary: only the reserved
     * UEFI address is interpreted as a boot_context; all other x0 values
     * retain their direct-boot DTB/zero meaning. */
    boot_context_init(&ctx);
    if (aarch64_select_boot_mode(0, &ctx) != AARCH64_BOOT_DIRECT_ZERO)
        return 1;
    if (aarch64_select_boot_mode(UINT64_C(0x40000000), &ctx) !=
        AARCH64_BOOT_DIRECT_FDT)
        return 1;
    if (aarch64_select_boot_mode(UINT64_C(0x401e0000), &ctx) !=
        AARCH64_BOOT_UEFI)
        return 1;
    ctx.magic ^= 1u;
    if (aarch64_select_boot_mode(UINT64_C(0x401e0000), &ctx) !=
        AARCH64_BOOT_CORRUPT)
        return 1;
    puts("bootinfo ABI: PASS");
    return 0;
}
