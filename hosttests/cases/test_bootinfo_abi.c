#include <stddef.h>
#include <stdio.h>
#include <kernel/bootinfo.h>

_Static_assert(BOOT_CONTEXT_MAGIC == UINT32_C(0x4f533031),
               "boot context magic");
_Static_assert(BOOT_CONTEXT_VERSION == 2u, "boot context version");
_Static_assert(BOOT_MEMORY_FORMAT_UEFI_RAW == 3,
               "UEFI raw memory map format");
_Static_assert(sizeof(struct BOOT_MEMORY_MAP) == 24, "memory map ABI");
_Static_assert(offsetof(struct BOOT_MEMORY_MAP, descriptor_version) == 20,
               "UEFI descriptor version offset");
_Static_assert(sizeof(struct BOOT_FIRMWARE) == 16, "firmware ABI");
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

    boot_context_init(&ctx);
    if (ctx.magic != BOOT_CONTEXT_MAGIC ||
        ctx.version != BOOT_CONTEXT_VERSION ||
        ctx.size != sizeof(ctx) || ctx.memory.descriptor_version != 0 ||
        !boot_context_valid(&ctx))
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

    puts("bootinfo ABI: PASS");
    return 0;
}
