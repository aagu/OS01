#include <stddef.h>
#include <stdio.h>
#include <core/bootinfo.h>

_Static_assert(BOOT_CONTEXT_MAGIC == UINT32_C(0x4f533031),
               "boot context magic");
_Static_assert(BOOT_CONTEXT_VERSION == 3u, "boot context version");
_Static_assert(BOOT_CONTEXT_VERSION_RP1_PCI == 4u,
               "boot context rp1 pci version");
_Static_assert(BOOT_CONTEXT_V3_SIZE == 200u, "boot context v3 wire size");
_Static_assert(BOOT_CONTEXT_HAS_BOOT_DEVICE == (1u << 6),
               "boot device flag");
_Static_assert(BOOT_CONTEXT_HAS_RP1_PCI == (1u << 9),
               "rp1 pci flag");
_Static_assert(BOOT_MEMORY_FORMAT_UEFI_RAW == 3,
               "UEFI raw memory map format");
_Static_assert(sizeof(struct BOOT_MEMORY_MAP) == 24, "memory map ABI");
_Static_assert(offsetof(struct BOOT_MEMORY_MAP, descriptor_version) == 20,
               "UEFI descriptor version offset");
_Static_assert(sizeof(struct BOOT_FIRMWARE) == 16, "firmware ABI");
_Static_assert(sizeof(struct boot_device_info) == 40, "boot device info ABI");
_Static_assert(offsetof(struct boot_device_info, partition_guid) == 0,
               "partition_guid offset");
_Static_assert(offsetof(struct boot_device_info, partition_start_lba) == 16,
               "partition_start_lba offset");
_Static_assert(offsetof(struct boot_device_info, partition_size_lba) == 24,
               "partition_size_lba offset");
_Static_assert(offsetof(struct boot_device_info, partition_number) == 32,
               "partition_number offset");
_Static_assert(offsetof(struct boot_device_info, signature_type) == 36,
               "signature_type offset");
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
_Static_assert(offsetof(struct boot_context, boot_device) == 136,
               "boot_device offset");
_Static_assert(offsetof(struct boot_context, early_mmio) == 176,
               "early_mmio offset");
_Static_assert(sizeof(struct BOOT_EARLY_MMIO) == 24, "early mmio ABI");
_Static_assert(offsetof(struct BOOT_EARLY_MMIO, console_base) == 0,
               "console_base offset");
_Static_assert(offsetof(struct BOOT_EARLY_MMIO, device_base) == 8,
               "device_base offset");
_Static_assert(offsetof(struct BOOT_EARLY_MMIO, device_blocks) == 16,
               "device_blocks offset");
_Static_assert(BOOT_CONTEXT_HAS_EARLY_MMIO == (1u << 7),
               "early mmio flag");
_Static_assert(BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED == (1u << 8),
               "firmware-configured console flag");
_Static_assert(sizeof(struct boot_rp1_pci_info) == 40, "rp1 pci info ABI");
_Static_assert(offsetof(struct boot_rp1_pci_info, bar0_pci) == 0,
               "bar0_pci offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, bar0_cpu) == 8,
               "bar0_cpu offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, bar0_size) == 16,
               "bar0_size offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, table_offset) == 24,
               "table_offset offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, pba_offset) == 28,
               "pba_offset offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, segment) == 32,
               "segment offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, entries) == 34,
               "entries offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, bus) == 36,
               "bus offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, device) == 37,
               "device offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, function) == 38,
               "function offset");
_Static_assert(offsetof(struct boot_rp1_pci_info, msix_cap) == 39,
               "msix_cap offset");
_Static_assert(offsetof(struct boot_context, rp1_pci) == 200,
               "rp1_pci offset");
_Static_assert(sizeof(struct boot_context) == 240, "boot context v4 ABI");

int main(void)
{
    struct boot_context ctx;

    boot_context_init(&ctx);
    if (ctx.magic != BOOT_CONTEXT_MAGIC ||
        ctx.version != BOOT_CONTEXT_VERSION ||
        ctx.size != BOOT_CONTEXT_V3_SIZE || ctx.memory.descriptor_version != 0 ||
        !boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != NULL)
        return 1;

    ctx.magic ^= 1u;
    if (boot_context_valid(&ctx))
        return 1;
    ctx.magic = BOOT_CONTEXT_MAGIC;

    ctx.version = 2u;
    ctx.size = 136u;
    if (boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != NULL)
        return 1;
    ctx.version = BOOT_CONTEXT_VERSION;
    ctx.size = BOOT_CONTEXT_V3_SIZE;

    ctx.version = 5u;
    ctx.size = sizeof(ctx);
    if (boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != NULL)
        return 1;
    ctx.version = BOOT_CONTEXT_VERSION;
    ctx.size = BOOT_CONTEXT_V3_SIZE;

    ctx.size--;
    if (boot_context_valid(&ctx))
        return 1;
    ctx.size = BOOT_CONTEXT_V3_SIZE;

    /* Early-MMIO handoff (appended v3 fields): zero-filled handoff
     * must degrade to "flag clear", and the flag-carrying handoff
     * must keep the fields at their pinned offsets. */
    if (ctx.flags & BOOT_CONTEXT_HAS_EARLY_MMIO)
        return 1;
    if (ctx.early_mmio.console_base != 0 ||
        ctx.early_mmio.device_base != 0 ||
        ctx.early_mmio.device_blocks != 0)
        return 1;
    ctx.early_mmio.console_base = UINT64_C(0x09000000);
    ctx.early_mmio.device_base = UINT64_C(0x08000000);
    ctx.early_mmio.device_blocks = 9;
    ctx.flags |= BOOT_CONTEXT_HAS_EARLY_MMIO;
    if (!boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != NULL)
        return 1;
    if (ctx.early_mmio.device_blocks != 9)
        return 1;

    /* v3 with RP1 flag is invalid. */
    ctx.flags |= BOOT_CONTEXT_HAS_RP1_PCI;
    if (boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != NULL)
        return 1;

    /* v4 with RP1 flag and full size is valid. */
    ctx.version = BOOT_CONTEXT_VERSION_RP1_PCI;
    ctx.size = sizeof(ctx);
    ctx.rp1_pci.bar0_pci = UINT64_C(0x100000);
    ctx.rp1_pci.bar0_cpu = UINT64_C(0x1f00100000);
    ctx.rp1_pci.bar0_size = UINT64_C(0x4000);
    ctx.rp1_pci.entries = 61u;
    if (!boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != &ctx.rp1_pci)
        return 1;

    /* v4 missing RP1 flag is invalid. */
    ctx.flags &= ~BOOT_CONTEXT_HAS_RP1_PCI;
    if (boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != NULL)
        return 1;

    /* Truncated v4 is invalid. */
    ctx.flags |= BOOT_CONTEXT_HAS_RP1_PCI;
    ctx.size = BOOT_CONTEXT_V3_SIZE;
    if (boot_context_valid(&ctx) || boot_context_rp1_pci(&ctx) != NULL)
        return 1;

    puts("bootinfo ABI: PASS");
    return 0;
}
